# Killzone: Liberation Findings

This document summarizes the Killzone: Liberation facts that guided the shared
runtime but remain title-specific. The
[Killzone: Liberation Recomp repository](https://github.com/Tzeentchnet/KillzoneLiberation-Recomp)
is authoritative for current hashes, tests, and release status.

## Supported source

| Property | Value |
|---|---|
| Disc ID | `UCUS-98646` |
| Version | US v1.20 community-distributed image |
| Supported size | `568,498,176` bytes |
| Supported SHA-1 | `5bc4fc8cd8d72c9c2e7c1c03a4325a29bed6e69a` |
| Main module | `KZL.PRX` |
| Main base | `0x08804000` |
| Main entry | `0x08804000` |

The supported image is not byte-identical to the Redump retail USA disc
(`581,632,000` bytes,
`dda1a72c28ccdd8ddd2f3cbfd2fa3188962b0458`). The parent project rejects
unrecognized content before generation or boot.

Authoritative configuration:
[config/kzl.toml](https://github.com/Tzeentchnet/KillzoneLiberation-Recomp/blob/main/config/kzl.toml).

## Executable and boot model

- `KZL.PRX` is the large plaintext title module used for recompilation.
- The original bootloader is thin enough for this supported build to skip, but
  its resulting state must be reproduced.
- Standalone boot calls the module entry with the exact `"modulename"`
  argument and measured PSP-compatible thread/kernel state.
- This loader bypass is a Killzone result, not a generic PSP rule.
- The module is stripped C++ code with Havok, Lua, libgu, SN runtime, and
  title-specific jump-table recovery requirements.
- The final generated inventory contains tens of thousands of functions, so
  split translation units and measured hot/cold optimization are required.

Function-reuse validation generated 42,600 functions with no trapping stubs.
The first cache pass wrote 42,298 ordinary-function entries; the unchanged
second pass hit all 42,298 and preserved every generated file timestamp. A
temporary catalog built from the 37 existing F-096 PPSSPP identities matched
all 37 exactly.

Related evidence:
[FINDINGS F-004 and F-010 through F-019](https://github.com/Tzeentchnet/KillzoneLiberation-Recomp/blob/main/docs/FINDINGS.md).

## Title content model

### `PSP.DAT`

Most title content is stored in the proprietary `PSP.DAT` archive. Its reader
can encounter duplicate logical paths backed by different physical records.
The game chooses among them using stream/cursor locality; deduplicating the
records changes behavior.

### Chapter 5

Chapter 5 content is external downloadable content rather than ordinary
on-disc content. The recompilation keeps it user-supplied through the mapped
memory-stick tree and never redistributes it.

### Retired online services

Infrastructure services are no longer available. The title integration keeps
the Downloads route in-menu with an unavailable-service result and disables
the retired `KILLZONE.COM` action. Ad-hoc multiplayer remains a separate,
unsupported feature.

Related evidence:
[FINDINGS F-020 through F-022](https://github.com/Tzeentchnet/KillzoneLiberation-Recomp/blob/main/docs/FINDINGS.md).

## Filesystem and saves

- `disc0:` reads from the verified ISO.
- `ms0:`, `fatms0:`, and the title's `cache:` alias share an explicit
  user-data tree.
- Writable storage defaults outside the repository and supports an override
  for isolated tests.
- Path traversal is rejected.
- Secure savedata is PSP/PPSSPP compatible; legacy plaintext saves require an
  explicit migration path.
- Chapter 5 remains in the user's memory-stick content.

These aliases and content paths are Killzone policy built on generic PSP
filesystem primitives.

Related evidence:
[FINDINGS F-021 and F-057](https://github.com/Tzeentchnet/KillzoneLiberation-Recomp/blob/main/docs/FINDINGS.md).

## Graphics and GE behavior

Killzone drove the initial native D3D12 renderer and exposed:

- Heavy framebuffer feedback and render-to-texture behavior.
- PSP color/depth formats and destination-fragment paths.
- Morphing, skinning, and patch geometry.
- GE signal/finish callback ordering.
- Internal-resolution scaling with PSP-logical framebuffer identity.
- Performance sensitivity to fragment rate and framebuffer readback.

D3D12 at 2x is the validated primary route. Higher scales are available but
remain title-validation work. The software path remains the pixel oracle and
Vulkan remains an alternate/reference backend.

Killzone-specific checkpoint frames and draw behavior must stay in the parent
project, not in shared renderer code.

## Audio and video

- Native audio uses XAudio2 with guest completion kept separate from host
  queue lead.
- SAS voices and VAG loop markers must honor the title's requested loop mode;
  blindly honoring embedded loop markers caused accumulating menu sounds.
- The intro and credits use PSMF/MPEG with H.264 and ATRAC audio. Windows uses
  Media Foundation; macOS uses hardware-accelerated VideoToolbox. The macOS
  intro was tested on Apple M5; the credits have not yet been validated there.
- Decoder EOF includes reordered tail frames after ring-buffer exhaustion.
- Incorrect AU delays and scaled framebuffer upload behavior caused visible
  movie stutter.

The generic lesson is documented as
[PSP-F-022 through PSP-F-024](../SHARED_FINDINGS.md#psp-f-022--guest-timing-and-host-device-timing-are-separate).

## Input and twin-stick extension

The generic runtime exposes ordinary PSP buttons, analog input, portable
configuration, native remapping, and hot reload.

Killzone additionally has an opt-in experimental twin-stick extension:

- The movement stick synthesizes the title's D-pad movement profile.
- The aim stick uses a native side channel into the on-foot facing path.
- A hidden movement profile is installed only while a controller is connected.
- Title-specific labels and reversal options appear in the controller dialog.

This extension is compiled out for other titles with
`SR_ENABLE_KZL_TWIN_STICK=0`. The side channel and game-memory integration
must never become generic PSP input behavior.

Research:
[twinstickresearch.md](https://github.com/Tzeentchnet/KillzoneLiberation-Recomp/blob/main/docs/twinstickresearch.md).

## Validated routes

Public validation covers:

- Extraction/configuration tools and content auditing.
- Shared codegen/trace/image comparison tools.
- Runtime-only clang-cl compilation.

Protected validation adds:

- Bounded PPSSPP scheduler traces.
- Deterministic pad scripts and frame manifests.
- Native/software renderer comparisons.
- Intro movie and audio routes.
- Boot, menus, Chapter 1 presentation, tutorial, and Mission 1.

Full chapters, challenges, user-supplied Chapter 5, and long soaks remain
separate release gates in the parent project.

## Do not generalize

- The supported ISO/hash or `KZL.PRX` addresses.
- The `"modulename"` boot argument as a PSP-wide default.
- `PSP.DAT` lookup policy.
- Chapter 5 paths.
- Retired-online menu patches.
- Killzone twin-stick memory hooks.
- Title frame manifests, performance numbers, or control labels.

## Reusable lessons contributed

Killzone established the shared foundation for:

- Standalone module boot with explicit content verification.
- Split static code generation and immutable dispatch tables.
- PSP scheduler/HLE bring-up.
- D3D12 rendering and deterministic frame capture.
- XAudio2, ATRAC, MPEG/PSMF, SAS, secure savedata, and native dialogs.
- Portable input/graphics configuration and diagnostic instrumentation.

See the parent
[README](https://github.com/Tzeentchnet/KillzoneLiberation-Recomp/blob/main/README.md),
[FINDINGS](https://github.com/Tzeentchnet/KillzoneLiberation-Recomp/blob/main/docs/FINDINGS.md),
and
[REPOSITORY TODO](https://github.com/Tzeentchnet/KillzoneLiberation-Recomp/blob/main/docs/TODO.md)
for current status.
