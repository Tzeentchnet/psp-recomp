# PSP Recompilation Porting Workflow

This workflow is the proven path for bringing a new PSP title from an exact
disc image to a validated native Windows build. Each phase has an explicit
exit gate. Do not skip a gate by enabling permissive runtime behavior.

## Guiding rules

1. **Pin inputs before analyzing addresses.**
2. **Measure the title's boot contract; do not copy another game's.**
3. **Make mechanisms generic and policies title-specific.**
4. **Keep an interpreter and software renderer as correctness oracles.**
5. **Advance through deterministic checkpoints, not visual impressions.**
6. **Fail explicitly on unsupported behavior.**

## Phase 0 — Create the evidence boundary

Create a source-only title repository before extracting game content.

Record:

- Disc ID, region, version, byte size, and SHA-1.
- Hashes of every executable or module used by the build.
- The pinned `psp-recomp` repository and commit.
- Legal and provenance notes for every redistributed dependency.
- Ignore rules for disc data, decrypted modules, generated C, saves, traces,
  dumps, captures, and build output.

Use [the title template](TITLE_PROJECT_TEMPLATE.md) for the initial repository
and documentation layout.

**Exit gate:** a mismatched source image is rejected before extraction or
guest-memory allocation.

## Phase 1 — Survey the disc and executable topology

Determine the title's actual module model instead of assuming a single EBOOT.

Inventory:

- `EBOOT.BIN`, `BOOT.BIN`, the main PRX, and whether each is plaintext or
  encrypted.
- Every additional PRX, its path, hash, imports, exports, and encryption
  status.
- Asset archives, raw-LBN paths, install data, DLC, and writable content.
- Main module base, entry point, GP, image size, and start-thread parameters.
- Runtime-loaded overlays and whether address ranges are reused.

Useful toolkit commands:

```powershell
python tools\analyze.py <module.elf> --base=<base> --toml=<analysis.toml>
python tools\imports.py <module.elf> <base> --toml=<imports.toml>
python tools\prxload.py <module.elf> <base> --out=<image.bin>
```

Place extracted and generated files under ignored directories.

**Exit gate:** the tracked title configuration describes every module required
for the first deterministic route, including explicit handling for encrypted
or unsupported modules.

## Phase 2 — Capture the PSP boot oracle

Use PPSSPP in interpreter mode to capture the state at the title's real native
entry point.

Measure:

- Argument length and exact argument bytes.
- Initial registers, stack, GP, return address, and module start entry.
- Main thread UID, next UID, priority, stack size, and attributes.
- Initial GE list identifier and any other consumed kernel objects.
- Bootloader memory writes or patches that must be reproduced.

The shared trace format is documented in
[TRACE_FORMAT.md](../tools/TRACE_FORMAT.md).

Do not infer these values from another title. A game may:

- Skip a thin loader safely.
- Require the original loader's state.
- Use `BOOT.BIN` as the main plaintext program.
- Load a separate PRX with title-specific arguments.

**Exit gate:** standalone boot begins with state that matches the PPSSPP
oracle, and any intentionally replaced bootloader work is documented.

## Phase 3 — Establish static translation coverage

Generate a complete inventory before attempting full gameplay:

- Discovered functions and uncovered executable ranges.
- Relocations, imports, exports, switch tables, and jump tables.
- Indirect calls and computed callbacks.
- Scalar FPU and VFPU/COP2 instruction coverage.
- Trapping stubs and unsupported encodings.

Match the reviewed shared identity catalog before assigning library names:

```powershell
python tools\function_catalog.py match <module.elf> `
  --base <base> `
  --output <function-matches.toml> `
  --symbols <function-symbols.csv>
