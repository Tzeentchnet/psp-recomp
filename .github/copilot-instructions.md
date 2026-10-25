# Project Git Attribution

- Preserve human authorship on every commit.
- Never add `Co-authored-by`, `Signed-off-by`, or similar trailers naming
  GitHub Copilot, Copilot, or another AI identity.
- Never set an AI identity as a commit author or committer.

# PSP Recompilation Workflow

- For new-title bring-up, PSP ISO/PRX analysis, shared runtime changes, HLE,
  modules/overlays, graphics, audio/video, input, savedata, or validation work,
  use the `psp-recomp-port` skill and start at `docs/README.md`.
- Keep reusable mechanisms and synthetic tests in this repository. Keep disc
  hashes, addresses, module policy, game patches, content paths, control side
  channels, and scripted gameplay routes in the title repository.
- Pin exact source and runtime revisions before interpreting evidence.
- Use the public function catalog only for reviewed identity/provenance.
  `PSP_RECOMP_FUNCTION_CATALOG` may select a private local research catalog;
  its normalized matches remain review candidates. Generated C may be reused
  only through the exact private cache documented in
  `docs/FUNCTION_REUSE.md`.
- Never commit game-derived executables, generated code, saves, traces,
  captures, dumps, or disc content.
- Do not accept permissive dispatch, fake-success HLE, broad module
  replacement, or unmeasured boot defaults as completed behavior.
- Validate shared changes with synthetic tests and every affected title before
  advancing consumer runtime pins.
