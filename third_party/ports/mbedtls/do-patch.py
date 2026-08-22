#!/usr/bin/env python3
"""
Patch script for mbedtls.

Applies transformations to pristine mbedtls sources for integration into
the ladybug codebase:

1. Rename library/*.c to *.cpp (the port compiles mbedtls as C++).
2. Strip extern "C" guards: this port is compiled as C++ everywhere and is
   never consumed by a C compiler, so C linkage is unnecessary.
3. Wrap top-level declarations/definitions in `namespace lbug_mbedtls`.

The namespace prevents duplicate-symbol link errors when ladybug is linked
statically alongside another library that embeds its own mbedtls copy (e.g.
a statically-linked DuckDB, whose libduckdb_static.a also bundles unprefixed
`mbedtls_*` symbols).

Because mbedtls sources interleave code with preprocessor conditionals, a
single namespace pair per file cannot always be balanced (e.g. sibling
`#if defined(MBEDTLS_AES_ALT)` / `#if defined(MBEDTLS_SELF_TEST)` branches).
Instead, each *segment* - a maximal run of code sharing the same conditional
context at brace depth 0 - gets its own `namespace lbug_mbedtls { ... }`
pair. The namespace is reopened as often as needed, which is legal C++ and
always balances.

Public headers re-expose the names with `using namespace lbug_mbedtls;` so
in-tree consumers (src/common/sha256.cpp, extension/httpfs/src/crypto.cpp)
keep working unchanged with unqualified names.
"""

import re
import sys
from pathlib import Path

NAMESPACE = "lbug_mbedtls"

# Headers containing only preprocessor macros - nothing to wrap.
SKIP_HEADERS = {
    "include/mbedtls/build_info.h",
    "include/mbedtls/check_config.h",
    "include/mbedtls/mbedtls_config.h",
    "include/mbedtls/private_access.h",
}

EXTERN_C_OPEN = 'extern "C" {'


def strip_extern_c(text):
    """Remove extern "C" linkage blocks and their __cplusplus guards."""
    lines = text.splitlines()
    changed = True
    while changed:
        changed = False
        for i, line in enumerate(lines):
            stripped = line.strip()
            prev = next((lines[j].strip() for j in range(i - 1, -1, -1)
                         if lines[j].strip()), "")
            nxt = next((lines[j].strip() for j in range(i + 1, len(lines))
                        if lines[j].strip()), "")

            def is_cplusplus_guard(s):
                return s.startswith("#if") and "__cplusplus" in s

            # Opening block: guard + extern "C" { + #endif
            if stripped == EXTERN_C_OPEN and is_cplusplus_guard(prev) and nxt == "#endif":
                del lines[i + 1]
                del lines[i]
                del lines[i - 1]
                changed = True
                break
            # Closing block: guard + } + #endif
            if stripped == "}" and is_cplusplus_guard(prev) and nxt == "#endif":
                del lines[i + 1]
                del lines[i]
                del lines[i - 1]
                changed = True
                break
    return "\n".join(lines)


def _strip_comments_and_strings(line, in_block_comment):
    """Replace comment interiors and string/char literal contents with spaces
    so braces inside them don't confuse brace counting. Tracks multi-line
    block comment state."""
    out = []
    i = 0
    n = len(line)
    while i < n:
        if in_block_comment:
            j = line.find("*/", i)
            if j < 0:
                out.append(" " * (n - i))
                i = n
            else:
                out.append(" " * (j + 2 - i))
                i = j + 2
                in_block_comment = False
            continue
        c = line[i]
        if c == "/" and i + 1 < n and line[i + 1] == "/":
            out.append(" " * (n - i))
            break
        if c == "/" and i + 1 < n and line[i + 1] == "*":
            in_block_comment = True
            out.append("  ")
            i += 2
            continue
        if c == '"' or c == "'":
            quote = c
            out.append(" ")
            i += 1
            while i < n:
                if line[i] == "\\":
                    out.append("  ")
                    i += 2
                    continue
                if line[i] == quote:
                    out.append(" ")
                    i += 1
                    break
                out.append(" ")
                i += 1
            continue
        out.append(c)
        i += 1
    return "".join(out), in_block_comment


def analyze(lines):
    """Per-line info: conditional-context signature, brace depth at line
    start, and whether the line carries code (vs blank/comment/directive).
    Backslash-continued lines are merged into their logical line first, so
    macro bodies are treated as part of their #define directive."""
    n = len(lines)
    infos = [{"sig": None, "depth": None, "content": False} for _ in range(n)]
    stack = []  # conditional context
    depth = 0
    in_comment = False
    i = 0
    while i < n:
        code, in_comment = _strip_comments_and_strings(lines[i], in_comment)
        logical = code
        j = i
        while logical.rstrip().endswith("\\") and j + 1 < n:
            j += 1
            nxt, in_comment = _strip_comments_and_strings(lines[j], in_comment)
            logical = logical.rstrip()[:-1] + " " + nxt
        sig = tuple(stack)
        depth_at_start = depth
        is_directive = logical.lstrip().startswith("#")
        is_content = (not is_directive) and bool(logical.strip())
        if is_directive:
            s = logical.lstrip()
            if re.match(r"#\s*(if|ifdef|ifndef)\b", s):
                stack.append(s)
            elif re.match(r"#\s*endif\b", s):
                if stack:
                    stack.pop()
            # #else / #elif keep the stack shape; boundaries are handled by
            # the segmentation rules below.
        else:
            depth += logical.count("{") - logical.count("}")
        infos[i] = {"sig": sig, "depth": depth_at_start,
                    "content": is_content}
        # Continuation tails belong to this logical line.
        for k in range(i + 1, j + 1):
            infos[k] = {"sig": None, "depth": None, "content": False}
        i = j + 1
    return infos


