#!/usr/bin/env python3
"""Split debug symbols out of built Python wheels and repack stripped wheels.

Used by .github/workflows/python-wheel-workflow.yml after cibuildwheel finishes:

  Linux:   objcopy --only-keep-debug / --strip-debug / --add-gnu-debuglink
           (mirrors the CMake POST_BUILD recipe in tools/python_api/CMakeLists.txt,
           but runs on the final repaired wheel so auditwheel/delocate edits are
           already baked in). If the wheel already ships a .debug file produced by
           the in-build split and the .so is already stripped, that file is reused.
  macOS:   dsymutil into a .dSYM bundle + strip -S, regenerated from the final
           wheel binary so it matches the shipped (delocated) .so.
  Windows: move *.pdb (plus *.exp/*.lib byproducts) out of the wheel; the .pyd
           itself carries no debug data when linked with /DEBUG.

Every repacked wheel gets a correct RECORD (hashes/sizes recomputed), and all
extracted symbol files land in --symbols-dir for upload as separate artifacts.
"""

import argparse
import base64
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path


def _has_debug_sections(shared_obj: Path) -> bool:
    """Best-effort check for DWARF debug sections (False if unknown)."""
    readelf = shutil.which("readelf")
    if readelf is None:
        return True  # Unknown: assume symbols present so we attempt a split.
    try:
        out = subprocess.run(
            [readelf, "-S", str(shared_obj)],
            capture_output=True,
            text=True,
            check=False,
        )
        return ".debug_info" in out.stdout
    except OSError:
        return True


def _run(cmd: list[str], cwd: Path | None = None) -> None:
    print(f"+ {' '.join(cmd)}" + (f" (cwd={cwd})" if cwd else ""), flush=True)
    subprocess.run(cmd, check=True, cwd=cwd)


def _hash_record_entry(data: bytes) -> str:
    digest = base64.urlsafe_b64encode(hashlib.sha256(data).digest()).rstrip(b"=")
    return f"sha256={digest.decode()}"


def _repack_wheel(unpacked_dir: Path, wheel_path: Path) -> None:
    """Rewrite wheel zip from unpacked_dir, dropping stale RECORD entries and
    recomputing hashes for every file (required after stripping binaries)."""
    record_file = None
    for dist_info in unpacked_dir.glob("*.dist-info"):
        candidate = dist_info / "RECORD"
        if candidate.is_file():
            record_file = candidate
            break
    if record_file is None:
        raise FileNotFoundError(f"No RECORD found in unpacked wheel at {unpacked_dir}")

    record_rel = record_file.relative_to(unpacked_dir).as_posix()
    rows: list[str] = []
    with tempfile.NamedTemporaryFile(delete=False, suffix=".whl") as tmp:
        tmp_path = Path(tmp.name)
    try:
        with zipfile.ZipFile(tmp_path, "w", zipfile.ZIP_DEFLATED) as zf:
            for root, _, files in os.walk(unpacked_dir):
                for name in sorted(files):
                    full = Path(root) / name
                    rel = full.relative_to(unpacked_dir).as_posix()
                    if rel == record_rel:
                        continue
                    data = full.read_bytes()
                    zf.writestr(rel, data)
                    rows.append(f"{rel},{_hash_record_entry(data)},{len(data)}")
            rows.append(f"{record_rel},,")
            zf.writestr(record_rel, "\n".join(rows) + "\n")
        shutil.move(str(tmp_path), str(wheel_path))
    finally:
        if tmp_path.exists():
            tmp_path.unlink()


def _find_extensions(tree: Path, suffixes: tuple[str, ...]) -> list[Path]:
    hits = []
    for suffix in suffixes:
        hits.extend(tree.rglob(f"_lbug*{suffix}"))
    # Skip files that live inside symbol bundles/dirs themselves.
    return [
        p
        for p in hits
        if p.is_file() and ".dSYM" not in p.parts and not p.name.endswith(".debug")
    ]


