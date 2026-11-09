#!/usr/bin/env python3
"""Resolve categorized instruction-pointer samples against executable symbols."""

import argparse
import bisect
import collections
import pathlib
import re
import subprocess
import sys


CATEGORY_NAMES = (
    "native",
    "hle",
    "interpreter",
    "ge",
    "vertex",
    "sync",
    "backend",
    "scheduler",
    "primitive",
    "bezier",
    "spline",
    "patch_eval",
    "patch_draw",
    "vertex_decode",
    "capture",
    "texture_capture",
    "state_capture",
    "point",
    "line",
    "sprite",
    "bezier_eval",
    "bezier_draw",
    "spline_eval",
    "spline_draw",
)


def parse_args():
    parser = argparse.ArgumentParser(
        description=(
            "Resolve psp-recomp instruction-pointer samples and summarize "
            "exclusive runtime categories."
        )
    )
    parser.add_argument("samples", type=pathlib.Path)
    symbols = parser.add_mutually_exclusive_group(required=True)
    symbols.add_argument(
        "--executable",
        type=pathlib.Path,
        help="executable to inspect with 'nm -n'",
    )
    symbols.add_argument(
        "--symbols",
        type=pathlib.Path,
        help="existing 'nm -n' output",
    )
    parser.add_argument(
        "--image-base",
        type=lambda value: int(value, 0),
        help="symbol image base; defaults to __mh_execute_header or zero",
    )
    parser.add_argument(
        "--category",
        default="all",
        help="exclusive category name, numeric ID, or 'all' (default)",
    )
    parser.add_argument(
        "--top",
        type=int,
        default=30,
        help="number of executable symbols and external modules to show",
    )
    return parser.parse_args()


def category_id(value):
    if value == "all":
        return None
    try:
        parsed = int(value, 10)
    except ValueError:
        try:
            return CATEGORY_NAMES.index(value)
        except ValueError as exc:
            choices = ", ".join(CATEGORY_NAMES)
            raise ValueError(
                f"unknown category '{value}' (known: {choices})"
            ) from exc
    if parsed < 0 or parsed >= len(CATEGORY_NAMES):
        raise ValueError(
            f"category ID must be 0..{len(CATEGORY_NAMES) - 1}"
        )
    return parsed


def read_lines(path):
    try:
        return path.read_text(errors="replace").splitlines()
    except OSError as exc:
        raise ValueError(f"cannot read {path}: {exc}") from exc


def read_samples(path, selected_category):
    if selected_category is None:
        result = []
        for line in read_lines(path):
            if not line or line.startswith("#"):
                continue
            fields = line.split()
            if len(fields) != 2:
                raise ValueError(f"invalid sample row in {path}: {line}")
            result.append((int(fields[0], 16), int(fields[1], 10)))
        return result

    categories_path = path.with_name(path.name + ".categories.txt")
    result = []
    for line in read_lines(categories_path):
        if not line or line.startswith("#"):
            continue
        fields = line.split()
        if len(fields) != 3:
            raise ValueError(
                f"invalid categorized sample row in {categories_path}: {line}"
            )
        rva = int(fields[0], 16)
        category = int(fields[1], 10)
        count = int(fields[2], 10)
        if category == selected_category:
            result.append((rva, count))
    return result


def read_symbol_text(args):
    if args.symbols:
        return "\n".join(read_lines(args.symbols))
    try:
        process = subprocess.run(
            ["nm", "-n", str(args.executable)],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
    except (OSError, subprocess.CalledProcessError) as exc:
        detail = getattr(exc, "stderr", "") or str(exc)
        raise ValueError(f"cannot read executable symbols: {detail}") from exc
    return process.stdout


def parse_symbols(text, image_base):
    absolute = []
    detected_base = None
    pattern = re.compile(
        r"^([0-9a-fA-F]+)\s+([Tt])\s+(\S+)$"
    )
    for line in text.splitlines():
        match = pattern.match(line)
        if not match:
            continue
        address = int(match.group(1), 16)
        name = match.group(3)
        absolute.append((address, name))
        if name in ("__mh_execute_header", "_mh_execute_header"):
            detected_base = address
    base = image_base
    if base is None:
        base = detected_base if detected_base is not None else 0
    symbols = sorted(
        (address - base, name)
        for address, name in absolute
        if address >= base
    )
    if not symbols:
        raise ValueError("symbol input contains no text symbols")
    return base, symbols


def resolve_samples(samples, symbols):
    addresses = [address for address, _ in symbols]
    resolved = collections.Counter()
    unresolved = 0
    external = 0
    for rva, count in samples:
        if rva == 0xFFFFFFFF:
            external += count
            continue
        index = bisect.bisect_right(addresses, rva) - 1
        if index < 0:
            unresolved += count
            continue
        resolved[symbols[index][1]] += count
    return resolved, external, unresolved


def read_external_modules(samples_path, selected_category):
    modules_path = samples_path.with_name(
        samples_path.name + ".modules.txt"
    )
    modules = collections.Counter()
    for line in read_lines(modules_path):
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) != 3:
            if selected_category is not None:
                raise ValueError(
                    f"{modules_path} lacks category data; capture it again"
                )
            legacy = line.rsplit(" ", 1)
            if len(legacy) != 2:
                raise ValueError(
                    f"invalid external sample row in {modules_path}: {line}"
                )
            modules[legacy[0]] += 1
            continue
        category = int(fields[0], 10)
        if selected_category is None or category == selected_category:
            modules[fields[1]] += 1
    return modules


def percentage(part, total):
    return 100.0 * part / total if total else 0.0


def main():
    args = parse_args()
    if args.top <= 0:
        print("--top must be positive", file=sys.stderr)
        return 2
    try:
        selected_category = category_id(args.category)
        samples = read_samples(args.samples, selected_category)
        base, symbols = parse_symbols(
            read_symbol_text(args), args.image_base
        )
        resolved, external, unresolved = resolve_samples(samples, symbols)
        modules = read_external_modules(
            args.samples, selected_category
        )
    except ValueError as exc:
        print(exc, file=sys.stderr)
        return 2

    total = sum(count for _, count in samples)
    executable = sum(resolved.values())
    generated = sum(
        count
        for name, count in resolved.items()
        if re.search(r"(?:^|_)f_[0-9a-fA-F]{8}$", name)
    )
    category_label = (
        "all"
        if selected_category is None
        else f"{CATEGORY_NAMES[selected_category]} ({selected_category})"
    )
    print(f"category: {category_label}")
    print(f"image base: 0x{base:x}")
    print(f"samples: {total}")
    print(
        "executable: "
        f"{executable} ({percentage(executable, total):.2f}%)"
    )
    print(
        "generated functions: "
        f"{generated} ({percentage(generated, total):.2f}%)"
    )
    print(f"external: {external} ({percentage(external, total):.2f}%)")
    print(f"unresolved: {unresolved}")
    print("\ntop executable symbols:")
    for name, count in resolved.most_common(args.top):
        print(
            f"{count:8d} {percentage(count, total):6.2f}% {name}"
        )
    print("\ntop external modules:")
    for name, count in modules.most_common(args.top):
        print(
            f"{count:8d} {percentage(count, total):6.2f}% {name}"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
