#!/usr/bin/env python3
# Compare two CPU-state traces (tools/TRACE_FORMAT.md) and report the first divergence.
# Exit 0 when the two traces are identical step-for-step, 1 when they diverge, 2 on a
# malformed input. The first divergence is reported with the guest PC where it happened,
# which is the located bug for differential testing against the PPSSPP oracle.

import argparse
import sys


class TraceError(ValueError):
    pass


def parse_step_line(line, lineno, path):
    # "<step> pc=<hpc> op=<hword> <tokens...>" -> (step, pc, op, {name: value})
    parts = line.split()
    if len(parts) < 3:
        raise TraceError(
            f"{path}:{lineno}: step line has fewer than 3 fields: {line!r}"
        )
    try:
        step = int(parts[0], 10)
    except ValueError as exc:
        raise TraceError(
            f"{path}:{lineno}: step index is not a decimal integer: {parts[0]!r}"
        ) from exc
    if not parts[1].startswith("pc=") or not parts[2].startswith("op="):
        raise TraceError(
            f"{path}:{lineno}: expected 'pc=' then 'op=', "
            f"got {parts[1]!r} {parts[2]!r}"
        )
    pc = parts[1][3:]
    op = parts[2][3:]
    try:
        if len(pc) != 10 or len(op) != 10:
            raise ValueError
        int(pc, 16)
        int(op, 16)
    except ValueError as exc:
        raise TraceError(
            f"{path}:{lineno}: pc/op must be 0x plus eight hex digits"
        ) from exc
    writes = {}
    for tok in parts[3:]:
        name, eq, value = tok.partition("=")
        if eq != "=" or not name or not value:
            raise TraceError(
                f"{path}:{lineno}: malformed write token: {tok!r}"
            )
        if name in writes:
            raise TraceError(
                f"{path}:{lineno}: duplicate write token for {name!r}"
            )
        writes[name] = value
    return step, pc, op, writes


def is_syscall(op_hex):
    w = int(op_hex, 16)
    return (w & 0xFC00003F) == 0x0000000C


def iter_steps(path, ignored_ranges=()):
    header = None
    with open(path, "r", encoding="ascii", newline="\n") as handle:
        for lineno, raw in enumerate(handle, start=1):
            line = raw.rstrip("\n")
            if line == "":
                continue
            if line.startswith("#"):
                if header is None:
                    header = line
                continue
            step = parse_step_line(line, lineno, path)
            pc = int(step[1], 16)
            if any(start <= pc < end for start, end in ignored_ranges):
                continue
            yield step
    if header is None:
        raise TraceError(f"{path}: missing required '# psp-recomp trace' header line")


def load(path, ignored_ranges=()):
    return None, list(iter_steps(path, ignored_ranges))


def describe_writes(writes):
    return " ".join(f"{name}={writes[name]}" for name in sorted(writes))


def diff(path_a, path_b, ignored_ranges=(), ignore_syscall_writes=False):
    a = iter(iter_steps(path_a, ignored_ranges))
    b = iter(iter_steps(path_b, ignored_ranges))
    sentinel = object()
    logical_step = 0
    last_pc = "0x????????"
    while True:
        value_a = next(a, sentinel)
        value_b = next(b, sentinel)
        if value_a is sentinel or value_b is sentinel:
            if value_a is sentinel and value_b is sentinel:
                return None
            shorter = path_a if value_a is sentinel else path_b
            return (
                logical_step,
                last_pc,
                f"trace length differs ({shorter} ends first)",
            )
        step_a, pc_a, op_a, w_a = value_a
        step_b, pc_b, op_b, w_b = value_b
        last_pc = pc_a
        if not ignored_ranges and step_a != step_b:
            return (
                logical_step, pc_a, f"step index {step_a} vs {step_b}"
            )
        if pc_a != pc_b:
            return (logical_step, pc_a, f"pc {pc_a} vs {pc_b}")
        if op_a != op_b and not (is_syscall(op_a) or is_syscall(op_b)):
            # Import-stub delay slots hold whatever syscall word each loader patched in
            # (PPSSPP uses its own numbering), so a syscall on either side matches any word.
            return (logical_step, pc_a, f"op {op_a} vs {op_b}")
        syscall = is_syscall(op_a) or is_syscall(op_b)
        if w_a != w_b and not (ignore_syscall_writes and syscall):
            only_a = {k: v for k, v in w_a.items() if w_b.get(k) != v}
            only_b = {k: v for k, v in w_b.items() if w_a.get(k) != v}
            detail = f"writes differ: A[{describe_writes(only_a)}] B[{describe_writes(only_b)}]"
            return (logical_step, pc_a, detail)
        logical_step += 1


def parse_pc_range(value):
    fields = value.split("-", 1)
    if len(fields) != 2:
        raise argparse.ArgumentTypeError("expected START-END")
    try:
        start = int(fields[0].removeprefix("0x"), 16)
        end = int(fields[1].removeprefix("0x"), 16)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("range bounds must be hexadecimal") from exc
    if start >= end or end > 0x100000000:
        raise argparse.ArgumentTypeError("range must be nonempty and within 32 bits")
    return start, end


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace_a")
    parser.add_argument("trace_b")
    parser.add_argument(
        "--ignore-pc-range",
        action="append",
        default=[],
        type=parse_pc_range,
        help="ignore steps in the half-open hexadecimal START-END range",
    )
    parser.add_argument(
        "--ignore-syscall-writes",
        action="store_true",
        help="ignore register/memory deltas on syscall instructions",
    )
    args = parser.parse_args(argv)
    try:
        result = diff(
            args.trace_a,
            args.trace_b,
            args.ignore_pc_range,
            args.ignore_syscall_writes,
        )
    except (OSError, TraceError) as exc:
        print(f"tracediff: {exc}", file=sys.stderr)
        return 2
    if result is None:
        count = sum(1 for _ in iter_steps(args.trace_a, args.ignore_pc_range))
        print(f"OK: traces identical, {count} compared steps, zero divergences")
        return 0
    step, pc, detail = result
    print(f"DIVERGENCE at step {step}, pc {pc}: {detail}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
