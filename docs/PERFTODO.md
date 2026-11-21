# Shared Performance Implementation TODO

This is the implementation roadmap for improving base `psp-recomp`
performance, with emphasis on moving reusable graphics work from the host CPU
to D3D12 or Vulkan without weakening PSP correctness.

The shared runtime owns implementation, capability detection, diagnostics,
synthetic tests, and fallback behavior. Title repositories own deterministic
routes, protected captures, and title-specific performance evidence. No game
addresses, content paths, draw signatures, or special cases belong in this
plan's shared code.

Priority: **P0** establishes trustworthy measurement or blocks later work;
**P1** is a planned shared optimization; **P2** is conditional on profiling.

## Completion rules

A task is complete only when:

- the software renderer remains the authoritative PSP behavior oracle;
- unsupported devices and draws take an explicit tested CPU fallback;
- public synthetic tests and shader checks pass;
- D3D12 and Vulkan validation report no new warnings or errors;
- the established Killzone: Liberation and Metal Gear Acid title gates pass;
- repeated A/B measurements improve more than observed run-to-run variance;
- p95 frame time, startup, transfers, and thermal state do not regress
  materially; and
- only reduced metrics, hashes, thresholds, and reproduction instructions are
  committed.

An optimization is not successful when it only moves time from a measured CPU
category into submission, synchronization, or GPU time without improving the
complete frame.

## Phase 0 - Reconcile and measure

- [ ] **PERF-0.1 P0** Reconcile the standalone shared-runtime revision with the
  newer runtime pinned by Killzone before performance implementation begins.
  At plan creation, the standalone checkout is at `030bdcf` and Killzone pins
  descendant `613cd1e`. Preserve the title-independent Vulkan instrumentation,
  pipeline-cache work, transfer accounting, and GPU framebuffer upload
  expansion while keeping title policy in the parent repository.
  - [x] Advance the standalone checkout to `613cd1e`.
  - [x] Default the Killzone twin-stick extension off in shared builds and move
    its enablement and ISO-picker title into the Killzone parent build.
  - [x] Repair standalone Apple arm64 unit-test linkage for the reconciled
    native fiber implementation.
  - [x] Pass `make test-runtime`, the Vulkan shader check, and an isolated
    macOS runtime-only build using the standalone checkout.
  - [ ] Re-run the protected Killzone: Liberation and Metal Gear Acid title
    gates before closing this task.
- [ ] **PERF-0.2 P0** Standardize the current performance CSV schema in base
  `psp-recomp` across D3D12 and Vulkan. It must report host frame, frontend,
  backend and fenced-GPU time; native, HLE, interpreter, scheduler, GE,
  primitive, vertex, decode, capture and synchronization categories; workload
  counters; framebuffer snapshots; texture-cache activity; pipeline creation;
  host upload/readback bytes; and thermal state where available.
  - [x] Preserve every required common CSV category, counter, backend stage,
    GPU source flip, and thermal field while advancing the schema through v7
    for framebuffer-upload and GPU T&L metrics.
  - [x] Populate D3D12 pipeline-key hits, pipeline creation count/time, texture
    cache/decode/scale counters, and host upload/readback byte totals.
  - [x] Include mapped Vulkan vertex traffic in the shared host-upload total.
  - [x] Pass the common performance-statistics test, Vulkan shader/layout test,
    shader check, and macOS runtime-only build.
  - [x] Cross-compile the new D3D12 providers against MinGW D3D12 headers.
  - [ ] Run and validate those providers on a native Windows D3D12 runtime; the
    local Mac still has no Windows SDK runtime or debug layer.
  - [ ] Validate nonzero per-frame deltas on the protected D3D12 and Vulkan
    title routes before closing this task.
- [ ] **PERF-0.3 P0** Add stable A/B path identifiers for every optimization
  introduced by this plan. A path must switch only at presentation boundaries,
  mark transition frames as unusable, and support a forced CPU fallback for
  correctness comparison.
  - [x] Verify presentation-boundary switching and bit-31 transition marking
    in the existing common A/B framework.
  - [x] Append the stable `gpuupload` identifier without renumbering existing
    paths and cover named and unnamed-path behavior in the public test.
  - [x] Append the stable `gputnl` identifier and cover presentation-boundary
    fallback alternation without renumbering existing paths.
  - [x] Append the stable `gputexdecode` identifier so opt-in compute decode
    can alternate directly against the asynchronous CPU snapshot baseline.
  - [ ] Append identifiers for later plan optimizations as they are
    implemented.
- [ ] **PERF-0.4 P0** Record repeated warm-cache baselines for both supported
  title integrations. Use the same input fixture and presentation window at
  1x, 2x and 4x, with optional quality features disabled unless they are the
  subject of the test. Record median, p95 and p99 frame time; category totals;
  GPU time; workload counts; transfer bytes; validation status; and thermal
  state.
- [ ] **PERF-0.5 P0** Define action thresholds from the baseline variance before
  selecting an implementation. Report CPU time per transformed vertex, per
  decoded texture byte, per submitted primitive and per framebuffer upload.
  Do not use average FPS alone to choose an offload.

### Phase 0 exit gate

Both title routes produce repeatable structured profiles, every measured
interval is associated with its source presentation, and the next
optimization is selected from an observed category rather than a synthetic
proxy.

## Phase 1 - Promote GPU framebuffer upload expansion

- [ ] **PERF-1.1 P0** Move the title-independent Vulkan logical-framebuffer
  upload expansion from the newer Killzone runtime pin into the shared
  baseline. Upload the 512x512 logical RGBA image once, then use a nearest GPU
  blit to populate the scaled target.
  - [x] Reconcile the implementation into the standalone shared runtime.
  - [ ] Pass protected title-route validation before closing the task.
- [ ] **PERF-1.2 P0** Keep capability-gated CPU expansion as the explicit
  fallback. Validate required format, transfer-source, transfer-destination
  and nearest-blit support before enabling the GPU path.
  - [x] Keep initialization-time format, feature and nearest-blit checks
    authoritative.
  - [x] Preserve `SR_VULKAN_GPU_UPLOAD_SCALE=0` and make A/B alternation able
    only to force, never bypass, the CPU fallback.
  - [ ] Confirm both capability paths on protected title routes.
