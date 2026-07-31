#!/usr/bin/env python3
"""Sanity-check MGL_DUMP_MSL output for Metal argument-buffer layout errors."""

from __future__ import annotations

import argparse
import glob
import re
import sys
from pathlib import Path

STRUCT_RE = re.compile(
    r"struct\s+spvDescriptorSetBuffer(?P<set>\d+)\s*\{(?P<body>.*?)\};",
    re.DOTALL,
)
ID_RE = re.compile(r"\[\[id\((\d+)\)\]\]")
BUFFER_RE = re.compile(r"\[\[buffer\((\d+)\)\]\]")
ENTRY_SET_RE = re.compile(
    r"spvDescriptorSet(?P<set>\d+)\s*\[\[buffer\((?P<slot>\d+)\)\]\]"
)


def check_file(path: Path) -> int:
    text = path.read_text(encoding="utf-8", errors="replace")
    errors: list[str] = []
    warnings: list[str] = []

    structs = list(STRUCT_RE.finditer(text))
    entry_sets = {(int(m.group("set")), int(m.group("slot"))) for m in ENTRY_SET_RE.finditer(text)}
    buffer_slots = [int(value) for value in BUFFER_RE.findall(text)]

    for slot in buffer_slots:
        if slot > 30:
            errors.append(f"top-level [[buffer({slot})]] exceeds Metal slot 30")

    for match in structs:
        set_index = int(match.group("set"))
        if set_index not in (0, 1):
            errors.append(
                f"descriptor set {set_index} unexpectedly became an argument buffer; "
                "MGL keeps texture/sampler set 2 and internal set 3 discrete"
            )
        ids = [int(value) for value in ID_RE.findall(match.group("body"))]
        duplicates = sorted({value for value in ids if ids.count(value) > 1})
        if duplicates:
            errors.append(f"descriptor set {set_index} has duplicate [[id]] values: {duplicates}")
        if (set_index, set_index) not in entry_sets:
            warnings.append(
                f"descriptor set {set_index} was not found at top-level [[buffer({set_index})]]"
            )

    if "spvBufferSizeConstants" in text and "[[id(25)]]" not in text:
        warnings.append("spvBufferSizeConstants exists but was not found at [[id(25)]]")

    status = "OK" if not errors else "FAIL"
    print(f"{status}: {path}")
    print(
        f"  argument_sets={len(structs)} entry_sets={sorted(entry_sets)} "
        f"max_buffer_slot={max(buffer_slots, default=-1)}"
    )
    for warning in warnings:
        print(f"  warning: {warning}")
    for error in errors:
        print(f"  error: {error}")
    return 1 if errors else 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "paths",
        nargs="*",
        help="MSL files or glob patterns (default: /tmp/mgl_program_*_stage_*.msl)",
    )
    args = parser.parse_args()

    patterns = args.paths or ["/tmp/mgl_program_*_stage_*.msl"]
    paths: list[Path] = []
    for pattern in patterns:
        matches = glob.glob(pattern)
        if matches:
            paths.extend(Path(match) for match in matches)
        else:
            candidate = Path(pattern)
            if candidate.is_file():
                paths.append(candidate)

    unique_paths = sorted(set(paths))
    if not unique_paths:
        print("No MSL dump files found.", file=sys.stderr)
        return 2

    return max(check_file(path) for path in unique_paths)


if __name__ == "__main__":
    raise SystemExit(main())
