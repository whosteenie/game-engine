# PT mirror-chain RR validation fixture

Companion checklist for `pt_mirror_chain_hall.gameproject` and
`devdoc/dxr/pt/mirror-chain-rr-guides.md`.

## Fixture contract

- The center camera ray follows Mirror A -> Mirror B -> textured receiver. A third mirror panel,
  emissive object, ordinary objects, and sky terminal exercise the remaining ownership cases.
- `mirror-chain-orbit-target` is the fixed center for automated motion captures.
- All transforms are static. The project is saved as real-time path traced, 16 bounces, DLAA + RR,
  full PT depth/motion/material guides, and Transformer E. The capture override is authoritative for
  the mirror-chain feature arm; do not edit the project between A/B runs.

## Fixed-pose baseline and reference

Run the same command twice, changing `Disabled`/`fixed-disabled` to
`Enabled`/`fixed-enabled` for the second arm:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/capture-s1p6-cornell.ps1 `
    -Project tests/fixtures/pt_mirror_chain_hall.gameproject `
    -MirrorChainPsr Disabled `
    -OutputDirectory artifacts/mirror-chain-rr/fixed-disabled `
    -RealtimeWarmupFrames 120 -ReferenceWarmupFrames 1024 -SampleFrames 60
```

The historical `cornell` script name is generic: it records raw real-time PT, the three material
guides, final RR, stopped reference, timing CSVs, images, logs, project SHA-256, and validated
capture manifests at one pose. Before comparing images, require matching revision, project hash,
camera, extents, DLSS mode, RR preset, RR bundle, max bounces, and every setting except the explicit
mirror-chain override.

## Continuous orbit and chain AOVs

Run this once disabled and once enabled, using distinct output directories:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/capture-pt-optical-orbit.ps1 `
    -Project tests/fixtures/pt_mirror_chain_hall.gameproject `
    -Target mirror-chain-orbit-target `
    -MirrorChainPsr Disabled `
    -OutputDirectory artifacts/mirror-chain-rr/orbit-disabled `
    -Modes raw-radiance,final-rr,mirror-chain-owner,mirror-chain-length,mirror-chain-confidence,mirror-chain-receiver-id,mirror-chain-receiver-depth,mirror-chain-receiver-motion `
    -WarmupFrames 120 -OrbitFrames 240 -FrameStride 8
```

Each mode runs in a fresh process while its own orbit remains temporally continuous. The schema-v2
orbit manifest records the project hash, renderer settings, output/render extents, adapter/runtime
capability, camera path, and capture frames. Per-frame GPU timestamps cannot be truthfully paired
with asynchronous image readbacks; use the fixed-pose timing CSV as the performance evidence.

## Review gates

Corrected automated implementation gates completed on 2026-07-18:

```text
cmake --build build --config Debug --target engine-tests
build\Debug\engine-tests.exe
```

The regular suite passes 40/40, including shader compilation, affine reflection composition,
virtual-versus-physical projection, and perfect-metal lobe sampling. The focused RTX results recorded
before the virtual-geometry correction are useful only as fixture history and must not be treated as
delivery evidence. Rerun the manual command below on the target GPU; the strengthened gate checks
unfolded depth/normal/motion and sixteen RNG seeds.

```text
build\Debug\d3d12-render-tests.exe --tier=5 --filter=PtMirrorChainPsr*
```

- [ ] Feature disabled matches the pre-feature raw/final baseline; reference convergence, hybrid RT,
  opaque non-mirror PT, and glass routing remain unchanged.
- [ ] Stopped enabled RR is visibly closer to the stopped reference and keeps repeated frames sharp.
- [ ] Slow orbit reports chain length 2 and stable receiver identity/motion through Mirror A ->
  Mirror B -> textured receiver, without broad smear, double image, or owner flicker.
- [ ] The focused oracle reports virtual depth greater than 11.5 units, unfolded receiver normal near
  `(-0.707, 0, 0.707)`, and motion distinct from direct projection of the physical receiver.
- [ ] All sixteen focused RNG seeds retain the green length-2 owner with no black/magenta samples.
- [ ] Fast pan may lose reconstruction temporarily but leaves no persistent ghost after settling.
- [ ] Mirror -> sky is cyan in the owner AOV, keeps far depth, and has no black tail.
- [ ] Roughness sweep confidence is continuous; non-delta/moving fallbacks are magenta and preserve
  the primary bundle, with no cutoff seam, NaN, or global shimmer.
- [ ] Logs contain no Streamline or D3D12 diagnostic errors and both capture suites retain their
  manifests, images, and timing evidence.

Moving mirrors/receivers and broad glossy multi-receiver lobes remain intentionally unsolved. The
former needs previous-scene geometry; the latter needs a validity/multi-layer interface RR does not
currently expose.