- [ ] **PERF-1.3 P1** Add an output-identical `gpuupload` A/B path and counters
  for logical bytes, host-transferred bytes, expanded physical pixels, upload
  count, fallback count and synchronization time.
  - [x] Select the GPU or CPU expansion once per upload from capability state
    and the presentation-boundary A/B mask.
  - [x] Publish cumulative Vulkan counters as per-presentation CSV deltas
    (introduced in v6 and preserved in v7); unsupported backends leave the
    common columns zero.
  - [x] On the available Killzone route, compare four full-resolution 2x movie
    captures byte for byte and verify that 180 framebuffer uploads transfer
    188,743,680 bytes through the GPU path versus 754,974,720 bytes through
    CPU expansion.
  - [ ] Repeat the capture and byte-delta gate on Metal Gear Acid.
- [ ] **PERF-1.4 P1** Add focused public tests for scale calculations, transfer
  extents, overflow rejection, byte accounting and capability fallback. Run
  Vulkan validation plus native-resolution and scaled title captures.
  - [x] Add a shared pure upload planner and cover 1x, scaled GPU, forced CPU,
    missing-capability and overflow cases.
  - [x] Pass the public performance-statistics test, Vulkan layout/state test,
    shader check and isolated macOS runtime compilation.
  - [x] Run Vulkan validation and native/scaled captures on the available
    Killzone route.
  - [ ] Repeat protected native/scaled captures on Metal Gear Acid.

### Phase 1 exit gate

At scale `N`, a framebuffer reacquisition transfers one logical image instead
of `N * N` expanded pixels, output remains within the existing title capture
thresholds, and unsupported devices retain the tested CPU path.

## Phase 2 - GPU transform and lighting

The first T&L implementation keeps packed PSP vertex decoding, morphing and
initially skinning on the CPU. It moves reusable per-vertex matrices, lighting,
texture generation, fog and viewport work to shaders.

- [x] **PERF-2.1 P0** Define a backend-neutral canonical model-vertex ABI and an
  immutable transform-state snapshot. Include position, normal, texture
  coordinates, color, required flags, world/view/projection and texture
  matrices, lights, material, fog, viewport, offsets and depth-clip state.
  - [x] Add versioned canonical vertex, primitive, draw, light and copied-state
    structures to the shared backend contract. Morphing and skinning remain
    CPU-decoded inputs, and canonical color byte order is explicit.
  - [x] Add a pure snapshot builder that owns no pointers, decodes viewport
    float24 and offsets, and copies every matrix, light, material, texture,
    fog, culling and depth-clip input.
  - [x] Cover flags, layout, state copying and post-capture immutability in a
    public test, then pass C and C++ compilation, the full runtime suite and
    isolated macOS Vulkan runtime compilation.
- [x] **PERF-2.2 P0** Extend `GeBackend` with an optional transform-draw hook
  before CPU screen-space transformation. The hook receives a deduplicated
  canonical vertex table, submission-order indices, primitive type and state;
  returning decline must execute the existing CPU path unchanged.
  - [x] Decode packed source vertices once into the canonical 36-byte ABI,
    deduplicate indexed submissions, and assemble triangle-list indices while
    preserving strip parity and PSP cull ordering.
  - [x] Gate canonical construction behind `SR_GPU_TNL` and stable `gputnl`
    alternation so the default CPU path incurs no decode or classification
    overhead.
  - [x] Preserve the existing CPU transform/raster path on allocation,
    classifier, capability, or backend decline.
- [x] **PERF-2.3 P0** Implement a pure eligibility classifier with focused
  tests. The initial path accepts only transform-mode triangle lists, strips
  and fans whose clipping, provoking-vertex, culling and flat-shading behavior
  is representable. Through mode, rectangles, points, lines, patches and
  uncertain clip states remain on the CPU.
  - [x] Reject invalid ABI/count/index data, clears, flat shading, unsupported
    primitives, nonfinite clip values, nonpositive clip `w`, near crossings,
    and uncertain PSP screen/depth acceptance.
  - [x] Validate each deduplicated canonical vertex once and cover accepted
    lists/strips/fans plus every rejection class in the public test.
- [ ] **PERF-2.4 P1** Implement D3D12 GPU T&L using the canonical stream and
  existing indexed draw path. Avoid geometry shaders and keep feature-level
  requirements compatible with the current renderer floor.
  - [x] Capture and upload canonical vertices, 16-bit indices, and packed
    transform state through the existing fenced upload allocator.
  - [x] Extend the root signature with a vertex-state SRV and switch the
    existing raw-SRV vertex shader between screen-space and canonical streams
    without changing the current fragment shaders or pipeline floor.
  - [x] Cross-compile the changed host objects and mixed screen-space/GPU-T&L
    capture test against MinGW D3D12 headers.
  - [x] Compile every D3D12 HLSL entry with a native source-built Microsoft DXC,
    generate the changed vertex include with the canonical script, and verify
    that its checked-in bytes exactly match the validated DXIL container.
  - [ ] **Deferred:** Run the test binary and renderer under a native Windows
    D3D12 runtime with the Microsoft DXIL validator and debug layer.
- [x] **PERF-2.5 P1** Implement the same ABI and eligibility rules in Vulkan
  using core Vulkan functionality suitable for native drivers, MoltenVK and
  KosmicKrisp.
  - [x] Add dedicated canonical vertex, 32-bit index, and packed-state buffers;
    a static storage-buffer descriptor; a model-space pipeline key; and
    indexed batch submission while preserving ordinary screen-space batches.
  - [x] Revalidate backend indices and overflow-safe capacities before capture,
    bind valid descriptor/buffer state across mixed batches, and account for
    all canonical upload bytes.
  - [x] Pass SPIR-V validation, exact packed-state offset inspection, generated
    shader checks, Vulkan ABI tests, and isolated backend syntax compilation.
  - [x] Execute the real validation vertex shader through a headless indexed
    draw and compare storage-buffer readback with the packed CPU oracle under
    the Khronos validation layer on Apple M5 / MoltenVK 1.4.2.
  - [x] Exercise the opt-in path under Vulkan validation on MoltenVK and
    KosmicKrisp. Real Apple M5 / KosmicKrisp readback passed direct T&L,
    point/sprite expansion, clipping preprocess, and texture-decode shaders.
    Explicit integer RGBA8 fetch and shader normalization made the protected
    Killzone CPU/GPU route byte-identical on all 50 frames within that driver.
