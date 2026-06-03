#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PYTHON_BIN="${PYTHON_BIN:-/Users/arun/src/ladybug-python/.venv/bin/python}"

cd "${ROOT_DIR}"

"${PYTHON_BIN}" tools/navix_paper_benchmark.py \
  --query-limit 100 \
  --selectivities 0.9,0.5,0.3,0.1,0.01 \
  --modes navix,adaptive_l,auto,one_hop,directed,blind \
  --efs 96 \
  --threads 32 \
  --output data/navix_vs_vanilla_efs96_results.json
