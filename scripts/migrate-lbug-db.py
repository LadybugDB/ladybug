#!/usr/bin/env python3
"""
Lbug Database Migration Script

This script helps migrate Lbug databases between versions.
- Sets up isolated Python environments for each Lbug version
- Exports data from the source database using the old version
- Imports data into the target database using the new version
- If `overwrite` is enabled, the target database will replace the source database and the source database will be backed up with an `_old` suffix
- If `delete-old` is enabled, the source database will be deleted

Usage Examples:
    # Basic migration from 0.9.0 to a current release
    uv run python scripts/migrate-lbug-db.py --old-version 0.9.0 --new-version 0.16.1 --old-db /path/to/old/database --new-db /path/to/new/database

Notes:
- Can only be used to migrate to newer Lbug versions, from 0.11.0 onwards.
- Automatic old-version detection maps storage versions to the oldest PyPI release known to
  read that storage format. Pass --old-version when you need a specific patch release.
"""

import tempfile
import sys
import struct
import shutil
import subprocess
import argparse
import os

# Database file extensions
LBUG_FILE_EXTENSIONS = ["", ".wal", ".shadow"]


# Maps storage version codes to the oldest Lbug release known to read that storage format.
# Storage version 40 intentionally covers 0.12.0 through 0.16.x.
lbug_version_mapping = {
    34: "0.7.0",
    35: "0.7.1",
    36: "0.8.2",
    37: "0.9.0",
    38: "0.10.1",
    39: "0.11.0",
    40: "0.12.0",
}

minimum_lbug_migration_version = "0.11.0"
MIGRATION_PYTHON_VERSION = "3.12"


def lbug_version_comparison(version: str, target: str) -> bool:
    """Return True if Lbug *v* is greater or equal to target version"""
    # Transform version string to version tuple to use in version tuple comparison
    # NOTE: If version info contains non digit info (like dev release info 0.11.0.dev1) set the value of the non digit
    # tuple part to be 0 (transform it to 0.11.0.0)
    target = tuple(int(part) if part.isdigit() else 0 for part in target.split("."))
    current = tuple(int(part) if part.isdigit() else 0 for part in version.split("."))
    max_len = max(len(current), len(target))
    current = current + (0,) * (max_len - len(current))
    target = target + (0,) * (max_len - len(target))
    return current >= target


def package_for_lbug_version(version: str) -> tuple[str, str]:
    """Return the PyPI package and Python module for a released database version."""
    if lbug_version_comparison(version=version, target="0.16.0"):
        return "ladybug", "ladybug"
    if not lbug_version_comparison(version=version, target="0.11.3"):
        return "kuzu", "kuzu"
    return "real_ladybug", "real_ladybug"


def read_lbug_storage_version(lbug_db_path: str) -> str:
    """
    Reads the Lbug storage version.

    :param lbug_db_path: Path to the Lbug database file/directory.
    :return: Oldest Lbug release known to read the storage version code.
    """
    if os.path.isdir(lbug_db_path):
        lbug_version_file_path = os.path.join(lbug_db_path, "catalog.lbdb")
        if not os.path.isfile(lbug_version_file_path):
            raise FileNotFoundError("Lbug catalog.lbdb file does not exist")
    else:
        lbug_version_file_path = lbug_db_path

    with open(lbug_version_file_path, "rb") as f:
        f.seek(4)
        # Read the next 8 bytes as a little-endian unsigned 64-bit integer
        data = f.read(8)
        if len(data) < 8:
            raise ValueError(
                f"File '{lbug_version_file_path}' does not contain a storage version code."
            )
        version_code = struct.unpack("<Q", data)[0]

    if version_code in lbug_version_mapping:
        return lbug_version_mapping[version_code]
    else:
        raise ValueError(f"Could not map version_code {version_code} to proper Lbug version.")


