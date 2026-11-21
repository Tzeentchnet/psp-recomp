#!/usr/bin/env python3
"""Validate interpreter miss/profile reports against a reviewed budget."""

import argparse
import dataclasses
import pathlib
import sys


@dataclasses.dataclass(frozen=True)
class Profile:
    calls: int
    instructions: int
    syscalls: int
    native_handoffs: int
    step_limits: int
    returns: int
    failures: int


def _report_lines(path):
    try:
        return path.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        raise ValueError(f"cannot read {path}: {exc}") from exc


def load_misses(path):
    misses = set()
    for line_number, line in enumerate(_report_lines(path), start=1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        fields = line.split()
        if len(fields) != 2:
            raise ValueError(
                f"{path}:{line_number}: expected module and target"
            )
        key = (fields[0], int(fields[1], 0))
        if key in misses:
            raise ValueError(
                f"{path}:{line_number}: duplicate target "
                f"{key[0]} 0x{key[1]:08x}"
            )
        misses.add(key)
    return misses


def load_profiles(path):
    profiles = {}
    for line_number, line in enumerate(_report_lines(path), start=1):
        line = line.strip()
        if not line or line.startswith("#") or line.startswith("module\t"):
            continue
        fields = line.split("\t")
        if len(fields) != 9:
            raise ValueError(
                f"{path}:{line_number}: expected nine profile fields"
            )
        key = (fields[0], int(fields[1], 0))
        if key in profiles:
            raise ValueError(
                f"{path}:{line_number}: duplicate target "
                f"{key[0]} 0x{key[1]:08x}"
            )
        profiles[key] = Profile(*(int(value, 10) for value in fields[2:]))
    return profiles


def load_policy(path):
    if path is None:
        return {}
    policy = {}
    for line_number, line in enumerate(_report_lines(path), start=1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        fields = line.split()
        if len(fields) != 4:
            raise ValueError(
                f"{path}:{line_number}: expected module, target, "
                "max_calls, and max_instructions"
            )
        key = (fields[0], int(fields[1], 0))
        if key in policy:
            raise ValueError(
                f"{path}:{line_number}: duplicate target "
                f"{key[0]} 0x{key[1]:08x}"
            )
        max_calls = int(fields[2], 10)
        max_instructions = int(fields[3], 10)
        if max_calls < 0 or max_instructions < 0:
            raise ValueError(
                f"{path}:{line_number}: budgets must be nonnegative"
            )
        policy[key] = (max_calls, max_instructions)
    return policy


def validate(misses, profiles, policy):
    errors = []
    if misses != set(profiles):
        for module, target in sorted(misses - set(profiles)):
            errors.append(
                f"miss has no profile: {module} 0x{target:08x}"
            )
        for module, target in sorted(set(profiles) - misses):
            errors.append(
                f"profile has no miss: {module} 0x{target:08x}"
            )
    for key in sorted(misses):
        module, target = key
        if key not in policy:
            errors.append(
                f"unexpected interpreter target: "
                f"{module} 0x{target:08x}"
            )
            continue
        profile = profiles.get(key)
        if profile is None:
            continue
        max_calls, max_instructions = policy[key]
        if profile.calls > max_calls:
            errors.append(
                f"{module} 0x{target:08x}: calls "
                f"{profile.calls} exceed {max_calls}"
            )
        if profile.instructions > max_instructions:
            errors.append(
                f"{module} 0x{target:08x}: instructions "
                f"{profile.instructions} exceed {max_instructions}"
            )
        if profile.step_limits:
            errors.append(
                f"{module} 0x{target:08x}: "
                f"{profile.step_limits} step-limit stops"
            )
        if profile.failures:
            errors.append(
                f"{module} 0x{target:08x}: "
                f"{profile.failures} interpreter failures"
            )
        if profile.returns != profile.calls:
            errors.append(
                f"{module} 0x{target:08x}: "
                f"{profile.returns}/{profile.calls} calls returned"
            )
    return errors


def parse_args():
    parser = argparse.ArgumentParser(
        description=(
            "Require zero interpreter fallback by default, or validate "
            "reviewed per-target call/instruction budgets."
        )
    )
    parser.add_argument("misses", type=pathlib.Path)
    parser.add_argument("profile", type=pathlib.Path)
    parser.add_argument(
        "--policy",
        type=pathlib.Path,
        help=(
            "optional rows: module target max_calls max_instructions; "
            "omitting it requires zero targets"
        ),
    )
    return parser.parse_args()


def main():
    args = parse_args()
    try:
        misses = load_misses(args.misses)
        profiles = load_profiles(args.profile)
        policy = load_policy(args.policy)
    except (ValueError, OverflowError) as exc:
        print(exc, file=sys.stderr)
        return 2
    errors = validate(misses, profiles, policy)
    if errors:
        for error in errors:
            print(f"interpreter report gate: {error}", file=sys.stderr)
        return 1
    calls = sum(profile.calls for profile in profiles.values())
    instructions = sum(
        profile.instructions for profile in profiles.values()
    )
    print(
        "interpreter report gate: PASS "
        f"targets={len(misses)} calls={calls} "
        f"instructions={instructions}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
