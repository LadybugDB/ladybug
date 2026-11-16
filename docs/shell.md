# Shell Development Guide

## Building the Shell

```bash
make shell           # Build shell
make shell-debug     # Debug shell build
```

The shell is built with `duckdb` and `json` extensions statically linked.

## Testing

```bash
make shell-test      # Run shell tests
```

Shell tests are located in `tools/shell/test/`.

## Running the Shell

```bash
# After building
./build/release/tools/shell/lbug

# Or with debug build
./build/debug/tools/shell/lbug
```

## Shell Features

The Ladybug shell supports:
- Interactive Cypher query execution
- Tab completion
- History navigation
- Output formatting options
- Agent mode for AI coding agents (see below)

## Agent Mode

Agent mode renders results and errors for AI coding agents that drive the
shell through a pipe. It turns on automatically when an agent-identifying
environment variable (`AI_AGENT`, `AGENT`, `CLAUDECODE`, `CODEX_CI`,
`CODEX_SANDBOX`, `CODEX_THREAD_ID`, `CURSOR_AGENT`, `GEMINI_CLI`,
`COPILOT_AGENT`, `COPILOT_CLI`, `COPILOT_AGENT_SESSION_ID`) is set, stdout is
not a terminal, and no output-format flag was passed. `--agent` forces it on
(`LBUG_AGENT_MODE=1` does the same from the environment); `--no_agent` forces
it off.

When active (see `.help agent` for the full rules):

- Results print as compact Markdown tables with the column type in the header
  (`| name:STRING | n:INT64 |`), without alignment padding.
- At most 1000 rows / 10000 bytes print in full (`.maxrows N`, `.maxbytes N`;
  `0`/`-1` means unlimited). Larger results show the first and last 20 rows
  around an explicit omitted-rows marker, plus a footer with an
  order-independent hash of the full result.
- Long cell values are cut at 500 characters (`.maxcellwidth N`) with a
  `…(+N chars)` marker.
- Errors go to stderr as JSON (`exception_type`, `exception_message`).
- `EXPLAIN` uses a compact one-operator-per-line plan (also available outside
  agent mode via `.explain compact`; `.explain box` restores the default).
- `.tables` lists tables with approximate row counts and columns; `.show`
  (or bare `:stats`) reports agent status; a startup line on stderr explains
  why agent mode is on.

Explicit settings (output-mode flags, `.lbugrc` commands such as `:mode`,
`.maxrows`, `.explain`) take precedence over agent defaults.

Not yet implemented: stopping runaway queries early once the cap is passed,
and planner cost estimates / stderr progress lines for long queries (the
terminal progress bar is disabled in agent mode instead).
