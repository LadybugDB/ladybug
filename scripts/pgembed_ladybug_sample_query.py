#!/usr/bin/env python3

"""

  uv run --python 3.12 --with pgembed --with 'psycopg[binary]' \
    python scripts/pgembed_ladybug_sample_query.py
"""

from __future__ import annotations

import importlib.util
import os
import platform
import subprocess
import sys
import tempfile
from pathlib import Path
from urllib.parse import parse_qs, unquote, urlparse


def is_musl() -> bool:
    libc_name, _ = platform.libc_ver()
    if libc_name == "musl":
        return True
    try:
        return "musl" in os.confstr("CS_GNU_LIBC_VERSION").lower()
    except (AttributeError, OSError, ValueError):
        return False


def import_pgembed_or_bootstrap():
    try:
        import pgembed

        return pgembed
    except ModuleNotFoundError:
        if is_musl():
            raise
        if os.environ.get("LBUG_PGEMBED_BOOTSTRAPPED") == "1":
            raise
        env = os.environ.copy()
        env["LBUG_PGEMBED_BOOTSTRAPPED"] = "1"
        os.execvpe(
            "uv",
            [
                "uv",
                "run",
                "--python",
                env.get("PGEMBED_PYTHON", "3.12"),
                "--with",
                "pgembed",
                "--with",
                "psycopg[binary]",
                "python",
                __file__,
            ],
            env,
        )
        raise RuntimeError("unreachable")


def first_query_value(query: dict[str, list[str]], key: str) -> str | None:
    values = query.get(key)
    return values[0] if values else None


def quote_libpq_value(value: str) -> str:
    if value and not any(char.isspace() or char in "\\'" for char in value):
        return value
    return "'" + value.replace("\\", "\\\\").replace("'", "\\'") + "'"


def uri_to_libpq_connection_string(uri: str, database_name: str, user: str) -> str:
    parsed = urlparse(uri)
    query = parse_qs(parsed.query)
    values = {
        "dbname": database_name,
        "user": user,
        "host": parsed.hostname or first_query_value(query, "host") or "localhost",
        "password": "ci",
    }
    port = parsed.port or first_query_value(query, "port")
    if port is not None:
        values["port"] = str(port)
    if parsed.password:
        values["password"] = unquote(parsed.password)
    return " ".join(f"{key}={quote_libpq_value(value)}" for key, value in values.items())


def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def import_ladybug_from_submodule(root: Path):
    package_dir = root / "tools" / "python_api" / "src_py"
    os.environ.setdefault(
        "LBUG_C_API_LIB_PATH",
        str(root / "build" / "relwithdebinfo" / "src" / "liblbug.dylib"),
    )
    spec = importlib.util.spec_from_file_location(
        "ladybug",
        package_dir / "__init__.py",
        submodule_search_locations=[str(package_dir)],
    )
    if spec is None or spec.loader is None:
        raise RuntimeError(f"failed to load ladybug package from {package_dir}")
    module = importlib.util.module_from_spec(spec)
    sys.modules["ladybug"] = module
    spec.loader.exec_module(module)
    return module


def run_ladybug_query(conn, query: str):
    result = conn.execute(query)
    return result.get_all()


def main() -> int:
    pgembed = import_pgembed_or_bootstrap()
    import psycopg

    root = repo_root()
    lb = import_ladybug_from_submodule(root)

    extension_path = root / "extension" / "postgres" / "build" / "libpostgres.lbug_extension"
    if not extension_path.exists():
        raise RuntimeError(f"missing postgres extension: {extension_path}")

    with tempfile.TemporaryDirectory(prefix="lbug_pgembed_repro_") as tmpdir:
        with pgembed.get_server(tmpdir) as pg:
            admin_uri = pg.get_uri("postgres")
            repro_uri = pg.get_uri("pgscan")

            with psycopg.connect(admin_uri, autocommit=True) as conn:
                conn.execute(
                    """
                    DO $$
                    BEGIN
                        IF NOT EXISTS (SELECT FROM pg_catalog.pg_roles WHERE rolname = 'ci') THEN
                            CREATE ROLE ci WITH LOGIN SUPERUSER PASSWORD 'ci';
                        END IF;
                    END
                    $$;
                    """
                )
                if (
                    conn.execute(
                        "SELECT 1 FROM pg_database WHERE datname = 'pgscan'"
                    ).fetchone()
                    is None
                ):
                    conn.execute("CREATE DATABASE pgscan OWNER ci")

            with psycopg.connect(repro_uri) as conn:
                conn.execute(
                    """
                    CREATE TABLE organisation (
                        id BIGINT PRIMARY KEY,
                        name TEXT NOT NULL,
                        score BIGINT NOT NULL,
                        mark DOUBLE PRECISION NOT NULL,
                        orgcode BIGINT NOT NULL
                    )
                    """
                )
                conn.execute(
                    """
                    INSERT INTO organisation VALUES
                        (1, 'ABFsUni', 4, 3.7, 325),
                        (4, 'CsWork', 7, 4.1, 934),
                        (6, 'DEsWork', 2, 4.1, 824)
                    """
                )
                conn.commit()

            conn_string = uri_to_libpq_connection_string(repro_uri, "pgscan", "ci")
            db = lb.Database(":memory:", backend="capi")
            conn = lb.Connection(db)
            try:
                run_ladybug_query(conn, f"LOAD EXTENSION '{extension_path}'")
                run_ladybug_query(
                    conn,
                    f"ATTACH '{conn_string}' AS pg (dbtype POSTGRES)",
                )

                checks = [
                    (
                        "select-star-projection",
                        "CALL SQL_QUERY('pg', 'select * from organisation') "
                        "RETURN name, orgcode",
                        [["ABFsUni", 325], ["CsWork", 934], ["DEsWork", 824]],
                    ),
                    (
                        "select-star-reordered-projection",
                        "CALL SQL_QUERY('pg', 'select * from organisation') "
                        "RETURN orgcode, name",
                        [[325, "ABFsUni"], [934, "CsWork"], [824, "DEsWork"]],
                    ),
                    (
                        "select-star-filter-skipped-column",
                        "CALL SQL_QUERY('pg', 'select * from organisation') "
                        "WHERE score > 4 RETURN name, orgcode",
                        [["CsWork", 934]],
                    ),
                    (
                        "explicit-query-filter-skipped-column",
                        "CALL SQL_QUERY('pg', "
                        "'select name, score, mark, orgcode from organisation') "
                        "WHERE score > 4 YIELD name, score, mark, orgcode AS code "
                        "RETURN name, code",
                        [["CsWork", 934]],
                    ),
                ]

                for name, query, expected in checks:
                    actual = run_ladybug_query(conn, query)
                    print(f"{name}: {actual}")
                    if actual != expected:
                        raise AssertionError(f"{name}: expected {expected}, got {actual}")
            finally:
                conn.close()
                db.close()

    print("postgres select-star repro passed")
    return 0


if __name__ == "__main__":
    if (
        "uv" not in Path(sys.executable).name
        and os.environ.get("LBUG_PGEMBED_BOOTSTRAPPED") != "1"
    ):
        try:
            import pgembed  # noqa: F401
            import psycopg  # noqa: F401
        except ModuleNotFoundError:
            cmd = [
                "uv",
                "run",
                "--python",
                os.environ.get("PGEMBED_PYTHON", "3.12"),
                "--with",
                "pgembed",
                "--with",
                "psycopg[binary]",
                "python",
                __file__,
            ]
            raise SystemExit(subprocess.run(cmd).returncode)
    raise SystemExit(main())
