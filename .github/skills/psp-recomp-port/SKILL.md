---
name: psp-recomp-port
description: "Use when: starting, planning, implementing, debugging, or validating a PSP-to-PC static recompilation; surveying PSP ISO/PRX files; adding HLE, dynamic modules, overlays, graphics, audio/video, input, filesystem, or savedata support; or deciding whether behavior belongs in shared psp-recomp versus a title project. Applies the proven Killzone Liberation and Metal Gear Acid workflow."
argument-hint: "<title or porting task>"
---

# PSP Recompilation Porting Workflow

Use this skill to avoid rediscovering PSP runtime requirements or copying
title-specific behavior into the shared toolkit.

## Required reading

Choose the smallest relevant path:

- **New title:** `docs/README.md` → `docs/PORTING_WORKFLOW.md` →
  `docs/TITLE_PROJECT_TEMPLATE.md` → `docs/VALIDATION.md`
- **Shared runtime/codegen change:** `docs/SHARED_FINDINGS.md` →
  `docs/VALIDATION.md` → affected files/tests
- **Function recognition or reuse:** `docs/FUNCTION_REUSE.md` →
  `docs/VALIDATION.md`
- **Existing title regression:** `docs/titles/README.md` → matching title
  summary → that title repository's authoritative config/findings
- **Ownership decision:** the boundary table in
  `docs/SHARED_FINDINGS.md#shared-runtime-versus-title-project`

For a new project, copy and complete
[the title intake](./assets/title-intake.md) before implementation.

## Procedure

### 1. Pin the evidence boundary

Record:

- Disc ID, region, version, size, and SHA-1.
- Main and dynamic module hashes.
- Exact `psp-recomp` and PPSSPP revisions.
- Toolchain and renderer configuration.
- Which artifacts must remain ignored.

Stop if the input is not identified exactly.

### 2. Classify the title architecture

Determine from evidence:

- Relationship among `EBOOT.BIN`, `BOOT.BIN`, and the main PRX.
- Plaintext and encrypted module inventory.
- Main module base, entry, GP, and image bounds.
- Runtime PRXs, overlays, reused address ranges, and raw-LBN paths.
- Asset archives, DLC, install data, and writable devices.

Do not assume the Killzone loader model or the Metal Gear Acid overlay model.

### 3. Capture the real boot contract

Use a PPSSPP interpreter trace to measure:

- Argument bytes and length.
- Registers, stack, GP, and return state.
- Thread UID, next UID, priority, stack size, and attributes.
- Initial GE list ID and bootloader-created objects.
- Required memory writes or patches.

Put measured title values in title configuration, never shared source.

### 4. Plan work by validation gate

Create tasks that end in measurable outcomes:

1. Exact extraction and module inventory.
2. Zero unexpected codegen traps.
3. One-function trace match.
4. Bounded boot/scheduler trace match.
5. Deterministic front-end checkpoint.
6. Software-renderer frame match.
7. Media/input/save route.
8. Playable vertical slice.
9. Stage/mission route.
10. Soak/campaign/release matrix.

Do not describe “boots” or “works” without the checkpoint that proves it.

### 5. Preserve the shared/title boundary

Implement in `psp-recomp` only when the change is:

- A reusable parser, allocator, scheduler, dispatcher, HLE primitive, renderer
  rule, media/input/filesystem API, or diagnostic.
- Free of title addresses, hashes, paths, menu states, and content policy.
- Covered by a synthetic regression.

Normalized function-catalog matches may transfer reviewed identity only.
Generated C may be reused only by the exact local cache described in
`docs/FUNCTION_REUSE.md`.
Honor `PSP_RECOMP_FUNCTION_CATALOG` and `PSP_RECOMP_CODEGEN_CACHE` when they
are configured. Never copy either private store into a title repository or
promote a local catalog entry without reviewing its source revision and
license.

Keep in the title repository:

- Disc and module identity.
- Base/entry/thread values.
- Explicit function entries and module policy.
- Game patches and asset/archive logic.
- Hidden controls and native side channels.
- Scripted routes, frame manifests, and acceptance thresholds.

### 6. Fail explicitly

Do not complete work by:

- Enabling permissive dispatch.
- Returning fake success from unsupported HLE.
- Replacing modules by broad filename patterns.
- Ignoring relocation, import, memory, or interpreter failures.
- Caching native overlay verification across later guest mutations.
- Accepting a hardware frame without a software reference.

Use diagnostics to isolate a problem, then implement or explicitly reject it.

### 7. Validate in layers

Run the smallest focused test first, then the complete affected ladder in
`docs/VALIDATION.md`.

Shared changes normally require:

- A new or updated synthetic test.
- Shared Python tests and `make test-runtime`.
- Runtime-only/parent builds for affected titles.
- Deterministic protected smoke routes when available.
- Updated title pins and documentation.

### 8. Record findings while working

Use stable identifiers and the format in
`docs/TITLE_PROJECT_TEMPLATE.md#findings-format`.

Every finding must separate:

- Inputs.
- Observation.
- Evidence.
- Interpretation.
- Shared/title/diagnostic impact.
- Regression requirement.

### 9. Finish the integration

Before reporting completion:

- Run `git diff --check`.
- Confirm no protected artifacts are tracked.
- State exactly which validation levels passed.
- State which protected or full-game routes were not run.
- Update the shared knowledge base if a cross-title rule changed.
- Update consumer runtime pins only after the shared commit is available.

## Stop and investigate

Pause rather than add a workaround when:

- Input hashes differ.
- Boot state is guessed.
- The first divergence is unknown.
- Module identity is ambiguous.
- An overlay changed after verification.
- A renderer delta has no feature classification.
- A route depends on manual timing.
- A “generic” change has only one-title evidence.

Return the blocking evidence and the next bounded experiment.
