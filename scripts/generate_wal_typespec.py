#!/usr/bin/env python3
"""Generate WAL record files from TypeSpec declarations via tsc-py."""

from __future__ import annotations

import argparse
import dataclasses
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
SPEC_ROOT = REPO_ROOT / "src/storage/wal/typespec"
RECORD_SPEC_ROOT = SPEC_ROOT / "records"
TEMPLATE_ROOT = SPEC_ROOT / "templates"


IMPORT_RE = re.compile(r'^\s*import\s+(?:"([^"]+)"|<([^>]+)>);\s*$', re.MULTILINE)
META_RE = re.compile(r"^\s*//\s*@([a-zA-Z_][a-zA-Z0-9_]*)\s*(?:=\s*(.*?))?\s*$")
WAL_RECORD_MODEL_RE = re.compile(
    r"\bmodel\s+([A-Za-z_][A-Za-z0-9_]*)\s+extends\s+WalRecordBase\b"
)


@dataclasses.dataclass(frozen=True)
class RecordSpec:
    path: Path
    model_name: str
    content: str
    tsp_imports: tuple[Path, ...]
    cpp_imports: tuple[str, ...]
    metadata: dict[str, str]

    @property
    def debug_fields(self) -> bool:
        return self.metadata.get("debug_fields", "true").lower() not in {"0", "false", "no"}

    @property
    def record_type(self) -> str:
        return self.metadata.get("record_type", camel_to_enum(self.model_name))

    @property
    def header_output(self) -> Path:
        name = camel_to_snake(self.model_name)
        return Path("src/include/storage/wal/record") / f"{name}.h"

    @property
    def source_output(self) -> Path:
        name = camel_to_snake(self.model_name)
        return Path("src/storage/wal/records") / f"{name}.cpp"


def camel_to_snake(value: str) -> str:
    return re.sub(r"(?<!^)(?=[A-Z])", "_", value).lower()


def camel_to_enum(value: str) -> str:
    snake = camel_to_snake(value)
    return snake.removesuffix("_record").upper() + "_RECORD"


def read_metadata(content: str) -> dict[str, str]:
    metadata: dict[str, str] = {}
    for line in content.splitlines():
        match = META_RE.match(line)
        if match:
            metadata[match.group(1)] = (match.group(2) or "true").strip()
    return metadata


def parse_metadata_map(value: str) -> str:
    entries = []
    for item in value.split(","):
        if not item.strip():
            continue
        name, mapped_value = item.split(":", 1)
        entries.append(f'"{name.strip()}": "{mapped_value.strip()}"')
    return "{" + ", ".join(entries) + "}"


def strip_line_comments(content: str) -> str:
    return "\n".join(line for line in content.splitlines() if not line.strip().startswith("//"))


def split_imports(spec_path: Path, content: str) -> tuple[str, tuple[Path, ...], tuple[str, ...]]:
    tsp_imports: list[Path] = []
    cpp_imports: list[str] = []

    def replace(match: re.Match[str]) -> str:
        imported = match.group(1) or f"<{match.group(2)}>"
        if imported.endswith(".tsp"):
            tsp_imports.append((spec_path.parent / imported).resolve())
        else:
            cpp_imports.append(imported)
        return ""

    stripped = strip_line_comments(IMPORT_RE.sub(replace, content))
    return stripped, tuple(tsp_imports), tuple(cpp_imports)


def load_record_spec(path: Path) -> RecordSpec:
    raw = path.read_text(encoding="utf-8")
    content, tsp_imports, cpp_imports = split_imports(path, raw)
    model_names = WAL_RECORD_MODEL_RE.findall(content)
    if len(model_names) != 1:
        raise ValueError(
            f"{path} must declare exactly one model extending WalRecordBase, "
            f"found {model_names}"
        )
    return RecordSpec(
        path=path,
        model_name=model_names[0],
        content=content,
        tsp_imports=tsp_imports,
        cpp_imports=cpp_imports,
        metadata=read_metadata(raw),
    )