- [ ] **PERF-2.6 P0** Add a synthetic differential harness that compares CPU
  and GPU transform output for matrices, normals, lighting modes, texture
  generation, fog, viewport signs, depth ranges and indexed vertex reuse.
  Read back only in validation mode.
  - [x] Add independent raw-state CPU-oracle and packed-state
    shader-equivalent evaluators.
  - [x] Cover matrix packing, transformed/normalized normals, all texture
    generation sources, environment mapping, material-update bits,
    directional and attenuated point lights, powered diffuse, Blinn specular,
    finite/NaN/infinite fog, viewport signs, depth clamp, 28.4 snapping, and
    reused canonical indices.
  - [x] Add opt-in `SR_GPU_TNL_REFERENCE` timing for accepted canonical
    vertices without affecting normal runs.
  - [x] Add `SR_GPU_TNL_VALIDATE` backend readback so the packed reference is
    compared with screen position, reciprocal W, UV, fog, and color written by
    separate validation variants of each real vertex shader. Normal shaders
    retain their existing feature requirements and issue no validation writes.
  - [x] Pass the real Vulkan shader readback test with synchronization
    validation and no warning/error messages on MoltenVK.
  - [ ] **Deferred:** Run the D3D12 validation variant and readback comparison
    on native Windows with the Microsoft debug layer.
- [x] **PERF-2.7 P0** Add a `gputnl` A/B path and counters for eligible,
  accepted and declined draws; canonical and submitted vertices; CPU decode
  time; CPU transform time removed; upload bytes; backend time; and GPU time.
  - [x] Advance the common performance record to CSV v7 with eligible,
    accepted, backend-declined, canonical/submitted vertex, decode,
    CPU-reference, and upload-byte fields while retaining common backend/GPU
    timing.
  - [x] Advance the record to CSV v8 with GPU texture-decode job, fallback and
    raw-source byte counters.
  - [x] Advance the record to CSV v9 with class-specific geometry workload and
    exclusive timing fields.
  - [x] Cover all counters and `gputnl` fallback alternation in the public
    performance test and verify the CSV header/row column counts.
- [ ] **PERF-2.8 P1** Validate protected title routes and classify every
  changed pixel by draw feature and edge proximity. Do not widen a global
  image tolerance to hide an unclassified transform difference.
  - [x] Keep `SR_GPU_TNL` opt-in and retain exact CPU fallback behavior until
    this gate closes.
  - [x] Run the deterministic Killzone: Liberation Vulkan route with generated
    title code, the protected image/save, and the presentation-timed pad and
    50-frame fixtures.
    - [x] Reproduce the reported startup failure as a null-descriptor crash in
      MoltenVK: an empty-scissor draw bypassed the ordinary no-op append path
      and queued incomplete texture/palette descriptor state. Drop that batch
      as handled and reject any remaining incomplete batch before recording.
    - [x] Fix the live validation mismatch caused by declaring the canonical
      two-float UV field as `R32_SFLOAT`. Production and validation now share
      one authoritative `R32G32_SFLOAT` vertex-input layout.
    - [x] Add bit-exact and delayed live validation. Canonical color now enters
      as integer RGBA8 and is converted explicitly, while ordered `precise`
      Vulkan transform/viewport/UV arithmetic mirrors the CPU build's
      `-ffp-contract=off` boundaries.
    - [x] Pass bit-exact live validation for 2,292 boot-route indexed vertices
      and tolerant validation for the late gameplay route. Remaining
      reciprocal-W/Z ULP differences were raster-inert after exact X/Y, UV
      expression and color behavior were restored.
  - [x] Compare the complete Killzone CPU/GPU capture manifests and record
    repeated timing without widening an image threshold.
    - [x] All 50 boot, menu, cutscene, gameplay and HUD pairs are byte-identical.
      The ordered capture-byte SHA-256 is
      `6b965a7e1fb1051a50ef5acb0923ca6d70208e6fb6a6a4c1fba5ed162b73821c`.
    - [x] Reject the earlier non-exact 3.483 ms mean improvement as
      non-promotable evidence. Two corrected direct-shader runs measured
      17.574 and 17.569 ms mean versus 17.577 ms CPU, with nearest-rank p95
      33.330 and 33.098 ms versus 29.466 ms CPU.
    - [x] Validate an opt-in transform-once compute variant with the same 50
      exact frames. It reduced backend/GPU work but measured 17.579 ms mean,
      16.599 ms median, 29.024 ms p95, 34.684 ms p99 and 87.67 s wall time,
      versus 17.577/16.691/29.466/34.697 ms and 87.78 s for CPU. Retain it only
      as a diagnostic because the mean and wall-time deltas are within noise.
    - [x] Repeat the protected CPU/GPU route on Apple M5 / KosmicKrisp
      1.4.363.1 after replacing implementation-defined screen-vertex UNORM8
      conversion with integer RGBA8 fetch plus explicit normalization. All
      50 pairs were byte-identical with ordered SHA-256
      `6bda9f5158b692cc20bea45898ee7d4e6d85d08d59b167769f5b9e41f18195fd`.
      Cross-driver capture hashes are not compared because even the CPU path
      can rasterize differently between MoltenVK and KosmicKrisp.
  - [ ] Run the same deterministic correctness and timing gate on Metal Gear
    Acid; the title and protected route remain unavailable.

### Phase 2 exit gate

Eligible draws use the GPU by default only when transform and frame validation
pass on both titles and the reduction in CPU vertex/capture time exceeds added
upload, submission, synchronization and GPU time beyond run variance.

## Phase 3 - Expand T&L eligibility

- [x] **PERF-3.1 P1** Add CPU-decoded skinned vertices to the canonical GPU T&L
  path, preserving the existing CPU morphing and bone-weight oracle.
  - [x] Reuse the software renderer's packed vertex decoder so morph targets,
    fixed-point or float weights, reverse normals, and bone transforms are
    resolved before the backend receives the canonical 36-byte vertex.
  - [x] Keep single-weight and multi-weight draws eligible and propagate
    explicit skinned and morphed flags without adding weights or bones to the
    canonical ABI.
  - [x] Cover u8, u16, and float weights; single-weight count encoding;
    position and translation-free normal skinning; morph-before-skin ordering;
    morphed UV/color data; backend acceptance; and exact CPU fallback after a
    backend decline in the public runtime test.
