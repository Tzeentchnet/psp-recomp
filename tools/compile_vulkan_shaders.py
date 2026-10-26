#!/usr/bin/env python3
"""Compile Vulkan GE shaders and emit deterministic C initializer includes."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
BACKEND_DIR = ROOT / "src" / "rt" / "gpu_sdl3vk"
SHADER_DIR = BACKEND_DIR / "shaders"
SHADERS = (
    ("psp.vert", "vert", "psp_vert.inc"),
    ("psp.frag", "frag", "psp_frag.inc"),
)


def sdk_glslang() -> Path | None:
    explicit_sdk = os.environ.get("VULKAN_SDK")
    candidates: list[Path] = []
    if explicit_sdk:
        candidates.append(Path(explicit_sdk) / "Bin" / "glslangValidator.exe")
        candidates.append(Path(explicit_sdk) / "bin" / "glslangValidator")
    if os.name == "nt":
        sdk_root = Path("C:/VulkanSDK")

        def sdk_version(path: Path) -> tuple[int, ...]:
            try:
                return tuple(int(part) for part in path.name.split("."))
            except ValueError:
                return ()

        if sdk_root.is_dir():
            versions = sorted(
                (path for path in sdk_root.iterdir() if path.is_dir()),
                key=sdk_version,
                reverse=True,
            )
            candidates.extend(
                path / "Bin" / "glslangValidator.exe"
                for path in versions
            )
    return next((path for path in candidates if path.is_file()), None)


def find_glslang(requested: str | None) -> Path:
    requested_on_path = shutil.which(requested) if requested else None
    configured = os.environ.get("GLSLANG_VALIDATOR")
    configured_on_path = shutil.which(configured) if configured else None
    installed = sdk_glslang()
    candidates = (
        requested,
        requested_on_path,
        configured,
        configured_on_path,
        shutil.which("glslangValidator"),
        str(installed) if installed else None,
    )
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return Path(candidate).resolve()
    raise FileNotFoundError(
        "glslangValidator was not found; pass --glslang, set "
        "GLSLANG_VALIDATOR or VULKAN_SDK, or install the Vulkan SDK"
    )


def compile_shader(
    glslang: Path, source: Path, stage: str
) -> bytes:
    with tempfile.TemporaryDirectory(prefix="psprecomp-spirv-") as directory:
        output = Path(directory) / f"{source.stem}.spv"
        command = [
            str(glslang),
            "-V",
            "--target-env",
            "vulkan1.0",
            "-S",
            stage,
            "-o",
            str(output),
            str(source),
        ]
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode:
            if result.stdout:
                print(result.stdout, file=sys.stderr, end="")
            if result.stderr:
                print(result.stderr, file=sys.stderr, end="")
            raise RuntimeError(
                f"glslangValidator failed for {source.name} "
                f"({result.returncode})"
            )
        data = output.read_bytes()
        if len(data) % 4:
            raise RuntimeError(
                f"{source.name} produced non-word-aligned SPIR-V"
            )
        if len(data) < 4 or struct.unpack_from("<I", data)[0] != 0x07230203:
            raise RuntimeError(
                f"{source.name} produced an invalid SPIR-V header"
            )
        return data


def render_include(source: Path, stage: str, data: bytes) -> str:
    source_hash = hashlib.sha256(source.read_bytes()).hexdigest().upper()
    spirv_hash = hashlib.sha256(data).hexdigest().upper()
    words = struct.unpack(f"<{len(data) // 4}I", data)
    lines = [
        f"/* Generated from {source.name} ({stage}) by "
        "tools/compile_vulkan_shaders.py.",
        f" * source SHA-256: {source_hash}",
        f" * SPIR-V SHA-256: {spirv_hash} */",
        "{",
    ]
    for offset in range(0, len(words), 4):
        chunk = words[offset : offset + 4]
        suffix = "," if offset + len(chunk) < len(words) else ""
        lines.append(
            "    " + ",".join(f"0x{word:08x}" for word in chunk) + suffix
        )
    lines.append("}")
    return "\n".join(lines) + "\n"


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--glslang", help="path to glslangValidator executable"
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="compile and fail if checked-in includes differ",
    )
    args = parser.parse_args(argv)

    try:
        glslang = find_glslang(args.glslang)
        stale: list[Path] = []
        for source_name, stage, output_name in SHADERS:
            source = SHADER_DIR / source_name
            output = BACKEND_DIR / output_name
            rendered = render_include(
                source, stage, compile_shader(glslang, source, stage)
            )
            if args.check:
                if (
                    not output.is_file()
                    or output.read_text(encoding="ascii") != rendered
                ):
                    stale.append(output)
            else:
                output.write_text(
                    rendered, encoding="ascii", newline="\n"
                )
                print(f"wrote {output.relative_to(ROOT)}")
        if stale:
            for output in stale:
                print(
                    "stale generated shader: "
                    f"{output.relative_to(ROOT)}",
                    file=sys.stderr,
                )
            return 1
    except (FileNotFoundError, OSError, RuntimeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
