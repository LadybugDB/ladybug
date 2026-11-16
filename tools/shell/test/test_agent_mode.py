"""Tests for `lbug --agent` mode (compact Markdown, caps/footers, JSON errors)."""

import json
import re

import pytest

from conftest import ShellTest

AGENT_VARS = [
    "AI_AGENT",
    "AGENT",
    "CLAUDECODE",
    "CODEX_CI",
    "CODEX_SANDBOX",
    "CODEX_THREAD_ID",
    "CURSOR_AGENT",
    "GEMINI_CLI",
    "COPILOT_AGENT",
    "COPILOT_CLI",
    "COPILOT_AGENT_SESSION_ID",
    "LBUG_AGENT_MODE",
]


def clean_env():
    # Empty values count as unset for agent detection.
    return dict.fromkeys(AGENT_VARS, "")


def agent_test(*stmts, env=None, args=()):
    test = ShellTest()
    for a in args:
        test.add_argument(a)
    for s in stmts:
        test.statement(s)
    merged = clean_env()
    if env:
        merged.update(env)
    test.environment = merged
    return test.run()


def test_agent_auto_detection(temp_db) -> None:
    result = agent_test('RETURN 1 AS a;', env={"CLAUDECODE": "1"})
    result.check_stdout("| a:INT64 |")
    result.check_stdout("|---|")
    result.check_stderr(
        "lbug agent mode on: CLAUDECODE is set and stdout is not a terminal"
    )


def test_agent_flag_forces_on(temp_db) -> None:
    test = ShellTest().add_argument(temp_db).add_argument("--agent")
    test.statement('RETURN 1 AS a;')
    test.environment = clean_env()
    result = test.run()
    result.check_stdout("| a:INT64 |")
    assert "agent mode on" not in result.stderr


def test_lbug_agent_mode_env_forces_on(temp_db) -> None:
    result = agent_test('RETURN 1 AS a;', env={"LBUG_AGENT_MODE": "1"})
    result.check_stdout("| a:INT64 |")


def test_no_agent_flag_forces_off(temp_db) -> None:
    test = (
        ShellTest()
        .add_argument(temp_db)
        .add_argument("--no_agent")
        .statement('RETURN 1 AS a;')
    )
    test.environment = {**clean_env(), "CLAUDECODE": "1"}
    result = test.run()
    result.check_not_stdout("| a:INT64 |")
    result.check_stdout("INT64")


def test_explicit_format_disables_auto_detection(temp_db) -> None:
    test = (
        ShellTest()
        .add_argument(temp_db)
        .add_argument("-m")
        .add_argument("csv")
        .statement('RETURN 1 AS a;')
    )
    test.environment = {**clean_env(), "CLAUDECODE": "1"}
    result = test.run()
    result.check_stdout("a\n1")
    assert "agent mode on" not in result.stderr


def test_explicit_format_with_agent_flag(temp_db) -> None:
    # --agent keeps agent behavior (JSON errors) and only changes row rendering.
    test = (
        ShellTest()
        .add_argument(temp_db)
        .add_argument("--agent")
        .add_argument("-m")
        .add_argument("csv")
        .statement("MATCH (a:Nope) RETURN a;")
    )
    test.environment = clean_env()
    result = test.run()
    payload = json.loads(result.stderr.strip().splitlines()[-1])
    assert payload["exception_type"] == "Binder"


def test_compact_table_types_in_header(temp_db) -> None:
    result = agent_test(
        'RETURN 1 AS n, "x" AS s;', env={"CLAUDECODE": "1"}, args=(temp_db, "--agent")
    )
    result.check_stdout("| n:INT64 | s:STRING |")
    result.check_stdout("|---|")


def test_pipe_escaped_in_cells(temp_db) -> None:
    result = agent_test('RETURN "a|b" AS v;', args=(temp_db, "--agent"))
    result.check_stdout("| a\\|b |")


def test_no_footer_for_small_result(temp_db) -> None:
    result = agent_test("UNWIND RANGE(1, 9) AS n RETURN n;", args=(temp_db, "--agent"))
    assert "hash" not in result.stdout


def test_footer_for_ten_rows(temp_db) -> None:
    result = agent_test("UNWIND RANGE(1, 10) AS n RETURN n;", args=(temp_db, "--agent"))
    assert re.search(r"10 rows, hash [0-9a-f]{16}", result.stdout)


def test_footer_for_empty_result(temp_db) -> None:
    result = agent_test("UNWIND [] AS n RETURN n;", args=(temp_db, "--agent"))
    assert re.search(r"empty result, hash [0-9a-f]{16}", result.stdout)


def test_cap_sampling_and_footer(temp_db) -> None:
    result = agent_test(
        ".maxrows 10", "UNWIND RANGE(1, 50) AS n RETURN n;", args=(temp_db, "--agent")
    )
    result.check_stdout("| … 10 rows omitted … |")
    assert re.search(
        r"first 20 and last 20 of 50 rows \(\.maxrows 0 for all\), hash [0-9a-f]{16}",
        result.stdout,
    )


def test_maxbytes_cap(temp_db) -> None:
    result = agent_test(
        ".maxbytes 100", "UNWIND RANGE(1, 50) AS n RETURN n;", args=(temp_db, "--agent")
    )
    assert re.search(r"\(\.maxbytes 0 for all\), hash [0-9a-f]{16}", result.stdout)


def test_unlimited_caps(temp_db) -> None:
    result = agent_test(
        ".maxrows 0",
        ".maxbytes -1",
        "UNWIND RANGE(1, 50) AS n RETURN n;",
        args=(temp_db, "--agent"),
    )
    assert "omitted" not in result.stdout
    assert re.search(r"50 rows, hash [0-9a-f]{16}", result.stdout)