def _split_linux(wheel_path: Path, tree: Path, symbols_dir: Path) -> None:
    objcopy = shutil.which("objcopy")
    if objcopy is None:
        print("objcopy not found; leaving Linux wheel unstripped.", flush=True)
        return
    for so in _find_extensions(tree, (".so",)):
        stem = f"{wheel_path.stem}-{so.name}"
        shipped = list(tree.rglob("*.debug"))
        if shipped and not _has_debug_sections(so):
            # In-build CMake split already stripped this binary; reuse it.
            for dbg in shipped:
                dest = symbols_dir / f"{stem}.debug"
                print(f"Reusing shipped debug file {dbg} -> {dest}", flush=True)
                shutil.move(str(dbg), str(dest))
            continue
        # Regenerate from the final (repaired) binary.
        for dbg in shipped:
            dbg.unlink()
        dbg_out = so.parent / f"{so.name}.debug"
        _run([objcopy, "--only-keep-debug", str(so), str(dbg_out)])
        _run([objcopy, "--strip-debug", str(so)])
        # --strip-debug leaves a pre-existing .gnu_debuglink behind, and
        # --add-gnu-debuglink refuses to overwrite it, so drop it first.
        subprocess.run(
            [objcopy, "-R", ".gnu_debuglink", str(so)],
            capture_output=True,
            check=False,
        )
        # NOTE: --add-gnu-debuglink resolves a relative filename against the
        # process CWD (not the binary's dir), so run from so.parent so the
        # stored link stays a bare filename while objcopy can read the file.
        _run([objcopy, f"--add-gnu-debuglink={dbg_out.name}", str(so)], cwd=so.parent)
        dest = symbols_dir / f"{stem}.debug"
        print(f"Moving {dbg_out} -> {dest}", flush=True)
        shutil.move(str(dbg_out), str(dest))


def _split_macos(wheel_path: Path, tree: Path, symbols_dir: Path) -> None:
    dsymutil = shutil.which("dsymutil")
    strip = shutil.which("strip")
    if dsymutil is None or strip is None:
        print("dsymutil/strip not found; leaving macOS wheel unstripped.", flush=True)
        return
    for so in _find_extensions(tree, (".so",)):
        # Drop any pre-repair dSYM shipped inside the wheel; regenerate below so
        # the bundle matches the final delocated binary.
        for stale in tree.rglob("*.dSYM"):
            if stale.is_dir():
                shutil.rmtree(stale)
            else:
                stale.unlink()
        stem = f"{wheel_path.stem}-{so.name}"
        dsym_out = Path(tempfile.mkdtemp()) / f"{so.name}.dSYM"
        _run([dsymutil, str(so), "-o", str(dsym_out)])
        _run([strip, "-S", str(so)])
        dest = symbols_dir / f"{stem}.dSYM"
        if dest.exists():
            shutil.rmtree(dest)
        print(f"Moving {dsym_out} -> {dest}", flush=True)
        shutil.move(str(dsym_out), str(dest))


def _split_windows(wheel_path: Path, tree: Path, symbols_dir: Path) -> None:
    stem = wheel_path.stem
    for pattern in ("*.pdb", "*.exp", "*.lib"):
        for artifact in tree.rglob(pattern):
            if not artifact.is_file():
                continue
            if pattern == "*.pdb":
                dest = symbols_dir / f"{stem}-{artifact.name}"
                print(f"Moving {artifact} -> {dest}", flush=True)
                shutil.move(str(artifact), str(dest))
            else:
                # Linker byproducts: never ship in the wheel.
                print(f"Removing linker byproduct {artifact}", flush=True)
                artifact.unlink()
    # The .pyd linked with /DEBUG already carries no debug data; nothing to strip.


def process_wheel(wheel_path: Path, symbols_dir: Path, os_name: str) -> None:
    print(f"=== {wheel_path.name} ({os_name}) ===", flush=True)
    with tempfile.TemporaryDirectory() as tmp:
        tree = Path(tmp) / "wheel"
        with zipfile.ZipFile(wheel_path, "r") as zf:
            zf.extractall(tree)
        if os_name == "linux":
            _split_linux(wheel_path, tree, symbols_dir)
        elif os_name == "macos":
            _split_macos(wheel_path, tree, symbols_dir)
        elif os_name == "windows":
            _split_windows(wheel_path, tree, symbols_dir)
        else:
            raise ValueError(f"Unknown os: {os_name}")
        _repack_wheel(tree, wheel_path)
    print(f"Repacked stripped wheel: {wheel_path.name}", flush=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wheel-dir", required=True, help="Directory with *.whl files")
    parser.add_argument("--symbols-dir", required=True, help="Output dir for symbols")
    parser.add_argument("--os", required=True, choices=["linux", "macos", "windows"])
    args = parser.parse_args()

    wheel_dir = Path(args.wheel_dir)
    symbols_dir = Path(args.symbols_dir)
    symbols_dir.mkdir(parents=True, exist_ok=True)

    wheels = sorted(wheel_dir.glob("*.whl"))
    if not wheels:
        print(f"No wheels found in {wheel_dir}; nothing to do.", flush=True)
        return 0
    for wheel in wheels:
        process_wheel(wheel, symbols_dir, args.os)

    produced = sorted(p.name for p in symbols_dir.iterdir())
    print(f"Symbol files produced ({len(produced)}):", flush=True)
    for name in produced:
        print(f"  {name}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
