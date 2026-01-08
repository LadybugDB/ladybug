import os
import pytest
from conftest import ShellTest
from test_helper import LBUG_ROOT


def get_extension_path(extension_name: str) -> str:
    return os.path.join(
        LBUG_ROOT,
        "extension",
        extension_name,
        "build",
        f"lib{extension_name}.lbug_extension",
    )


EXTENSIONS_TO_TEST = [
    name for name in os.listdir(os.path.join(LBUG_ROOT, "extension"))
    if os.path.isdir(os.path.join(LBUG_ROOT, "extension", name))
    and name not in {"test_list", "extension_config.cmake"}
    and os.path.exists(get_extension_path(name))
]


@pytest.mark.parametrize("extension_name", EXTENSIONS_TO_TEST)
def test_dynamic_extension_install_and_load(temp_db, extension_name: str) -> None:
    extension_path = get_extension_path(extension_name)

    test = (
        ShellTest()
        .add_argument(temp_db)
        .statement(f"LOAD EXTENSION '{extension_path}';")
    )
    result = test.run()
    assert result.status_code == 0, f"Non-zero status code: {result.status_code}"
    assert "Exception" not in str(result.stdout), f"Exception found in stdout: {result.stdout}"
    assert "Error" not in str(result.stdout), f"Error found in stdout: {result.stdout}"


def test_dynamic_duckdb_attach(temp_db) -> None:
    extension_path = get_extension_path("duckdb")
    if not os.path.exists(extension_path):
        pytest.skip(f"Extension duckdb not built: {extension_path}")

    duckdb_db = os.path.join(
        LBUG_ROOT, "dataset", "databases", "duckdb_database", "tinysnb.db"
    )
    if not os.path.exists(duckdb_db):
        pytest.skip(f"DuckDB test database not found: {duckdb_db}")

    test = (
        ShellTest()
        .add_argument(temp_db)
        .statement(f"INSTALL '{extension_path}';")
        .statement("LOAD EXTENSION duckdb;")
        .statement(
            f"ATTACH '{duckdb_db}' AS tinysnb (dbtype duckdb, skip_unsupported_table = true);"
        )
        .statement("LOAD FROM tinysnb.person RETURN count(*);")
    )
    result = test.run()
    result.check_stdout("8")
