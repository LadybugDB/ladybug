#!/usr/bin/env python3
"""Recreate the NaviX filtered SIFT test with Ladybug Python bindings.

The default configuration follows the NaviX README/paper setup:

- SIFT vectors from data/sift_base.parquet
- range predicates over id to model selectivity
- k=10, efs=64
- HNSW efc=200, mu=32, ml=64, metric=l2
- exact filtered ground truth through QUERY_VECTOR_INDEX(..., use_knn := true)

Use this as a correctness/recall harness first. Full paper-scale timing runs can
take a long time because index construction over SIFT1M is intentionally large.
"""

from __future__ import annotations

import argparse
import json
import os
import statistics
import sys
import time
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PYTHON_REPO = REPO_ROOT.parent / "ladybug-python"
DEFAULT_PYTHON_BUILD = DEFAULT_PYTHON_REPO / "build"
DEFAULT_VECTOR_EXTENSION = REPO_ROOT / "extension" / "vector" / "build" / "libvector.lbug_extension"
DEFAULT_DB = REPO_ROOT / "data" / "navix_sift.lbug"
DEFAULT_BASE = REPO_ROOT / "data" / "sift_base.parquet"
DEFAULT_QUERIES = REPO_ROOT / "data" / "vector_queries.jsonl"


@dataclass
class QueryStats:
    mode: str
    selectivity: float
    queries: int
    k: int
    efs: int
    recall: float
    mean_ms: float
    median_ms: float
    p95_ms: float
    mismatches: int


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--python-repo", type=Path, default=DEFAULT_PYTHON_REPO)
    parser.add_argument("--python-build", type=Path, default=DEFAULT_PYTHON_BUILD)
    parser.add_argument("--db", type=Path, default=DEFAULT_DB)
    parser.add_argument("--base", type=Path, default=DEFAULT_BASE)
    parser.add_argument("--queries", type=Path, default=DEFAULT_QUERIES)
    parser.add_argument("--vector-extension", type=Path, default=DEFAULT_VECTOR_EXTENSION)
    parser.add_argument("--output", type=Path, default=REPO_ROOT / "data" / "navix_paper_results.json")
    parser.add_argument("--prepare", action="store_true", help="Create and index the SIFT database.")
    parser.add_argument("--force", action="store_true", help="Delete the database path before --prepare.")
    parser.add_argument("--limit", type=int, default=0, help="Optional number of base vectors to load.")
    parser.add_argument("--query-limit", type=int, default=100)
    parser.add_argument("--k", type=int, default=10)
    parser.add_argument("--efs", type=int, default=64)
    parser.add_argument("--threads", type=int, default=32)
    parser.add_argument("--efc", type=int, default=200)
    parser.add_argument("--mu", type=int, default=32)
    parser.add_argument("--ml", type=int, default=64)
    parser.add_argument("--selectivities", default="0.9,0.5,0.3,0.1,0.01")
    parser.add_argument(
        "--modes",
        default="navix,adaptive_l,adaptive_g,blind,directed,one_hop,auto",
        help="Comma-separated QUERY_VECTOR_INDEX search_type values to test.",
    )
    parser.add_argument(
        "--rebuild-python",
        action="store_true",
        help="Run make build-pybind-subdir in ladybug-python before importing ladybug.",
    )
    return parser.parse_args()


def run_python_build(python_repo: Path) -> None:
    import subprocess

    env = os.environ.copy()
    env["LBUG_SOURCE_DIR"] = str(REPO_ROOT)
    subprocess.run(["make", "build-pybind-subdir"], cwd=python_repo, env=env, check=True)


def import_ladybug(python_build: Path) -> tuple[Any, Any]:
    sys.path.insert(0, str(python_build))
    from ladybug import Connection, Database

    return Connection, Database


def load_queries(path: Path, limit: int) -> list[list[float]]:
    queries: list[list[float]] = []
    with path.open("r", encoding="utf-8") as f:
        for line in f:
            if len(queries) >= limit:
                break
            queries.append(json.loads(line)["embedding"])
    if not queries:
        raise RuntimeError(f"No queries loaded from {path}")
    return queries


def vector_literal(vec: list[float]) -> str:
    return "[" + ",".join(f"{x:.9g}" for x in vec) + "]"


def scalar_list(value: str) -> list[str]:
    return [item.strip() for item in value.split(",") if item.strip()]


def float_list(value: str) -> list[float]:
    return [float(item) for item in scalar_list(value)]


def execute_ddl(conn: Any, statement: str) -> None:
    result = conn.execute(statement)
    result.close()


def load_extension(conn: Any, extension_path: Path) -> None:
    execute_ddl(conn, f"LOAD EXTENSION '{extension_path}'")


