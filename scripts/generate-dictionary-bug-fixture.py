#!/usr/bin/env python3
"""Generate parquet fixtures for the relationship property corruption e2e.

The e2e in
``test/test_files/dictionary_bug/orb383_relationship_projection_obfuscated.test``
needs a specific import layout. Sequential ``id = 1..N`` node tables do not
reproduce the pre-fix failure; the minimized node insertion order and the
exact 953-row relationship table do.

This script reconstructs that layout from compact zlib payloads next to the
generated parquet files under
``test/test_files/dictionary_bug/orb383_relationship_projection_obfuscated_fixture/``
and writes:

- ``A.parquet`` / ``B.parquet``: 34,747 nodes with the original insertion order
- ``A_TO_B.parquet``: 953 relationships with the original endpoints and types

Generated parquet files should not be committed (see ``.gitignore``).

Usage:
    uv run --with pyarrow python3 scripts/generate-dictionary-bug-fixture.py
"""

from __future__ import annotations

import argparse
import hashlib
import os
import struct
import sys
import zlib

import pyarrow as pa
import pyarrow.parquet as pq

SCRIPT_DIR = os.path.dirname(os.path.realpath(__file__))
PROJECT_ROOT = os.path.dirname(SCRIPT_DIR)
DEFAULT_FIXTURE_DIR = os.path.join(
    PROJECT_ROOT,
    "test",
    "test_files",
    "dictionary_bug",
    "orb383_relationship_projection_obfuscated_fixture",
)

NODE_IDS_PAYLOAD = os.path.join(DEFAULT_FIXTURE_DIR, "node_ids.u32le.zlib")
RELS_PAYLOAD = os.path.join(DEFAULT_FIXTURE_DIR, "rels.u32u32u8.zlib")

NUM_NODES = 34747
NUM_RELS = 953
TYPE_BY_CODE = {0: "REL_A", 1: "REL_B", 2: "REL_C"}
EXPECTED_TYPE_COUNTS = {"REL_A": 739, "REL_B": 202, "REL_C": 12}
EXPECTED_NODE_IDS_SHA256 = (
    "9563bacdbfe3afc53a7c9c0a377e8a9cdb617794171c56ffc1bcd205e65f9ed8"
)
EXPECTED_RELS_SHA256 = (
    "43f42222288bd8c1097396fb787855dba0aa6e75fa35e48b46a62c52ff191b2f"
)


def _sha256(data: bytes) -> str:
    """Return the hex SHA-256 digest of ``data``."""
    return hashlib.sha256(data).hexdigest()


def _read_zlib(path: str) -> bytes:
    """Read and decompress a zlib payload file.

    Args:
        path: Absolute path to a zlib-compressed file.

    Returns:
        The decompressed payload bytes.
    """
    with open(path, "rb") as handle:
        return zlib.decompress(handle.read())


def load_node_ids(path: str = NODE_IDS_PAYLOAD) -> list[int]:
    """Load the minimized node insertion-order IDs.

    Args:
        path: Path to the little-endian uint32 zlib payload.

    Returns:
        Exactly ``NUM_NODES`` unique IDs forming the set ``1..NUM_NODES``.
    """
    raw = _read_zlib(path)
    if _sha256(raw) != EXPECTED_NODE_IDS_SHA256:
        raise ValueError(f"node id payload checksum mismatch for {path}")
    if len(raw) != NUM_NODES * 4:
        raise ValueError(f"unexpected node id payload size: {len(raw)}")
    ids = list(struct.unpack(f"<{NUM_NODES}I", raw))
    if set(ids) != set(range(1, NUM_NODES + 1)):
        raise ValueError("node id payload is not a permutation of 1..NUM_NODES")
    return ids