```

Treat normalized matches as review candidates, not proof. See
[safe function reuse](FUNCTION_REUSE.md).

Generate the main module:

```powershell
python tools\codegen.py <module.elf> <output.c> --base=<base>
```

Large titles should split generated output and may use a measured hot-function
report:

```text
--split --hot-functions=<report>
```

For repeat generation, point `PSP_RECOMP_CODEGEN_CACHE` at a private local
directory. The cache accepts only exact generation contexts; it never reuses C
from a normalized catalog match.

Explicit computed entries belong in title configuration and must have evidence.
The generic analyzer and code generator must not contain game addresses.

**Exit gate:** zero unexpected trapping stubs on the target route, with every
manual function entry or jump-table override justified by evidence.

## Phase 4 — Reach deterministic headless boot

Bring up the minimum observed HLE surface:

- PSP memory layout and stack initialization.
- Cooperative thread scheduling and priorities.
- UID allocation and object lifetimes.
- Per-thread GP and callback continuation.
- Filesystem reads needed before the first presentation.
- GE list submission and interrupts.

Derive HLE work from observed missing calls and traces. Do not implement a
large assumed syscall list before the title needs it.

Use strict dispatch. Unsupported instructions, imports, module formats, memory
faults, and exhausted interpreter budgets must terminate with actionable
diagnostics.

**Exit gate:** a bounded call or scheduler trace matches the PPSSPP oracle
through a stable headless milestone.

## Phase 5 — Add dynamic modules and overlays when required

For multi-module titles, implement the complete module lifecycle:

1. Load by path and file descriptor.
2. Allocate PSP guest memory.
3. Copy `PT_LOAD` segments and clear BSS.
4. Apply supported relocations.
5. Patch imports and register exports by library and NID.
6. Set module GP and create the start thread.
7. Register native/interpreted dispatch tables.
8. Unload exports, code identity, and memory.
9. Permit deterministic reuse of overlay address ranges.

Define an explicit title policy for each module:

- Execute as generated native code.
- Execute through the interpreter.
- Replace with a named HLE implementation.
- Reject with a clear reason.

Never replace modules with broad filename patterns. Select policy using
validated path, module identity, and hash.

**Exit gate:** synthetic loader tests pass, and the first real dynamic module
loads, starts, calls HLE/native/interpreted code, and unloads correctly.

## Phase 6 — Build graphics from a software oracle

Establish fixed software-renderer checkpoints before relying on D3D12 or
Vulkan output.

Validate:

- Vertex decoding, morphing, skinning, and patch tessellation.
- Framebuffer aliases, formats, strides, and readback.
- Texture formats, CLUT behavior, wrapping, clamping, and PSP bilinear weights.
- Integer texture-function and fog math.
- Blend, depth, stencil, logic, mask, and dithering behavior.
- Display aspect, scaling, and presentation conversion.

Compare exact frames at deterministic inputs and presentation indices. Analyze
hardware residuals by feature and edge class rather than one global image
score.

**Exit gate:** fixed software checkpoints match, and hardware differences are
either eliminated or classified with bounded acceptance thresholds.

## Phase 7 — Validate media, input, filesystems, and saves

### Audio and video

- Measure guest queueing separately from host device queueing.
- Track underruns, dropped samples, drift, and load-gap recovery.
- Match MPEG ring-buffer progress, AU timing, decoder reordering, and EOF tail
  frames.

### Input

- Begin with deterministic pad scripts.
- Validate gameplay state transitions, not just host input events.
- Keep generic PSP bindings separate from title-specific hidden controls.

### Filesystems and savedata

- Keep `disc0:` immutable.
- Map writable devices to an explicit user-data root.
- Preserve PSP path aliases and reject traversal.
- Test missing, empty, existing, migrated, and newly created saves separately.
- Preserve title archive lookup semantics; do not deduplicate physical records
  without proving equivalence.

**Exit gate:** deterministic front-end and vertical-slice routes include
working audio/video/input/filesystem behavior and explicit save-state tests.

## Phase 8 — Promote overlays and optimize incrementally

Start with interpreter fallback for uncertain code. Promote one module or
overlay at a time:

1. Capture its interpreter baseline.
2. Generate native code with exact module identity.
3. Verify live bytes before native entry.
4. Preserve runtime-JAL targets where the title mutates calls.
5. Compare the same scripted checkpoints.
6. Keep an option to force that module back to the interpreter for diagnosis.

Optimization must not change PSP-visible results. Keep precise floating-point
behavior, deterministic dispatch, and software/hardware comparison paths.

**Exit gate:** the promoted module is checkpoint-identical or has a documented,
bounded difference with a regression test.

## Phase 9 — Complete gameplay and release hygiene

Advance in this order:

1. Scripted front end.
2. Playable vertical slice.
3. Complete stage or mission.
4. Save/load round trip.
5. Additional content routes.
6. Long soak.
7. Full campaign or release acceptance matrix.

Before release:

- Run the complete [validation ladder](VALIDATION.md).
- Audit tracked content for game-derived payloads and credentials.
- Verify a clean recursive clone.
- Build the complete corresponding-source archive.
- Confirm release configurations do not enable permissive diagnostics.
- Pin the exact shared runtime commit in the title repository.

## Stop conditions

Stop and gather evidence instead of adding a workaround when:

- The source image or module hash differs.
- Boot state is guessed rather than measured.
- A missing target is hidden by permissive dispatch.
- A module is replaced without exact policy.
- A generated entry lacks code-identity verification.
- Hardware graphics are being tuned without a software checkpoint.
- A validation route depends on untracked manual input.
- A proposed shared change has no synthetic test or title regression result.