- [x] **PERF-3.2 P2** Evaluate GPU skinning only when profiles show skinning
  decode as a material cost. Add bone matrices and normalized weights to the
  ABI without changing ordinary draws.
  - [x] **Evidence gate:** The existing categorized 4 kHz sampler separated
    skinning without adding a clock read to every decoded vertex. In the
    Killzone gameplay window at presentations 4440-4679, 3,753
    `vertex_decode` samples estimate 938.250 ms versus 942.376 ms in CSV v10.
    Counting every self sample in `decode_model_vtx` (36) plus every
    `read_weight_raw` sample (10) as skinning gives a deliberately conservative
    upper bound of 11.500 ms across 240 presentations: 0.0479 ms per
    presentation, 1.226% of vertex-decode time and 0.295% of complete-frame
    time. Isolating the weight and bone-transform instruction range reduces
    that estimate to 23 samples, 5.750 ms total and 0.0240 ms per
    presentation.
  - [x] Do not add per-vertex timers, advance the CSV schema, or extend the
    canonical ABI for this result. Even perfect removal of the conservative
    upper bound is below measured end-to-end variance before accounting for
    weight and bone-matrix upload, synchronization, or GPU work. Retain
    CPU-decoded skinned canonical vertices and reopen this task only when a
    later title/host profile exceeds the action threshold.
- [x] **PERF-3.3 P2** Add shader-side PSP acceptance and clipping support for
  additional transform draws. Use compute preprocessing when exact
  per-primitive rejection cannot be represented safely by the graphics
  pipeline; do not require geometry shaders.
  - [x] Define shared 48-byte clip-space and 32-byte screen-space ABIs plus a
    pure reference preprocessor for PSP screen-grid rejection, depth-side
    rejection, near-plane clipping, generated-vertex range checks, color/UV/
    fog interpolation, and fixed-point screen snapping.
  - [x] Add Vulkan compute passes that transform each deduplicated canonical
    vertex once and expand each submitted triangle into fixed zero-, one-, or
    two-triangle screen slots. Degenerate unused slots avoid atomics, indirect
    draws, geometry shaders, and primitive-order changes.
  - [x] Route finite draws that need preprocessing through the Vulkan compute
    path while preserving backend decline and the complete CPU fallback on
    D3D12 and unsupported inputs.
  - [x] Execute both real compute shaders under Khronos validation on Apple M5
    / MoltenVK and compare clip and screen-buffer readback with the CPU oracle,
    including a near-clipped quad and a whole-triangle screen-grid rejection.
  - [x] Implement equivalent D3D12 Shader Model 6 compute passes using the
    shared clip/screen ABIs and six fixed slots per submitted triangle.
    Source-built Microsoft DXC produced deterministic checked-in DXIL, and
    the capture/oracle test plus `device.cpp` cross-compiled with MinGW
    `-Werror`. Invalid input, allocation/capacity failure, and unsupported
    classes still decline to the complete CPU path.
  - [x] Validate the real Vulkan path on native KosmicKrisp and the protected
    Killzone route. Exact live shader validation passed 85,590 gameplay
    vertices, and the corrected CPU/GPU capture pair was exact on all 50
    frames. Three nominal-thermal `gputnl` A/B repetitions measured
    GPU-minus-CPU complete-frame mean deltas of -0.718, -1.429 and -1.319 ms;
    the GPU half processed about 0.84% more GE draws/vertices. The first run
    also contained a one-time backend spike that worsened p99, so the path
    remains opt-in pending another title and later-driver repetition.
  - [ ] **Deferred hardware gate:** Execute the D3D12 compute/readback path
    under a native Windows D3D12 runtime with the DXIL validator and debug
    layer.
- [x] **PERF-3.4 P2** Evaluate vertex-shader expansion for points, lines and
  sprites using vertex IDs and compact control vertices.
  - [x] Add an independent safe-point eligibility rule and compact point
    control stream. Vulkan expands each accepted control to two one-pixel
    triangles from `gl_VertexIndex`; unsafe points, validation mode, D3D12,
    and unsupported backends retain the complete CPU path.
  - [x] Cover unindexed and indexed/deduplicated point controls, backend
    acceptance and decline, unsafe-point fallback, CPU corner expansion, and
    real Vulkan validation-shader readback for all six generated corners.
  - [x] Evaluate compact line-pair expansion against the existing PSP DDA
    pixel set and exact line clipping. Vertex-only quads cannot reproduce the
    discrete `i / steps` coverage, integer coordinate truncation, per-step
    interpolation, or repeated-write behavior. Keep transform-mode lines on
    the CPU path rather than promoting the current approximate backend quad;
    reconsider fragment-level DDA reconstruction only under the PERF-5.2
    profile gate.
  - [x] Add compact two-control sprite expansion with exact floor,
    second-vertex depth/color/fog, unit reciprocal W, and
    nearest-versus-linear half-texel sampling behavior. Odd unpaired controls,
    degenerate rectangles, backend decline, unsafe controls, validation mode,
    and D3D12 retain the established CPU behavior or fallback.
  - [x] Compare nearest and linear six-corner sprite output from the real
    Vulkan validation shader with the packed CPU reference.
  - [x] Record protected-title prevalence before considering default
    enablement. Three complete Killzone routes contained zero point draws and
    zero line draws, so point-specific or line-specific timing cannot be
    claimed from this title. They contained 76,851-76,977 sprites, including
    16,080 sprites in presentations 4440-4679. The exact sprite prototype
    reduced uploads but repeated complete-frame timing remained within
    variance, so no primitive class is promoted.