def ensure_env(version: str, export_dir) -> tuple[str, str]:
    """
    Creates a venv at `{export_dir}/.lbug_envs/{version}` and installs the package
    matching that database version. Returns the venv's python executable path and
    module name.
    """
    package_name, module_name = package_for_lbug_version(version)
    # Use temp directory to create venv
    lbug_envs_dir = os.path.join(export_dir, ".lbug_envs")

    # venv base under the script directory
    base = os.path.join(lbug_envs_dir, version)
    py_bin = os.path.join(base, "Scripts" if os.name == "nt" else "bin", "python")
    # If environment already exists clean it
    if os.path.isdir(base):
        shutil.rmtree(base)

    print(
        f"→ Setting up Python {MIGRATION_PYTHON_VERSION} venv for Lbug {version}...",
        file=sys.stderr,
    )
    # Create venv with Python 3.12 because released Lbug wheels are not available
    # for every interpreter that may be running this script.
    subprocess.run(
        ["uv", "venv", "--python", MIGRATION_PYTHON_VERSION, "--seed", base],
        check=True,
    )
    # Install the specific version
    subprocess.run([py_bin, "-m", "pip", "install", "--upgrade", "pip"], check=True)
    subprocess.run(
        [py_bin, "-m", "pip", "install", f"{package_name}=={version}"],
        check=True,
    )
    return py_bin, module_name


def run_migration_step(python_exe: str, module_name: str, db_path: str, cypher: str):
    """
    Uses `python_exe` to connect to the Lbug database at `db_path` and run the `cypher` query.
    """
    snippet = f"""
import {module_name} as lbug
db = lbug.Database(r"{db_path}")
conn = lbug.Connection(db)
conn.execute(r\"\"\"{cypher}\"\"\")
"""
    proc = subprocess.run([python_exe, "-c", snippet], capture_output=True, text=True)
    if proc.returncode != 0:
        print(f"Error: query failed:\n{cypher}\n{proc.stderr}", file=sys.stderr)
        sys.exit(proc.returncode)


def resolve_export_dir(export_file: str) -> str:
    """
    Returns the directory containing schema.cypher after EXPORT DATABASE.

    Older Kuzu releases can create an extra directory named after the export path's
    basename, e.g. EXPORT DATABASE '/tmp/foo' writes '/tmp/foo/foo/schema.cypher'.
    """
    candidates = [
        export_file,
        os.path.join(export_file, os.path.basename(export_file.rstrip(os.sep))),
    ]
    for candidate in candidates:
        schema_file = os.path.join(candidate, "schema.cypher")
        if os.path.exists(schema_file):
            return candidate
    search_root = os.path.dirname(export_file)
    for root, dirs, files in os.walk(search_root):
        if ".lbug_envs" in dirs:
            dirs.remove(".lbug_envs")
        if "schema.cypher" in files:
            return root
    raise ValueError(
        "Schema file not found in exported database directories: "
        + ", ".join(os.path.join(candidate, "schema.cypher") for candidate in candidates)
    )


def get_migration_temp_dir() -> str | None:
    """Use a stable writable temp root for subprocesses when available."""
    private_tmp = "/private/tmp"
    if os.path.isdir(private_tmp) and os.access(private_tmp, os.W_OK):
        return private_tmp
    return None


def lbug_migration(
    new_db, old_db, new_version, old_version=None, overwrite=None, delete_old=None
):
    """
    Main migration function that handles the complete migration process.
    """
    if new_db == old_db:
        raise ValueError(
            "The new database path cannot be the same as the old database path. Please provide a different path for the new database."
        )

    if not lbug_version_comparison(
        version=new_version, target=minimum_lbug_migration_version
    ):
        raise ValueError(
            f"New version for lbug is not supported, has to be equal or higher than version: {minimum_lbug_migration_version}"
        )

    print(
        f"Migrating Lbug database from {old_version} to {new_version}", file=sys.stderr
    )
    print(f"Source: {old_db}", file=sys.stderr)
    print("", file=sys.stderr)

    # If version of old lbug db is not provided try to determine it based on file info
    if not old_version:
        old_version = read_lbug_storage_version(old_db)

    # Check if old database exists
    if not os.path.exists(old_db):
        raise FileNotFoundError(f"Source database '{old_db}' does not exist.")

    # Prepare target - ensure parent directory exists but remove target if it exists
    parent_dir = os.path.dirname(new_db)
    if parent_dir:
        os.makedirs(parent_dir, exist_ok=True)

    if os.path.exists(new_db):
        raise FileExistsError(
            f"File already exists at {new_db}, remove file or change new database file path to continue"
        )

    # Use temp directory for all processing, it will be cleaned up after with statement
    with tempfile.TemporaryDirectory(dir=get_migration_temp_dir()) as export_dir:
        # Set up environments
        print(f"Setting up Lbug {old_version} environment...", file=sys.stderr)
        old_py, old_module = ensure_env(old_version, export_dir)
        print(f"Setting up Lbug {new_version} environment...", file=sys.stderr)
        new_py, new_module = ensure_env(new_version, export_dir)

        export_file = os.path.join(export_dir, "lbug_export")
        print(f"Exporting old DB → {export_dir}", file=sys.stderr)
        run_migration_step(
            old_py, old_module, old_db, f"EXPORT DATABASE '{export_file}'"
        )
        print("Export complete.", file=sys.stderr)

        export_file = resolve_export_dir(export_file)

        print(f"Importing into new DB at {new_db}", file=sys.stderr)
        run_migration_step(
            new_py, new_module, new_db, f"IMPORT DATABASE '{export_file}'"
        )
        print("Import complete.", file=sys.stderr)

    # Rename new lbug database to old lbug database name if enabled
    if overwrite or delete_old:
        # Remove lbug lock from migrated DB
        lock_file = new_db + ".lock"
        if os.path.exists(lock_file):
            os.remove(lock_file)
        rename_databases(old_db, old_version, new_db, delete_old)

    print("Lbug graph database migration finished successfully!")