def load_relationships(
    path: str = RELS_PAYLOAD,
) -> tuple[list[int], list[int], list[str]]:
    """Load the minimized relationship endpoints and type labels.

    Args:
        path: Path to the packed ``(a_id, b_id, type_code)`` zlib payload.

    Returns:
        Parallel lists of source IDs, destination IDs, and type strings.
    """
    raw = _read_zlib(path)
    if _sha256(raw) != EXPECTED_RELS_SHA256:
        raise ValueError(f"relationship payload checksum mismatch for {path}")
    record_size = 9
    if len(raw) != NUM_RELS * record_size:
        raise ValueError(f"unexpected relationship payload size: {len(raw)}")

    src_ids: list[int] = []
    dst_ids: list[int] = []
    types: list[str] = []
    for offset in range(0, len(raw), record_size):
        src, dst, type_code = struct.unpack_from("<IIB", raw, offset)
        if type_code not in TYPE_BY_CODE:
            raise ValueError(f"unknown relationship type code: {type_code}")
        src_ids.append(src)
        dst_ids.append(dst)
        types.append(TYPE_BY_CODE[type_code])

    counts = {label: types.count(label) for label in EXPECTED_TYPE_COUNTS}
    if counts != EXPECTED_TYPE_COUNTS:
        raise ValueError(f"unexpected relationship type counts: {counts}")
    if (src_ids[0], dst_ids[0], types[0]) != (3379, 3382, "REL_A"):
        raise ValueError("watched survivor row 0 does not match expected endpoints")
    if (src_ids[952], dst_ids[952], types[952]) != (10049, 10050, "REL_A"):
        raise ValueError("trigger delete row 952 does not match expected endpoints")
    return src_ids, dst_ids, types


def write_node_table(output_path: str, column_prefix: str, ids: list[int]) -> None:
    """Write a node parquet table with ``id`` / ``key`` columns.

    Args:
        output_path: Destination parquet path.
        column_prefix: Column name prefix (``a`` or ``b``).
        ids: Node IDs in insertion order.
    """
    keys = [f"node_{node_id}" for node_id in ids]
    table = pa.table(
        {
            f"{column_prefix}.id": pa.array(ids, type=pa.int64()),
            f"{column_prefix}.key": pa.array(keys, type=pa.utf8()),
        }
    )
    pq.write_table(table, output_path, compression="snappy", use_dictionary=False)
    size_kb = os.path.getsize(output_path) // 1024
    print(f"Generated {output_path} ({len(ids)} rows, {size_kb} KB)")


def write_relationship_table(
    output_path: str,
    src_ids: list[int],
    dst_ids: list[int],
    types: list[str],
) -> None:
    """Write the relationship parquet table with dictionary-encoded types.

    Args:
        output_path: Destination parquet path.
        src_ids: Source node IDs.
        dst_ids: Destination node IDs.
        types: Relationship ``type`` property values.
    """
    table = pa.table(
        {
            "a.id": pa.array(src_ids, type=pa.int64()),
            "b.id": pa.array(dst_ids, type=pa.int64()),
            "r.type": pa.array(types, type=pa.utf8()),
        }
    )
    pq.write_table(
        table,
        output_path,
        compression="snappy",
        use_dictionary=["r.type"],
    )
    size_kb = os.path.getsize(output_path) // 1024
    print(f"Generated {output_path} ({len(src_ids)} rows, {size_kb} KB)")


def generate_fixtures(fixture_dir: str) -> None:
    """Generate all three parquet fixtures into ``fixture_dir``.

    Args:
        fixture_dir: Directory that receives ``A.parquet``, ``B.parquet``, and
            ``A_TO_B.parquet``.
    """
    os.makedirs(fixture_dir, exist_ok=True)
    node_ids = load_node_ids()
    src_ids, dst_ids, types = load_relationships()

    write_node_table(os.path.join(fixture_dir, "A.parquet"), "a", node_ids)
    write_node_table(os.path.join(fixture_dir, "B.parquet"), "b", node_ids)
    write_relationship_table(
        os.path.join(fixture_dir, "A_TO_B.parquet"),
        src_ids,
        dst_ids,
        types,
    )


def parse_args(argv: list[str]) -> argparse.Namespace:
    """Parse CLI arguments.

    Args:
        argv: Argument vector excluding the program name.

    Returns:
        Parsed argparse namespace.
    """
    parser = argparse.ArgumentParser(
        description="Generate dictionary-bug relationship projection parquet fixtures."
    )
    parser.add_argument(
        "--fixture-dir",
        default=DEFAULT_FIXTURE_DIR,
        help="Output directory for A.parquet, B.parquet, and A_TO_B.parquet.",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    """Entry point for fixture generation.

    Args:
        argv: Optional argument vector excluding the program name.

    Returns:
        Process exit code (``0`` on success).
    """
    args = parse_args(sys.argv[1:] if argv is None else argv)
    generate_fixtures(args.fixture_dir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