def flatten_typespec(record: RecordSpec) -> str:
    seen: set[Path] = set()
    chunks: list[str] = []

    def visit(path: Path) -> None:
        path = path.resolve()
        if path in seen:
            return
        seen.add(path)
        raw = path.read_text(encoding="utf-8")
        content, imports, _ = split_imports(path, raw)
        for imported in imports:
            visit(imported)
        chunks.append(content)

    for imported in record.tsp_imports:
        visit(imported)
    chunks.append(record.content)
    return "\n\n".join(chunks)


def render_template(base_template: Path, record: RecordSpec) -> str:
    default_map = parse_metadata_map(record.metadata.get("defaults", ""))
    wire_name_map = parse_metadata_map(record.metadata.get("wire_names", ""))
    owned_name_map = parse_metadata_map(record.metadata.get("owned_names", ""))
    includes = ", ".join(f'"{include}"' for include in record.cpp_imports)
    return (
        "{% set target_name = "
        + repr(record.model_name)
        + " %}\n"
        + "{% set record_type = "
        + repr(record.record_type)
        + " %}\n"
        + "{% set cpp_includes = ["
        + includes
        + "] %}\n"
        + "{% set defaults = "
        + default_map
        + " %}\n"
        + "{% set wire_names = "
        + wire_name_map
        + " %}\n"
        + "{% set owned_names = "
        + owned_name_map
        + " %}\n"
        + "{% set debug_fields = "
        + ("true" if record.debug_fields else "false")
        + " %}\n"
        + base_template.read_text(encoding="utf-8")
    )


def run_tsc_py(spec_text: str, template_text: str, output: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="wal-typespec.") as tmp:
        tmp_path = Path(tmp)
        spec_path = tmp_path / "record.tsp"
        template_path = tmp_path / "template.j2"
        spec_path.write_text(spec_text, encoding="utf-8")
        template_path.write_text(template_text, encoding="utf-8")
        subprocess.run(
            [
                "uvx",
                "--from",
                "tsc-py>=0.2.0",
                "tsc-py",
                str(spec_path),
                "--language",
                "cpp",
                "--template",
                str(template_path),
                "-o",
                str(output),
            ],
            cwd=REPO_ROOT,
            check=True,
        )
        generated = output.read_text(encoding="utf-8").strip()
        output.write_text(generated + "\n", encoding="utf-8")


def find_clang_format() -> str | None:
    override = os.environ.get("CLANG_FORMAT")
    if override:
        return override
    for candidate in ("clang-format-18", "clang-format"):
        path = shutil.which(candidate)
        if path:
            return path
    return None


def format_outputs(outputs: list[Path]) -> None:
    clang_format = find_clang_format()
    if not clang_format:
        print("warning: clang-format not found; generated WAL files were not formatted")
        return
    subprocess.run([clang_format, "-i", *(str(output) for output in outputs)], check=True)


def generate_record(record: RecordSpec, output_root: Path) -> None:
    spec_text = flatten_typespec(record)
    outputs = []
    for template_name, relative_output in [
        ("wal_record_header.h.j2", record.header_output),
        ("wal_record_source.cpp.j2", record.source_output),
    ]:
        output = output_root / relative_output
        output.parent.mkdir(parents=True, exist_ok=True)
        template_text = render_template(TEMPLATE_ROOT / template_name, record)
        run_tsc_py(spec_text, template_text, output)
        outputs.append(output)
    format_outputs(outputs)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output-root",
        type=Path,
        default=REPO_ROOT,
        help="Root to write generated src/ and include/ files into.",
    )
    parser.add_argument("records", nargs="*", help="Specific .tsp record basenames to generate.")
    args = parser.parse_args()

    requested = {name if name.endswith(".tsp") else f"{name}.tsp" for name in args.records}
    spec_paths = sorted(RECORD_SPEC_ROOT.glob("*.tsp"))
    for path in spec_paths:
        if requested and path.name not in requested:
            continue
        record = load_record_spec(path)
        print(f"generate {record.model_name}")
        generate_record(record, args.output_root)


if __name__ == "__main__":
    main()