- [x] **PERF-3.5 P2** Consider packed PSP vertex decoding on the GPU only when
  `vertex_decode_ms` remains significant after T&L offload. Compare it against
  a specialized CPU decoder before adding raw-format shader variants.
  - [x] Separate useful packed-decode work from timing overhead with the 4 kHz
    categorized sampler. Of 3,753 `vertex_decode` samples, 3,052 were in
    `libsystem_kernel`; a conservative non-transform packed-decode upper bound
    was about 425 samples, or 0.443 ms per gameplay presentation.
  - [x] Profile 10,000 gameplay transform draws. Ten formats submitted
    2,102,066 vertices; the proposed fixed/float specialized families covered
    9,214 draws (92.140%) and 1,686,782 vertices (80.244%).
  - [x] Prototype the specialized CPU decoder and compare every eligible live
    vertex with the generic decoder. All 25,987,199 compared vertices were
    byte-identical. Three nominal-thermal A/B runs reduced normalized decode
    cost by only 0.8%, 1.1% and 7.0% (the last run was an outlier);
    complete-frame mean deltas were -0.016, -0.080 and -0.470 ms while p95
    regressed by 0.091, 0.124 and 1.349 ms.
  - [x] Reject GPU packed decoding and remove the specialized CPU prototype.
    Its small, non-repeatable decode delta does not justify shader variants,
    raw-format upload, synchronization, or permanent default-disabled code.

### Phase 3 exit gate

Phase 3 is complete on the available hardware. Each added draw class has an
independent eligibility rule, differential test, A/B measurement and CPU
fallback. No additional class is promoted: KosmicKrisp GPU T&L is promising
but lacks the second-title and later-driver gates, points/lines have no
Killzone workload, sprite timing stayed within variance, and packed vertex
decode did not justify implementation complexity. Native Windows D3D12
execution and the unavailable Metal Gear Acid route remain external gates.

## Phase 4 - Texture decode and scaling

- [x] **PERF-4.1 P1** Give Vulkan the same thread-safe asynchronous CPU texture
  snapshot/decode control path already available to D3D12. Vulkan now copies
  immutable source and CLUT state on the GE thread, decodes and xBRZ-scales
  bounded jobs on up to three SDL workers, and waits only before the first
  pending batch upload. `SR_VULKAN_SYNC_TEXTURE_DECODE=1` and
  `SR_PERF_ALTERNATE=texdecode` preserve the exact synchronous A/B fallback;
  `SR_VULKAN_VERIFY_TEXTURE_DECODE=1` compares both outputs byte for byte.
- [x] **PERF-4.2 P0** Use texture miss, decode wait, renderer decode/scale time
  and upload-byte counters to decide whether GPU decode crosses the Phase 0
  action threshold on either title.
  - [x] Three complete Killzone baseline routes recorded 660-668 texture
    misses and 29.649-44.813 ms of CPU decode across 4,687 presentations.
    Decode wait totaled 11.770-19.695 ms and texture scaling was zero. Even
    combining the largest decode and wait totals gives less than 0.014 ms per
    presentation, below the action threshold.
  - [x] The stable gameplay window at presentations 4440-4679 contained zero
    texture misses, decode waits, uploads or scaling work in all three runs.
    Use the opening-video transition only as a texture-heavy diagnostic; do
    not label it gameplay evidence.
- [x] **PERF-4.3 P1** Define a shared raw-texture job ABI containing source
  extents, format, swizzle, mip level, CLUT state and destination dimensions.
  `GeTextureRawJob` is a fixed 64-byte, all-`uint32_t` descriptor with explicit
  source/CLUT byte offsets, RGBA8 destination offsets and validated bounds.
  Immutable snapshots expose the exact copied level and 2 KiB CLUT payloads
  that future Vulkan and D3D12 compute decoders will upload. Synthetic direct,
  indexed, swizzled, compressed, mip, immutability and overflow cases cover
  the shared builder; defining the ABI does not enable GPU decode.
- [x] **PERF-4.4 P1** Implement exact compute decoders for direct 5650, 5551,
  4444 and 8888 textures, then CLUT4/8/16/32 and DXT1/3/5. Each format requires
  synthetic byte fixtures and exact CPU/GPU RGBA comparisons.
  - [x] Add a validation-only Vulkan compute decoder for direct 5650, 5551,
    4444 and 8888. Linear and swizzled 7x9 fixtures cross PSP tile
    boundaries, include channel/alpha extrema, and match the shared CPU
    unpack/offset oracle byte for byte under Khronos validation.
  - [x] Add exact CLUT4/8/16/32 compute decode and differential fixtures.
    Linear and swizzled jobs cover all four palette entry formats, CLUT
    shift/mask/start, shared and per-mip palettes, CLUT4 mip offsets and the
    CLUT8 16-bit odd-mip bank rule.
  - [x] Add exact DXT1/3/5 compute decode and differential fixtures. Six-block
    7x9 fixtures cover partial right/bottom blocks, both DXT1 endpoint modes,
    all color and alpha selectors, DXT3's high-nibble alpha and both DXT5
    interpolation ladders with the PSP decoder's exact rounding.
  - [x] Keep all compute texture decoding disconnected from runtime caches
    until PERF-4.2 supplies title evidence and PERF-4.5 preserves identity and
    invalidation rules.
- [x] **PERF-4.5 P1** Integrate GPU-decoded images into the existing bounded
  texture caches without weakening guest-memory invalidation, framebuffer
  alias handling or content identity.
  - [x] Add an opt-in Vulkan scale-1 path that dispatches the validated
    direct/indexed/DXT compute decoder before the first pending batch samples
    a cache entry and copies the complete RGBA8 result into the existing
    sampled image.
  - [x] Reuse immutable snapshots, source-content hashes, palette identity,
    same-key replacement, LRU eviction and watched-memory invalidation.
    Oversized sources, xBRZ scaling, unsupported storage-buffer capabilities,
    synchronous override and A/B-off intervals retain exact CPU fallback.
  - [x] Extend `SR_VULKAN_VERIFY_TEXTURE_DECODE=1` to compare compute output
    against the snapshot CPU oracle before the decoded image is consumed.
  - [x] Add CSV v8 job, fallback and raw-source byte counters so mixed
    compute/CPU runs remain attributable.
  - [x] Run the protected Killzone route with
    `SR_VULKAN_VERIFY_TEXTURE_DECODE=1`; every scheduled compute result matched
    the CPU snapshot oracle before image consumption, with no Vulkan
    validation error. Fixed-presentation capture pairs were not used as an
    exactness claim because the performance-changing paths reached different
    guest-vblank states even under pacing.
  - [x] Run three nominal-thermal `gputexdecode` A/B repetitions over the
    texture-heavy opening-video window, presentations 313-552. Each GPU half
    scheduled 123 jobs, decoded 124,928,000 source bytes and recorded zero
    fallbacks. GPU-minus-CPU complete-frame mean deltas were +0.261, +0.303
    and -0.089 ms. GPU decode/wait totals were 110.097/110.037,
    200.130/200.077 and 50.173/50.119 ms versus CPU totals of
    4.679/4.551, 6.230/6.018 and 3.403/3.465 ms. Frontend p99 regressed by
    2.550, 3.887 and 1.964 ms.
  - [x] Keep compute texture decode opt-in and retain asynchronous CPU decode
    as the baseline. Per-texture command submission and fence waits dominate
    a CPU category that was already below the action threshold; no format is
    enabled by default.
