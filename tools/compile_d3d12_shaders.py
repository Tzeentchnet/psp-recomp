#!/usr/bin/env python3
"""Compile the D3D12 GE shaders and emit deterministic C initializer includes."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SHADER_DIR = ROOT / "src" / "rt" / "gpu_d3d12" / "shaders"
SHADERS = (
    ("psp_vs.hlsl", "vs_6_0", "psp_vs.inc"),
    ("psp_ps.hlsl", "ps_6_0", "psp_ps.inc"),
)


def windows_sdk_dxc() -> Path | None:
    kits = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"))
    root = kits / "Windows Kits" / "10" / "bin"
    if not root.is_dir():
        return None
    def sdk_version(path: Path) -> tuple[int, ...]:
        try:
            return tuple(int(part) for part in path.parent.parent.name.split("."))
        except ValueError:
            return ()

    candidates = sorted(root.glob("*/x64/dxc.exe"), key=sdk_version, reverse=True)
    return candidates[0] if candidates else None


def find_dxc(requested: str | None) -> Path:
    requested_on_path = shutil.which(requested) if requested else None
    sdk_dxc = windows_sdk_dxc()
    candidates = (
        requested,
        requested_on_path,
        os.environ.get("DXC"),
        shutil.which("dxc"),
        str(sdk_dxc) if sdk_dxc else None,
    )
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return Path(candidate).resolve()
    raise FileNotFoundError(
        "dxc was not found; pass --dxc, set DXC, or install the Windows SDK shader tools"
    )


def compile_shader(dxc: Path, source: Path, profile: str) -> bytes:
    with tempfile.TemporaryDirectory(prefix="psprecomp-dxil-") as directory:
        output = Path(directory) / (source.stem + ".dxil")
        command = [
            str(dxc),
            "-nologo",
            "-E",
            "main",
            "-T",
            profile,
            "-HV",
            "2021",
            "-Ges",
            "-WX",
            "-O3",
            "-Qstrip_debug",
            "-Qstrip_reflect",
            "-Fo",
            str(output),
            str(source),
        ]
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode:
            if result.stdout:
                print(result.stdout, file=sys.stderr, end="")
            if result.stderr:
                print(result.stderr, file=sys.stderr, end="")
            raise RuntimeError(f"DXC failed for {source.name} ({result.returncode})")
        return output.read_bytes()


def render_include(source: Path, profile: str, data: bytes) -> str:
    source_hash = hashlib.sha256(source.read_bytes()).hexdigest().upper()
    dxil_hash = hashlib.sha256(data).hexdigest().upper()
    lines = [
        f"/* Generated from {source.name} ({profile}) by tools/compile_d3d12_shaders.py.",
        f" * source SHA-256: {source_hash}",
        f" * DXIL SHA-256:   {dxil_hash} */",
        "{",
    ]
    for offset in range(0, len(data), 12):
        chunk = data[offset : offset + 12]
        suffix = "," if offset + len(chunk) < len(data) else ""
        lines.append("    " + ",".join(f"0x{byte:02x}" for byte in chunk) + suffix)
    lines.append("}")
    return "\n".join(lines) + "\n"


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dxc", help="path to dxc executable")
    parser.add_argument(
        "--check",
        action="store_true",
        help="compile and fail if checked-in includes differ",
    )
    args = parser.parse_args(argv)

    try:
        dxc = find_dxc(args.dxc)
        stale: list[Path] = []
        for source_name, profile, output_name in SHADERS:
            source = SHADER_DIR / source_name
            output = SHADER_DIR / output_name
            rendered = render_include(
                source, profile, compile_shader(dxc, source, profile)
            )
            if args.check:
                if not output.is_file() or output.read_text(encoding="ascii") != rendered:
                    stale.append(output)
            else:
                output.write_text(rendered, encoding="ascii", newline="\n")
                print(f"wrote {output.relative_to(ROOT)}")
        if stale:
            for output in stale:
                print(f"stale generated shader: {output.relative_to(ROOT)}", file=sys.stderr)
            return 1
    except (FileNotFoundError, OSError, RuntimeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
