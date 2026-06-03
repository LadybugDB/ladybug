# NaviX vs Vanilla Filtered Vector Search

This run used the rebuilt `ladybug-python` pybind artifact with Homebrew LLVM:

```bash
CC=/opt/homebrew/opt/llvm/bin/clang CXX=/opt/homebrew/opt/llvm/bin/clang++ make build-pybind-subdir LBUG_SOURCE_DIR=/Users/arun/src/ladybug
```

Benchmark command:

```bash
tools/run_navix_vs_vanilla_efs96.sh
```

Configuration:

- Dataset: full local SIFT database at `data/navix_sift.lbug`
- Queries: first 100 rows from `data/vector_queries.jsonl`
- `k=10`
- `efs=96`
- Threads: 32
- Selectivities: `0.9, 0.5, 0.3, 0.1, 0.01`
- Output: `data/navix_vs_vanilla_efs96_results.json`

| Selectivity | NaviX recall / ms | Vanilla `auto` recall / ms | One-hop recall / ms | Directed recall / ms | Blind recall / ms |
|---:|---:|---:|---:|---:|---:|
| 0.90 | 0.976 / 40.58 | 0.966 / 38.34 | 0.966 / 38.23 | 0.993 / 96.40 | 0.977 / 61.72 |
| 0.50 | 0.990 / 60.15 | 0.925 / 28.74 | 0.925 / 28.59 | 0.997 / 100.00 | 0.989 / 64.81 |
| 0.30 | 0.998 / 74.02 | 0.999 / 94.20 | 0.904 / 21.75 | 0.999 / 93.89 | 0.998 / 55.53 |
| 0.10 | 1.000 / 33.10 | 1.000 / 63.40 | 0.806 / 11.56 | 1.000 / 63.43 | 0.999 / 19.25 |
| 0.01 | 1.000 / 23.44 | 1.000 / 23.03 | 0.440 / 11.14 | 1.000 / 76.20 | 1.000 / 23.13 |

Interpretation:

- Vanilla means the non-NaviX filtered modes exposed by the current vector extension: `auto`, `one_hop`, `directed`, and `blind`.
- `navix` and `adaptive_l` are the same implementation path in this build.
- NaviX is most useful where vanilla `auto` either drops recall or chooses a slower two-hop strategy.
- At 50% selectivity, NaviX improves recall from `0.925` to `0.990`, but is slower than `auto`.
- At 30% and 10% selectivity, NaviX keeps target recall and is faster than `auto`/`directed`.
- At 1% selectivity, NaviX and vanilla `auto` are effectively tied.
- One-hop alone is fast but loses recall badly as selectivity drops, so it is not a reliable filtered-search baseline.