- [x] **PERF-4.6 P2** Prototype GPU xBRZ only if texture scaling remains a
  measured startup or transition bottleneck after GPU decode. All three
  complete Killzone baselines recorded zero texture-scaling time at the
  validated 1x setting, while compute decode itself regressed frontend tails.
  Do not implement GPU xBRZ without a future scaled-title profile that crosses
  the action threshold.

### Phase 4 exit gate

Phase 4 is closed for the available title with no default promotion. Exact
direct, indexed and DXT compute fixtures and protected live verification pass,
but CPU texture work is below the action threshold, stable gameplay performs
none, and synchronous compute submission regresses frontend tails. The
asynchronous CPU snapshot decoder remains the baseline; GPU decode remains an
opt-in diagnostic with complete fallback.

## Phase 5 - Conditional GPU geometry work

- [x] **PERF-5.1 P2** Measure point, line, sprite, Bezier and spline counts and
  exclusive CPU time on representative title routes.
  - [x] Add exclusive point, line and sprite categories plus draw and generated
    primitive counters. Line lists count complete pairs, line strips count
    adjacent segments, and odd sprite controls remain ignored.
  - [x] Split Bezier and spline evaluation/draw timing while retaining
    aggregate `patch_eval_ms` and `patch_draw_ms`, and record control and
    evaluated vertex counts.
  - [x] Publish the fields in CSV v9 and cover counter reset plus actual GE
    point/line/sprite route arithmetic in public tests.
  - [x] Record the available protected Killzone route and compare each class
    against the Phase 0 action threshold. Presentations 700-4699, after one
    complete warm-up, produced a median 76,851 sprites
    (range 76,851-76,977) and 8,796.721 ms exclusive sprite time
    (range 8,307.579-8,831.652 ms) across three runs. Point, line, Bezier and
    spline workloads and all patch timing were zero. No Metal Gear Acid route
    was locally available, so this evidence cannot satisfy a cross-title
    promotion gate.
- [x] **PERF-5.2 P2** Evaluate compact point/line/sprite shader expansion and
  promote only when capture and expansion time crosses the action threshold.
  - [x] Add an independent opt-in Vulkan through-sprite path using two compact
    36-byte controls and exact vertex-ID expansion. Keep transform-mode point
    and sprite expansion under `gputnl`; keep exact PSP DDA lines on CPU.
  - [x] Add `SR_VULKAN_GPU_PRIMITIVE_EXPANSION=1` and the stable `gpuprim`
    A/B fallback without renumbering earlier paths. The prototype remains off
    by default and does not enable model-space GPU T&L.
  - [x] Validate nearest and linear UV rules against the CPU oracle with real
    Vulkan readback. A paired 50-frame Killzone capture was byte-identical;
    the SHA-256 of its ordered frame-name/per-frame-digest manifest was
    `fe16da4733122acac17ad4a1971c973e94060709c5b4601494e5ce43d3b8f6cb`.
  - [x] Reject promotion from the repeated A/B result. In the balanced
    presentation 4440-4679 gameplay window, each half measured 8,040 sprites.
    Compact controls saved 963,840 host-upload bytes per half
    (120 bytes/sprite), but three GPU-minus-CPU mean-frame deltas were
    -0.091, +0.035 and -0.050 ms; frontend deltas per sampled flip were
    -0.063, +0.043 and -0.042 ms. The median-frame result regressed by
    0.010 ms and neither sprite CPU time nor p95 improved consistently.
- [x] **PERF-5.3 P2** Evaluate compute-based Bezier/spline work only when
  `patch_eval_ms` or `patch_draw_ms` is material. The protected route submitted
  no Bezier or spline commands and recorded zero evaluation/draw time in all
  three runs, so no compute prototype is justified. Preserve quantized
  evaluation, generated normals, UVs and topology exactly if future evidence
  reopens this task.

Phase 5 evidence record:

- Identity: shared base commit `613cd1ed4f95e3f91f41f4e9ec6cd257083739bd`
  with the Phase 1-5 worktree changes; Killzone title commit
  `bcff373e69bf61bdaa903e2af5cfed4f3ba1c0a8`; generated image SHA-256
  `6dce709433ef9883d9b9ecc2fbea285b10b578fdf39019a507cb83cee927e520`;
  extracted module SHA-256
  `13fae03aaf462ca90ed15fd78dce56eea603add9b89bb1d25dbc7f049490ce74`.
- Route: deterministic Killzone pad script SHA-256
  `b927c93797263e170168308ad7f5e53a7fe0ce689a2c9fefed1b79c6a20fa1e6`;
  clean copied memstick per run; one full warm-up; warm pipeline cache;
  presentations 700-4699 for workload and 4440-4679 for balanced A/B.
- Host: Apple M5 arm64 on macOS 27.0.1 (build 26A434), Vulkan loader
  1.4.363, MoltenVK 1.4.2 (driver version 0.2.2210, device API 1.4.357),
  AC power, charged battery and nominal thermal state.
- Settings: Vulkan, turbo scheduler, manual 1x internal resolution, 1x
  texture scale, anti-aliasing off, asynchronous CPU texture decode, and
  normal validation disabled during timing.
