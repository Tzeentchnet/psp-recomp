# PSP-to-PC Recompilation Knowledge Base

This directory captures the reusable engineering knowledge from the
Killzone: Liberation and Metal Gear Acid recompilation projects. Its purpose
is to let a new title start from proven methods instead of rediscovering PSP
boot, code-generation, runtime, graphics, media, and validation requirements.

The documentation separates:

- **Shared mechanisms and invariants** that belong in `psp-recomp`.
- **Title policy and evidence** that must remain in a game repository.
- **Validated examples** from the two existing title integrations.

## Start here

| Task | Read first |
|---|---|
| Start a new PSP recompilation | [Porting workflow](PORTING_WORKFLOW.md), then [title project template](TITLE_PROJECT_TEMPLATE.md) |
| Change shared runtime or codegen behavior | [Shared findings](SHARED_FINDINGS.md), then [validation](VALIDATION.md) |
| Recognize or reuse previously translated functions | [Safe function reuse](FUNCTION_REUSE.md) |
| Investigate a regression in an existing title | [Title findings](titles/README.md), then [validation](VALIDATION.md) |
| Decide whether behavior is generic or title-specific | [Shared findings: ownership boundary](SHARED_FINDINGS.md#shared-runtime-versus-title-project) |
| Prepare a release or update a runtime pin | [Validation and evidence](VALIDATION.md) |

AI agents should also use the workspace
[`psp-recomp-port`](../.github/skills/psp-recomp-port/SKILL.md) skill. It
turns this knowledge base into an execution workflow with explicit stop
conditions and evidence requirements.

## Documents

- [Porting workflow](PORTING_WORKFLOW.md) — ordered phases, outputs, and exit
  gates from disc survey through release.
- [Shared findings](SHARED_FINDINGS.md) — stable cross-title findings and the
  shared-runtime/title-policy boundary.
- [Validation and evidence](VALIDATION.md) — validation ladder, commands,
  diagnostics, protected evidence rules, and release gates.
- [Safe function reuse](FUNCTION_REUSE.md) — provenance catalog, exact local
  codegen cache, invalidation keys, and trust boundary.
- [Title project template](TITLE_PROJECT_TEMPLATE.md) — copyable structure for
  configuration, findings, plans, and acceptance matrices.
- [Title findings](titles/README.md) — index of title-specific architecture,
  policies, exceptions, and reusable lessons.

## Source projects

The consolidated findings were derived from:

- [Killzone: Liberation Recomp](https://github.com/Tzeentchnet/KillzoneLiberation-Recomp)
- [Metal Gear Acid Recomp](https://github.com/Tzeentchnet/MetalGearAcid-Recomp)

Their exact title configuration, protected validation evidence, and current
status remain authoritative for those games. This directory summarizes the
knowledge needed to build the next project; it does not replace each title's
tracked configuration or tests.

## Source-of-truth order

When documents disagree, use this order:

1. Exact title configuration and pinned hashes.
2. Executable tests and deterministic validation manifests.
3. Evidence-backed findings with stable identifiers.
4. Current status and reproduction documentation.
5. Plans, TODO lists, and historical work logs.

Never promote an observation into shared runtime behavior without a synthetic
test and regression evidence from every affected title.

## Documentation contract

For every new finding:

1. Assign a stable title-local identifier.
2. Record the exact input revision and configuration.
3. State the observation separately from the interpretation.
4. Include a reproducible command, trace, checkpoint, or threshold.
5. State whether the result changes shared code, title policy, or only
   diagnostics.
6. Update the relevant title summary here when the result is reusable.

Do not commit ISOs, decrypted executables, generated game code, saves, traces,
captures, or dumps. Commit reduced metrics, synthetic fixtures, hashes, and
instructions that let an authorized developer reproduce the evidence.
