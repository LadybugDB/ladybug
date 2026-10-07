# Checkpoint Recovery

## Commit point

A checkpoint commits when the main WAL's `CHECKPOINT` record is durable. Before that record exists,
recovery discards the shadow files written for it and replays the WAL. After it exists, recovery
applies the shadow files of the main database, every graph created with `CREATE GRAPH`, and every
partition child, then retires the WALs and removes the shadows. A crash at any point either repeats
the shadow apply or finds nothing left to do.

The checkpointer keeps that ordering:

1. Flush the main, graph, and partition shadow files and sync their shared parent directory.
2. Write and sync the main `CHECKPOINT` record.
3. Apply the shadow pages to the data files.
4. Retire the graph WALs, then the main WAL: truncate, sync, unlink, and sync the parent directory.
5. Remove the shadow files.

Recovery repairs a file before parsing anything from it. The main file's shadow pages are
replayed before its catalog and metadata are read, and partition children follow the same rule:
recovery registers each child's file, replays its committed shadow pages, and only then reads the
child's database header, page manager, and table metadata. A power loss during step 3 can leave a
child's new header page durable while the page-manager pages it references are still torn; parsing
the file before the replay would read garbage and make the committed checkpoint unrecoverable.

The parent-directory syncs make renames and removals durable against power loss only where the
filesystem supports directory syncing: not on Windows, not where the OS reports it unsupported
(the error is accepted silently), and not on extension filesystems that do not implement
directory sync. There the ordering still recovers from a process crash — file operations already
reached the OS — but power-loss durability of those directory entries is not guaranteed.

If a step fails after the commit point, the database refuses further writes until it is reopened,
and recovery finishes the checkpoint.

## Checkpoint bundle format

`CheckpointRecord::bundleFormatVersion` is 2 for checkpoints written by current builds, including
nightly builds made from them. Version 1 records come from the first builds of this recovery
format, and version 0 records come from older builds, including 0.21.2 and every earlier release,
and have no version field. Recovery rejects an unsupported checkpoint version before applying that
checkpoint's shadow pages; opening a database may already have recovered other graphs before it
encounters the unsupported version.

Version 1 and version 2 share the bundle layout. A checkpoint of either version stamps every
shadow file header with `ShadowFile::CHECKPOINT_BUNDLE_DATABASE_ID` instead of a real database ID,
and recovery identifies the data file through the last database-header page in the shadow, which
must match the data file's database ID before any page is written.

Version 2 changes the WAL record encoding. Every record a version 2 build writes ends with an
`ownerCatalogName` trailer naming the graph catalog the record replays against; the trailer is
empty for main-database records, and its absence in a version 1 record is detected from the
record's framed length. A sequence update that names its sequence is written as the distinct
`UPDATE_SEQUENCE_NAMED` record type — the name lets replay find the sequence by name, because an
implicit serial sequence has no create record, so its entry ID can shift between logging and
replay — while a version 1 build writes only the plain `UPDATE_SEQUENCE` record. Recovery decodes
both versions' records. An older build cannot: it reads a record type it does not know as an
invalid record type, and a record type it does know decodes with the trailer skipped and replays
against the main catalog, so a WAL written by a version 2 build must be replayed and checkpointed
by a version 2 build before an older build opens the database (see Downgrades).

A graph data file opened on its own (outside the database it was created in) checkpoints through
the same format: its WAL ends in a version 2 `CHECKPOINT` record and its shadow carries the
sentinel. Reopening the parent database recovers such a graph from that record — including a
checkpoint interrupted after the record became durable — after validating the graph's WAL header
against the graph data file and requiring the sentinel in its shadow header.

Every bundle also records the database ID of the database that is writing it. When the parent
database checkpoints, its graphs' shadows are stamped with the parent's ID, so a graph opened on
its own can tell that a pending bundle belongs to the parent: the bundle's fate is decided by the
parent's WAL, which the standalone open cannot see. Such an open refuses with an error rather
than replaying or discarding the bundle, and leaves the shadow and WAL files untouched. Reopen
the database that owns the bundle so its recovery finishes the checkpoint, then open the graph on
its own again.

Recovery reads the checksum flag of an attached-graph WAL from the WAL's own header, so a graph
opened on its own may be opened with either checksum setting: its committed changes are replayed
or its committed checkpoint recovered when the parent database is reopened.

## Downgrades

Before opening a database that a version 2 build has written with an older build, run `CHECKPOINT`
on a version 2 build and let it succeed. The checkpoint is the step that empties the WAL; a clean
close does not, because `force_checkpoint_on_close=false` leaves version 2 records in the WAL tail
and an auto-checkpoint fires only once the WAL grows past `checkpoint_threshold`.

A pending version 2 checkpoint means `<db>.wal` or `<db>.wal.checkpoint` ends in a `CHECKPOINT`
record and `<db>.shadow` exists. Older builds refuse such a database because the shadow header does
not match the data file. Their error message suggests deleting the shadow file. **Do not delete it.**
After the commit point, the shadow files hold the only copy of the committed pages, and deleting them
loses committed data. Reopen the database with a build that writes version 2, let recovery finish,
and checkpoint before going back to an older build.

An ordinary version 2 WAL tail — owner trailers on the records but no pending checkpoint — is the
quieter hazard: an older build does not reliably refuse it. A record type the older build knows
decodes with the trailer skipped as unknown trailing bytes and replays against the main catalog,
silently misapplying records meant for a graph. A record type the older build does not know, such
as `UPDATE_SEQUENCE_NAMED`, is no safer: with the default non-throwing replay configuration the
decoding error is treated like a corrupt WAL tail — the older build replays the committed prefix
and truncates the WAL there, silently discarding that transaction and every transaction recorded
after it. Setting `throwOnWalReplayFailure` to true rejects an unknown record before that WAL
is applied, but it still does not make downgrading safe: a WAL containing only recognized
record types passes validation, and its owner trailers are ignored. This is why the successful
`CHECKPOINT`, not a clean close, is the downgrade gate.

A database with neither a pending checkpoint nor any remaining version 2 records has the same
on-disk format as before and opens with older builds.

## Version 0 checkpoints

Older builds stamped the WAL and shadow headers with whichever graph was selected when the file was
written, so these IDs can differ from the file's own database ID. Recovery accepts such headers
only for version 0 records, and then requires the shadow's last database-header page to match the
data file it is applied to.

Older builds also committed each graph's checkpoint separately, with a `CHECKPOINT` record in the
graph's own WAL, after the main checkpoint had finished. Recovery applies a graph shadow when the
graph's WAL ends in such a record, even if the main WAL has none. It refuses to open the database
if that graph shadow is missing, because the graph pages may not have been applied. A graph shadow
without a graph `CHECKPOINT` record was never committed, and recovery discards it.