- Sampling: three baseline repetitions, three decision-grade in-run A/B
  repetitions, one paired 50-frame capture, and one instrumentation-only A/B
  repetition with Vulkan stage timers. Every A/B half contained 120 unique
  presentations and 8,040 sprites after transition rows were discarded.
- Attribution: the stage-timed run again saved 963,840 upload bytes. Its
  GPU-minus-CPU deltas per sampled row were -0.000170 ms in vertex work and
  +0.009877 ms in submit work; GPU samples differed by -0.006031 ms. The
  favorable backend delta was concentrated in presentation time, which the
  primitive path does not control, so it is noise rather than evidence for
  promotion. Stage-timed results were not mixed into the lower-overhead
  decision repetitions.
- Correctness: CPU and GPU oracle/readback fixtures passed; all 50 paired
  title frames were byte-identical; no runtime validation path was enabled in
  performance runs. DirectX execution remains deferred.

### Phase 5 exit gate

No specialized geometry path becomes shared behavior without a measured
cross-title workload or a documented capability-specific reason.
Phase 5 is closed for the available route with no default promotion:
through-sprite expansion remains an exact Vulkan-only diagnostic, lines stay
on the CPU oracle, and patch offload was not implemented. A second title route
is still required before reconsidering shared behavior.

## Phase 6 - Parallel CPU/runtime performance

GPU offload cannot accelerate general Allegrex execution, the scheduler, HLE
or small synchronous guest operations. Track these in parallel so graphics
work is not blamed for non-graphics bottlenecks.

- [x] **PERF-6.1 P0** Keep interpreter profiles and generated-dispatch miss
  reports at zero or bounded by reviewed policy. Generate hot overlays instead
  of attempting to hide interpreter cost with renderer changes.
  - [x] Make configured miss/profile outputs explicit on a clean zero-target
    exit and close both reports deterministically.
  - [x] Add `tools/check_interp_reports.py`. Its default policy requires zero;
    reviewed module-qualified targets require per-target call and instruction
    budgets, while step limits, failures, and incomplete returns always fail.
  - [x] Pass the gate on repeated Killzone routes with zero targets, calls,
    interpreted instructions, step limits and failures. No hot overlay is
    needed for this route.
- [x] **PERF-6.2 P1** Use the instruction-pointer sampler and exclusive native
  category to identify generated-code hot functions. Evaluate changes only
  with exact scheduler/function traces and repeated title routes.
  - [x] Extend the existing Windows 4 kHz sampler to macOS and guarantee a
    final report on normal shutdown. Preserve executable RVA buckets and add
    per-sample exclusive categories plus external module attribution.
  - [x] Add `tools/analyze_ip_samples.py` for symbol resolution and category
    filtering.
  - [x] Sample presentations 4440-4679 twice. The categorized repetition
    captured 15,606 samples: 1,139 were exclusive native, 967 were generated
    functions, and only 13 native samples were external. The 4 kHz native
    estimate was 284.750 ms versus 278.914 ms in CSV (2.09% difference).
  - [x] Reject a generated-function optimization on this evidence. The two
    hottest generated functions held only 34 and 31 samples (2.99% and 2.72%
    of native time); no single generated function crossed the action
    threshold.
- [x] **PERF-6.3 P1** Measure direct-call recovery, yield density, memory helper
  cost and hot/cold translation-unit policy independently. Reject compiler
  flags whose result is within variance or increases binary size without an
  end-to-end benefit.
  - [x] Advance the common performance record to CSV v10 with
    `native_dispatches` and `scheduler_yields`; the schema now has 102 columns.
  - [x] Record 121,997 statically direct generated calls and 23,232 dynamic
    dispatch sites (84.00% direct among those emitted control transfers). The
    gameplay window executed 488,400 dynamic dispatches, but `sr_lookup`
    represented only 30 samples (2.63% of native, 0.19% overall).
  - [x] Record exactly 240 actual scheduler yields in 240 gameplay
    presentations and 3.475 ms exclusive scheduler time. Do not instrument
    every generated function entry merely to restate the 20,000-entry
    timeslice.
  - [x] Bound memory-helper cost through native samples: `sr_vread` and
    `sr_vwrite` combined for 88 samples (7.73% of native, 0.56% overall).
    This is below the end-to-end action threshold.
  - [x] Retain the title's existing measured hot/cold policy: 32 hot O2
    translation units and 320 cold O1 units. One of the two hottest sampled
    functions was already hot; the cold one represented only 0.22% of all
    samples. No shared compiler-flag change is justified.
- [x] **PERF-6.4 P1** Keep media hardware decode, audio queueing and guest
  completion metrics separate from graphics timing.
  - [x] Add a clean MPEG shutdown summary for ring fills, guest AVC requests,
    native video frames, decoded audio frames, no-data returns and maximum A/V
    drift. Keep it out of the display CSV.
  - [x] Verify the route used the 480x272 VideoToolbox hardware decoder and
    completed 484 video plus 184 audio frames with zero no-data returns.
    The separate SDL summary reported 2,048,000 played frames, an 8,192-frame
    maximum guest queue, zero drops/resynchronizations and 10 silent blocks.

Phase 6 evidence record:

- Identity, generated image/module hashes, host, backend and graphics settings
  match the Phase 5 evidence record above. The deterministic pad script,
  clean-memstick policy, warm cache and presentation limits were unchanged.
- Sampling window: presentations 4440-4679, 240 unique presentations, 480 CSV
  rows, 3,902.046 ms total frame time, 278.914 ms exclusive native time,
  11.946 ms HLE time, 3.243 ms scheduler time and zero interpreter time.
- Counter window: the same presentations recorded 3,609.578 ms total frame
  time, 239.616 ms native time, 3.475 ms scheduler time, 488,400 dispatches
  and 240 yields without instruction sampling.
- Workload decisions use repeated deterministic routes; diagnostic sampling
  and `SR_MPEGLOG` runs are not mixed into ordinary frame comparisons.
  DirectX execution remains deferred, and the unavailable second title blocks
  promotion of any title-specific CPU/compiler result.

### Phase 6 exit gate

Phase 6 is closed for the available route with no speculative CPU
optimization: interpreter fallback is explicitly zero, no generated function
is individually hot enough, lookup/yield/helper costs are below the
end-to-end action threshold, and media health is reported independently.
The portable instrumentation and policy gates remain available for future
titles and repeated cross-title evidence.

