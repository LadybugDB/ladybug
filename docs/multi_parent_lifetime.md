# Multi-parent packed child slices: lifetime & representation

Context: the `enable_packed_path_extend` path reuses the adjacency-list (CSR)
rel scan to implement a physical packed extend. `ScanRelTable` attaches a
`PackedChildSlices` descriptor to the **output** `DataChunkState`
(`scanState->outState`, the nbr/child group) so a downstream consumer can
correlate each batch of children with the parent that produced them.

This note records the lifetime and representation constraints for that
descriptor, in particular whether `parentPositions` can be a **non-owning
pointer to the parent's selection vector** rather than an owned copy.

## Current state (single-parent-per-batch)

The CSR scan processes **one bound node per output batch**. Concretely, the
scan calls `RelTableScanState::setNodeIDVectorToFlat(selPos)` so the bound
node vector is flat with `selSize == 1`, and `ScanRelTable` does:

```cpp
const auto& boundSelVector = scanState->nodeIDVector->state->getSelVector();
DASSERT(boundSelVector.getSelSize() == 1);
scanState->outState->setSingleParentPackedChildSlice(boundSelVector[0], outputSize);
```

So `parentPositions` is effectively a single element today. A pointer to a
one-element selection vector saves nothing and adds aliasing risk for no
benefit. The pointer approach only pays off in the aspirational multi-parent
packed scan (e.g. 2048 parents, ~10 outputs each) that the review comment
describes — which the CSR scan does **not** currently perform in one batch.

Also note: **no production operator reads `getPackedChildSlices()` today.**
`PackedFilteredCount::countMatchesForCurrentTuple` uses
`lhsValueVector` / `rhsValueVector` / `multiplicityStates`, not the packed
slices. Only the unit test reads them. So whatever representation is chosen,
the consumer contract is still ours to define.

## Can `parentPositions` be a pointer to the parent's selection vector?

**Yes — but only if the offsets convention is changed, and only under a strict
synchronous-consumption lifetime rule.**

### Semantic change required

Today `parentPositions` is an **owned copy** holding only parents that
produced children (the "drop parents without matches" invariant):

```
parentPositions.size() == num non-empty parents
offsets.size()          == parentPositions.size() + 1   (prefix sum)
```

If `parentPositions` becomes a **non-owning pointer to the parent's selection
vector**, it necessarily points at *all* parents in the input batch, including
zero-child ones. The invariant must become:

```
parentPositions points to parentSelVector (size = parentSelSize)
offsets.size()  == parentSelSize + 1
offsets[i] == offsets[i+1]  ⟹  parent i has no children  (skip it)
```

The "drop" then happens at the *consumer* (it skips zero-length ranges) rather
than at scan time. This is arguably cleaner and is exactly what the review
comment meant by "the active slice is implicitly available" — no explicit
start/end is maintained, just a prefix sum over the input sel vector.

So: a pointer works **iff** the "prefix-sum over all parents, zeros allowed"
offsets convention is adopted. The current "only non-empty parents" copy
semantics is incompatible with a raw pointer to the full sel vector.

### The real constraint: lifetime / aliasing

This is the part to be careful about.

- `parentPositions` lives on the **output** `DataChunkState`
  (`scanState->outState`, the nbr/child group).
- The parent's selection vector lives on the **input** `DataChunkState`
  (`scanState->nodeIDVector->state`, the bound/parent group). These are
  distinct `DataChunkState` objects (see
  `ScanRelTable::initLocalStateInternal` — `RelTableScanState` is constructed
  with `boundNodeIDVector` and `nbrNodeIDVector->state` as outState; the bound
  vector carries its own state).

A pointer is safe **only while that input selection vector's contents remain
valid**. In a strict pull pipeline the input is stable from the moment the
extend returns until the consumer re-pulls the extend's child
(`children[0]->getNextTuple`), at which point `setNodeIDVectorToFlat` /
`initScanState` overwrite it. So:

- **Safe**: a consumer that reads `getPackedChildSlices()` synchronously off
  the output vectors before pulling the extend again (the immediate parent
  operator in the pipeline).
- **Unsafe**: across a materialization boundary. If the child vectors + slices
  are ever appended to a `FactorizedTable` / hash table and read back later,
  the input data chunk will have been advanced and the pointer dangles (or
  silently aliases wrong data). `FactorizedTable` does not know about
  `packedChildSlices` at all, so the slices wouldn't even survive — but if
  someone later wires that up, a raw pointer into the transient input sel
  vector is a latent bug.

Note also that `SelectionVector` is held by `shared_ptr` in `DataChunkState`,
so the *object* is stable, but its **contents are mutable in place**
(`setToFiltered` / `setToUnfiltered` rewrite the buffer). Aliasing the buffer
therefore means "I see whatever the input currently holds," which is the
hazard.

## Recommendation

- For now (one-parent-per-batch): keep the owned `std::vector<sel_t>` (or
  even just `sel_t parentPos` + `sel_t numValues`, since it is always
  single-parent). Do not introduce a pointer.
- When true multi-parent packed scanning is implemented: switch to
  `const SelectionVector* parentSelVector` + prefix-sum `offsets` over *all*
  parents (zeros allowed), and document the lifetime rule: "valid only for
  synchronous consumption of this output batch; do not persist across
  materialization." Better still, store a `shared_ptr<SelectionVector>` (the
  state already exposes `getSelVectorShared()`) so the pointer is kept alive
  even if the input state is reused — that removes the dangling risk at the
  cost of one refcount, while still aliasing mutable contents (so snapshot
  contents, not just share). The safest middle ground is
  `shared_ptr<SelectionVector>` + copy the *selected positions* lazily, but
  that is back to copying.

## Summary

Technically `parentPositions` can be a pointer to the parent's selection
vector; semantically it forces the "all parents, zeros-in-offsets" convention;
and the lifetime hazard means a raw pointer is only safe for synchronous
in-pipeline consumption, not across a `FactorizedTable`. Defer this change
until the multi-parent scan actually exists.
