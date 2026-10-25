#!/usr/bin/env python3
"""Manage and match provenance-bearing PSP function catalogs."""

from __future__ import annotations

import argparse
import csv
import io
import os
from pathlib import Path
import sys
from typing import Any

import analyze
import codegen
from function_reuse import (
    CATALOG_SCHEMA_VERSION,
    FINGERPRINT_ALGORITHM,
    CatalogError,
    add_catalog_entry,
    fingerprint_instructions,
    index_catalog,
    load_catalog,
    match_catalog,
    write_catalog,
    write_text_if_changed,
)


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CATALOG = ROOT / "assets" / "functions" / "catalog.toml"


def default_catalog_path() -> Path:
    configured = os.environ.get("PSP_RECOMP_FUNCTION_CATALOG")
    if not configured:
        return DEFAULT_CATALOG
    return Path(os.path.expandvars(configured)).expanduser()


def parse_int(value: str) -> int:
    return int(value, 0)


def add_module_options(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("elf", type=Path)
    parser.add_argument("--base", type=parse_int)
    parser.add_argument("--jumptables", type=Path)
    parser.add_argument("--overrides", type=Path)
    parser.add_argument(
        "--function-entry", type=parse_int, action="append", default=[]
    )


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    check = subparsers.add_parser("check", help="validate a catalog")
    check.add_argument(
        "--catalog", type=Path, default=default_catalog_path()
    )

    match = subparsers.add_parser(
        "match", help="match discovered module functions against a catalog"
    )
    add_module_options(match)
    match.add_argument(
        "--catalog", type=Path, default=default_catalog_path()
    )
    match.add_argument("--output", type=Path)
    match.add_argument("--symbols", type=Path)

    add = subparsers.add_parser(
        "add", help="add one reviewed function identity to a catalog"
    )
    add_module_options(add)
    add.add_argument(
        "--catalog", type=Path, default=default_catalog_path()
    )
    add.add_argument("--address", type=parse_int, required=True)
    add.add_argument("--name", required=True)
    add.add_argument("--library")
    add.add_argument("--source", required=True)
    add.add_argument("--source-revision", required=True)
    add.add_argument("--license", required=True)
    add.add_argument("--note")
    return parser.parse_args(argv)


def module_functions(
    args: argparse.Namespace,
) -> tuple[analyze.Elf, set[int], list[tuple[int, int]]]:
    codegen.JUMPTABLES.clear()
    codegen.JAL_NEXT.clear()
    codegen.HOST_FUNCTIONS.clear()
    codegen.INSTRUCTION_HOOKS.clear()
    if args.jumptables:
        codegen.JUMPTABLES.update(
            codegen.cx.load_jumptables(args.jumptables)
        )
    if args.overrides:
        gotos, jal_next, host_functions, instruction_hooks = (
            codegen.cx.load_overrides(args.overrides)
        )
        codegen.JUMPTABLES.update(gotos)
        codegen.JAL_NEXT.update(jal_next)
        codegen.HOST_FUNCTIONS.update(host_functions)
        codegen.INSTRUCTION_HOOKS.update(instruction_hooks)

    elf = analyze.Elf(args.elf, base=args.base)
    known, ranges = analyze.analyze(elf)
    known.update(args.function_entry)
    known -= {address + 8 for address in codegen.JAL_NEXT}
    invalid = [
        address
        for address in known
        if address & 3 or not analyze.in_ranges(address, ranges)
    ]
    if invalid:
        rendered = ", ".join(f"0x{address:08x}" for address in sorted(invalid))
        raise ValueError(
            f"function entries are unaligned or outside executable ranges: {rendered}"
        )
    return elf, known, ranges


def function_fingerprint(
    elf: analyze.Elf,
    address: int,
    ranges: list[tuple[int, int]],
    known: set[int],
) -> dict[str, Any]:
    instructions, _ = codegen.function_flow(elf, address, ranges, known)
    words = []
    for instruction_address in sorted(instructions):
        word = codegen.read32(elf, instruction_address)
        if word is not None:
            words.append((instruction_address, word))
    return fingerprint_instructions(address, words)


def report_text(
    module: Path,
    catalog_path: Path,
    matches: list[dict[str, Any]],
) -> str:
    lines = [
        "# Function catalog match report. Contains identities and hashes, not code.",
        f"schema_version = {CATALOG_SCHEMA_VERSION}",
        f"fingerprint_algorithm = {catalog_quote(FINGERPRINT_ALGORITHM)}",
        f"module = {catalog_quote(str(module))}",
        f"catalog = {catalog_quote(str(catalog_path))}",
        "",
    ]
    order = (
        "address",
        "name",
        "library",
        "match",
        "normalized_sha256",
        "exact_sha256",
        "instruction_count",
        "span",
        "source",
        "source_revision",
        "license",
    )
    for match in matches:
        lines.append("[[match]]")
        for field in order:
            if field not in match:
                continue
            value = match[field]
            if field == "address":
                rendered = f"0x{value:08x}"
            elif isinstance(value, int):
                rendered = str(value)
            else:
                rendered = catalog_quote(str(value))
            lines.append(f"{field} = {rendered}")
        lines.append("")
    return "\n".join(lines)


def catalog_quote(value: str) -> str:
    import json

    return json.dumps(value, ensure_ascii=True)


def run_match(args: argparse.Namespace) -> int:
    catalog = load_catalog(args.catalog)
    catalog_index = index_catalog(catalog)
    elf, known, ranges = module_functions(args)
    stubs = analyze.stub_ranges(elf)
    matches = []
    for address in sorted(known):
        if analyze.in_ranges(address, stubs):
            continue
        fingerprint = function_fingerprint(elf, address, ranges, known)
        result = match_catalog(catalog_index, fingerprint)
        if result is None:
            continue
        entry, strength = result
        match = {
            "address": address,
            "name": entry["name"],
            "match": strength,
            "normalized_sha256": fingerprint["normalized_sha256"],
            "exact_sha256": fingerprint["exact_sha256"],
            "instruction_count": fingerprint["instruction_count"],
            "span": fingerprint["span"],
            "source": entry["source"],
            "source_revision": entry["source_revision"],
            "license": entry["license"],
        }
        if entry.get("library"):
            match["library"] = entry["library"]
        matches.append(match)

    if args.output:
        write_text_if_changed(
            args.output,
            report_text(args.elf, args.catalog, matches),
            encoding="ascii",
        )
    if args.symbols:
        stream = io.StringIO(newline="")
        writer = csv.writer(stream, lineterminator="\n")
        writer.writerow(("address", "name", "source", "match"))
        for match in matches:
            writer.writerow(
                (
                    f"0x{match['address']:08x}",
                    match["name"],
                    f"catalog:{match['normalized_sha256']}",
                    match["match"],
                )
            )
        write_text_if_changed(
            args.symbols, stream.getvalue(), encoding="utf-8"
        )
    exact = sum(match["match"] == "exact" for match in matches)
    print(
        f"catalog matches: {len(matches)} "
        f"({exact} exact, {len(matches) - exact} normalized)"
    )
    return 0


def run_add(args: argparse.Namespace) -> int:
    catalog = load_catalog(args.catalog)
    elf, known, ranges = module_functions(args)
    if not analyze.in_ranges(args.address, ranges) or args.address & 3:
        raise ValueError(
            f"0x{args.address:08x} is unaligned or outside executable ranges"
        )
    known.add(args.address)
    fingerprint = function_fingerprint(elf, args.address, ranges, known)
    entry = {
        "name": args.name,
        "normalized_sha256": fingerprint["normalized_sha256"],
        "instruction_count": fingerprint["instruction_count"],
        "span": fingerprint["span"],
        "exact_sha256": [fingerprint["exact_sha256"]],
        "source": args.source,
        "source_revision": args.source_revision,
        "license": args.license,
    }
    if args.library:
        entry["library"] = args.library
    if args.note:
        entry["note"] = args.note
    updated, changed = add_catalog_entry(catalog, entry)
    if changed:
        write_catalog(args.catalog, updated)
        print(
            f"cataloged {args.name} at 0x{args.address:08x}: "
            f"{fingerprint['normalized_sha256']}"
        )
    else:
        print(f"catalog already contains {args.name} exact variant")
    return 0


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        if args.command == "check":
            catalog = load_catalog(args.catalog)
            print(
                f"catalog valid: {len(catalog['function'])} function identities"
            )
            return 0
        if args.command == "match":
            return run_match(args)
        if args.command == "add":
            return run_add(args)
        raise AssertionError(args.command)
    except (CatalogError, OSError, ValueError) as exc:
        print(f"function_catalog: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