def rename_databases(old_db: str, old_version: str, new_db: str, delete_old: bool):
    """
    When overwrite is enabled, back up the original old_db (file with .shadow and .wal or directory)
    by renaming it to *_old, and replace it with the newly imported new_db files.

    When delete_old is enabled, replace the old database with the new one and delete old database.

    :raises FileNotFoundError: If the original database path is not found
    :raises OSError: If file operations fail
    """
    base_dir = os.path.dirname(old_db)
    name = os.path.basename(old_db.rstrip(os.sep))
    # Add _old_ and version info to backup graph database
    backup_database_name = f"{name}_old_" + old_version.replace(".", "_")
    backup_base = os.path.join(base_dir, backup_database_name)

    if os.path.isfile(old_db):
        # File-based database: handle main file and accompanying lock/WAL
        for ext in LBUG_FILE_EXTENSIONS:
            src = old_db + ext
            dst = backup_base + ext
            if os.path.exists(src):
                if delete_old:
                    os.remove(src)
                else:
                    os.rename(src, dst)
                    print(f"Renamed '{src}' to '{dst}'", file=sys.stderr)
    elif os.path.isdir(old_db):
        # Directory-based Lbug database
        backup_dir = backup_base
        if delete_old:
            shutil.rmtree(old_db)
        else:
            os.rename(old_db, backup_dir)
            print(f"Renamed directory '{old_db}' to '{backup_dir}'", file=sys.stderr)
    else:
        print(
            f"Original database path '{old_db}' not found for renaming.",
            file=sys.stderr,
        )
        sys.exit(1)

    # Now move new files into place
    for ext in ["", ".wal", ".shadow"]:
        src_new = new_db + ext
        dst_new = os.path.join(base_dir, name + ext)
        if os.path.exists(src_new):
            os.rename(src_new, dst_new)
            print(f"Renamed '{src_new}' to '{dst_new}'", file=sys.stderr)


def main():
    p = argparse.ArgumentParser(
        description="Migrate Lbug DB via PyPI versions",
        epilog="""
Examples:
  uv run python %(prog)s --old-version 0.9.0 --new-version 0.16.1 \\
    --old-db /path/to/old/db --new-db /path/to/new/db --overwrite

Note: Run this script with uv from the repository root. The script creates temporary
virtual environments in .lbug_envs/ to isolate different Lbug versions.
        """,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument(
        "--old-version",
        required=False,
        default=None,
        help="Source Lbug version (e.g., 0.9.0). If not provided, automatic lbug version detection will be attempted.",
    )
    p.add_argument(
        "--new-version", required=True, help="Target Lbug version (e.g., 0.16.1)"
    )
    p.add_argument("--old-db", required=True, help="Path to source database directory")
    p.add_argument(
        "--new-db",
        required=True,
        help="Path to target database directory, it can't be the same path as the old database. Use the overwrite flag if you want to replace the old database with the new one.",
    )
    p.add_argument(
        "--overwrite",
        required=False,
        action="store_true",
        default=False,
        help="Rename new-db to the old-db name and location, and keeps old-db as backup if delete-old is not True",
    )
    p.add_argument(
        "--delete-old",
        required=False,
        action="store_true",
        default=False,
        help="When overwrite and delete-old are True, old-db will not be stored as backup",
    )

    args = p.parse_args()

    lbug_migration(
        new_db=args.new_db,
        old_db=args.old_db,
        new_version=args.new_version,
        old_version=args.old_version,
        overwrite=args.overwrite,
        delete_old=args.delete_old,
    )


if __name__ == "__main__":
    main()