_INCOMPLETE_ENDING = re.compile(
    r"([,(+=*&|?:\[{>-]|\b(?:static|inline|extern|const|unsigned|signed|struct|union|enum|"
    r"return|if|else|while|for|do|switch|case|default|sizeof|typedef))\s*$")


def _ends_incomplete(line):
    """True if a code line syntactically expects continuation, e.g. a lone
    `static` storage specifier ahead of a conditional attribute block."""
    code, _ = _strip_comments_and_strings(line, False)
    return bool(_INCOMPLETE_ENDING.search(code.rstrip()))


def plan_segments(lines, infos):
    """Return [(open_idx, close_idx)] pairs. Each pair brackets one segment:
    a maximal run of code sharing the same conditional context at brace
    depth 0. Insertions sit right next to top-level lines, so every pair is
    balanced."""
    segments = []
    cur = None  # [sig, open_idx, last_member_idx, dangling]

    def close(at):
        nonlocal cur
        if cur is not None:
            segments.append((cur[1], at))
            cur = None

    for i, info in enumerate(infos):
        line = lines[i]
        if info["depth"] is None:
            continue  # continuation tail
        if info["depth"] == 0:
            if info["content"]:
                if cur is None:
                    cur = [info["sig"], i, i, False]
                elif info["sig"] != cur[0] and not cur[3]:
                    close(i)
                    cur = [info["sig"], i, i, False]
                else:
                    cur[2] = i
                # A complete statement/definition ends a dangling continuation.
                if cur[3] and re.search(r"[;}]\s*$", _strip_comments_and_strings(line, False)[0]):
                    cur[3] = False
            elif line.lstrip().startswith("#"):
                # A top-level conditional directive is normally a segment
                # boundary - but not if the segment's last line syntactically
                # expects continuation (e.g. a lone `static` ahead of a
                # conditional attribute block), which would split a single
                # declaration across two namespaces. While dangling, all
                # nested content joins the same segment.
                s = line.lstrip()
                if re.match(r"#\s*(if|ifdef|ifndef|endif|else|elif)\b", s):
                    if cur is not None and (cur[3] or _ends_incomplete(lines[cur[2]])):
                        cur[3] = True
                        continue
                    close(i)
        else:
            if info["content"] and cur is not None:
                cur[2] = i
    close(len(lines))
    return segments


def rename_c_to_cpp(srcdir):
    """Rename library/*.c to *.cpp for C++ compilation."""
    lib_dir = srcdir / "library"
    for c_file in lib_dir.glob("*.c"):
        cpp_file = c_file.with_suffix(".cpp")
        c_file.rename(cpp_file)
        print(f"    Renamed: library/{c_file.name} -> {cpp_file.name}")


def wrap_file(path, add_using_directive):
    text = path.read_text(errors="replace")
    text = strip_extern_c(text)
    lines = text.splitlines()

    infos = analyze(lines)
    segments = plan_segments(lines, infos)
    if not segments:
        print(f"    Skipped (nothing to wrap): {path}")
        return False

    # Insert bottom-up so indices stay valid.
    for open_idx, close_idx in sorted(segments, reverse=True):
        lines.insert(close_idx, f"}} // namespace {NAMESPACE}")
        lines.insert(open_idx, f"namespace {NAMESPACE} {{")

    if add_using_directive:
        lines.append("")
        lines.append(f"using namespace {NAMESPACE};"
                     " // keep unqualified names for in-tree consumers")

    path.write_text("\n".join(lines) + "\n")
    return True


def main():
    if len(sys.argv) < 2:
        print("Usage: do-patch.py <source-directory>")
        sys.exit(1)

    srcdir = Path(sys.argv[1]).resolve()
    if not srcdir.exists():
        print(f"Error: Source directory {srcdir} does not exist")
        sys.exit(1)

    print("Applying mbedtls patches...")

    rename_c_to_cpp(srcdir)

    patched = 0
    for pattern in ("include/mbedtls/*.h", "library/*.h", "library/*.cpp"):
        for path in sorted(srcdir.glob(pattern)):
            rel = path.relative_to(srcdir).as_posix()
            if rel in SKIP_HEADERS:
                continue
            # Public headers re-expose names unqualified; internal headers and
            # TUs are used from within the namespace itself.
            add_using = pattern.startswith("include/")
            if wrap_file(path, add_using):
                patched += 1
                print(f"    Namespaced: {rel}")

    print(f"Patched {patched} files successfully!")
    return 0


if __name__ == "__main__":
    sys.exit(main())