## Phase 7 - Promotion and maintenance

- [x] **PERF-7.1 P0** Publish a reduced before/after result for every promoted
  optimization: runtime revision, title route, hardware, driver, scale,
  settings, repetitions, variance, category deltas, GPU delta, p95 delta,
  transfer delta, thermal state and correctness result.
  No new optimization qualified for promotion; the reduced audit below records
  the existing scaled-upload baseline and every rejected experimental path.
- [x] **PERF-7.2 P0** Enable a path by default only for the capabilities and draw
  classes that passed. Retain the diagnostic fallback until at least one later
  runtime revision repeats both title gates.
  Phase 7 enables no additional path. GPU T&L, compute texture decode and
  through-sprite expansion remain opt-in, and every diagnostic CPU fallback
  remains available.
- [x] **PERF-7.3 P1** Update shared renderer architecture, environment-variable,
  validation and porting documentation together with each promotion.
  The renderer status table and promotion-record rules now distinguish movie,
  menu, cutscene and gameplay workloads instead of inferring state from a
  presentation number alone.
- [x] **PERF-7.4 P1** Update title runtime pins only after their public builds
  and protected routes pass against the same shared commit.
  No pin is advanced: Killzone remains at shared commit
  `613cd1ed4f95e3f91f41f4e9ec6cd257083739bd`, Metal Gear Acid is unavailable,
  and the active worktree has no public build revision.

Phase 7 promotion audit:

| Path | Default state | Reduced evidence | Decision |
|---|---|---|---|
| Vulkan scaled framebuffer upload | Capability-gated baseline at scales above 1x | Three paired 2x Killzone opening-movie runs, presentations 120-299, transferred 188,743,680 framebuffer bytes with GPU expansion versus 754,974,720 with CPU expansion in every run. GPU-minus-CPU mean-frame deltas were -0.911, +0.003 and +0.083 ms; p95 deltas were -12.656, +0.090 and -0.129 ms. Four 960x544 capture pairs were byte-identical; their ordered manifest SHA-256 was `325eeed1ffab84e5afa3676da1d28e0281dcb4ea27108c2b3666d2aeb633d99c`. Thermal state remained nominal. | Retain the reconciled capability-gated baseline and forced CPU fallback, but do not claim a new timing promotion. The deterministic route reaches gameplay at presentations 4400-4609; the measured gameplay window 4440-4679 contained 16,080 sprites and zero framebuffer uploads, so movie evidence is not labeled as gameplay evidence. |
| Vulkan/D3D12 GPU T&L | Opt-in | MoltenVK passed bit-exact live validation and all 50 Killzone captures (ordered SHA-256 `6b965a7e1fb1051a50ef5acb0923ca6d70208e6fb6a6a4c1fba5ed162b73821c`) but direct and transform-once means stayed within variance. KosmicKrisp passed real shader readback, 85,590 exact gameplay vertices and all 50 protected CPU/GPU captures after explicit integer color normalization (ordered SHA-256 `6bda9f5158b692cc20bea45898ee7d4e6d85d08d59b167769f5b9e41f18195fd`); three A/B mean deltas were -0.718, -1.429 and -1.319 ms, with a first-run p99 spike. D3D12 direct and compute-preprocess paths compile with deterministic DXIL and MinGW `-Werror`; native Windows and the second-title gate remain unavailable. | Not promoted: the improvement is driver-specific, one repetition had a tail regression, and neither the second-title nor later-driver gate has passed. |
| Vulkan compute texture decode | Opt-in | Exact direct, indexed and DXT readback plus protected live CPU-oracle verification passed. Three complete Killzone baselines spent only 29.649-44.813 ms decoding and 11.770-19.695 ms waiting across 4,687 presentations; gameplay 4440-4679 performed no texture decode. In the texture-heavy opening-video A/B window, three GPU-minus-CPU mean deltas were +0.261, +0.303 and -0.089 ms, while frontend p99 regressed by 2.550, 3.887 and 1.964 ms. | Not promoted: per-texture GPU submission/fence waits cost more than the already-small asynchronous CPU workload. |
| Vulkan through-sprite expansion | Opt-in | Fifty paired Killzone frames were byte-identical and upload traffic fell, but complete-frame and p95 timing did not improve beyond variance. | Not promoted. |
| Generated-code/compiler changes | Existing title policy | Interpreter fallback was zero, and no generated function, dispatch, scheduler or memory-helper result crossed the action threshold. | No shared change. |

### Phase 7 exit gate

Phase 7 is closed as a no-promotion audit. No experimental GPU or generated
CPU path changes default behavior, diagnostic fallbacks remain available, and
no title pin moves ahead of a public shared revision. Future promotion requires
the same reduced record on both protected title routes and a repeat on a later
runtime revision.

## Portability constraints

- Preserve the software renderer and CPU transform path.
- Target the existing D3D12 feature-level floor and core Vulkan functionality.
- Do not require geometry shaders for shared behavior.
- Keep Vulkan paths viable on native drivers, MoltenVK and KosmicKrisp.
- Keep all GPU paths capability-gated and independently disableable.
- Avoid synchronous GPU readback outside validation or required guest access.
- Do not trade average throughput for worse p95 frame time or larger
  transition stalls.

## Required benchmark record

Every result used for a decision must include:

| Field | Requirement |
|---|---|
| Identity | Shared commit, title commit, generated image/module hashes |
| Route | Script/fixture revision, warm-up point, measured presentations |
| Host | OS, CPU, GPU, driver, power state, thermal range |
| Graphics | Renderer, driver/ICD, internal scale, texture scale, AA |
| CPU timing | Native, HLE, interpreter, scheduler, GE, vertex/decode/capture, sync |
| GPU timing | Backend stages, fenced GPU sample, submission source flip |
| Workload | Draws, vertices, unique vertices, texture misses, snapshots, transfers |
| Outcome | Median, p95, p99, variance, validation, capture comparison |

Use [Shared Finding PSP-F-038](SHARED_FINDINGS.md#psp-f-038--profile-across-guest-scheduler-frontend-backend-and-gpu-boundaries)
and the [Validation and Evidence Guide](VALIDATION.md) as the governing
measurement and promotion rules.
