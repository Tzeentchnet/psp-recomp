# Title-Specific Findings

This directory records facts that are useful as examples but must not become
shared defaults. Each title document identifies:

- Exact supported input and executable topology.
- Measured boot contract.
- Module and HLE policy.
- Content, control, filesystem, and validation behavior.
- Lessons that were generalized into
  [shared findings](../SHARED_FINDINGS.md).

## Existing titles

| Title | Architecture | Primary contribution |
|---|---|---|
| [Killzone: Liberation](killzone-liberation.md) | Large mostly monolithic title module with proprietary asset archive | Standalone boot, broad HLE/gameplay runtime, D3D12 backend, media, savedata, configurable input |
| [Metal Gear Acid](metal-gear-acid.md) | Main module plus many runtime PRXs and same-address stage overlays | Dynamic module lifecycle, hybrid native/interpreter execution, overlay identity, cross-title generalization |

## Adding another title

1. Create `docs/titles/<slug>.md`.
2. Link the title's exact configuration and authoritative findings.
3. Document only title-specific facts in that file.
4. Promote a rule to [shared findings](../SHARED_FINDINGS.md) only after it
   has a generic implementation, a synthetic test, and regression evidence.
5. Add the title to the shared-change matrix in
   [validation](../VALIDATION.md).

Use the [title project template](../TITLE_PROJECT_TEMPLATE.md) to create the
consumer repository.