def prepare_database(args: argparse.Namespace, Connection: Any, Database: Any) -> None:
    if args.force and args.db.exists():
        import shutil

        shutil.rmtree(args.db)
    args.db.parent.mkdir(parents=True, exist_ok=True)
    conn = Connection(Database(str(args.db), backend="pybind", max_num_threads=args.threads))
    load_extension(conn, args.vector_extension)
    execute_ddl(conn, f"CALL threads={args.threads}")
    execute_ddl(conn, "CREATE NODE TABLE IF NOT EXISTS sift(id INT64, embedding FLOAT[128], PRIMARY KEY(id));")

    limit_clause = f" LIMIT {args.limit}" if args.limit else ""
    copy_sql = f"""
        COPY sift FROM (
            LOAD FROM '{args.base}'
            RETURN cast(id as INT64) as id, cast(embedding as FLOAT[128]) as embedding
            {limit_clause}
        )
    """
    execute_ddl(conn, copy_sql)
    execute_ddl(
        conn,
        "CALL CREATE_VECTOR_INDEX("
        "'sift', 'sift_idx', 'embedding', "
        f"metric := 'l2', efc := {args.efc}, mu := {args.mu}, ml := {args.ml})",
    )
    conn.close()


def get_count(conn: Any) -> int:
    return int(conn.execute("MATCH (n:sift) RETURN count(*)").get_all()[0][0])


def project_filter(conn: Any, graph_name: str, upper_id: int) -> None:
    try:
        execute_ddl(conn, f"CALL DROP_PROJECTED_GRAPH('{graph_name}')")
    except RuntimeError:
        pass
    execute_ddl(conn, f"CALL PROJECT_GRAPH('{graph_name}', {{'sift': 'n.id < {upper_id}'}}, [])")


def query_ids(conn: Any, graph: str, vec: list[float], k: int, efs: int, mode: str | None) -> list[int]:
    mode_clause = ", use_knn := true" if mode is None else f", efs := {efs}, search_type := '{mode}'"
    sql = (
        f"CALL QUERY_VECTOR_INDEX('{graph}', 'sift_idx', "
        f"CAST({vector_literal(vec)} AS FLOAT[128]), {k}{mode_clause}) "
        "RETURN node.id ORDER BY distance"
    )
    return [int(row[0]) for row in conn.execute(sql).get_all()]


def percentile(values: list[float], pct: float) -> float:
    if not values:
        return 0.0
    sorted_values = sorted(values)
    idx = min(len(sorted_values) - 1, round((pct / 100.0) * (len(sorted_values) - 1)))
    return sorted_values[idx]


def run_mode(
    conn: Any,
    graph: str,
    selectivity: float,
    queries: list[list[float]],
    ground_truth: list[list[int]],
    mode: str,
    k: int,
    efs: int,
) -> QueryStats:
    latencies: list[float] = []
    hits = 0
    expected = 0
    mismatches = 0
    for vec, truth in zip(queries, ground_truth, strict=True):
        start = time.perf_counter()
        ids = query_ids(conn, graph, vec, k, efs, mode)
        latencies.append((time.perf_counter() - start) * 1000.0)
        truth_set = set(truth)
        hits += len([id_ for id_ in ids if id_ in truth_set])
        expected += len(truth)
        if ids[: len(truth)] != truth:
            mismatches += 1
    return QueryStats(
        mode=mode,
        selectivity=selectivity,
        queries=len(queries),
        k=k,
        efs=efs,
        recall=hits / expected if expected else 0.0,
        mean_ms=statistics.fmean(latencies),
        median_ms=statistics.median(latencies),
        p95_ms=percentile(latencies, 95.0),
        mismatches=mismatches,
    )


def main() -> int:
    args = parse_args()
    if args.rebuild_python:
        run_python_build(args.python_repo)
    Connection, Database = import_ladybug(args.python_build)

    if args.prepare:
        prepare_database(args, Connection, Database)

    conn = Connection(Database(str(args.db), backend="pybind", max_num_threads=args.threads))
    load_extension(conn, args.vector_extension)
    execute_ddl(conn, f"CALL threads={args.threads}")

    n_vectors = args.limit or get_count(conn)
    queries = load_queries(args.queries, args.query_limit)
    selectivities = float_list(args.selectivities)
    modes = scalar_list(args.modes)
    results: list[QueryStats] = []

    for selectivity in selectivities:
        graph = "sift_filter_" + str(selectivity).replace(".", "_")
        upper_id = max(args.k, int(n_vectors * selectivity))
        project_filter(conn, graph, upper_id)
        ground_truth = [query_ids(conn, graph, vec, args.k, args.efs, None) for vec in queries]
        for mode in modes:
            stats = run_mode(conn, graph, selectivity, queries, ground_truth, mode, args.k, args.efs)
            results.append(stats)
            print(
                f"{mode:10s} sel={selectivity:g} recall={stats.recall:.4f} "
                f"mean_ms={stats.mean_ms:.2f} p95_ms={stats.p95_ms:.2f} mismatches={stats.mismatches}"
            )

    payload = {
        "config": {
            "db": str(args.db),
            "base": str(args.base),
            "queries": str(args.queries),
            "n_vectors": n_vectors,
            "query_limit": args.query_limit,
            "k": args.k,
            "efs": args.efs,
            "threads": args.threads,
            "efc": args.efc,
            "mu": args.mu,
            "ml": args.ml,
            "selectivities": selectivities,
            "modes": modes,
        },
        "results": [asdict(result) for result in results],
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    print(f"Wrote {args.output}")
    conn.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
