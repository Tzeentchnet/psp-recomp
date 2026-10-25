# Metal Gear Acid Findings

This document summarizes the Metal Gear Acid facts that forced the shared
runtime to support dynamic modules, overlays, and hybrid execution. The
[Metal Gear Acid Recomp repository](https://github.com/Tzeentchnet/MetalGearAcid-Recomp)
is authoritative for current hashes, tests, and release status.

## Supported source

| Property | Value |
|---|---|
| Disc ID | `ULUS-10006` |
| Version | USA v1.00 |
| ISO size | `201,392,128` bytes |
| ISO SHA-1 | `d29eaba896be28c2575ead246bdaa3a9712929d4` |
| Main module | `PSP_GAME/USRDIR/program.prx` |
| Module name | `mgp_main` |
| Main base | `0x08804000` |
| Main entry | `0x0880432c` |
| Image size | `0x001e1800` |

`program.prx` and `BOOT.BIN` are byte-identical plaintext modules for this
release. `EBOOT.BIN` is not the same payload. This relationship is
title-specific and does not imply that other games can bypass decryption.

Authoritative configuration:
[config/mga.toml](https://github.com/Tzeentchnet/MetalGearAcid-Recomp/blob/master/config/mga.toml).

## Measured boot contract

The title configuration pins:

- Exact entry and module name.
- Start-thread UID, priority, stack size, and attributes.
- Next UID and initial GE list identifier.
- The exact `disc0:/PSP_GAME/SYSDIR/EBOOT.BIN` argument.
- An explicit computed function entry at `0x08919f10`.

These values came from PPSSPP interpreter measurements. Copying Killzone's
thread or boot argument would diverge before meaningful title execution.

Related evidence:
[FINDINGS MGA-F-004 and MGA-F-005](https://github.com/Tzeentchnet/MetalGearAcid-Recomp/blob/master/docs/FINDINGS.md).

## Multi-module architecture

Metal Gear Acid includes:

- A statically translated main program.
- 71 additional plaintext PRXs under module and stage roots.
- Encrypted Sony modules.
- Private libraries and service modules.
- Stage overlays that reuse guest address ranges.
- Raw-LBN paths for some runtime-loaded content.

This topology required a real PRX module lifecycle rather than fake-success
`sceKernelLoadModule` calls.

Function-reuse validation covered the main module and all eight configured
native modules: 16,541 functions, 16,288 cacheable entries, no trapping stubs,
and a complete 16,288-hit unchanged rerun. Killzone's temporary 37-entry
library catalog found five normalized soft-float candidates but no exact
identity variants; they remain review evidence only.

The shared implementation now supports:

- Path and file-descriptor loading.
- PSP memory allocation and base reuse.
- Segment copy, BSS clearing, and relocations.
- Library-qualified import and export binding.
- Per-thread module GP.
- Start, stop, unload, and overlay invalidation.

Related evidence:
[FINDINGS MGA-F-006 through MGA-F-016](https://github.com/Tzeentchnet/MetalGearAcid-Recomp/blob/master/docs/FINDINGS.md).

## Title module policy

Metal Gear Acid does not use one blanket policy:

- Selected Sony media modules are replaced by named HLE implementations.
- KJFS, ZLIB, SOUND, and stage modules execute as guest code, interpreted code,
  generated native code, or a validated combination.
- Encrypted modules without a supported HLE policy fail explicitly.
- Static native overlays are selected by exact identity and verified against
  live guest code.
- Same-address overlays remain distinct through module-qualified dispatch.

Exact paths, hashes, bases, runtime-JAL flags, and verification policy belong
in `config/mga.toml`.

Related design:
[UPSTREAM.md](https://github.com/Tzeentchnet/MetalGearAcid-Recomp/blob/master/docs/UPSTREAM.md).

## Hybrid execution and overlays

Metal Gear Acid exposed cases where:

- Interpreted code performs syscalls and must resume correctly.
- Interpreted code calls a generated native function and returns to the
  interpreter.
- Native code calls a missing function that must continue in the interpreter.
- A module patches a JAL target at runtime.
- An overlay at a reused address must not resolve to a previous module.
- A previously verified function can be changed later and must fall back.

The shared runtime therefore preserves module identity, verifies live code,
supports nested native/interpreter handoff, and keeps module-level diagnostic
overrides.

Related evidence:
[FINDINGS MGA-F-034, MGA-F-039, MGA-F-040, and MGA-F-057](https://github.com/Tzeentchnet/MetalGearAcid-Recomp/blob/master/docs/FINDINGS.md).

## Graphics findings

The title's card presentation, UI, text, overlays, and stage routes expanded
renderer validation beyond Killzone:

- PSP-quantized nearest and bilinear texture sampling.
- Integer texture-function and fog math.
- Native-format logical capture canonicalization.
- Feature- and edge-class analysis of hardware residuals.
- Full internal-resolution capture for scaling analysis.
- Structured CPU, backend, and fenced-GPU performance reports.

The software/GDI build remains the pixel-exact baseline. D3D12 is the
recommended high-performance path; Vulkan remains a supported alternate path.

Related evidence:
[FINDINGS MGA-F-058 through MGA-F-060](https://github.com/Tzeentchnet/MetalGearAcid-Recomp/blob/master/docs/FINDINGS.md).

## Input and deterministic routes

Metal Gear Acid compiles out the Killzone twin-stick extension and exposes
standard PSP controls.

Title-specific scripted routes include:

- Front-end navigation.
- Default-deck setup.
- A six-card prologue route.
- Save/load probes.
- Forest and STAGE01 overlay transitions.

These scripts prove state progression, overlay activation, renderer output,
audio continuity, and bounded fallback behavior. Their button sequences and
frame indices are game content, not shared defaults.

Related evidence:
[REPRODUCTION sections 6, 11, and 12](https://github.com/Tzeentchnet/MetalGearAcid-Recomp/blob/master/docs/REPRODUCTION.md).

## Current validation boundary

The deterministic front end and selected gameplay routes work. Remaining
title work includes:

- Full campaign validation.
- Save creation and reload coverage.
- Movie validation.
- Remaining renderer parity.
- Long-run stability and release acceptance.

Use the parent
[STATUS.md](https://github.com/Tzeentchnet/MetalGearAcid-Recomp/blob/master/docs/STATUS.md)
for the current milestone rather than treating this historical summary as a
release claim.

## Do not generalize

- The ISO/hash or main-module addresses.
- The EBOOT argument and start-thread UID state.
- Raw-LBN overlay paths.
- Stage names, fixed bases, and module hashes.
- The `0x08919f10` computed entry.
- The title's front-end allowlist or runtime-JAL entries.
- Deck-building input scripts and gameplay frame probes.
- The exact set of modules selected for HLE replacement.

## Reusable lessons contributed

Metal Gear Acid established or hardened:

- Complete dynamic PRX loading and unloading.
- PSP allocator and deterministic overlay base reuse.
- Library-qualified import/export identity.
- Per-module native dispatch and live-code verification.
- Nested interpreter/native/HLE continuation.
- Runtime-JAL-aware overlays.
- Module-qualified miss logging and profiling.
- Generic diagnostic, capture, and performance surfaces.
- Build-time gating of title-specific input extensions.

See the parent
[README](https://github.com/Tzeentchnet/MetalGearAcid-Recomp/blob/master/README.md),
[FINDINGS](https://github.com/Tzeentchnet/MetalGearAcid-Recomp/blob/master/docs/FINDINGS.md),
[REPRODUCTION](https://github.com/Tzeentchnet/MetalGearAcid-Recomp/blob/master/docs/REPRODUCTION.md),
and
[UPSTREAM](https://github.com/Tzeentchnet/MetalGearAcid-Recomp/blob/master/docs/UPSTREAM.md)
for current evidence.