def test_hash_distinguishes_null_from_string(temp_db) -> None:
    nulls = agent_test(
        'UNWIND ["a","b","c","d","e","f","g","h","i",NULL] AS n RETURN n;',
        args=(temp_db, "--agent"),
    )
    strings = agent_test(
        'UNWIND ["a","b","c","d","e","f","g","h","i","NULL"] AS n RETURN n;',
        args=(temp_db, "--agent"),
    )
    assert re.search(r"hash [0-9a-f]{16}", nulls.stdout)
    assert re.search(r"hash [0-9a-f]{16}", strings.stdout)
    assert nulls.stdout != strings.stdout


def test_hash_is_order_independent(temp_db) -> None:
    fwd = agent_test(
        "UNWIND RANGE(1, 10) AS n RETURN n;", args=(temp_db, "--agent")
    )
    bwd = agent_test(
        "UNWIND RANGE(1, 10) AS n RETURN n ORDER BY n DESC;", args=(temp_db, "--agent")
    )
    assert re.search(r"hash [0-9a-f]{16}", fwd.stdout).group(0) == re.search(
        r"hash [0-9a-f]{16}", bwd.stdout
    ).group(0)


def test_maxcellwidth_truncation(temp_db) -> None:
    result = agent_test(
        ".maxcellwidth 10", 'RETURN REPEAT("ab", 300) AS s;', args=(temp_db, "--agent")
    )
    result.check_stdout("…(+590 chars)")


def test_json_error(temp_db) -> None:
    test = (
        ShellTest()
        .add_argument(temp_db)
        .add_argument("--agent")
        .statement("MATCH (a:Nope) RETURN a;")
    )
    test.environment = clean_env()
    result = test.run()
    assert "Error:" not in result.stdout
    payload = json.loads(result.stderr.strip().splitlines()[-1])
    assert payload["exception_type"] == "Binder"
    assert "Nope" in payload["exception_message"]


def test_shell_error_as_json(temp_db) -> None:
    result = agent_test(".frobnicate", args=(temp_db, "--agent"))
    payload = json.loads(result.stderr.strip().splitlines()[-1])
    assert payload["exception_type"] == "Shell"
    assert ".frobnicate" in payload["exception_message"]


def test_compact_explain_in_agent_mode(temp_db) -> None:
    result = agent_test(
        "CREATE NODE TABLE A(id INT64, PRIMARY KEY(id));",
        "EXPLAIN MATCH (a:A) RETURN a;",
        args=(temp_db, "--agent"),
    )
    result.check_stdout("SCAN_NODE_TABLE")
    result.check_not_stdout("┌")


def test_explain_compact_as_explicit_format(temp_db) -> None:
    test = (
        ShellTest()
        .add_argument(temp_db)
        .statement("CREATE NODE TABLE A(id INT64, PRIMARY KEY(id));")
        .statement(".explain compact")
        .statement("EXPLAIN MATCH (a:A) RETURN a;")
    )
    test.environment = clean_env()
    result = test.run()
    result.check_stdout("SCAN_NODE_TABLE Tables: A")


def test_explain_box_reverts(temp_db) -> None:
    test = (
        ShellTest()
        .add_argument(temp_db)
        .add_argument("--agent")
        .statement("CREATE NODE TABLE A(id INT64, PRIMARY KEY(id));")
        .statement(".explain box")
        .statement("EXPLAIN MATCH (a:A) RETURN a;")
    )
    test.environment = clean_env()
    result = test.run()
    result.check_stdout("SCAN_NODE_TABLE")


def test_tables_listing(temp_db) -> None:
    result = agent_test(
        "CREATE NODE TABLE Person(id INT64, name STRING, PRIMARY KEY(id));",
        "CREATE (:Person {id: 1, name: 'a'});",
        ".tables",
        args=(temp_db, "--agent"),
    )
    result.check_stdout("Person (node, ~1 rows): id INT64 PK, name STRING")


def test_show_status(temp_db) -> None:
    result = agent_test(".show", env={"CLAUDECODE": "1"}, args=(temp_db,))
    result.check_stdout("agent: on (claude-code, auto-detected via CLAUDECODE)")
    result.check_stdout("mode: markdown (agent compact)")


def test_stats_reports_agent(temp_db) -> None:
    result = agent_test(":stats", args=(temp_db, "--agent"))
    result.check_stdout("agent: on")


def test_stats_off_when_disabled(temp_db) -> None:
    test = ShellTest().add_argument(temp_db).statement(":stats")
    test.environment = clean_env()
    result = test.run()
    result.check_stdout("agent: off")


def test_help_agent(temp_db) -> None:
    result = agent_test(".help agent", args=(temp_db, "--agent"))
    result.check_stdout("Markdown tables")
    result.check_stdout(".maxrows")


def test_hint_when_piped_run_fails(temp_db) -> None:
    test = (
        ShellTest().add_argument(temp_db).statement("MATCH (a:Nope) RETURN a;")
    )
    test.environment = clean_env()
    result = test.run()
    result.check_stderr("hint: --agent renders errors as JSON")


def test_no_hint_with_flag(temp_db) -> None:
    test = (
        ShellTest()
        .add_argument(temp_db)
        .add_argument("--no_agent")
        .statement("MATCH (a:Nope) RETURN a;")
    )
    test.environment = clean_env()
    result = test.run()
    assert "hint:" not in result.stderr
