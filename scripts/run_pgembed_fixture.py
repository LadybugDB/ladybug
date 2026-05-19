#!/usr/bin/env python3
import argparse
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
            return None
        if os.environ.get("LBUG_PGEMBED_BOOTSTRAPPED") == "1":
            raise
        env = os.environ.copy()
        env["LBUG_PGEMBED_BOOTSTRAPPED"] = "1"
        python_version = env.get("PGEMBED_PYTHON", "3.12")
        os.execvpe(
            "uv",
            [
                "uv",
                "run",
                "--python",
                python_version,
                "--with",
                "pgembed",
                "--with",
                "psycopg[binary]",
                "python",
                __file__,
                *sys.argv[1:],
            ],
            env,
        )
        raise RuntimeError("unreachable")


def first_query_value(query: dict[str, list[str]], key: str) -> str | None:
    values = query.get(key)
    if values:
        return values[0]
    return None


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
    for key in ("sslmode",):
        if key in query and query[key]:
            values[key] = query[key][0]
    return " ".join(f"{key}={quote_libpq_value(value)}" for key, value in values.items())


def import_sql_dump(conn, sql_path: Path) -> None:
    with conn.cursor() as cursor:
        statement_lines = []
        with sql_path.open(encoding="utf-8") as sql_file:
            while True:
                line = sql_file.readline()
                if line == "":
                    break
                stripped = line.strip()
                if not stripped or stripped.startswith("--"):
                    continue
                statement_lines.append(line)
                if not stripped.endswith(";"):
                    continue

                statement = "".join(statement_lines)
                statement_lines.clear()
                if statement.lstrip().upper().startswith("COPY ") and " FROM stdin;" in statement:
                    with cursor.copy(statement) as copy:
                        while True:
                            copy_line = sql_file.readline()
                            if copy_line == "":
                                raise RuntimeError(f"Unterminated COPY block in {sql_path}")
                            if copy_line.strip() == r"\.":
                                break
                            copy.write(copy_line)
                else:
                    cursor.execute(statement)

        if statement_lines:
            cursor.execute("".join(statement_lines))
    conn.commit()


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run a command with an embedded pgembed PostgreSQL test database."
    )
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if args.command and args.command[0] == "--":
        args.command = args.command[1:]
    if not args.command:
        parser.error("missing command to run")

    pgembed = import_pgembed_or_bootstrap()
    if pgembed is None:
        return subprocess.run(args.command).returncode
    import psycopg

    repo_root = Path(__file__).resolve().parent.parent
    sql_path = repo_root / "extension" / "postgres" / "test" / "test_files" / "create_test_db.sql"

    with tempfile.TemporaryDirectory(prefix="lbug_pgembed_") as tmpdir:
        with pgembed.get_server(tmpdir) as pg:
            admin_uri = pg.get_uri("postgres")
            pgscan_uri = pg.get_uri("pgscan")

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
                exists = conn.execute(
                    "SELECT 1 FROM pg_database WHERE datname = 'pgscan'"
                ).fetchone()
                if exists is None:
                    conn.execute("CREATE DATABASE pgscan OWNER ci")

            with psycopg.connect(pgscan_uri) as conn:
                import_sql_dump(conn, sql_path)

            env = os.environ.copy()
            env["POSTGRES_CONNECTION_STRING"] = uri_to_libpq_connection_string(
                pgscan_uri, "pgscan", "ci"
            )
            return subprocess.run(args.command, env=env).returncode


if __name__ == "__main__":
    sys.exit(main())
