# D3D12/DXR path-tracing pipeline audit

**Status:** read-only source audit complete; remediation not started.<br>
**Audit date:** 2026-07-15.<br>
**Source snapshot:** commit `9b65f29127b818abaa142fbc851c157b409df02c`.<br>
**Representative target:** RTX 4070 Ti, approximately 1440p, 1 spp, 16 maximum bounces,
the RR reconstruction feature with DLAA quality (native internal/output extent); DLSS Super
Resolution qualities remain separately selectable under either reconstruction feature.<br>
**Related living documents:** [performance roadmap](performance-roadmap.md),
[ReSTIR production roadmap](restir-production-roadmap.md),
[P7 diagnostics tracker](p7-diagnostics-tracker.md), [full-PT RR guides](full-rr-guides.md),
[transmission RR guides](transmission-rr-guides.md), and [RR specular hit distance](rr4-spec-hitdist.md).

This document is an as-of-code audit, not an implementation change or a visual sign-off. It is the
authoritative snapshot for the source revision above when an older status statement conflicts with
current source. In particular, completed items in [the older PT audit](pt-audit.md) must not be
assumed to remain fixed without checking this snapshot.

## Evidence notation

| Tag | Meaning |
|---|---|
| **C** | Confirmed directly from active source, shader math, resource formats, or an API contract. |
| **HC** | High-confidence causal diagnosis. The mechanism is present in source, but its visible contribution has not been isolated in a live capture. |
| **M** | Measurement already recorded in a repository diagnostic or roadmap. It was not freshly measured by this audit. |
| **T** | Audit-time build or automated test result. |
| **V** | Requires a controlled visual, GPU-capture, capability, or timing verification before acceptance. |

Line links are anchors into the audited revision and may drift after edits. “RR” means DLSS Ray
Reconstruction except where “Russian roulette” is written out explicitly.

---

## 1. Executive summary

The current full path-tracing mode is a working DXR megakernel followed by a modern temporal
reconstruction chain, but it is not yet an isolated path-tracing pipeline. The renderer still builds
and draws the shadow maps and complete raster G-buffer before dispatching the path tracer. The PT
shader does not consume those raster textures for radiance, yet the CPU dispatch contract and the
DLSS/RR integration still require them. That is both redundant work and a hard cross-mode dependency.

The highest-priority correctness defects are upstream of denoiser tuning:

1. **Previous-camera data is not populated.** Valid temporal frames use an identity previous view and
   a world-origin previous camera. Both transmission virtual motion and ReSTIR previous-domain
   reconstruction consume those defaults. This is a direct glass replay and reservoir-validation
   defect, not a parameter-tuning issue.
2. **The reference partial-transmission mixture divides out the selection probability twice.** A
   material with transmission between zero and one has an expected contribution equal to the sum of
   both full lobes instead of their authored mixture, with rare high weights near the endpoints.
3. **Geometry, shading, and guide normals do not have a single contract.** The value labelled
   geometric normal is normally an interpolated vertex normal; non-uniform scaling uses the wrong
   transform; the radiance path can use a normal-mapped, view-bent normal while the transmission
   guide uses a different normal; and normal maps are sampled at mip 0 with incomplete ray-cone
   propagation. These are credible direct causes of glass and rough-metal shimmer.
4. **Rough dielectric sampling is treated as a unit-weight delta event.** GGX VNDF directions are
   sampled, but continuous rough reflection/transmission PDFs and microfacet weights are not
   evaluated. The estimator is not a valid rough dielectric BSDF.
5. **The RR specular hit-distance guide traces a separate ideal-mirror ray.** On rough surfaces it is
   neither the hit distance of the radiance event nor, in the transition band, the distance of any
   physical ray. It also adds another TLAS traversal for many opaque pixels.
6. **Manual exposure is lost after RR and is handled incorrectly after DLAA/SR.** Exposure supplied
   to reconstruction is guidance, not permission to set the renderer tonemapper to EV 0. RR does
   not support this exposure input in the currently integrated contract.

The largest confirmed structural performance defect is CPU/GPU serialization in
`GfxContext::BeginFrame`: every new frame waits for the latest submitted fence rather than only the
fence protecting the frame context being reused. This prevents the intended two-frame overlap. The
existing formal capture records roughly 13.1–13.8 ms in “previous GPU wait”; that entire number must
not be advertised as recoverable, but the overlap prohibition itself is confirmed. Existing formal
GPU captures also show that the floor-scene slowdown is predominantly inside the path tracer:
13.623 ms versus 6.583 ms for PT, with the megakernel explaining about 60% of the PT delta. P7's
recorded diagnostic effective-FPS and motion metrics show that its current broad spatial/temporal
correlation is a quality and performance regression, not merely residual independent noise.

SER is structurally integrated in the expected `HitObject::TraceRay -> MaybeReorderThread -> Invoke`
position and the payload/root-signature/SBT contracts look sound. It is **not performance-proven**.
The current capability gate incorrectly requires DXR Tier 1.2 in addition to Shader Model 6.9, the
UI does not expose whether the driver reports that SER actually reorders, and the existing formal
baseline reports Shader Model 6.8 / SER unavailable. That baseline and the user's newer observation
that the UI says SER is active must be reconciled with a fresh capability capture before benchmarking.

DLAA and DLSS SR are represented by distinct quality selections and their ordinary AA/preset/resize
paths mostly invalidate and reallocate history correctly. They are **not yet fully isolated or SDK-
correct as a matrix**: RR is an orthogonal `kFeatureDLSS_RR` selection that can receive either DLAA or
an SR quality, fixed render-scale ratios replace optimal-settings queries, feature/quality history
identity is incomplete, Streamline frame tokens are evaluation-scoped, and exposure is mishandled.
The remediation must preserve and test DLSS+DLAA, DLSS+SR, RR+DLAA, and RR+SR rather than treating
“RR” as a mutually exclusive fourth quality mode.

No confirmed BLAS/TLAS-build, SBT-alignment, payload-size, recursion-depth, or Russian-roulette
termination defect was found in the active serialized design. Static AS build skipping, 40-byte
payload, 8-byte attributes, recursion depth 1, opaque inline visibility, and Russian-roulette
throughput compensation are internally consistent. Do not spend the first remediation cycle
repacking the payload, changing recursion depth, or tuning RR/ReSTIR around invalid temporal inputs.

**Audit limits:** source/data flow, shader math, repository diagnostics, official contracts, builds,
and existing automated tests were reviewed; no live visual sequence, PIX/Nsight capture, GPU-based
validation run, or matched old/new performance A/B was obtained. Review depth was concentrated on the
representative full-PT path and its raster/hybrid/DLSS boundaries. It was not exhaustive for every
optional project configuration, editor-only/debug shader, vendor SDK binary internals, animated/
deforming AS workload, or the visual correctness of the complete raster and hybrid renderers. Alpha
materials, Opacity Micromaps, and Displaced Micro-Meshes are absent by project scope and were not
audited as latent features.

---

## 2. Active pipeline map

### 2.1 Frame and viewport ownership

```text
Application frame
  BeginFrame()                 currently waits for the most recent submission
    Scene viewport render      optional, stable DLSS viewport id 0
    Game viewport render       optional, stable DLSS viewport id 1
  EndFrame() / Present()
```

Both viewports can render between one `BeginFrame` and `EndFrame`. Each owns a separate
`ScreenSpaceEffects` instance, but DLSS evaluations currently generate their own monotonically
increasing Streamline frame tokens instead of sharing the application-frame token.

### 2.2 Per-view renderer path

```text
SceneRenderer::Render
  GPU-scene build/upload + lighting synchronization
  RenderShadowPass                      [still active in full PT]
  PrepareSceneRasterTarget
  RenderGeometryPass                    [all renderables still drawn in full PT]
  RenderPostProcessPass
    upload previous transforms + emissive-light data
    RecordDxrPass
      ensure/update acceleration structures
      path-tracer DispatchRays
      ReSTIR DI temporal/spatial/boiling stages, when enabled
      PT surface-history and reference-accumulation maintenance
      hybrid GI/reflection/shadow rays   [correctly disabled in full PT]
    ScreenSpaceEffects::Apply
      SSAO/SSGI/SSR/TAA/hybrid composite [correctly disabled in full PT]
      PT HDR + PT guides
        -> direct, or Streamline feature {DLSS, RR} x quality {DLAA, SR modes}
        -> bloom
        -> renderer tonemap
        -> viewport/UI composition
```

The application-level ownership is visible in
[`Application.cpp`](../../../src/app/core/Application.cpp#L1606). The unconditional shadow and raster
sequence is in [`SceneRenderer.cpp`](../../../src/app/scene/SceneRenderer.cpp#L2079), with the shadow
pass at [line 568](../../../src/app/scene/SceneRenderer.cpp#L568), geometry at
[line 1748](../../../src/app/scene/SceneRenderer.cpp#L1748), post processing at
[line 1834](../../../src/app/scene/SceneRenderer.cpp#L1834), and the DXR pass at
[line 974](../../../src/app/scene/SceneRenderer.cpp#L974). Full PT only suppresses the raster skybox;
it does not suppress the scene draws.

### 2.3 Full-PT data ownership

The path tracer produces its own HDR radiance, metadata, R32 depth, RGBA16F motion,
RGBA8 diffuse/specular guide albedos, RGBA16F normal/roughness, surface records, and ReSTIR
reservoirs. The active ray-generation shader does not read the raster depth, normal, material,
direct-light, shadow, indirect-light, or velocity bindings for PT radiance; the shared prefiltered
environment is a legitimate common input. Nevertheless, `DxrPathTracerDispatch::FrameInputs` rejects
missing raster attachments, RR activation remains presence-gated on the scene material G-buffer, and
diagnostic/fallback bundle modes can select raster depth or motion. The default complete PT bundle
does consume PT depth/motion and copied PT material guides; its defect is the hard raster presence/
fallback dependency and duplicate preparation, not that default RR silently consumes raster pixels.

Before RR evaluation, `PreparePathTracerRrBundle` performs five full-resolution preparation passes:
three format-compatible PT guide copies, R32 depth to D24 depth resolve, and alpha extraction from
the specular guide into a separate R16 hit-distance texture. The reconstruction API accepts R32
hardware depth, so D24 is not an inherent RR requirement. Directly tagging PT-owned resources is a
valid optimization only after state/lifetime and raster fallback contracts are explicit.

### 2.4 Intentionally preserved paths

- Raster mode, hybrid RT shadows/reflections/GI, DLAA, DLSS SR, and RR are product modes and must
  remain independently functional.
- Hybrid DXR dispatches are correctly gated by `!pathTracingActive`.
- SSAO, SSGI, SSR, legacy TAA, and the hybrid composite are correctly inactive in full PT.
- NRD is a hybrid denoiser and SVGF is used by SSR; neither is a steady-state full-PT dispatch.
  Their eager construction may affect startup memory, not the reported PT frame cost.
- Raster framebuffers remain appropriate for editor overlays, grids, selection, and non-PT modes.
  Isolating PT does not mean deleting those systems.

### 2.5 Feature selection, reconstruction, reset, and presentation references

- Render mode is selected in
  [`RayTracingSection.cpp:838`](../../../src/app/panels/lighting/RayTracingSection.cpp#L838) and stored
  by [`DxrSettings`](../../../src/engine/rendering/DxrSettings.h#L54). AA selects `DlssQuality::DLAA`
  or an SR quality in
  [`ScreenSpaceEffectsApply.cpp:284`](../../../src/engine/rendering/ScreenSpaceEffectsApply.cpp#L284);
  RR is an independent feature axis.
- PT dispatch enters at
  [`DxrPathTracerDispatch.cpp:386`](../../../src/engine/raytracing/DxrPathTracerDispatch.cpp#L386).
  ReSTIR temporal and spatial orchestration begins at
  [lines 420 and 515](../../../src/engine/raytracing/DxrPathTracerDispatch.cpp#L420), using
  [`restir_di_temporal.hlsl`](../../../assets/shaders/dxr/restir_di_temporal.hlsl) and the sibling
  spatial/boiling-filter libraries.
- Screen-space/post presentation is orchestrated from
  [`ScreenSpaceEffects.cpp:3068`](../../../src/engine/rendering/ScreenSpaceEffects.cpp#L3068).
  PT/raster temporal guides are selected at
  [`DlssResolvePass.cpp:109`](../../../src/engine/rendering/post/DlssResolvePass.cpp#L109), Streamline
  evaluates at [line 436](../../../src/engine/rendering/post/DlssResolvePass.cpp#L436), and the feature
  is selected as `kFeatureDLSS` or `kFeatureDLSS_RR` while both receive the current DLAA/SR quality at
  [`DlssContext.cpp:520`](../../../src/engine/rhi/DlssContext.cpp#L520). Display bloom/tonemap follows
  at [`DlssResolvePass.cpp:468`](../../../src/engine/rendering/post/DlssResolvePass.cpp#L468).
- AA-quality, RR-feature, and viewport resize paths invalidate/recreate relevant DLSS targets at
  [`ScreenSpaceEffects.cpp:1465`](../../../src/engine/rendering/ScreenSpaceEffects.cpp#L1465) and
  [line 3666](../../../src/engine/rendering/ScreenSpaceEffects.cpp#L3666). Camera-cut detection/reset
  occurs at [`DlssResolvePass.cpp:94`](../../../src/engine/rendering/post/DlssResolvePass.cpp#L94).
  PT producer/mode changes reach
  [`SetDxrPathTracerDisplay`](../../../src/engine/rendering/ScreenSpaceEffects.cpp#L1948) without an
  equivalent DLSS invalidation, which is PT-11.
- Application composition ends through
  [`Application.cpp:1755`](../../../src/app/core/Application.cpp#L1755),
  [`Renderer::EndFrame`](../../../src/engine/rendering/Renderer.cpp#L20), and
  [`GfxContext::EndFrame/Present`](../../../src/engine/rhi/GfxContext.cpp#L1032).

---

## 3. Findings summary

| ID | Title | Category | Sev. | Confidence | Primary symptom/risk | Main affected files/passes | Stage |
|---|---|---|---|---|---|---|---|
| PT-01 | Missing previous camera inputs | Temporal data | Critical | Confirmed | Glass/ReSTIR ghosting from wrong prior domain | `SceneRenderer`, `DxrPathTracerDispatch`, PT/RDI temporal shaders | S1 |
| PT-02 | Biased partial-transmission mixture | Light transport | Critical | Confirmed | Energy bias and rare high weights | `path_tracer.hlsl::SampleMaterialBounce` | S4A |
| PT-03 | Shading normal stored as geometric | Surface frame | Critical | Confirmed | Bad offsets, PDFs, boundaries, temporal surfaces | PT closest hit/payload/surface records | S3 |
| PT-04 | Radiance/guide dielectric normal split | RR guides | High | Confirmed | Glass guide/color disagreement | PT raygen transmission and guide generation | S3 |
| PT-05 | Incorrect scaled-normal transform | Geometry | High | Confirmed | Scaled-mesh shading/guide errors | `hit_shading.hlsli`, PT closest hit | S3 |
| PT-06 | Mip-0 normal map/incomplete ray cone | Texture footprint | High | Confirmed | Rough-metal/specular shimmer | PT closest hit and bounce-loop cone state | S3 |
| PT-07 | Rough dielectric treated as delta | BSDF/BTDF | High | Confirmed | Invalid rough-glass energy/PDF | `pt_dielectric.hlsli`, PT bounce sampling | S4A |
| PT-08 | Detached specular hit-distance trace | RR guides/performance | High | Confirmed | Rough-metal smear and extra traversal | PT primary guide block/RR bundle | S3 |
| PT-09 | Reconstruction consumes display exposure | DLSS/postprocess | High | Confirmed | Manual EV lost, especially under RR | `DlssResolvePass`, bloom/tonemap | S2 |
| PT-10 | Streamline token advances per evaluation | SDK temporal identity | High | Confirmed | Multi-viewport cadence/history errors | `DlssContext::Evaluate`, app frame | S1 |
| PT-11 | Mode switch retains incompatible history | History lifecycle | High | Confirmed | Cross-signal ghosting | render-mode UI, `ScreenSpaceEffects`, DLSS reset | S1 |
| PT-12 | Environment rotation applied twice on replay | ReSTIR/environment | High | Confirmed | Rotated-HDR temporal bias | PT initial sample, RDI temporal replay | S5 |
| PT-13 | Emissive NEE/hit integrands differ | Light sampling/MIS | High | Confirmed | Emissive bias and ghosting | `SceneRenderer::UploadEmissiveLights`, PT hit/NEE | S5 |
| PT-14 | Overlapping terminal environment estimate | Integrator/MIS | Medium | Confirmed | Depth-dependent bias/double filtering | PT NEE, miss, bounce-cap tail | S4A |
| PT-15 | Sanitation tied to firefly clamp | Numerical robustness | Medium | Confirmed | Non-finite HDR poisons history | `ClampRadiance`, PT accumulation/output | S4A |
| PT-16 | RGB transmissive visibility becomes scalar | Transmission | Medium | Confirmed | Gray shadows from colored glass | `TraceTransmissiveVisibility` | S4B |
| PT-17 | Invalid sun connection through curved solid | Transmission/NEE | Medium | Confirmed | Invented direct sun through lenses | PT transmissive visibility | S4B |
| PT-18 | One-bit medium state | Medium handling | Medium | Confirmed | Nested/camera-inside media unsupported | PT path state/dielectric boundaries | S4B |
| PT-19 | Non-reciprocal opaque diffuse Fresnel | Material model | Medium | Confirmed | Cross-mode/material energy mismatch | PT BSDF/direct/ReSTIR evaluations | S4B |
| PT-20 | Environment cell/PDF latitude mismatch | Environment PDF | Low | Confirmed | Small polar bias | PT environment CDF sampling/PDF | S4B |
| PT-21 | ReSTIR H1-H4 and P7 correlation | ReSTIR integration | High | Confirmed + measured | Broad emissive/rough-metal patches and cost | RDI temporal/spatial/filter stages | S5 |
| PT-22 | Global submission-fence serialization | D3D12 scheduling | High | Confirmed | Lost CPU/GPU overlap | `GfxContext::BeginFrame`, frame resources | S6 |
| PT-23 | Raster scene is produced/required in full PT | Mode isolation | Medium | Confirmed | Redundant work and hard coupling | shadow/G-buffer/PT input/post chain | S6 |
| PT-24 | Five redundant RR preparation passes | Bandwidth | Medium | Confirmed | Full-resolution copy/resolve cost | PT RR bundle preparation | S7 |
| PT-25 | Copied PT histories have no consumer | Resource/bandwidth | Medium | Confirmed | 42.19 MiB allocation; calculated traffic | PT history allocation/copies | S7 |
| PT-26 | Emissive data rebuilt every frame | CPU/upload | Medium | Confirmed | Scene-dependent CPU/copy cost | `UploadEmissiveLights` | S7 |
| PT-27 | Triangle LOD invariant recomputed per hit | Shader performance | Medium | Confirmed | Closest-hit/megakernel cost | `ComputeTriangleAlbedoLodConstant` | S7 |
| PT-28 | SER gate unnecessarily requires Tier 1.2 | Capability | High | Confirmed | Valid RT+SM 6.9 device rejected | `DxrCapabilities` | S8 |
| PT-29 | SER status/effectiveness is unproven | SER/performance | Medium | Confirmed structure; effect unmeasured | Misleading UI/possible neutral cost | capability UI, SM 6.9 RTPSO, PT trace loop | S8 |
| PT-30 | SDK optimal settings not used | DLAA/DLSS/RR sizing | Medium | Confirmed | Unsupported/suboptimal extents/resets | DLSS context/resolve resource allocation | S2 |
| PT-31 | Transmission replay uses current geometry | Dynamic temporal data | Medium | Confirmed | Moving pane/object ghosting | PT virtual motion/TLAS history | S1 |
| PT-32 | Relocated TLAS SRVs live to destruction | Descriptor lifetime | Low | Confirmed | Long-session heap exhaustion | `DxrDispatchContext` | S9 |
| PT-33 | ReSTIR GPU markers are mislabelled | Observability | Low | Confirmed | Wrong performance attribution | RDI temporal/spatial dispatch markers | S9 |

---

## 4. Detailed findings

### Contract/reference map

The code evidence below is primary. Where a finding also evaluates an external contract, the precise
reference is:

| Findings | Requirement type | Primary reference and pinpoint |
|---|---|---|
| PT-03, PT-05 | Geometry/math implementation | NVIDIA, [Solving Self-Intersection Artifacts in DirectX Raytracing](https://developer.nvidia.com/blog/solving-self-intersection-artifacts-in-directx-raytracing/), Listings 2-3: inverse-transpose normal handling and error-aware offsets. |
| PT-02, PT-13, PT-14, PT-19 | Rendering mathematics | Veach and Guibas, [Optimally Combining Sampling Techniques](https://www.cs.jhu.edu/~misha/ReadingSeminar/Papers/Veach95.pdf), estimator/PDF weighting; Veach, [non-symmetric scattering](https://graphics.stanford.edu/papers/non-symmetric/), reciprocity/adjoint conventions. |
| PT-07 | Rendering mathematics | Walter et al., [Microfacet Models for Refraction through Rough Surfaces](https://www.graphics.cornell.edu/~bjw/microfacetbsdf.pdf), sections 5.1-5.2; PBRT 4e, [Dielectric BSDF](https://pbr-book.org/4ed/Reflection_Models/Dielectric_BSDF); Heitz, [GGX VNDF sampling](https://www.jcgt.org/published/0007/04/01/paper.pdf). |
| PT-08 | SDK integration requirement/sample pattern | NVIDIA Streamline 2.12, [DLSS RR Programming Guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_RR.md), section 4.1.9; NVIDIA, [vk_denoise_dlssrr sample](https://github.com/nvpro-samples/vk_denoise_dlssrr), specular hit-distance/motion implementation. |
| PT-09 | SDK integration requirement | NVIDIA, [DLSS Programming Guide](https://github.com/NVIDIA/DLSS/blob/main/doc/DLSS_Programming_Guide_Release.pdf), 2026-03-31, section 3.9; Streamline 2.12 DLSS RR guide, section 3.7. |
| PT-10 | SDK integration requirement | NVIDIA Streamline 2.12, [Programming Guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuide.md), section 2.10 and its multi-viewport example. |
| PT-17 | Rendering mathematics/supported algorithm | Jakob and Marschner, [Manifold Exploration: A Markov Chain Monte Carlo Technique for Rendering Scenes with Difficult Specular Transport](https://doi.org/10.1145/2601097.2601132), specular-chain connection problem. The project need not implement this algorithm; it may declare the connection unsupported. |
| PT-21 | Official reference implementation/guidance | NVIDIA, [RTXDI](https://github.com/NVIDIA-RTX/RTXDI), `RestirGI.md`, `NoiseAndBias.md`, and library temporal/spatial reuse implementations; repository-specific evidence remains in [P7 diagnostics](p7-diagnostics-tracker.md). |
| PT-22 | D3D12 resource-lifetime guidance | Microsoft, [Fence-Based Resource Management](https://learn.microsoft.com/en-us/windows/win32/direct3d12/fence-based-resource-management), frame-resource/ring-buffer fence ownership. |
| PT-23, PT-24, PT-30 | SDK resource/extent requirements | NVIDIA Streamline 2.12, [DLSS guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS.md), optimal settings and tags; [DLSS RR guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_RR.md), sections 3 and 4.1 (including supported R32/D32/D24 depth and no RR DRS). |
| PT-28, PT-29 | API requirement and optional optimization guidance | Microsoft, [DXR specification v1.44](https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html), 2026-06-26 SER/Options22 sections; Microsoft, [HLSL SER proposal](https://microsoft.github.io/hlsl-specs/proposals/0027-shader-execution-reordering/); NVIDIA, [SER whitepaper](https://developer.nvidia.com/sites/default/files/akamai/gameworks/ser-whitepaper.pdf). |

PT-01, PT-04, PT-06, PT-11/12, PT-15/16/18, PT-20, PT-25-27, and PT-31-33 are established by
producer/consumer mismatch, implemented formula, reachability, or resource lifetime; they do not
depend on treating an optional sample pattern as an API mandate.

### 4.1 Temporal state and reconstruction contracts

#### PT-01 — missing previous view and previous camera position

**Evidence: C; causal confidence: high; severity: critical.**

`DxrPathTracerDispatch::FrameInputs` defaults `prevView` to identity and `prevCameraPos` to zero in
[`DxrPathTracerDispatch.h`](../../../src/engine/raytracing/DxrPathTracerDispatch.h#L49).
`SceneRenderer` supplies `motionHistoryValid` and `prevViewProjection`, but not those two fields, at
[`SceneRenderer.cpp:1143`](../../../src/app/scene/SceneRenderer.cpp#L1143). The dispatch accepts the
defaults on a valid-history frame and constructs previous-view data from them in
[`DxrPathTracerDispatch.cpp:245`](../../../src/engine/raytracing/DxrPathTracerDispatch.cpp#L245).

The values are live. `ComputeTransmissionVirtualMotion` consumes them in
[`path_tracer.hlsl:580`](../../../assets/shaders/dxr/path_tracer.hlsl#L580), and ReSTIR temporal replay
uses previous camera position for prior-view receiver reconstruction in
[`restir_di_temporal.hlsl:773`](../../../assets/shaders/dxr/restir_di_temporal.hlsl#L773).
Consequently, a stable camera located away from the world origin still presents the temporal stages
with a synthetic previous camera at the origin. This directly invalidates glass virtual motion and
previous-domain light/receiver terms before any disocclusion threshold or blend constant is applied.

**Remediation contract:** pass one authoritative previous-camera structure containing view,
projection, inverse view-projection, camera position, jitter, and validity. Derive all consumers from
it and assert that the independently supplied matrices/position describe the same camera. Reset the
history instead of substituting identity/zero when any member is unavailable.

**Gate:** a static camera away from origin must generate zero virtual motion on static opaque
surfaces and stable expected parallax through a pane; a lateral glass orbit must reproject a known
checker; ReSTIR current/previous target recomputation must agree in a static scene.

#### PT-09 — reconstruction exposure is confused with display exposure

**Evidence: C; severity: high.**

[`DlssResolvePass.cpp:351`](../../../src/engine/rendering/post/DlssResolvePass.cpp#L351) converts manual
EV to `exp2(EV)` and supplies it to reconstruction. After any successful DLSS/RR evaluation, the
post chain forces bloom and the renderer tonemapper to EV 0 at
[`DlssResolvePass.cpp:468`](../../../src/engine/rendering/post/DlssResolvePass.cpp#L468), on the premise
that DLSS already applied exposure. The current DLSS contract describes exposure as reconstruction
guidance normally shared with the later tonemapper, not as final display exposure. The RR integration
contract does not support exposure in this path. The result is a confirmed pipeline error: manual EV
is omitted downstream, and behavior differs by reconstruction mode/preset.

**Remediation contract:** represent pre-exposure/reconstruction exposure and display/tonemap EV as
separate values. Feed only supported guidance to the SDK and apply the renderer's authored display EV
exactly once after direct output and every Streamline feature/quality combination. Preserve bloom's
declared working space.

**Gate:** the same static HDR patch rendered at EV `-2`, `0`, and `+2` must have the declared four-stop
relationship in direct, DLSS+DLAA, DLSS+each SR quality, RR+DLAA, and RR+each supported SR quality,
without history settling changing the final exposure.

#### PT-10 — Streamline token is evaluation-scoped rather than frame-scoped

**Evidence: C; severity: high.**

[`DlssContext.cpp:422`](../../../src/engine/rhi/DlssContext.cpp#L422) increments an internal frame index
for every `Evaluate` call unless the optional submission index path is selected. The application can
render Scene and Game views within one `BeginFrame`, so the two viewports receive different tokens;
each individual viewport can also observe indices that jump by two. Streamline defines the token as
an application-frame identity and demonstrates multiple viewports with the same token.

**Remediation contract:** acquire/cache the Streamline frame token once in `BeginFrame`, pass it to
all feature evaluations in that application frame, and keep viewport identity separate. Do not use a
global per-evaluation counter as a temporal-frame proxy.

**Gate:** logs for simultaneous Scene/Game rendering show one shared token per application frame,
monotonic by one between frames, while viewport IDs and per-viewport resources remain distinct.

#### PT-11 — rendering-mode transitions retain reconstruction history

**Evidence: C; severity: high.**

AA-mode, preset, RR, and resize changes generally request a DLSS reset. Switching the renderer among
raster, hybrid, and full PT changes the signal and guide owner but does not invalidate the
`ScreenSpaceEffects` reconstruction history. `SetDxrPathTracerDisplay` resets PT accumulation and
diagnostics only when inactive; diagnostic guide switches also do not reset DLSS history. The next
evaluation can therefore reuse pixels produced by a semantically different pipeline.

The relevant split is visible between the ordinary history reset paths in
[`ScreenSpaceEffects.cpp:1440`](../../../src/engine/rendering/ScreenSpaceEffects.cpp#L1440) and
[AA/RR setters at line 3666](../../../src/engine/rendering/ScreenSpaceEffects.cpp#L3666), versus
[`SetDxrPathTracerDisplay`](../../../src/engine/rendering/ScreenSpaceEffects.cpp#L1948), which changes
the producer without invalidating DLSS history. The diagnostic
[`SetPtRrBundleMode`](../../../src/engine/rendering/ScreenSpaceEffects.cpp#L1868) similarly changes
guide ownership but resets only PT diagnostics.

**Remediation contract:** define a history compatibility key including render producer, guide
producer/version, reconstruction feature/preset, render/output extent, camera cut, and diagnostic
signal. Any incompatible change must set the next SDK reset flag and clear the relevant TAA, bloom,
PT accumulation, and ReSTIR state according to ownership.

**Gate:** instrument the key and reset reason; every raster/hybrid/PT producer switch, DLSS/RR feature
switch, DLAA/SR-quality switch, and PT-guide diagnostic transition must reset exactly once, while
compatible steady-state frames do not.

#### PT-30 — fixed reconstruction ratios replace optimal-settings queries

**Evidence: C; severity: medium.**

The integration loads the DLSS optimal-settings entry point at
[`DlssContext.cpp:245`](../../../src/engine/rhi/DlssContext.cpp#L245) but does not use it for the active
allocation policy; the corresponding RR recommendation path is not used. Instead, fixed ratios such
as 0.667, 0.58, 0.5, and 0.333 in
[`ScreenSpaceEffects.cpp:1378`](../../../src/engine/rendering/ScreenSpaceEffects.cpp#L1378) determine
internal resolution. Current DLSS/Streamline integration
guides direct applications to query recommended settings for the selected output size, quality, and
feature. RR also disallows arbitrary dynamic-resolution behavior. The hardcoded policy is especially
risky for DLSS SR modes; DLAA's 1.0 ratio is less exposed.

The 64-phase jitter table satisfies the representative RR+DLAA combination because RR requires at
least 32 phases. For the ordinary `kFeatureDLSS` path it also covers DLAA, Quality, Balanced, and
Performance, but is short of the documented 72 phases for Ultra Performance. Treat that as a low-
severity subfinding only for `kFeatureDLSS` Ultra Performance.

**Remediation contract:** query and cache SDK-recommended render extent/ranges per viewport, output
extent, feature, and quality mode; reallocate and reset histories on changes. Keep an explicit
fallback only for API/query failure and log it.

**Gate:** every exposed `kFeatureDLSS`/`kFeatureDLSS_RR` x DLAA/SR-quality combination reports its SDK
recommendation, uses supported extents, cycles the required jitter sequence, and survives resize and
feature/quality transitions.

#### PT-31 — dynamic transmission has no previous-geometry domain

**Evidence: C; severity: medium.**

Transmission virtual-motion replay in
[`path_tracer.hlsl:580`](../../../assets/shaders/dxr/path_tracer.hlsl#L580) and
[`TraceTransmissionGuide`](../../../assets/shaders/dxr/pt_dielectric.hlsli#L243) traces the current
TLAS with current transforms. It can recover a
camera-only optical path in suitable static geometry, but it cannot reconstruct where a ray passed
through a moving pane or where moving background geometry was on the previous frame. The current
guide motion is only a fallback and is not an exact previous optical path.

**Remediation contract:** either provide prior transforms/geometry for supported optical replay or
detect dynamic boundary/background participation and reject/reset the transmission history. Document
the supported dynamic scope instead of silently accepting invalid replay.

**Gate:** separate tests for moving checker behind a static pane, moving pane, moving receiver, and
camera-only motion show correct reuse or deterministic rejection.

### 4.2 Surface frames, ray footprints, and dielectric transport

#### PT-03 — interpolated shading normal is stored as geometric normal

**Evidence: C; causal confidence: high; severity: critical.**

Closest hit calls `ComputeWorldShadingNormal` and packs its interpolated vertex-normal result into the
payload field named `normalOct`/geometric normal in
[`path_tracer.hlsl:2611`](../../../assets/shaders/dxr/path_tracer.hlsl#L2611). A true face normal helper
is used only as a fallback. That pseudo-geometric normal drives ray offsets, front/back and medium
logic, ReSTIR surface records, emissive hit PDFs/Jacobians, and dielectric decisions. These consumers
need a stable macro/geometry frame; an interpolated, potentially artist-authored normal is not that
frame.

**Remediation contract:** carry a true oriented geometric normal `Ng` separately from shading normal
`Ns`. Use `Ng` for offsets, sidedness, boundary/medium transitions, geometric PDFs and Jacobians;
use a hemisphere-consistent `Ns` for the BSDF. Define the optical macro-normal policy explicitly for
smooth authored glass rather than blindly faceting every interface, and use the same declared
interface frame in radiance and guides.

**Gate:** add `Ng`/`Ns` AOVs and tests for flat/smooth triangles, mirrored winding, back-face hits,
scaled analytic primitives, and emissive-PDF reconstruction. No secondary ray may be offset through
the wrong side of the true surface.

#### PT-04 — radiance and transmission guides choose different interface normals

**Evidence: C; causal confidence: high; severity: high.**

Ray generation passes the payload shading normal through normal mapping and
`FinalizeShadingNormal`, including view-facing correction, before
`SampleMaterialBounce` at
[`path_tracer.hlsl:2322`](../../../assets/shaders/dxr/path_tracer.hlsl#L2322). The transmission guide is
built earlier from `hitNormalGeom` around
[`path_tracer.hlsl:2159`](../../../assets/shaders/dxr/path_tracer.hlsl#L2159). A normal-mapped or grazing
interface can therefore send the radiance event and its virtual-motion guide in different
directions. This regresses the invariant claimed as fixed in the older audit.

**Remediation contract:** after PT-03 defines the interface frame, compute the first dielectric event
once and share its incident point, oriented macro normal, eta pair, event type, and direction with
both transport and guide generation. View-bending a transport interface normal should not be a
hidden guide-only/radiance-only operation.

**Gate:** capture the first transmitted/reflected direction in both paths for normal-mapped and
grazing panes; they must agree within a documented epsilon and event convention.

#### PT-05 — world normal transform is wrong under non-uniform scale

**Evidence: C; severity: high.**

[`hit_shading.hlsli:260`](../../../assets/shaders/dxr/hit_shading.hlsli#L260) transforms normals with the
object-to-world 3x3 matrix. Normals require the inverse-transpose for non-uniform scale/shear. The
raster vertex path already implements the correct contract in `hlsl_common.hlsl` and `lit.vs.hlsl`,
so the same object can disagree between raster, PT radiance, and PT guides.

**Remediation contract:** centralize a robust object-normal-to-world helper using the available
world-to-object transpose, including normalization and determinant/orientation handling, and share
its convention across all hit shaders.

**Gate:** non-uniformly scaled sphere/capsule and a sheared test mesh must match analytic normals and
the raster reference in `Ng`/`Ns` AOVs.

#### PT-06 — mip-0 normal maps and incomplete ray-cone propagation

**Evidence: C; causal confidence: high; severity: high.**

Closest hit calls `ApplyWorldNormalMap(..., 0.0)` around
[`path_tracer.hlsl:2632`](../../../assets/shaders/dxr/path_tracer.hlsl#L2632), while other material maps
use a ray-cone-derived LOD. The cone grows by camera spread times travel distance but is not widened
according to diffuse, GGX, or refractive scattering. Secondary glossy hits can therefore select much
finer normal-map detail than their pixel footprint supports. A high-frequency normal map on a rough
metal is a direct recipe for temporally unstable highlights even with a static unbiased estimator.

**Remediation contract:** make the footprint/LOD part of the hit contract, use it for the normal map,
and propagate cone width/spread through reflection, rough scattering, and refraction with bounded
fallbacks at singular/grazing events. Keep derivatives unavailable to ray tracing out of shared
helpers unless their substitute is explicit.

**Gate:** footprint and selected-LOD AOVs plus a normal-map frequency/roughness/distance sweep must
show monotonic coarsening and reduced shimmer without material-energy drift.

#### PT-02 — partial-transmission reference mixture is biased

**Evidence: C; severity: critical.**

The reference branch defines dielectric selection weight as
`transmission * (1 - metallic)`. `SampleMaterialBounce` selects dielectric with probability `w` and
then divides the already selected dielectric contribution by `w`; the opaque branch similarly
divides by `1-w` in [`path_tracer.hlsl:1519`](../../../assets/shaders/dxr/path_tracer.hlsl#L1519).
Because the lobe coefficient equals the selection probability, coefficient/probability is one. The
extra divisions make the expectation `f_dielectric + f_opaque`, independent of the authored mixture,
and produce rare large weights as `w` approaches an endpoint.

The real-time path intentionally follows the glass branch and scales it by `w`, while relying on
other terms for the opaque residual. That is a separate biased product policy and must not be used as
the correctness oracle for the reference estimator.

**Remediation contract:** either remove the extra probability divisions for this exact coefficient
mixture or express every branch as explicit `(coefficient * sampled_lobe) / selection_pdf`. Keep the
real-time heuristic named and measured separately.

**Gate:** deterministic and Monte Carlo tests at transmission `0`, `.01`, `.5`, `.99`, and `1` match
the explicit weighted sum; endpoint variance remains finite and the reference image converges to the
same energy. For representative persistent over-bright samples from section 9, the trace records the
selected branch probability and throughput before and after mixture compensation.

#### PT-07 — rough dielectric is not a valid continuous BSDF

**Evidence: C; severity: high.**

[`pt_dielectric.hlsli:82`](../../../assets/shaders/dxr/pt_dielectric.hlsli#L82) samples a GGX VNDF
micro-normal, but `SampleDielectricInterface` labels reflection and transmission as delta events with
a sentinel PDF and unit throughput around
[`pt_dielectric.hlsli:145`](../../../assets/shaders/dxr/pt_dielectric.hlsli#L145). A rough microfacet
reflection/BTDF requires continuous directional PDFs, `D`/`G` terms, the reflection/refraction
Jacobian, Fresnel, and the chosen eta/radiance convention. Unit-weight delta bookkeeping is valid only
for the smooth limit.

**Remediation contract:** implement one canonical rough dielectric model, such as the Walter/PBRT
microfacet formulation, with matched sample/evaluate/PDF functions and an explicit smooth-delta
threshold. Declare transport mode and eta scaling. Share it with MIS logic and tests.

**Gate:** white-furnace energy, sample-vs-evaluate PDF, reciprocity/adjoint convention, normal/grazing
incidence, total internal reflection, and nested eta-pair tests pass over a roughness sweep. The
section-9 persistent-sample trace also records the sampled event/direction, BSDF or BTDF value, and
directional PDF at the offending bounce.

#### PT-16 — transmissive direct visibility collapses spectral attenuation

**Evidence: C; severity: medium.**

The camera path carries RGB Beer attenuation, but `TraceTransmissiveVisibility` computes RGB
attenuation and then averages it to one scalar around
[`path_tracer.hlsl:377`](../../../assets/shaders/dxr/path_tracer.hlsl#L377). Colored glass therefore
casts a gray analytic/emissive direct-light shadow even though its viewed transmission is colored.

**Remediation contract:** return `float3` visibility/transmittance, preserving RGB Beer attenuation
through every crossed supported boundary; keep binary opaque visibility as a specialized fast path.

**Gate:** analytic RGB absorbing slabs at multiple thicknesses match `exp(-sigma * distance)` in
camera and shadow paths.

#### PT-17 — curved solid dielectric cannot make the current directional-light connection

**Evidence: C; severity: medium and scope-dependent.**

For a solid transmissive shadow, the code refracts the selected sun direction through boundaries. If
the resulting ray misses geometry, it treats the original directional-light sample as visible without
checking that the final direction is still inside that light's directional support. A thin parallel
slab preserves direction; a curved lens generally does not. This is not a valid ordinary next-event
connection for curved specular interfaces.

The behavior is in
[`TraceTransmissiveVisibility`](../../../assets/shaders/dxr/pt_dielectric.hlsli#L342): its miss return
accepts accumulated transmittance after `rayDir` may have changed, with no final sun-cone comparison.

**Remediation contract:** either declare directional NEE through curved solid specular boundaries
unsupported and omit it, or implement an appropriate specular-manifold/connection method. Preserve
the valid thin/parallel case as an explicit specialization.

**Gate:** plane-parallel, wedge, and curved-lens tests demonstrate the supported policy without
inventing sun energy.

#### PT-18 — one-bit medium tracking limits supported scenes

**Evidence: C; severity: medium and scope-dependent.**

The path stores one `pathInMedium` Boolean and one tint/absorption state beginning at
[`path_tracer.hlsl:1747`](../../../assets/shaders/dxr/path_tracer.hlsl#L1747), toggling them at dielectric
crossings. It cannot represent nested dielectrics, overlapping volumes, camera-inside initialization,
or non-air interfaces robustly.

**Remediation contract:** first state the product scope. If those scenes are supported, track a small
boundary/medium stack keyed by instance/material with eta and absorption; define mismatch recovery.
If they are not supported, reject/diagnose them and keep the single-solid fast path.

**Gate:** air/glass/air, camera-inside, nested liquid/glass, overlapping solids, and unmatched-boundary
tests either render correctly or produce the declared diagnostic fallback.

#### PT-19 — opaque diffuse Fresnel is view-only and non-reciprocal

**Evidence: C; severity: medium.**

The opaque diffuse model scales its diffuse response using Fresnel evaluated from `NdotV` in the
duplicated material evaluations around
[`path_tracer.hlsl:877`](../../../assets/shaders/dxr/path_tracer.hlsl#L877) and
[line 1431](../../../assets/shaders/dxr/path_tracer.hlsl#L1431). Swapping
incoming and outgoing directions changes that value, so the resulting scattering function is not
reciprocal. The raster material path uses a half-vector-dependent Fresnel convention instead. The
current sampler/evaluator can be internally paired and still converge to a nonphysical, cross-mode
inconsistent material model.

**Remediation contract:** choose a documented reciprocal diffuse/specular energy-compensation model,
then update the PT sampler, evaluator, direct-light evaluation, and duplicated ReSTIR target
evaluation together. Do not “fix” only one copy.

**Gate:** direction-swap reciprocity, furnace energy, sample/evaluate agreement, and a raster/PT
material comparison pass over metallic/roughness/base-color grids.

#### PT-15 — numerical sanitation is coupled to a biased firefly clamp

**Evidence: C; severity: medium.**

`ClampRadiance` in [`hit_shading.hlsli`](../../../assets/shaders/dxr/hit_shading.hlsli#L203) first
sanitizes NaN/Inf and then applies a luminance cap. The path tracer calls it only when the firefly
clamp toggle is enabled. Turning off the optional, deliberately biased luminance clamp therefore also
turns off mandatory finite-value sanitation. FP16 guide/output paths can then propagate infinities or
NaNs into temporal reconstruction. The representative configuration has the clamp enabled, so this
is not the primary explanation for its current symptoms, but the toggle contract is still wrong.

**Remediation contract:** split an always-on `SanitizeRadiance` from an independently measured and
labelled optional clamp. Count non-finite replacements and clamp events separately.

**Gate:** injected NaN/Inf/overflow values never reach any PT/RR output with the firefly clamp both
enabled and disabled; normal finite HDR values remain unchanged when only sanitation runs. The
section-9 persistent-sample trace records finite-value sanitation, clamp input/output and placement,
and the final accumulation write.

### 4.3 Environment, emissive lighting, and ReSTIR

#### PT-12 — environment temporal replay applies rotation twice

**Evidence: C; severity: high.**

Initial environment sampling selects a texture-space UV from the CDF and rotates the sampled world
direction. The path tracer then stores `DirectionToEquirectUv(rotatedWorldDirection)` around
[`path_tracer.hlsl:1367`](../../../assets/shaders/dxr/path_tracer.hlsl#L1367). Temporal replay interprets
that stored value as the original texture UV and applies the environment rotation again in
[`restir_di_temporal.hlsl:269`](../../../assets/shaders/dxr/restir_di_temporal.hlsl#L269). At nonzero
rotation, the replayed candidate represents a different texel/direction.

**Remediation contract:** store the canonical CDF texture UV, or invert the rotation before encoding.
Give environment reservoir samples an unambiguous representation and a single encode/decode helper.

**Gate:** a bright single-texel environment at several rotations round-trips initial sample →
reservoir → temporal replay to the same UV, direction, radiance, and PDF.

#### PT-13 — emissive-light sampling and hit-side evaluation integrate different emitters

**Evidence: C; severity: high.**

The CPU emissive-light upload in
[`SceneRenderer.cpp:1041`](../../../src/app/scene/SceneRenderer.cpp#L1041) builds triangle power and
radiance from constant material emissive values. A BSDF ray that hits the same triangle resolves the
emissive texture in the hit shader. Thus NEE and hit-side MIS do not evaluate the same spatial
integrand. The CPU sampler also builds a flat triangle normal while hit-side PDF reconstruction uses
the interpolated pseudo-geometric normal described in PT-03. The selection PDF, conditional PDF, and
emission value are not one consistent light definition.

**Remediation contract:** define a texture-aware emission evaluation and importance approximation
that is shared by CPU/GPU light construction and hit evaluation, or exclude textured emitters from
that proposal and retain an unbiased BSDF-hit fallback. Use the same true face orientation and
one-/two-sided policy in selection, sampling, and hit-PDF reconstruction.

**Gate:** constant, checker-textured, UV-scaled, one-sided, two-sided, flat-normal, and smooth-normal
triangle emitters produce matching sampled and hit-side PDFs and stable reference energy. For any
section-9 persistent sample involving emission, the trace records emitted radiance, light-selection
and directional PDFs, the competing BSDF PDF, and the applied MIS weights.

#### PT-14 — terminal environment term duplicates path contributions

**Evidence: C; severity: medium.**

The bounce loop performs environment NEE at ordinary vertices, then at the bounce cap adds a
prefiltered reflection/transmission environment tail around
[`path_tracer.hlsl:2288`](../../../assets/shaders/dxr/path_tracer.hlsl#L2288) without a complementary MIS
partition. This is a second estimator for overlapping paths. In the no-CDF miss path, sampling a
roughness-prefiltered cubemap after a BSDF direction also filters the lobe twice. The older audit
raised the same family of issue; the active source still contains the terminal contribution.

The real-time renderer may intentionally retain a named finite-depth approximation, but the reference
mode cannot use it as an unbiased ground truth.

**Remediation contract:** remove the overlapping reference contribution or derive a complementary,
documented estimator. Sample radiance from an unfiltered environment for a BSDF-sampled direction;
reserve prefiltered maps for an explicit product approximation.

**Gate:** uniform-environment and white-furnace results are invariant, within Monte Carlo error, as
the maximum bounce count crosses the terminal path; CDF-on/off converge to the same reference. The
section-9 persistent-sample trace records every environment/BSDF proposal PDF, MIS weight, terminal
contribution, and resulting throughput before the final accumulation write.

#### PT-20 — environment within-cell PDF mismatch

**Evidence: C; severity: low.**

The CDF path in
[`pt_env_light.hlsli:131`](../../../assets/shaders/dxr/pt_env_light.hlsli#L131) chooses a latitude row
and samples latitude uniformly within the cell, while the reported
solid-angle density uses a midpoint cosine/cell-area approximation. The mismatch is small at the
current 512×256 distribution but is largest near the poles and is a real PDF inconsistency.

**Remediation contract:** sample uniformly in the cell's sine-latitude measure or report the exact
piecewise density of the implemented sampler. Keep CPU distribution construction and shader PDF
normalization in one tested convention.

**Gate:** numerical integration of the reported environment PDF equals one and per-bin observed
frequencies, including polar rows, match expected probability.

#### PT-21 — ReSTIR contract deviations remain; P7 moves error to broad patches

**Evidence: C/M; severity: high.**

The detailed experiment ledger is [P7 diagnostics tracker](p7-diagnostics-tracker.md); the production
phase contract is [ReSTIR roadmap](restir-production-roadmap.md). This audit confirms the active
source in [`restir_di_temporal.hlsl`](../../../assets/shaders/dxr/restir_di_temporal.hlsl) and its
shared [`restir_di.hlsli`](../../../assets/shaders/dxr/restir_di.hlsli) still contains the following
high-priority deviations:

- **H1:** a native connection is shadow-tested redundantly;
- **H2:** P6 temporal reuse omits the RTXDI BASIC previous-domain normalization term;
- **H2b:** reprojection accepts the first generic search candidate, then can reject it on depth,
  instead of continuing to other candidates;
- **H3:** Jacobian use is not one consistent proposal/target policy;
- **H4:** binary visibility is folded into temporal targets and then traced again, mixing target and
  final-visibility policy.

PT-01 makes H2 materially worse because the previous receiver/view can be reconstructed from a
world-origin camera. Fixing thresholds before the previous-camera and target/PDF contracts are valid
would tune around a deterministic domain error.

The recorded B0 measurements are also diagnostic. Relative to P5+P6, full P7 reduced some fine/local
variation but increased motion-reprojected four-pixel neighbor correlation by about 133%,
low-frequency variance ratio by 38.70%, and blurred-hot coverage by 40.63%. Matched diagnostic
effective FPS was 50.024/48.753 for P5+P6 static/orbit and 36.318/36.634 for full P7; full-P7 orbit
throughput was therefore 24.86% below baseline. These are wall-clock-derived effective-FPS
measurements, not isolated production GPU stage timings.

**Remediation contract:** after PT-01 and the canonical light/surface representation are fixed,
address H1, H2, H2b, and H3 in that order; make H4 an explicit visibility-reuse policy with unbiased
fallback. Only then tune spatial radius, candidate count, history length, or boiling filters.

**Gate:** the canonical P7 suite must improve local noise without increasing low-frequency variance,
blurred-hot coverage, neighbor correlation, temporal chroma, leaks, or mean-energy error beyond its
declared thresholds, and must include isolated GPU timings.

### 4.4 RR guide semantics

#### PT-08 — specular hit distance is detached from the sampled radiance event

**Evidence: C; causal confidence: high; severity: high.**

Before the stochastic bounce is selected, the path tracer launches a separate deterministic perfect
mirror trace for opaque surfaces under a roughness threshold around
[`path_tracer.hlsl:2227`](../../../assets/shaders/dxr/path_tracer.hlsl#L2227). For roughness in the blend
band it linearly interpolates `maxDistance` and the mirror hit distance. That blended number is not
the hit distance of any ray. The later radiance event may be GGX specular, diffuse, or another lobe
and may hit unrelated geometry. The extra guide trace also adds a TLAS traversal/closest-hit path for
many opaque primary pixels.

The RR contract defines specular hit distance relative to the primary surface and the first sampled
secondary/specular event; NVIDIA's sample integration notes that specular motion vectors can be more
important than a synthetic hit distance. An omitted/non-specular convention is safer than a precise
but false guide.

**Remediation contract:** write the hit distance of the actual sampled first specular event when it
exists and is semantically supported, otherwise use the SDK's omitted/non-specular convention. Do not
blend unrelated distances. Pair the result with correct specular/virtual motion.

**Gate:** A/B current guide, omitted guide, exact sampled-event distance, and specular motion vectors
on mirror, rough-metal, mixed-lobe, thin glass, and disocclusion scenes. Require both improved temporal
metrics and lower/equal PT GPU time before retaining an extra trace.

### 4.5 CPU/GPU scheduling, redundant work, and resource lifetime

#### PT-22 — global fence wait serializes successive frames

**Evidence: C/M; severity: high.**

[`GfxContext::BeginFrame`](../../../src/engine/rhi/GfxContext.cpp#L840) waits for the maximum of the
selected frame-context fence, the latest global submission fence, and the upload-ring fence. Because
the global submission value advances at the end of every frame, the next `BeginFrame` waits for the
immediately preceding GPU submission even when the other frame context is free. `FrameCount == 2`
therefore does not provide CPU-recording/GPU-execution overlap.

The existing preliminary captures record “CPU prior-GPU wait” around 13.092–13.778 ms. That counter
includes the present implementation and is evidence of a large wait, but it is not proof that every
millisecond is recoverable: present pacing, resource hazards, and workload balance must be separated
in a timeline capture.

**Remediation contract:** wait only for the fence protecting the frame context and ring segment being
reused. Before changing it, audit every resource whose safety is currently inherited from the global
drain: command allocators, upload-ring ranges, descriptor tables, TLAS result/scratch/update lifetime,
readback resources, and resize/capacity growth. Descriptor-capacity growth currently rewrites all
ring slots and must gain explicit fence ownership.

**Gate:** D3D12 debug layer and GPU-based validation are clean under stress; a PIX/Nsight timeline
shows frame N+1 CPU recording overlapping frame N GPU work; no frame resource is overwritten before
its fence; p95/frame pacing improves without output changes.

#### PT-23 — full PT renders and depends on an unused raster scene

**Evidence: C/M; severity: medium.**

Full PT still renders cascaded shadow maps and all scene geometry into the raster G-buffer through
[`SceneRenderer.cpp:2079`](../../../src/app/scene/SceneRenderer.cpp#L2079). The active
PT shader declares bindings inherited from shared layouts but does not read raster depth, normal,
material, direct, shadow, indirect, or velocity for radiance. Nevertheless, CPU validation and parts
of the DLSS path require those attachments; the explicit PT presence checks are at
[`DxrPathTracerDispatch.cpp:176`](../../../src/engine/raytracing/DxrPathTracerDispatch.cpp#L176).
This is a structural dependency, not evidence that every
raster pass is currently expensive.

The existing preliminary captured scene reports about 0.001 ms for scene raster and 0.026 ms for
shadows, so those GPU passes are not the main slowdown in that scene. Their CPU cost and behavior in
geometry-heavy/editor scenes remain unmeasured.

**Remediation contract:** introduce an explicit PT input/root contract and PT-owned reconstruction
bundle, then skip scene-object G-buffer and CSM work only in full PT. Preserve viewport targets,
editor overlays/grid/selection, raster mode, and hybrid modes. Supply a deliberate fallback for any
feature that truly needs raster data.

**Gate:** shader reflection/source search proves no hidden reads; full PT starts and renders with the
scene G-buffer/CSM producer disabled; raster/hybrid golden scenes and every DLSS/RR x DLAA/SR
combination remain intact; CPU and GPU captures show a non-negative result.

#### PT-24 — RR bundle preparation copies already suitable resources

**Evidence: C; severity: medium.**

[`PreparePathTracerRrBundle`](../../../src/engine/rendering/ScreenSpaceEffects.cpp#L1797) performs a
depth conversion and three guide blits; the subsequent RR-guide generation extracts hit distance,
for five full-resolution passes in total. The PT color guides and normal/roughness already have the
formats needed by the downstream tag path; supported RR depth formats include R32/D32/D24. Only
packing/extraction that corresponds to an actual SDK channel contract should remain.

**Remediation contract:** tag PT resources directly with correct resource states, extents, and
lifetime. If specular albedo and hit distance must remain in distinct tagged resources, generate that
layout once in the path tracer rather than copy/extract afterward. Retain independent raster/hybrid
guide targets where they are genuinely different.

**Gate:** resource-state validation is clean; SDK tags/formats match the active guide contract;
pixel/AOV comparison is equivalent; narrow GPU markers show fewer passes and lower bandwidth/time.

#### PT-25 — two copied PT history textures have no active consumer

**Evidence: C; severity: medium.**

`m_ptPrevDepth` (R32) and `m_ptPrevNormalRoughness` (RGBA16F) are allocated and copied each real-time
PT frame. Repository-wide getters/searches show no consumers; ReSTIR uses the separate current and
previous surface-record buffers. The allocations are at
[`DxrDispatchContext.cpp:883`](../../../src/engine/raytracing/DxrDispatchContext.cpp#L883) and copies
at [line 1127](../../../src/engine/raytracing/DxrDispatchContext.cpp#L1127). At 2560×1440 these two
textures occupy approximately 42.19 MiB and
their copy reads plus writes approximately 84.38 MiB per frame. Those are format/extent calculations,
not measured bandwidth cost.

**Remediation contract:** confirm zero runtime/debug/readback consumers and remove the allocations,
copies, barriers, and stale API together. If a planned consumer exists, defer allocation until that
feature is active.

**Gate:** source/reflection search and a captured frame show no reads; PT/ReSTIR/RR output and history
reset tests are unchanged; memory and copy markers decrease as predicted.

#### PT-26 — emissive distributions are rebuilt every PT frame

**Evidence: C; severity: medium.**

`RenderPostProcessPass` calls `UploadEmissiveLights` every PT frame at
[`SceneRenderer.cpp:1861`](../../../src/app/scene/SceneRenderer.cpp#L1861). The implementation transforms
emissive triangles, computes weights/lookup/alias data, and uploads five buffers even for a static
scene. This work scales with emissive geometry and is separate from the estimator correctness issue
in PT-13.

**Remediation contract:** fingerprint emissive topology, material/emission state, relevant transforms,
and environment/light policy. Rebuild only on a fingerprint change and populate every in-flight ring
slot safely. Separate transform-only updates from topology/alias rebuild where profitable.

**Gate:** static-scene counters remain at zero rebuilds after warm-up; edited transforms/materials
invalidate exactly the required data; all frame slots see coherent buffers; CPU/copy timing improves.

#### PT-27 — triangle texture LOD constant is recomputed per hit

**Evidence: C; severity: medium.**

`ComputeTriangleAlbedoLodConstant` around
[`path_tracer.hlsl:2554`](../../../assets/shaders/dxr/path_tracer.hlsl#L2554) fetches indices, three
positions, three UVs, transforms vertices, computes world/UV areas, and queries texture dimensions on
every shading hit. For a fixed mesh instance/material this quantity is invariant or can be represented
by a precomputed object-space term plus transform correction.

**Remediation contract:** precompute/cache per-triangle or per-mesh/material footprint data during
scene upload, with an exact policy for non-uniform animated transforms and missing UVs. Do not replace
it with a coarser heuristic without an image/LOD contract.

**Gate:** an AOV compares old/new selected LOD over static, scaled, and animated meshes within the
declared tolerance; closest-hit instruction/time metrics and whole-frame PT time improve.

#### PT-32 — relocated TLAS SRVs are retired only at context destruction

**Evidence: C; severity: low.**

When a TLAS SRV is relocated, `DxrDispatchContext` retains the old descriptor index in a retired list
at [`DxrDispatchContext.cpp:494`](../../../src/engine/raytracing/DxrDispatchContext.cpp#L494) and frees
it only when the context is destroyed at [line 180](../../../src/engine/raytracing/DxrDispatchContext.cpp#L180).
Repeated growth/relocation can consume the finite
offscreen SRV heap during a long editor session.

**Remediation contract:** use the renderer's fence-deferred descriptor reclamation mechanism, tied to
the last submission that could reference the old TLAS SRV.

**Gate:** repeated scene growth/rebuild stress keeps live descriptor count bounded and produces no
debug-layer use-after-free.

#### PT-33 — ReSTIR markers misattribute temporal and spatial cost

**Evidence: C; severity: low.**

The temporal dispatch marker in
[`DxrDispatchContext.cpp:1291`](../../../src/engine/raytracing/DxrDispatchContext.cpp#L1291) is labelled
spatial, while the actual spatial
dispatch does not have an equivalently narrow marker. This can make a GPU capture assign cost to the
wrong stage and undermines the requested optimization discipline.

**Remediation contract and gate:** name markers after the dispatch they enclose, add separate temporal,
spatial, boiling-filter, surface-history, and RR-preparation scopes, and verify their nesting in one
capture before using them for phase decisions.

### 4.6 Acceleration structures, shader tables, payloads, and SER

#### Cleared AS/SBT/payload hypotheses

**Evidence: C under the current serialized frame model.**

- Static BLAS/TLAS fingerprinting and unchanged-build skipping are coherent; static structures use
  trace-oriented flags.
- Required AS build/read ordering and scratch UAV barriers are present in the audited path.
- The raygen-owned bounce loop correctly uses `MaxTraceRecursionDepth = 1`.
- The 40-byte payload and 8-byte attributes match the RTPSO declarations.
- Shader-record/table alignment and local/global root-signature associations are consistent.
- Opaque visibility uses inline `RayQuery` on capable hardware and retains a fallback.

These statements must be re-audited as concurrency requirements if PT-22 removes the global fence
drain. They do not prove optimal traversal, only that no immediate current correctness defect was
found. Payload repacking and recursion-depth changes are not justified first-line optimizations.

**D3D12 coverage boundary:** the active direct-command-list PT/ReSTIR/DLSS transitions, tracked
resource states, dependent UAV barriers, AS build/read ordering, frame-context wait, descriptor
relocation, SBT/root/payload declarations, and principal resize/reset paths were inspected. This was
not an exhaustive proof of every bindless index in every debug/optional shader, every allocation-
failure/recreation path, dormant queue configuration, Streamline interposer internals, or behavior
under future concurrent AS updates. Audit-time tests used the debug layer but not GPU-based
validation. Absence of another D3D12 finding must not be presented as those untested areas being
cleared.

#### PT-28 — SER capability gate rejects valid hardware combinations

**Evidence: C; severity: high.**

[`DxrCapabilities.h:61`](../../../src/engine/rendering/DxrCapabilities.h#L61) requires both Shader Model
6.9 and DXR Tier 1.2. The current DXR specification states that SER is part of the SM 6.9 ray-tracing
contract on ray-tracing-capable devices; Tier 1.2 is sufficient but not a necessary additional gate.
A Tier 1.0/1.1 + SM 6.9 device can therefore be rejected incorrectly.

**Remediation contract:** gate compilation/use on ray-tracing tier at least 1.0, SM 6.9 support,
successful shader/PSO creation, and the relevant runtime capability report. Keep fallback `lib_6_6`
or lower paths intact.

**Gate:** a pure capability-decision test covers RT unavailable and Tier 1.0/1.1/1.2 crossed with SM
6.8/6.9; only the specification-supported cells attempt the SER permutation.

#### PT-29 — SER is structurally plausible but operationally unproven

**Evidence: C/M/V; severity: medium.**

The PT-only SM 6.9 library and matched RTPSO are prewarmed. The shader uses the canonical sequence
`HitObject::TraceRay`, `MaybeReorderThread`, then `HitObject::Invoke` around
[`path_tracer.hlsl:411`](../../../assets/shaders/dxr/path_tracer.hlsl#L411), placing reordering after
traversal and before closest-hit material work. Visibility rays use inline queries and do not need
the same path. Payload ABI and root/SBT associations match.

The explicit reorder hint is only a one-bit primary-versus-continuation class. At a given megakernel
bounce most active lanes share that bit, so it does not classify material, hit/miss, or shader work;
automatic `HitObject` grouping may still help. Primary rays are relatively coherent, and the large
live state of a 16-bounce megakernel can constrain occupancy or spill, so SER can be neutral or
negative. “Active” currently means that the permutation was selected, not that the driver reports
`ShaderExecutionReorderingActuallyReorders` or that a dispatch gained performance. The UI string is
derived from `IsPathTracerSerActive()` at
[`RayTracingSection.cpp:774`](../../../src/app/panels/lighting/RayTracingSection.cpp#L774), while that
state is only `m_activeSerPermutation` in
[`DxrPathTracerDispatch.h:107`](../../../src/engine/raytracing/DxrPathTracerDispatch.h#L107).

The formal 2026-07-14 baseline records RTX 4070 Ti, DXR Tier 1.2, Shader Model 6.8, `lib_6_6`, inline
visibility, and SER unavailable. That does not match the newer UI observation and requires a fresh
adapter/driver/Agility/SM/Options22 capture. The validation experiment is specified in section 7.

### 4.7 Performance interpretation from existing captures

**Evidence: M; do not present as a fresh audit measurement.**

The formal fresh-process 300-frame measurements in [performance roadmap](performance-roadmap.md)
recorded the following medians for the sky/floor comparison:

| Scope | Sky | Floor | Floor minus sky |
|---|---:|---:|---:|
| GPU frame | 11.280 ms | 18.883 ms | +7.603 ms |
| Path tracer | 6.583 ms | 13.623 ms | +7.040 ms |
| PT megakernel | 1.574 ms | 5.816 ms | +4.242 ms |
| ReSTIR spatial | 1.704 ms | 3.946 ms | +2.242 ms |
| ReSTIR temporal | 3.013 ms | 3.199 ms | +0.186 ms |
| DLSS/RR | 4.326 ms | 4.391 ms | +0.065 ms |

Thus PT accounts for about 92.6% of the recorded frame-time delta, and the megakernel about 60.3% of
the PT delta. This supports prioritizing material/hit/guide work and ReSTIR spatial behavior over
DLSS or the tiny raster markers for that scene. It does **not** isolate which shader finding is worth
how many milliseconds. The user's 40–50 FPS versus an older approximately 72 FPS observation is
credible as a regression report but cannot be attributed to one change without a controlled
revision/configuration A/B.

Russian-roulette termination around
[`path_tracer.hlsl:2372`](../../../assets/shaders/dxr/path_tracer.hlsl#L2372) uses full throughput for the
survival probability and divides continuing local throughput by that probability. Its algebra is
sound. Differences seen with “RR on” are more plausibly coupled to longer-path exposure to PT-01,
PT-02, PT-07, PT-13/21, guide behavior, or the optional firefly clamp than to missing survival
compensation.

### Per-finding impact, dependency, and regression ledger

The detailed prose above supplies code evidence, correction, and a completion gate. This ledger makes
the remaining implementation-plan fields explicit without inventing a measured magnitude.

| ID | Expected visual result | Expected performance direction | Dependency | Regression focus before completion |
|---|---|---|---|---|
| PT-01 | Stable off-origin glass motion and valid ReSTIR prior-domain reuse | Neutral; possibly better temporal convergence | First temporal fix | Camera cuts, jitter, dual viewport, first valid frame |
| PT-02 | Correct partial-transmission energy and fewer rare bright paths | Variance may fall; mean cost neutral | Trustworthy reference mode | Endpoints, real-time heuristic separation, path length |
| PT-03 | Stable boundaries/offsets and consistent surface identity | Small payload/shader cost possible | Before PT-04/08/13/21 | Smooth glass appearance, backfaces, self-intersection, SBT ABI |
| PT-04 | Guide follows the rendered reflection/refraction | Neutral or less temporal rejection | PT-03 interface policy | Normal-mapped/grazing glass and thin/solid modes |
| PT-05 | Scaled objects match authored shape/material | Neutral | Shared normal helper | Mirrored/sheared transforms and raster parity |
| PT-06 | Less texture-frequency shimmer on secondary glossy paths | Some LOD math; fewer wasted fine samples | PT-03 surface frame | Overblur, anisotropy, diffuse/refraction footprints |
| PT-07 | Physically coherent rough-glass energy/noise | Continuous BSDF math can cost more | PT-03/04; reference tests | Smooth delta limit, TIR, eta convention, variance |
| PT-08 | RR stops locking history to false reflected geometry | Likely removes an extra trace; measure | PT-03/04 event contract | Mixed lobes, omission convention, specular motion |
| PT-09 | Same authored EV in direct and every feature/quality pair | Neutral | S1 history/reset contract | Double exposure, bloom space, SDK presets/auto exposure |
| PT-10 | Correct temporal cadence in both viewports | Neutral | Application frame identity | Single viewport, skipped viewport, present failure |
| PT-11 | No stale blocks/trails after semantic switches | Reset frames cost more transiently only | History compatibility key | Unnecessary steady resets and every supported mode |
| PT-12 | Stable rotated HDR lighting across reuse | Neutral | PT-01 before ReSTIR retune | Zero/nonzero rotation and CDF convention |
| PT-13 | Stable textured-emitter energy and MIS | More accurate distribution may add rebuild/storage cost | PT-03 face normal; PT-26 cache | One/two-sided, textured/untextured, moving emitters |
| PT-14 | Bounce-limit/CDF-invariant environment energy | Likely less terminal work | Valid BSDF/MIS reference | Real-time approximation appearance and miss paths |
| PT-15 | Non-finite values never poison history | Negligible sanitation; clamp cost remains optional | None | Preserve finite HDR and independent clamp statistics |
| PT-16 | Colored glass produces colored direct shadows | RGB visibility bandwidth/ALU increase | Medium policy PT-18 | Opaque fast path, multiple boundaries, saturation |
| PT-17 | No invented sunlight through curved glass | May remove invalid NEE or add expensive solver | Declared product scope | Parallel thin slab and ordinary shadows |
| PT-18 | Correct or explicitly rejected nested media | Stack adds path state/divergence if implemented | PT-03/07 boundary convention | Camera-inside, mismatched boundaries, payload pressure |
| PT-19 | Reciprocal, cross-mode-consistent opaque materials | Neutral to small shader-model change | Update all duplicate evaluators together | Furnace energy, raster look, ReSTIR targets |
| PT-20 | Eliminate small polar environment bias | Neutral | PT-12 representation first | Distribution normalization and low-resolution CDFs |
| PT-21 | Less broad rough-metal/emissive correlation without leaks | H1 can save rays; other changes require timing | PT-01/03/12/13 first | Mean energy, occlusion leaks, fine-vs-broad noise, P5/P6 |
| PT-22 | No intended visual change; smoother frame pacing | Positive if overlap was limiting; magnitude unmeasured | Explicit ownership audit | In-flight overwrite, AS/descriptor/ring hazards, present pacing |
| PT-23 | Same full-PT image without raster scene dependency | Non-negative; current scene's GPU gain likely small | PT guide/input isolation and S1/S2 | Raster/hybrid, overlays, editor picking, debug views |
| PT-24 | Identical RR input/output | Lower bandwidth/pass overhead if direct tagging works | S2 formats/states | Resource state/lifetime, raster/hybrid fallback, debug AOVs |
| PT-25 | No visual change | Lower memory/copy traffic; runtime magnitude unmeasured | Confirm no hidden consumers | Reset/resize, diagnostics, future feature assumptions |
| PT-26 | No visual change in static/dynamic lighting | Lower static-scene CPU/upload cost | Frame-overlap ownership PT-22 | Dirty invalidation and every in-flight slot |
| PT-27 | Identical texture LOD/material result | Lower hit-shader work if cache wins | Scene-upload representation | Animated/nonuniform transform, missing UV, memory/cache cost |
| PT-28 | No image change on already supported hardware | Enables measurement; no intrinsic speed claim | Correct capability log | All tier/SM cells and fallback PSO creation |
| PT-29 | SER on/off images equivalent | Unknown until section-7 A/B | PT-28 and stable baseline | Coherent control scene, spills/occupancy, fallback |
| PT-30 | Correct native/upscaled sharpness and stability | SDK-selected extent can raise or lower cost | S1 reset/token | Resize, quality modes, Ultra Performance jitter, RR no-DRS |
| PT-31 | Dynamic transmission either reprojects correctly or does not ghost | More rejection can raise noise; prior geometry costs memory/work | PT-01 and declared motion scope | Camera-only replay and static panes |
| PT-32 | No image change | Negligible steady-state; bounded descriptor use | Fence-deferred free mechanism | Use-after-free under frame overlap/scene growth |
| PT-33 | No image change | No meaningful runtime change | Before performance decisions | Marker nesting and capture-tool visibility |

---

## 5. Symptom matrix

This matrix separates a source-proven mechanism from a live attribution. Multiple confirmed defects
can affect the same pixel; fixing one does not validate the others.

| Reported symptom | Direct, high-confidence mechanisms | Likely amplifiers / alternatives | Evidence that constrains attribution | Decisive isolation |
|---|---|---|---|---|
| Glass shimmers or ghosts under camera motion | PT-01 invalid previous camera; PT-04 guide/radiance normal split; PT-07 invalid rough dielectric; PT-31 current-geometry replay | PT-03/05 bad interface normals, PT-06 mip-0 normal maps, PT-10/11 history cadence/reset, PT-08 guide if reflection dominates | These mechanisms are all live in source; no fresh visual capture in this audit | Fix/visualize PT-01 first, then raw PT vs RR, guide-direction AOV, static/dynamic pane matrix |
| Mirror-like objects shimmer | PT-08 independent guide ray; PT-01/10/11 temporal inputs | specular motion vectors, subpixel geometry/texture aliasing, incomplete cone propagation | Perfect smooth delta transport itself is simpler than rough glass; do not assume rough-BSDF defect | Raw PT accumulation, RR with hit distance omitted/exact, static camera and controlled orbit |
| Rough metal develops broad bright blocks or smear | PT-06 footprint/normal-map aliasing; PT-08 false hit distance; PT-21 P7 correlation | PT-03/05 normal frame, emissive mismatch PT-13, genuine low-spp variance | P7 B0 measured +133% neighbor correlation, +38.70% low-frequency variance, +40.63% blurred-hot coverage | P5+P6 vs P7 canonical suite, raw post-stage GI, exact/omitted hit-distance A/B, texture-frequency sweep |
| Emissive objects ghost or change energy | PT-01 prior-domain receiver; PT-13 NEE/hit emitter mismatch; PT-21 temporal-target deviations | PT-10 tokens, PT-11 mode reset, textured-emitter motion, exposure PT-09 | CPU light data omits emissive textures; previous camera defaults are confirmed | Checker emitter with fixed camera, ReSTIR off/P5/P6/P7 ladder, current-vs-previous target/PDF AOVs |
| Image brightness changes across direct/DLSS/RR and DLAA/SR qualities | PT-09 display exposure omitted after SDK evaluation | preset-specific auto-exposure, history reset PT-11 | Control flow forces tonemapper EV 0 after every successful evaluation | HDR patch at EV `-2/0/+2`, direct plus the full feature x quality matrix, fixed adaptation |
| Ghost appears only after switching render/AA mode | PT-11 missing producer-history invalidation | PT-10 multi-view token cadence, stale PT/ReSTIR history | Individual AA/preset resets exist; rendering-mode reset does not | Log history key/reset reason and inspect first evaluation after every switch |
| 40-50 FPS where an older build was about 72 FPS | PT-22 frame serialization; measured PT megakernel/ReSTIR cost; PT-08 extra trace | PT-23/24/25/26/27 redundant work, changed resolution/config, P7, debug/diagnostics | Formal floor/sky delta is 92.6% PT; global wait is source-confirmed; no matched old/new A/B | Same commit/config fixed camera captures, CPU/GPU timeline, phase-disable timing without quality changes |
| UI says SER active but speed does not improve | PT-29 status reports permutation, not actual reorder or speed; weak hint | gate PT-28, register pressure, coherent primary rays, driver/Agility mismatch | Existing formal capture says SM 6.8/SER unavailable, conflicting with newer UI report | Capability dump, force-off/on ABBA, narrow megakernel timer, Nsight divergence/occupancy counters |
| Colored glass looks right to camera but casts gray shadows | PT-16 scalar transmissive visibility | unsupported nested/curved interface PT-17/18 | RGB is explicitly averaged in source | RGB absorbing slab with analytic direct light |
| Polar environment lighting drifts slightly | PT-20 within-cell PDF mismatch | PT-12 double rotation, terminal term PT-14 | PT-12 is much larger at nonzero rotation and should be fixed first | Rotation round trip, then PDF normalization/polar histogram |

### Unrelated but important findings

These should not be cited as direct explanations for the reported moving-camera trails/splotches
without new evidence:

- PT-15 is a robustness defect only when non-finite values arise and is masked in the representative
  clamp-on configuration.
- PT-16 through PT-20 primarily affect spectral transmission, supported optical scope, reciprocity,
  or small environment bias rather than the characteristic block-shaped RR history.
- PT-22 through PT-27 are scheduling/redundant-work findings; they explain throughput or resource
  pressure, not ghost geometry.
- PT-28/29 concern SER availability/reporting/effectiveness. PT-30 concerns the broader feature x
  quality matrix; the representative native extent alone does not prove an active size mismatch.
- PT-32/33 concern long-session descriptor lifetime and profiler attribution, not image formation in
  an ordinary short run.

The trail shape is diagnostically useful: straight trails with RR disabled show that at least one
upstream temporal producer (PT virtual motion, ReSTIR, or another active history) is already retaining
the wrong signal. RR can turn that invalid motion/depth/guide history into larger reconstructed blocks
or splotches, but the changed shape does not make RR the root cause. Capture raw PT, raw post-ReSTIR,
and final reconstruction for the same frames before changing RR thresholds.

### Interpretation order

For the reported quality symptoms, the shortest trustworthy diagnostic ladder is:

1. correct PT-01 and prove the previous-frame camera contract;
2. compare raw PT/reference output before temporal reconstruction;
3. prove `Ng`/`Ns`, interface direction, motion, depth, albedo, and hit-distance AOVs;
4. validate the transport estimator (PT-02/07/13/14) before calling an image a reference;
5. re-run ReSTIR P5/P6/P7 and RR isolation only on those valid inputs;
6. tune thresholds last.

---

## 6. Completion-gated remediation plan

Stages are ordered by dependency, not by estimated coding effort. A stage is complete only when its
listed gate is recorded with the source revision, adapter/driver/Agility versions, scene/configuration,
and artifacts. “Looks better” is useful triage but is not a gate.

### Stage S0 - freeze the baseline and make contracts observable

**Objective and reason:** make later changes attributable. The current SER capability conflict,
multi-viewport token behavior, history resets, and source-only guide diagnoses cannot be judged from
one aggregate FPS counter.

**Findings addressed:** PT-29 and PT-33 observability, plus the measurement prerequisite for every
later finding. S0 does not claim to correct an estimator or runtime behavior.

**Atomic pass sequence:** `S0-P1` -> `S0-P2` -> `S0-P3` -> `S0-P4` -> `S0-P5`; see section 11.

**Work areas:** no estimator change. Add/standardize capability logging; render/history compatibility
key and reset reason; application frame token/viewport ID; PT permutation; current/previous camera;
guide-format/extent tags; narrow GPU scopes named in PT-33; deterministic seed/camera/config capture.
Version the canonical test scenes and capture manifest.

**Required scenes:** sky control, material-divergence floor/interior, static and moving glass checker,
rough-metal normal-map target, textured emissive checker, uniform-environment furnace, raster/hybrid
goldens, and simultaneous Scene/Game viewports.

**Expected result and risks:** no intended visual or performance change; instrumentation adds a small,
measured diagnostic overhead only when enabled. Inspect shader liveness, readback stalls, logging I/O,
warm-up behavior, and accidental non-determinism before accepting the baseline.

**Completion gate:** two fresh-process repeats produce the same configuration hash and statistically
compatible images/timings; logs reconcile the SM 6.8 baseline with the current machine; every GPU
scope nests correctly; no visual or performance regression beyond repeatability noise.

### Stage S1 - establish one previous-frame and history lifecycle contract

**Objective and reason:** eliminate deterministic temporal-domain errors before any denoiser or
reservoir tuning.

**Findings:** PT-01, PT-10, PT-11, and the rejection portion of PT-31.

**Atomic pass sequence:** `S1-P1` -> `S1-P2` -> `S1-P3` -> `S1-P4` -> `S1-P5` -> `S1-P6`;
see section 11.

**Work areas:** authoritative current/previous camera packet; one application-frame Streamline token;
per-viewport history compatibility key; camera-cut/mode/guide/dynamic-transmission invalidation;
explicit reset propagation to PT accumulation, ReSTIR, TAA/DLSS/RR, and bloom where appropriate.

**Visual/performance checks:** static off-origin camera, lateral orbit, teleport/cut, Scene+Game views,
raster/hybrid/PT producer switches, DLSS/RR feature x DLAA/SR-quality switches, guide diagnostics,
and moving pane/receiver. Track history rejection coverage and ensure steady-state resets remain zero.

**Expected result and risks:** remove deterministic trails/blocks caused by invalid history; steady
performance should be neutral, while legitimately reset frames can be noisier/costlier. Inspect false
camera cuts, excessive steady resets, lost reuse, skipped viewports, and camera-jitter conventions.

**Completion gate:** all PT-01/10/11 tests from section 4 pass; invalid dynamic transmission is
rejected deterministically; no cross-viewport state; opaque static scenes retain the same reuse rate
and frame time within noise. At one saved Cornell-box pose, archive an identical-pose comparison of
raw real-time PT radiance, the RR guide bundle, final RR output, and accumulated reference output.

### Stage S2 - normalize DLAA, DLSS SR, RR, exposure, and allocation behavior

**Objective and reason:** preserve every reconstruction mode while removing feature-specific display,
resolution, jitter, and reset errors before transport changes alter the input signal again.

**Findings:** PT-09 and PT-30, plus the format/state prerequisites for PT-24.

**Atomic pass sequence:** `S2-P1` -> `S2-P2` -> `S2-P3` -> `S2-P4` -> `S2-P5`; see section 11.

**Work areas:** separate reconstruction exposure/pre-exposure from display EV; query optimal settings;
per-viewport/mode render extents; complete jitter-period policy; explicit RR dynamic-resolution
restriction; format/tag/state validation; resize/reset integration.

**Simple-scene checks:** HDR exposure patches, fine geometry, subpixel motion, output resize, every
exposed quality mode, Scene+Game viewport mismatch sizes.

**Expected result and risks:** brightness and sampling cadence become mode-invariant where intended;
SDK-sized SR inputs may change allocation and GPU cost in either direction. Inspect crops, motion/depth
scale, jitter sign, resize, resource states, manual EV, bloom, and every mode transition.

**Completion gate:** EV and output-resolution gates pass for direct, DLSS+DLAA/SR, and RR+DLAA/SR;
SDK debug logging reports valid tags/settings; all feature/quality transitions reset once; no lost
manual exposure, crop, jitter cycle, or cross-viewport allocation.

### Stage S3 - define surface-frame, footprint, and guide semantics

**Objective and reason:** make radiance, ReSTIR, ray offsets, and RR describe the same surface/event.

**Findings:** PT-03, PT-04, PT-05, PT-06, and PT-08.

**Atomic pass sequence:** `S3-P1` -> `S3-P2` -> `S3-P3` -> `S3-P4` -> `S3-P5` -> `S3-P6` ->
`S3-P7`; see section 11.

**Work areas:** explicit `Ng`, `Ns`, tangent frame, front-face/boundary state, inverse-transpose normal
transform, shared dielectric first event, ray-cone/LOD propagation, exact sampled-event hit distance or
omission, specular motion. Update payload/surface records only as required by the proven contract;
recheck SBT/payload sizes if layouts change.

**Simple-scene checks:** flat/smooth triangle, mirrored winding, non-uniform sphere/capsule, grazing and
normal-mapped pane, mirror/roughness sweep, high-frequency normal map at increasing distance,
foreground disocclusion.

**Expected result and risks:** stabilize glass/mirror/rough-metal guide ownership; removing PT-08's
extra trace should help, while added frame/footprint state may increase registers or payload pressure.
Inspect overblur, smooth-glass faceting, self-intersection, backfaces, SBT ABI, and raster/hybrid parity.

**Completion gate:** analytic/AOV gates for PT-03/04/05/06/08 pass; raw radiance and guide direction
agree; RR improves or matches temporal metrics without a false distance; D3D12 validation is clean;
no raster/hybrid normal regression. Recapture the S1 Cornell-box artifact set at the identical saved
pose and compare raw real-time PT radiance, RR guides, final RR output, and accumulated reference
output, with mirror sharpness reported separately at each stage.

### Stage S4A - restore a trustworthy core reference estimator

**Objective and reason:** reference mode must first become a dependable energy/PDF oracle for its core
mixture, dielectric, environment-partition, and numerical-safety behavior. This enables meaningful
visual comparison without waiting for broader scope-dependent transport work.

**Findings:** PT-02, PT-07, PT-14, PT-15, and the persistent over-bright-pixel investigation recorded
in section 9.

**Atomic pass sequence:** `S4A-P1` through `S4A-P12`, in numeric order; see section 11.

**Work areas:** explicit mixture coefficients/probabilities; matched rough dielectric sample/evaluate/
PDF; environment estimator partition; always-on finite sanitation; and a path-level diagnostic that
traces representative persistent over-bright samples from branch selection through the final
accumulation write. Keep product-biased real-time shortcuts separately named and selectable.

**Simple-scene checks:** lobe-weight sweep, dielectric/opaque furnaces, eta/TIR/roughness grid,
uniform/bright-texel environment, and the fixed-pose accumulated-reference scene containing the
reported persistent over-bright pixels.

**Expected result and risks:** correct core material/environment energy and either remove or
mechanistically classify persistent over-bright samples; valid rough dielectric can add ALU/divergence,
while removing duplicate terms can save work. Inspect variance, path length, TIR, real-time/reference
separation, authored material appearance, sanitation, clamp placement, and accumulation precision.

**Completion gate:** PT-02/07/14/15 analytic and statistical tests pass with confidence intervals;
reference energy is invariant to harmless bounce-limit/CDF changes; representative offending pixels
are traced through throughput, branch probability, directional/light PDFs, MIS weights, emission,
finite-value sanitation, clamp placement, and the final accumulation write. Any emitter-specific case
is retained as an explicit PT-13/S5 input rather than misclassified as an ordinary transient firefly.
Real-time deviations are documented with image/error budgets, and representative performance does
not regress without an accepted quality trade. Core reference visual evaluation may begin when S4A
passes; S4B does not block this gate.

### Stage S4B - extend physical transport correctness

**Objective and reason:** after the core reference estimator is trustworthy, extend the supported
physical-transport scope without making nested-media, curved-interface, reciprocity, or small
environment-PDF work a prerequisite for S4A completion and visual evaluation.

**Findings:** PT-16, PT-17, PT-18, PT-19, and PT-20.

**Atomic pass sequence:** `S4B-P1` through `S4B-P8`, in numeric order, with the documented
`S5-P1` environment-representation bridge before `S4B-P7`; see section 11.

**Work areas:** RGB transmissive visibility; declared curved-interface and medium scope; reciprocal
opaque material model; and exact environment within-cell PDF.

**Simple-scene checks:** parallel/wedge/curved glass, RGB Beer slab, nested/camera-inside media,
direction-swap/furnace material tests, and polar environment bins.

**Expected result and risks:** improve spectral shadows and the declared physical scope; medium state
or a curved-interface solver can add path state, ALU, and divergence. Inspect the valid thin-slab and
single-solid cases, raster material appearance, unsupported-scene diagnostics, and environment-PDF
normalization while keeping every S4A gate green.

**Completion gate:** PT-16/17/18/19/20 tests pass for the declared supported scope, unsupported cases
produce the documented fallback/diagnostic, S4A reference invariants remain unchanged, and each
retained extension has an accepted image/performance trade. Failure or deferral of S4B does not revoke
S4A completion or block core reference visual evaluation.

### Stage S5 - make light sampling and ReSTIR one estimator

**Objective and reason:** remove target/PDF/domain mismatches responsible for emissive and broad
rough-metal temporal artifacts.

**Findings:** PT-12, PT-13, and PT-21 (H1, H2, H2b, H3, then H4).

**Atomic pass sequence:** `S5-P1` is the environment-representation bridge for `S4B-P7`; after
`S4B-P8`, continue with `S5-P2` through `S5-P9` in numeric order. See section 11.

**Work areas:** canonical environment reservoir representation; texture-aware or explicitly excluded
emissive proposals; consistent face normal/sidedness; RTXDI BASIC previous-domain normalization;
continued reprojection search; one Jacobian convention; explicit visibility reuse/retrace policy.

**Simple-scene checks:** rotated single-texel HDR, checker emitter, one moving emitter, one occluder,
two-depth reprojection neighborhood, rough-metal capsule/floor, current/previous target and proposal
PDF AOVs.

**Expected result and risks:** reduce emissive trails and broad correlated rough-metal patches; H1 can
remove redundant rays, but reuse-policy changes have unmeasured cost. Inspect mean energy, occlusion
leaks, fine-noise redistribution, temporal chroma, moving lights, and P5/P6 regressions.

**Completion gate:** sample/replay round trips and PDF/target equalities pass; P5/P6 do not drift in
mean energy; canonical P7 improves fine noise without worsening the broad-correlation metrics listed
in PT-21; any section-9 persistent over-bright sample classified as emissive is retraced after PT-13
through emission, light/directional/BSDF PDFs, and MIS weights; isolated stage GPU timings satisfy the
production roadmap budget.

### Stage S6 - recover safe frame overlap and isolate full PT from raster production

**Objective and reason:** address the largest confirmed scheduling defect and remove cross-mode work
only after ownership is explicit.

**Findings:** PT-22 and PT-23.

**Atomic pass sequence:** `S6-P1` through `S6-P10`, in numeric order; see section 11.

**Work areas:** frame-context/ring/descriptor/AS fence ownership; allocator reuse; capacity-growth
safety; dedicated PT root/input contract; skip CSM/scene G-buffer only in full PT; preserve overlay and
all raster/hybrid consumers.

**Performance checks:** CPU and GPU queue timeline, present/pacing separation, command-record time,
GPU frame median/p95, geometry-heavy and representative scenes, editor dual viewport, resize/scene
growth stress.

**Expected result and risks:** no intended image change; CPU/GPU overlap should improve throughput or
pacing and PT-only raster suppression should be non-negative. The main risk is an in-flight allocator,
descriptor, upload, AS, or history overwrite, plus loss of editor/raster/hybrid functionality.

**Completion gate:** two frames demonstrably overlap; validation remains clean for long stress runs;
full PT renders without raster scene/CSM inputs; raster/hybrid and the DLSS/RR x DLAA/SR matrix pass;
measured
throughput/pacing improves enough to justify complexity.

### Stage S7 - remove proven redundant PT work

**Objective and reason:** reduce bandwidth, CPU upload, and hit-shader work without changing the
estimator.

**Findings:** PT-24, PT-25, PT-26, and PT-27.

**Atomic pass sequence:** `S7-P1` -> `S7-P2` -> `S7-P3` -> `S7-P4`; see section 11.

**Work areas:** direct PT guide tagging/layout, remove dead histories, emissive cache/invalidation,
precomputed triangle LOD invariant. Land and measure one change at a time.

**Simple-scene checks:** representative PT scene, 4K bandwidth stress, static/moving high-triangle
emissive scene, texture-heavy unique-model scene, resize, edit, and dual-viewport stress.

**Expected result and risks:** no intended visual change and a non-negative measured CPU/GPU/memory
direction. Inspect stale dirty data, frame-slot coherence, resource-state/lifetime errors, LOD drift,
debug AOV loss, and every raster/hybrid fallback.

**Completion gate:** each change passes its section-4 equivalence/invalidation test, lowers its narrow
marker/counter outside noise in a relevant stress scene, and does not trade raster/hybrid correctness
for PT speed. Revert changes with no repeatable benefit unless they materially simplify ownership.

### Stage S8 - validate and, only then, tune SER

**Objective and reason:** distinguish availability, dispatch, actual hardware reordering, and a useful
performance win.

**Findings:** PT-28 and PT-29. Execute the complete section-7 protocol.

**Atomic pass sequence:** `S8-P1` through `S8-P7`, in numeric order; see section 11.

**Work areas:** correct the capability decision, query/report Options22, separate availability from
successful per-frame dispatch and actual reorder status, capture the existing timestamp scopes, and
run force-off/on plus isolated hint/bounce-0 variants without changing other quality work.

**Simple-scene checks and expected result:** section 7's divergence-heavy floor/interior and coherent
sky control. SER on/off output must match within the stochastic contract; performance is unknown and
the feature is retained by default only for a repeatable whole-frame win. Inspect PSO fallback,
register spills, coherent-scene regressions, driver capability changes, and UI truthfulness.

**Completion gate:** correct capability matrix; force-off/on image equivalence; repeatable whole-frame
and megakernel result beyond run-to-run noise; GPU-counter explanation consistent with the timing. If
neutral/negative, default SER off or auto-disabled for that workload and retain the fallback.

### Stage S9 - lifetime and observability cleanup

**Objective and reason:** prevent long-session resource leaks and preserve trustworthy profiling.

**Findings:** PT-32 and PT-33, plus stale APIs/docs exposed by earlier stages.

**Atomic pass sequence:** `S9-P1` through `S9-P5`, in numeric order; see section 11.

**Work areas:** move retired TLAS SRVs to fence-deferred descriptor reclamation; correct/narrow ReSTIR
GPU events; remove stale fields/getters/permutations exposed by S1-S8; update ownership docs and test
manifests with their final evidence.

**Simple-scene checks and expected result:** repeated scene-growth/TLAS relocation plus a representative
GPU capture. No intended visual change or meaningful performance change; descriptors remain bounded
and attribution becomes trustworthy. Inspect premature descriptor reuse under S6 frame overlap and
capture-marker/tool regressions.

**Completion gate:** descriptor stress remains bounded; GPU events reflect real dispatch ownership;
dead fields/permutations/docs are removed; mode matrix and automated tests pass; this audit is updated
with final revision/results rather than silently marked complete.

---

## 7. SER validation plan

SER should be evaluated as a scheduling experiment, not enabled because a menu says “active.” No
benchmark implementation is part of this audit.

### 7.1 Preconditions and status reporting

Before timing, capture and display these as separate states:

1. adapter, driver, OS, Agility SDK, DXR tier, highest shader model, and Options22 data;
2. SER API/compiler support and successful SM 6.9 library/RTPSO creation;
3. requested policy: auto, force off, or force on;
4. permutation actually dispatched this frame;
5. `ShaderExecutionReorderingActuallyReorders` (where reported by the runtime/driver);
6. any dispatch/PSO failure and fallback reason.

Fix PT-28 first. If the RTX 4070 Ti still reports SM 6.8, the experiment is blocked by the active
runtime/toolchain configuration rather than being a negative SER result.

### 7.2 Fixed benchmark matrix

Use the exact current production-quality configuration: approximately 1440p, 1 spp, 16 bounces,
the RR feature with DLAA quality, identical ReSTIR/Russian-roulette/firefly settings, fixed output and
render extents, and fixed seeds. Include:

- **divergence-heavy floor/interior:** many visible materials and secondary hits;
- **unique-material/texture stress:** similar hit depth with divergent material work;
- **glass/rough dielectric stress:** only after S3/S4A core correctness;
- **sky control:** coherent primary misses and little closest-hit work;
- optionally a traversal-heavy, low-material-divergence scene to separate traversal from shading.

Disable vsync, overlays, diagnostics/readbacks, scene editing, and unrelated background capture. Do
not lower quality or resolution to create a larger percentage.

The primary timing camera path is deliberately static: load the stored floor/interior pose and hold it
for every warm-up/window so A and B execute the same pixels and material distribution. As a secondary
motion/equivalence check, replay the existing canonical P7 orbit: fixed camera Y/pitch, 13 XZ-plane
revolutions at 7.8 degrees/frame over 600 frames, starting from the same saved pose for each policy.
Do not mix the static and orbit samples into one timing statistic.

### 7.3 Timing protocol

- Fresh process or fully warmed identical state for each session.
- Warm at least 120 rendered frames after scene, PSO, or SER-policy change.
- Record three complete **A-B-B-A** blocks per scene; each letter is a 300-frame window, yielding six
  windows per policy. Reverse the starting policy (`B-A-A-B`) in a second fresh-process session to
  expose thermal/order effects. Apply the 120-frame warm-up whenever the letter/policy changes.
- Record median, p95, median absolute deviation/run spread for the narrow
  `PT.DispatchRays.PathTracer`, enclosing PT scope, total GPU frame, CPU record time, prior-fence wait,
  and presented frame time.
- Save frame/config hash, output image/statistics, clocks/temperature if available, and capability log.
- Compare fixed-seed images exactly where scheduling preserves floating-point order; otherwise use a
  predeclared epsilon/statistical equivalence test. SER is not allowed to hide a correctness change.

### 7.4 GPU-capture diagnosis

If timing shows a real delta, use Nsight Graphics or an equivalent counter capture on representative
frames to explain it: active lanes/warp or wave occupancy, branch/divergence efficiency, registers,
local-memory spills, occupancy limits, SM/RT-core utilization, hit/miss/material distribution, and
time around trace/reorder/invoke. A timing change without a repeatable counter/workload explanation is
not a stable tuning basis.

### 7.5 Acceptance and follow-on experiments

Retain SER by default only when it produces a repeatable whole-frame benefit beyond run noise in at
least the representative divergence-heavy scene, does not materially regress the sky/control scene,
and preserves output. Report both megakernel and whole-frame percentages; do not advertise a narrow
scope win that disappears in the frame.

Only after the base on/off result is established should hint experiments be attempted:

1. isolate the current one-bit primary/continuation hint versus automatic grouping;
2. isolate skipping reorder for bounce 0;
3. test a small, stable hit/material/work class only if it reflects work performed after reorder;
4. measure register/live-state changes for every added hint computation.

Keep `lib_6_6`/non-SER RTPSOs hot and functional. Auto policy may legitimately choose off for coherent
or register-limited workloads.

---

## 8. Mode dependency and isolation inventory

### 8.1 Renderer subsystems

| Component/resource | Raster | Hybrid RT | Full PT | Current coupling verdict | Required isolation/preservation |
|---|---:|---:|---:|---|---|
| GPU scene/material/instance buffers | Yes | Yes | Yes | Appropriate shared source of scene truth | Preserve shared ownership and explicit frame lifetime. |
| BLAS/TLAS | No/optional | Yes | Yes | Appropriate shared RT infrastructure | Preserve hybrid and PT permutations; add per-frame safety when S6 overlaps frames. |
| Cascaded shadow maps | Yes | Yes | Produced but not read by PT radiance | Redundant in full PT | Skip only in full PT after removing CPU/post hard dependency. |
| Scene G-buffer geometry pass | Yes | Yes | Produced; only skybox skipped | Redundant producer and current hard dependency | Give PT dedicated inputs/guides; retain target/overlay needs and all non-PT draws. |
| Raster depth/normal/material/direct/shadow/indirect/velocity | Yes | Yes | Required by CPU contract, not read by PT radiance | Incorrect hard dependency | Make optional/absent in full PT; validate no hidden debug/post consumer. |
| Environment/prefiltered maps | Yes | Yes | Yes | Legitimate shared lighting asset; PT estimator use needs PT-14 fix | Preserve asset sharing, separate reference vs product approximation semantics. |
| Hybrid RT shadows/reflections/GI dispatches | No | Yes | Correctly disabled | Correct gating | Regression-test hybrid; do not delete while isolating PT. |
| PT DispatchRays and PT surface/reservoir history | No | No | Yes | Correct mode ownership; history semantics incomplete | Reset on incompatible transitions and dynamic-domain failure. |
| ReSTIR DI temporal/spatial/boiling | No | Mode-dependent | Yes when selected | PT stages active; contracts H1-H4 unresolved | Keep phase toggles/test ladder; repair after canonical temporal/surface state. |
| Reference accumulation | No | No | Reference PT | Correct separate path, but estimator is not yet a correctness oracle | Gate core reference use on S4A; track extended scope under S4B. |
| SSAO/SSGI/SSR/TAA and hybrid composite | Yes/mode-dependent | Yes/mode-dependent | Correctly disabled | Correct steady-state gating | Preserve raster/hybrid behavior and transition resets. |
| NRD | No | Hybrid | No steady-state PT | Correctly inactive for PT | Preserve hybrid integration; lazy construction is optional startup cleanup. |
| SVGF | SSR path | SSR path | No steady-state PT | Correctly inactive for PT | Preserve SSR; do not cite as PT GPU cost. |
| PT HDR/depth/motion/albedo/normal/roughness guides | No | No | Yes | Correct producer; downstream does not consume directly enough | Establish versioned PT guide bundle and direct SDK tags. |
| RR preparation targets | Raster/hybrid variants | Hybrid variants | Five-copy/resolve path | Excessive PT duplication | Direct-tag PT resources while retaining genuinely distinct non-PT targets. |
| DLAA | Yes | Yes | Yes | Required supported mode; history/token/exposure shared defects | Preserve as first-class mode with per-viewport settings/resets. |
| DLSS SR | Yes | Yes | Yes | Required supported mode; fixed ratios/optimal settings defect | Preserve all exposed qualities and SDK sizing. |
| DLSS RR | Optional | Optional | Representative PT feature | Guide, token, exposure, sizing, reset defects | Cross with both DLAA and SR qualities; no arbitrary DRS. |
| Bloom/tonemap | Yes | Yes | Yes | Correct shared display chain; EV ownership wrong after DLSS | Apply authored display exposure once for every reconstruction branch. |
| Scene/Game viewport effects contexts | Yes | Yes | Yes | Separate resources/IDs are appropriate; token must be frame-shared | Preserve per-viewport histories and share only application-frame identity. |
| Grid, selection, editor/UI composition | Yes | Yes | Yes | Intentional raster/editor work | Preserve even if scene-object raster/CSM is skipped in full PT. |
| Previous transforms/emissive upload | Mode-dependent | Mode-dependent | Every PT frame | Needed data, overly broad update/rebuild | Explicit dirty tracking and ring ownership; preserve moving-scene correctness. |

The isolation goal is not “PT owns everything.” It is: shared immutable scene truth and RT
infrastructure remain shared; each renderer produces only the signals its active consumers require;
temporal histories are per viewport and per compatible producer; display/reconstruction interfaces
have explicit, versioned contracts.

### 8.2 DLAA, DLSS SR, RR, and resolution ownership

The implementation has two independent axes. `AntiAliasingMode`/`DlssQuality` chooses DLAA or an SR
quality. `useRayReconstruction` chooses `kFeatureDLSS` or `kFeatureDLSS_RR`. In
[`DlssContext.cpp:520`](../../../src/engine/rhi/DlssContext.cpp#L520), both `DLSSOptions.mode` and
`DLSSDOptions.mode` receive that same quality. Therefore RR+DLAA and RR+SR are real combinations; RR
is not a fourth mutually exclusive quality mode.

| Quality axis | Internal/render extent | Output extent | Spatial upscale | `kFeatureDLSS` jitter period | Dynamic-resolution expectation | Current verdict |
|---|---|---|---|---|---|---|
| Direct/native (no Streamline feature) | Viewport-native unless a separate renderer scale is selected | Viewport | None | Renderer-owned; must match primary rays/motion | Renderer-owned if exposed | Structurally separate; no confirmed direct-path extent defect. |
| DLAA / `eDLAA` | Native: internal equals output | Viewport | None | 8; current 64-entry sequence covers it | Must not inherit SR/DRS assumptions | Correct conceptual extent; this jitter cell is for ordinary DLSS, while representative RR+DLAA uses the RR rule below. |
| SR Quality / `eMaxQuality` | SDK-recommended sub-native | Viewport | Yes | 18 | Query SDK-supported range and reset on change | PT-30: current fixed 0.667 ratio is not an adequate SDK contract. |
| SR Balanced / `eBalanced` | SDK-recommended sub-native | Viewport | Yes | 24 | Same | PT-30: current fixed 0.58 ratio is not an adequate SDK contract. |
| SR Performance / `eMaxPerformance` | SDK-recommended sub-native | Viewport | Yes | 32 | Same | PT-30: current fixed 0.5 ratio is not an adequate SDK contract. |
| SR Ultra Performance / `eUltraPerformance` | SDK-recommended sub-native | Viewport | Yes | 72 | Same | For ordinary DLSS, current 64-entry sequence is short; current fixed 0.333 ratio is also not sufficient validation. |

| Feature axis | Quality modes carried into options | Extra inputs | DRS policy | Current verdict |
|---|---|---|---|---|
| `kFeatureDLSS` (`useRayReconstruction=false`) | DLAA or any exposed SR quality via `DLSSOptions.mode` | Color, depth, motion | Follow queried DLSS settings/ranges | Supported path; PT-09/10/11/30 apply. |
| `kFeatureDLSS_RR` (`useRayReconstruction=true`) | DLAA or any exposed SR quality via `DLSSDOptions.mode` | Color, depth, motion, diffuse/specular albedo, packed normal/roughness, optional specular hit distance | RR DRS is unsupported by the cited 2.12 guide | Representative is RR+DLAA; PT-01/03-11/24/30/31 affect its inputs/integration. |

The DLSS guide's quality-specific periods in the first table apply to `kFeatureDLSS`; the RR guide
requires at least 32 jitter phases for `kFeatureDLSS_RR`. The current 64-entry sequence covers the
representative RR+DLAA and the other current RR quality combinations. The 72-phase shortfall applies
to ordinary `kFeatureDLSS` Ultra Performance, not to the RR feature's 32-phase rule.

Viewport size and aspect ratio are variable, so every feature/quality pair needs a per-viewport tuple
of render extent, output extent, jitter phase/count, motion scale, guide generation, and history key.
The SDK query result, not a global fixed ratio, should own SR allocation. DLAA must never silently
reuse a smaller SR allocation; SR must never dispatch at native dimensions merely because DLAA was
active on the prior frame. `kFeatureDLSS` and `kFeatureDLSS_RR` transitions also require history reset
even when their quality/extent is unchanged.

### 8.3 Reconstruction-input audit

| Input | Active PT producer/format | Current status | Required convention/gate |
|---|---|---|---|
| Motion vectors | PT RGBA16F guide, including transmission virtual motion | Shader writes NDC `current - previous`; constants use scale `(-0.5,+0.5)`, Y flip, unjittered clips, `motionVectorsJittered=false`, and camera motion included, so the value convention is internally consistent. PT-01/04/10/31 still invalidate secondary content. The [RR 2.12 guide section 4.1.6](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_RR.md#416-motion-vectors) lists RG16F/RG32F, not the current RGBA16F resource, so tag-format compatibility remains unverified. | Lock sign/scale with translated-checker/orbit AOVs; run Streamline validation and an RGBA16F-vs-RG16F tag A/B before clearing format compatibility. |
| Depth | PT R32 hardware depth, currently converted/resolved to D24 for RR | Shader writes current jittered clip `z/w` in `[0,1]`; constants set `depthInverted=false` with near/far. This is structurally consistent. Conversion is redundant because R32/D32/D24 are supported. | Depth AOV/occlusion test and SDK debug validation across DLAA/SR, aspect change, resize, and near/far changes. |
| Normal/roughness | PT RGBA16F signed world normal xyz + linear roughness w, copied for RR | `DLSSDNormalRoughnessMode::ePacked` matches the resource layout, but PT-03/05/06 make its content unreliable on important surfaces. | Analytic signed-world-normal/roughness sweep and direct-tag state validation in every feature/quality pair. |
| Diffuse/specular albedo | PT RGBA8 guides, copied for RR | Transmission guide tests pass; PT-04 and material-model drift remain. | One canonical guide definition per lobe/material and raw AOV comparison across direct and every feature/quality pair. |
| Specular hit distance | Extracted to R16 from PT guide alpha | PT-08: independent mirror trace/non-distance blend is invalid for rough/mixed events. | Actual first sampled supported specular event or documented omission; primary-surface world distance. |
| Surface/material identity | PT metadata/surface records for PT/ReSTIR; reconstruction use is feature-specific | Surface identity inherits PT-03 and mode-transition PT-11 risk. No separate accepted ID-format defect. | Stable ID under camera motion; invalidate on topology/material-generation change; never reuse raster identity for an incompatible PT signal. |
| Exposure/pre-exposure | EV converted to reconstruction scale; post tonemap forced to EV 0 | PT-09 confirmed. | Reconstruction guidance and authored display EV are separate; apply display exposure exactly once. |
| Reactive/transparency/disocclusion | No reactive/transparency tag is supplied; disocclusion is inferred from depth/motion/history | No standalone missing-tag defect was accepted because the selected feature contract does not make such a project tag universally mandatory; transmission/disocclusion inputs are invalid upstream. | Use only tags supported by the selected feature/version; controlled occlusion/transmission captures decide any optional input. |
| Extent/states/lifetime | Per-viewport resources, then RR preparation copies; tags use the tracked D3D12 state and `eValidUntilPresent` | PT-24/30: unnecessary preparation and non-SDK sizing. No active debug-layer state error was observed in audit tests, but direct reuse is unproven. | SDK-recommended feature/quality extents, explicit tag states, fence-safe lifetime, reset on resize or producer/version change. |

Resize and AA/preset/RR changes already trigger several reset/recreation paths; that is why no blanket
resize failure is asserted. The missing semantic rendering-mode reset (PT-11), per-evaluation token
(PT-10), and fixed sizing policy (PT-30) prevent the overall integration from being considered
correctly isolated today.

### 8.4 Deprecated and inactive reconstruction paths

- **NRD:** retained for hybrid denoising, not dispatched in the representative full-PT path. Preserve
  hybrid behavior. Eager construction may affect startup allocation only; it is not evidence of a
  steady-state PT cost.
- **SVGF:** retained behind the SSR path and inactive in full PT. Preserve raster/hybrid SSR. It does
  not feed the active PT/RR guide bundle.
- **Legacy TAA/screen-space effects:** intentionally used by raster/hybrid configurations and gated
  out of full PT. Their history must still reset correctly on mode transitions, but they are not
  currently contaminating steady-state PT radiance.
- **RTXGI/other optional RT systems:** no dispatch from such a subsystem was identified in the
  representative full-PT call chain. This audit did not exhaustively certify every optional build or
  project configuration; a future feature addition must enter the mode inventory explicitly.

---

## 9. Needs measurement or verification

No live image comparison, PIX/Nsight capture, or new release-vs-old revision benchmark was performed
by this audit. The following questions remain open even though their mechanisms are source-confirmed:

| Priority | Question | Minimum evidence needed |
|---|---|---|
| 1 | Which runtime capability state is current on the RTX 4070 Ti, and can it actually reorder? | S0 capability dump including driver/Agility/SM/Options22 plus successful dispatched permutation. |
| 2 | How much of glass ghosting disappears with PT-01 alone? | Fixed raw/PT+RR image sequence and motion/previous-camera AOV before/after, away from origin. |
| 3 | Is rough-metal shimmer dominated by P7, false hit distance, or ray-footprint aliasing? | Factorial P5/P6/P7 x hit-distance omitted/exact x flat/high-frequency normal map. |
| 4 | What caused the reported 72 to 40-50 FPS change? | Same machine/config/scene/camera fresh-process A/B between known revisions, with narrow GPU scopes and CPU timeline. |
| 5 | What fraction of the 13 ms prior-GPU wait is safely recoverable? | Present-unblocked CPU/GPU timeline before/after frame-context fence ownership; pacing and p95. |
| 6 | What do the five RR preparation passes cost at representative and 4K output sizes? | Narrow per-pass GPU timestamps and bandwidth/resource capture, direct-tag A/B. |
| 7 | Do dead-history copies have a measurable cost beyond their calculated traffic? | Copy marker and memory-budget capture before/after, multiple resolutions. |
| 8 | How expensive is emissive rebuilding in the user's scene? | CPU scopes, triangle/light counts, upload bytes, copy-queue/direct-queue time, static cache A/B. |
| 9 | Does precomputed triangle LOD reduce closest-hit/megakernel time? | LOD equivalence AOV and shader/GPU metrics in texture-heavy unique-model stress. |
| 10 | Are CSM/G-buffer costs material in geometry-heavy PT/editor workloads? | CPU record and GPU markers with full PT producer enabled/disabled; retain tiny current-scene result as context. |
| 11 | Does valid rough dielectric materially change representative noise/performance? | Furnace/PDF correctness first, then matched rough-glass temporal variance and megakernel timing. |
| 12 | What P7 quality/performance point passes production gates? | Canonical diagnostics plus isolated temporal/spatial/filter GPU timings after H1-H4 and PT-01. |
| 13 | Does Streamline accept/use the PT RGBA16F motion tag exactly as intended when the RR 2.12 guide lists RG16F/RG32F? | Streamline validation/debug log, tag-result status, and matched RGBA16F-versus-RG16F motion/output sequence for RR+DLAA and RR+SR. |

### Additional user-observed verification evidence

**Status:** these observations were supplied by the user after the audit and were **not reproduced
during this audit**. They constrain verification priorities but do not create new confirmed findings
or prove that any one existing finding is the direct cause.

- **Cornell-box mirror sharpness:** at the same scene, mirror reflections are visibly sharp in
  accumulated PT reference mode but substantially blurrier in the real-time RR path. This constrains
  PT-01, PT-03/04, PT-06, and PT-08 collectively; it does not distinguish among temporal-camera,
  surface-frame, ray-footprint, event/guide, or reconstruction causes. S1 and S3 now require a saved,
  identical-pose comparison of raw real-time PT radiance, the RR guides, final RR output, and
  accumulated reference output, with mirror sharpness assessed at each boundary.
- **Persistent sparse over-bright pixels:** sparse bright pixels reportedly remain after more than
  20,000 accumulated reference samples and also remain with the PT firefly clamp enabled. They must
  not be classified as ordinary transient fireflies without path-level evidence. The PT-02, PT-07,
  PT-13, PT-14, PT-15, S4A, and S5 gates now require representative offending samples to be traced
  through throughput, branch probability, directional/light PDFs, MIS weights, emission,
  finite-value sanitation, clamp placement, and the final accumulation write.

The reported Cornell-box ceiling/wall seam leak is intentionally not entered as an audit finding
because it has not been source-attributed. A separate future geometry/visibility diagnostic tracker
may cover enclosure watertightness, true geometric-normal offsets, shadow-ray endpoints, back-face
policy, and environment escape.

Existing measurements are not interchangeable:

- [performance roadmap](performance-roadmap.md) contains formal/preliminary GPU measurements for its
  named configurations;
- [P7 tracker](p7-diagnostics-tracker.md) contains canonical image statistics and matched diagnostic
  effective-FPS measurements;
- `diagnostics/project-load-benchmarks/parallel-screen-space-stages/summary.json` concerns project
  loading/startup and does not establish steady-state full-PT frame cost.

Every future table should label measurement date, revision, fresh/warm process, adapter, driver,
Agility SDK, output/render extent, mode/preset, spp/bounces, ReSTIR phases, Russian roulette, clamp,
SER state, viewport count, frame count, and whether the number is CPU wall time or a GPU timestamp.

---

## 10. Maintainability recommendations

1. **One frame-history packet.** Replace parallel optional previous-camera fields with a validated
   structure owned by the renderer. Include current/previous transforms, jitter, extents, frame token,
   camera-cut reason, and validity. Derive constants once.
2. **One surface-frame vocabulary.** Use `Ng`, `Ns`, tangent frame, front face, interface macro normal,
   and offset normal consistently. Ban ambiguous payload names such as `normal` or “geometric” for an
   interpolated value. Centralize inverse-transpose transforms and orientation.
3. **One BSDF/light sample contract.** A sample should carry event flags, direction, value/weight,
   directional PDF, discrete selection probability, eta, and first-event hit data. Match sample,
   evaluate, PDF, ReSTIR target, emissive hit MIS, and RR guides through shared helpers/tests.
4. **Versioned guide bundles.** Define PT, raster, and hybrid guide producers with explicit formats,
   spaces, extents, motion convention, hit-distance convention, exposure/pre-exposure, and a
   compatibility version. Reconstruction history keys should include that version.
5. **A narrow SDK bridge.** Keep Streamline token creation, feature tags, optimal settings, exposure,
   reset policy, capability reporting, and fallback inside one integration layer. Do not let post
   processing infer that an SDK consumed display exposure.
6. **Explicit resource/frame ownership.** Associate every transient/ring resource and descriptor with
   the fence that protects it. Avoid safety that depends on a global GPU drain. Document ownership for
   TLAS scratch/result, upload buffers, history, and capacity growth.
7. **Dirty-driven scene derivation.** Emissive distributions and other derived GPU data should have a
   visible fingerprint, rebuild reason, generation counter, and per-frame-slot propagation state.
8. **Keep modes orthogonal.** Feature switches select producers and consumers through capability/
   dependency declarations rather than scattered Boolean gates. Add a compact automated matrix for
   raster, hybrid, PT direct, PT DLSS+DLAA, PT DLSS+SR, PT RR+DLAA, PT RR+SR, reference PT, dual
   viewport, resize, and mode cut.
9. **Make diagnostics non-perturbing and named precisely.** Narrow GPU markers, AOVs, counters, and
   isolate modes need declared ownership and reset behavior. Diagnostic enablement belongs in the
   capture manifest because it can change shader liveness and cost.
10. **Keep a tested reference, not a differently biased branch.** Every real-time approximation
    should name the omitted/reweighted paths and be compared against a furnace/PDF-tested reference.
    Avoid duplicate material/light formulas in raygen, hit, and ReSTIR shaders.
11. **Turn regressions into small tests.** Preserve the transmission guide tests added to the suite;
    add CPU math tests for mixture/capability/history keys and GPU scene tests for normal transforms,
    environment rotation, emissive PDFs, and exposure. Prefer deterministic invariants before image
    thresholds.
12. **Record status with evidence.** A roadmap checkbox is not a current-source guarantee. When a
    fix lands, record revision, test/capture artifact, gate result, and known unsupported scope. Keep
    this snapshot immutable or update its header/status explicitly rather than allowing conflicting
    audits to accumulate.

---

## 11. Atomic implementation pass catalog

This catalog converts the accepted findings into deliberately small implementation tasks. It does
not change their evidence, severity, priority, or historical status. The authoritative source
snapshot remains commit `9b65f29127b818abaa142fbc851c157b409df02c`; an implementation pass must
also record the revision on which it actually runs. Passing a gate means that pass may be offered for
review, not that the next pass is authorized.

### 11.1 Catalog rules and execution graph

Every pass leaves the repository compiling and testable, records exact commands and results, and
labels CPU tests, shader compilation, D3D12 validation, GPU visual evidence, and GPU timing as
different evidence classes. A missing live check is recorded as not run, never inferred from source.
Implementation status belongs in this audit's status companion (or a clearly identified status
appendix) without rewriting sections 1-10. A failed gate produces a diagnosis or rollback
recommendation; it does not authorize threshold tuning or adjacent work.

The normal order is S0, S1, S2, S3, S4A, S4B, S5, S6, S7, S8, and S9, with one source-proven bridge:
`S5-P1` must run after `S4B-P6` and before `S4B-P7`, because PT-20 depends on PT-12's canonical
environment representation. Then `S4B-P8` closes S4B and execution resumes at `S5-P2`. S4A may be
completed and visually evaluated without S4B. Conditional product-scope or SER passes may be skipped
only with a recorded owner decision and the documented fallback gate.

Unless a pass narrows the set, regression coverage means preserving raster, hybrid RT, full PT
direct, DLSS with DLAA and every supported SR quality, RR with DLAA and every supported SR quality,
reference PT, and both Scene and Game viewports. The audit-time build/test commands in the final
evidence section are the minimum reusable baseline; pass-specific tests supplement them. GPU work
uses the S0 manifest and records adapter, driver, Agility SDK, extents, mode, quality, spp, bounces,
ReSTIR phases, clamp/RR/SER state, viewport count, seed, warm-up, and capture/timing method.

### 11.2 S0 - baseline and observability passes

### S0-P1 - Capability and runtime-configuration snapshot

**Parent stage:** S0.

**Audit findings:** PT-28 and PT-29 observability only; measurement prerequisite for all findings.

**Purpose:** Emit one immutable, machine-readable snapshot of the runtime and selected PT/SER
configuration without changing any capability decision.

**Why this is a separate pass:** Later logs and captures need a trustworthy machine/config identity;
mixing the PT-28 fix into that baseline would erase the before state.

**Prerequisites:** Accepted audit at the stated snapshot; existing application logging operational.

**Source scope:**

- **Primary implementation:** `DxrCapabilities`, device/adapter initialization, PT RTPSO creation and
  `DxrPathTracerDispatch` active-permutation state.
- **Consumers/call sites:** application startup, `RayTracingSection` status display, PT dispatch
  selection, and fallback reporting.
- **Tests/diagnostics:** a serializable capability/config record and focused schema/unit checks.
- **Documentation:** audit implementation-status companion and capture-manifest schema.

**Implementation contract:** Record adapter/driver/OS/Agility versions, DXR tier, highest shader
model, Options22 fields, compiler/library/RTPSO success, requested SER policy, selected permutation,
actual per-frame dispatch state, and fallback reason. A missing query is explicit. Logging cannot
change feature gates, PSO selection, resource creation, or frame behavior.

**Implementation outline:** (1) define a versioned record; (2) populate it at the authoritative
device/RTPSO decision points; (3) expose the same record to logs and UI diagnostics; (4) test missing,
unsupported, fallback, and supported states.

**Non-goals:** Correcting PT-28, claiming SER effectiveness, changing hints, or timing SER.

**Interim tests:** Build the existing Debug targets; run the pure serialization tests; launch twice
with the same settings and diff normalized records; confirm the fallback RTPSO still builds.

**Pass completion gate:** Two fresh-process records agree on stable fields, distinguish every state
listed in section 7.1, and reconcile the current machine's reported SM/Options22/permutation state.

**Expected visible effect:** None outside diagnostic text.

**Expected performance effect:** None with diagnostics disabled; startup/logging overhead is measured
when enabled.

**Regression surface:** Adapter selection, non-SER fallback, headless tests, startup failure paths,
and UI status rendering.

**Stop and escalate conditions:** Stop if Options22 cannot be queried through the current runtime,
the audited capability flow is contradicted, or observing state would require changing device/PSO
selection.

**Handoff to next pass:** Record schema version, two normalized logs, source revision, build/test
commands, and unresolved runtime/toolchain conflicts.

**Suggested model:** `Terra High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-28 and PT-29 observability only; measurement prerequisite for all findings. Prerequisites: Accepted audit at the stated snapshot; existing application logging operational.
> Implement only **S0-P1**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S0-P1** from `devdoc/dxr/pt/pipeline-audit-2026-07-15.md`; treat that audit as the
> authoritative snapshot. Prerequisite: the accepted audit and working startup logging. Address only
> PT-28/PT-29 observability: add a versioned machine-readable record for adapter/driver/OS/Agility,
> DXR tier, SM, Options22, compiler/library/RTPSO results, requested policy, selected and dispatched
> permutation, and fallback reason, without changing capability gates or runtime behavior. Build and
> run serialization/fresh-process log checks; preserve fallback startup. Record the implementation
> revision, changed files, exact commands, results, and missing GPU/visual evidence in the status
> companion. Stop on a runtime-contract contradiction or if observation requires behavior change.
> Do not begin S0-P2.

### S0-P2 - Application-frame and viewport identity diagnostics

**Parent stage:** S0.

**Audit findings:** PT-10 observability only.

**Purpose:** Make application frame identity, viewport identity, evaluation order, and current
Streamline tokens visible before changing token ownership.

**Why this is a separate pass:** It proves the existing multi-viewport cadence and supplies the
before evidence for S1-P3 without silently fixing it.

**Prerequisites:** S0-P1.

**Source scope:**

- **Primary implementation:** `Application::BeginFrame`/end-frame flow, `DlssContext::Evaluate`, and
  the existing internal frame index/token path.
- **Consumers/call sites:** `SceneRenderer`, each `ScreenSpaceEffects`/`DlssResolvePass` instance,
  Scene and Game viewport evaluation sites.
- **Tests/diagnostics:** structured per-frame/per-evaluation trace and a dual-viewport log assertion.
- **Documentation:** implementation-status companion and capture-manifest field definitions.

**Implementation contract:** Log one application-frame serial, stable viewport ID, per-viewport
evaluation ordinal, feature/quality, submission index if present, token supplied to Streamline, and
whether an evaluation was skipped. These are observations only; token creation/caching is unchanged.

**Implementation outline:** Add the application-frame serial at its owner, thread diagnostic context
without using it for decisions, emit one record at every evaluation/skip, and assert stable viewport
identity in a deterministic Scene+Game test.

**Non-goals:** PT-10 correction, history resets, viewport resource sharing, or SDK heuristic changes.

**Interim tests:** Debug build; single-view, dual-view, skipped-view, and no-Streamline logs; verify
diagnostics do not change tokens or evaluation count.

**Pass completion gate:** A simultaneous Scene/Game capture unambiguously shows which evaluations
share an application frame, their distinct viewport IDs, and the pre-fix token cadence.

**Expected visible effect:** None.

**Expected performance effect:** None when disabled; bounded logging overhead when enabled.

**Regression surface:** Single/dual viewport, hidden viewport, failed evaluation, and present failure.

**Stop and escalate conditions:** Stop if no authoritative application-frame boundary exists, a
viewport ID is unstable, or threading diagnostic context would alter Streamline calls.

**Handoff to next pass:** Structured trace for all four viewport cases, instrumentation overhead,
and the exact frame/token ownership diagram observed.

**Suggested model:** `Terra Medium`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-10 observability only. Prerequisites: S0-P1.
> Implement only **S0-P2**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S0-P2** from the authoritative path-tracing audit. Prerequisite: S0-P1 evidence.
> Instrument `Application` frame boundaries through `DlssContext::Evaluate` to record application
> frame serial, stable viewport ID, evaluation/skip order, feature/quality, submission index, and the
> existing Streamline token. Do not change token creation, caching, resets, or viewport resources.
> Build and capture single-view, simultaneous Scene/Game, skipped-view, and failed-evaluation logs;
> the gate is an unambiguous pre-fix cadence with no behavior change. Record revision, changed files,
> exact commands/results, and diagnostic overhead. Stop if frame or viewport ownership contradicts
> the audit or instrumentation would affect evaluation. Do not begin S0-P3.

### S0-P3 - History-key and reset-reason instrumentation

**Parent stage:** S0.

**Audit findings:** PT-01, PT-11, and PT-31 observability only.

**Purpose:** Trace every current history-compatibility input, reset request, reset consumer, and reset
reason without introducing new invalidation behavior.

**Why this is a separate pass:** S1-P4 needs before/after reset evidence; changing the key while
inventing its observability would make excessive or missing resets hard to attribute.

**Prerequisites:** S0-P2.

**Source scope:**

- **Primary implementation:** `ScreenSpaceEffects` reset setters and `SetDxrPathTracerDisplay`,
  `DlssResolvePass` camera-cut/reset flow, PT accumulation and ReSTIR history-reset owners.
- **Consumers/call sites:** AA/RR/quality/resize setters, PT guide diagnostic selection, raster/hybrid/
  PT producer switches, bloom and temporal-history consumers.
- **Tests/diagnostics:** versioned reset event/key dump and transition-sequence assertions.
- **Documentation:** implementation-status companion and diagnostic schema.

**Implementation contract:** Record current producer/guide/feature/quality/extents/camera-cut/
diagnostic inputs, existing compatibility state, requested reason bitset, reset generation, and which
history owners consume it. Do not add, suppress, or merge resets in S0.

**Implementation outline:** Define stable reason names; instrument request and consumption sites;
correlate events by application frame and viewport; add steady-state and transition log tests.

**Non-goals:** Defining the final compatibility key, correcting PT-11, changing previous-camera data,
or tuning rejection thresholds.

**Interim tests:** Debug build; scripted mode/feature/quality/resize/diagnostic transitions; steady
frames; verify existing image and reset counts are unchanged by disabled diagnostics.

**Pass completion gate:** Every existing reset is attributable to one viewport and reason; every
listed transition shows whether each PT/ReSTIR/reconstruction/bloom history was reset; steady state
has no unexplained event.

**Expected visible effect:** None.

**Expected performance effect:** None when disabled; measured logging cost only.

**Regression surface:** All temporal modes, camera cuts, resize, dual viewport, guide diagnostics,
and repeated identical setters.

**Stop and escalate conditions:** Stop if a history has no discoverable owner, a reset is consumed
implicitly without an observable point, or instrumentation would require changing reset timing.

**Handoff to next pass:** Transition log, reason dictionary, reset-owner inventory, and gaps requiring
an explicit S1 design choice.

**Suggested model:** `Terra High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-01, PT-11, and PT-31 observability only. Prerequisites: S0-P2.
> Implement only **S0-P3**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S0-P3** from the authoritative audit; prerequisite S0-P2. Instrument existing
> history compatibility and reset flow across `ScreenSpaceEffects`, `DlssResolvePass`, PT accumulation,
> ReSTIR, and bloom. Record viewport/frame, producer, guide version, feature/quality, extents,
> camera-cut/diagnostic inputs, reason bits, generation, and each consumer. Do not add or suppress a
> reset and do not fix PT-01/PT-11/PT-31. Build and run steady-state plus all named transition logs;
> accept only complete attribution with unchanged behavior. Record revision, changed files, exact
> results, and unrun visual/GPU checks. Stop on an ownerless/implicit history contract. Do not begin
> S0-P4.

### S0-P4 - Narrow PT/ReSTIR/RR GPU scopes

**Parent stage:** S0.

**Audit findings:** PT-33; PT-29 measurement prerequisite.

**Purpose:** Give each real dispatch/preparation phase an accurately named, non-overlapping GPU scope.

**Why this is a separate pass:** Marker correctness can be capture-reviewed independently and must
precede any performance conclusion.

**Prerequisites:** S0-P3.

**Source scope:**

- **Primary implementation:** `DxrDispatchContext` PT/ReSTIR orchestration, including temporal,
  spatial, boiling-filter, surface-history, and reference-maintenance dispatches.
- **Consumers/call sites:** `DxrPathTracerDispatch`, `PreparePathTracerRrBundle`, RR guide preparation,
  and enclosing post-processing markers.
- **Tests/diagnostics:** marker-name uniqueness/nesting check and one representative PIX/Nsight capture.
- **Documentation:** status companion and marker dictionary used by performance roadmaps.

**Implementation contract:** A scope name describes exactly the enclosed GPU work; temporal and
spatial are distinct; preparation copies/extraction are independently visible; no readback, barrier,
dispatch, or timestamp placement changes except what the marker API requires.

**Implementation outline:** Inventory current event boundaries, rename the mislabeled temporal event,
add missing narrow scopes, validate balanced nesting, and update capture-name consumers.

**Non-goals:** Reordering stages, changing dispatch dimensions/barriers, removing RR preparation, or
claiming a speedup.

**Interim tests:** Debug/shader build; GPU debug-layer run; capture event-tree inspection; compare
outputs and timing with markers disabled/enabled.

**Pass completion gate:** One capture shows correct, balanced nesting and separately attributable PT
megakernel, temporal, spatial, boiling, surface-history, accumulation, and RR-preparation work.

**Expected visible effect:** None.

**Expected performance effect:** No meaningful production effect; timestamp/event overhead measured.

**Regression surface:** Capture-tool integration, command-list event balance, all ReSTIR phase
combinations, direct/DLSS/RR post paths.

**Stop and escalate conditions:** Stop if a named phase spans command lists without a supported event
model, marker insertion changes synchronization, or source ownership contradicts the audited nesting.

**Handoff to next pass:** Annotated event tree, marker dictionary, enabled/disabled overhead, revision,
and exact capture configuration.

**Suggested model:** `Terra Medium`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-33; PT-29 measurement prerequisite. Prerequisites: S0-P3.
> Implement only **S0-P4**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S0-P4** from the authoritative audit; prerequisite S0-P3. Correct PT-33 by making
> `DxrDispatchContext`/post GPU events accurately and narrowly name the PT megakernel, ReSTIR temporal,
> spatial, boiling, surface-history, accumulation, and RR-preparation work. Change no dispatch,
> barrier, resource, estimator, or ordering behavior. Build, run the debug layer, and save one capture
> proving balanced unique nesting plus marker overhead. Record revision, changed files, commands and
> evidence; do not claim performance improvement. Stop if correct scopes require synchronization or
> cross-command-list redesign. Do not begin S0-P5.

### S0-P5 - Deterministic capture manifest and hash

**Parent stage:** S0.

**Audit findings:** PT-29/33 measurement prerequisite and the verification prerequisite for all later
findings.

**Purpose:** Reproduce a capture from a versioned manifest whose semantic configuration has a stable
hash and whose nondeterministic outputs are compared with an explicit policy.

**Why this is a separate pass:** It binds the preceding diagnostics into evidence without changing
rendering contracts or conflating reproducibility with any corrective pass.

**Prerequisites:** S0-P1 through S0-P4.

**Source scope:**

- **Primary implementation:** existing diagnostics/capture entry points, `DxrSettings`, camera/scene
  state serialization, deterministic seed controls, and configuration hashing.
- **Consumers/call sites:** Scene/Game viewport launch paths, PT/ReSTIR/RR configuration, capability,
  token/reset, and GPU-marker diagnostics.
- **Tests/diagnostics:** manifest round-trip/hash tests and two fresh-process canonical captures.
- **Documentation:** manifest schema plus performance roadmap/P7 tracker references and status record.

**Implementation contract:** The versioned manifest records revision, scene asset identity, saved
camera pose/path, viewport, output/render extents, feature/quality, spp/bounces, seeds, ReSTIR phases,
RR/firefly/SER states, diagnostics, warm-up/window, and the S0 capability record. Hash only normalized
semantic fields; label image/timing comparison as exact or statistical. Loading a manifest cannot
silently fall back or mutate settings.

**Implementation outline:** Define schema/normalization; serialize and validate all fields; add
load-time diagnostics for missing assets/options; save output hashes/statistics and timing metadata;
run two clean reproductions of the canonical scenes.

**Non-goals:** Fixing any finding, choosing quality thresholds, or treating project-load benchmark
data as steady-state PT timing.

**Interim tests:** Schema round trip and field-order hash stability; missing/unknown-field handling;
fresh-process sky and floor captures; image/timing repeatability check.

**Pass completion gate:** Two fresh-process repeats have the same semantic hash, complete metadata,
statistically compatible images/timings, and correctly nested S0 diagnostics with measured overhead.

**Expected visible effect:** None; deterministic seeds may make diagnostic captures repeatable only.

**Expected performance effect:** None in production; capture mode overhead is reported, not hidden.

**Regression surface:** Asset lookup, camera precision, dual viewport, settings fallback, stochastic
seed ownership, and diagnostics-disabled production behavior.

**Stop and escalate conditions:** Stop if a required setting has no authoritative owner, replay
cannot reject a silent fallback, or deterministic capture changes the production estimator.

**Handoff to next pass:** Versioned manifest, hashes, two artifact sets, repeatability statistics,
capability/reset/token logs, marker capture, and exact commands.

**Suggested model:** `Terra High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-29/33 measurement prerequisite and the verification prerequisite for all later findings. Prerequisites: S0-P1 through S0-P4.
> Implement only **S0-P5**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S0-P5** from the authoritative audit; prerequisites S0-P1 through S0-P4. Add a
> versioned capture manifest and normalized configuration hash covering revision, scene/assets,
> saved camera, viewport/extents, feature/quality, spp/bounces, seed, ReSTIR/RR/clamp/SER/diagnostic
> state, warm-up/window, and S0 capability data. Reject silent fallback and distinguish exact from
> statistical output comparison. Make no estimator or renderer-behavior fix. Build, test schema/hash
> round trips, and produce two fresh-process canonical captures with compatible images/timings and
> complete S0 logs. Record changed files, revision, commands, artifacts, and overhead. Stop if state
> ownership is missing or replay would change production behavior. Do not begin S1-P1.

### 11.3 S1 - previous-frame and history-lifecycle passes

### S1-P1 - Authoritative current/previous camera packet

**Parent stage:** S1.

**Audit findings:** PT-01, implementation contract.

**Purpose:** Give every temporal consumer the same complete current and previous camera state for its
viewport.

**Why this is a separate pass:** This is the foundational ownership change; tests/AOVs, SDK tokens,
history policy, and transmission rejection should not obscure its review.

**Prerequisites:** S0-P5.

**Source scope:**

- **Primary implementation:** `DxrPathTracerDispatch::FrameInputs` in
  `DxrPathTracerDispatch.h`, `SceneRenderer` frame-input population, and
  `DxrPathTracerDispatch.cpp` constant construction.
- **Consumers/call sites:** `path_tracer.hlsl::ComputeTransmissionVirtualMotion` and previous-receiver
  reconstruction in `restir_di_temporal.hlsl`.
- **Tests/diagnostics:** CPU packet/packing tests, constant-buffer assertions, shader compilation.
- **Documentation:** camera-packet convention and implementation-status evidence.

**Implementation contract:** Current and previous packets contain view, projection, inverse view-
projection, world-space camera position, jitter, and validity. Any retained duplicate matrices must
agree. Previous means the previous compatible rendered frame for the same viewport, not the previous
evaluation or an application-global camera. If any required member is unavailable, history is
invalidated; identity/zero is never accepted as a valid substitute. Matrix, clip, and jitter
conventions remain explicit and coherent at CPU/GPU packing.

**Implementation outline:** Introduce the packet; populate it from per-viewport camera history;
derive GPU constants only from it; remove valid-history defaults; add consistency/packing tests and
invalid-member assertions.

**Non-goals:** Temporal threshold tuning, AOV UI, token ownership, previous geometry, PT-31 policy,
or ReSTIR estimator changes.

**Interim tests:** Existing Debug build and shaders; packet round-trip/invalid-member tests;
repository search for valid default previous-view/position use; D3D12 debug-layer startup.

**Pass completion gate:** Every valid temporal dispatch carries a complete same-viewport packet;
missing data invalidates history; CPU and GPU packed values agree; identity/zero cannot enter a
valid-history frame as previous camera state.

**Expected visible effect:** Temporal output may change, but symptom removal is not claimed until
S1-P2 and S1-P6 provide live evidence.

**Expected performance effect:** Neutral.

**Regression surface:** First frame, camera cuts, jitter, Scene/Game histories, PT virtual motion,
ReSTIR temporal constants, and constant-buffer layout.

**Stop and escalate conditions:** Stop if camera history is application-global, matrix conventions
cannot be reconciled, the audited data flow is contradicted, or packing changes invalidate declared
layouts.

**Handoff to next pass:** Packet layout/conventions, assertions, packing dump, revision, changed
files, and exact build/test results.

**Suggested model:** `Sol High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-01, implementation contract. Prerequisites: S0-P5.
> Implement only **S1-P1**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S1-P1** from the authoritative audit; prerequisite S0-P5. Correct PT-01 by adding
> one per-viewport current/previous camera packet with view, projection, inverse view-projection,
> world camera position, jitter, and validity; populate it in `SceneRenderer`, derive
> `DxrPathTracerDispatch` constants from it, and invalidate history rather than use identity/zero when
> incomplete. Preserve and document existing coordinate conventions and assert duplicate values
> agree. Do not tune shaders, add AOV UI, change Streamline tokens, implement previous geometry, or
> enter ReSTIR estimator work. Build, compile shaders, run packing/invalidity tests, and search for
> remaining valid defaults. Record revision, files, commands, and results. Stop on ownership,
> convention, source/audit, or layout conflict. Do not begin S1-P2.

### S1-P2 - Previous-camera tests and temporal AOVs

**Parent stage:** S1.

**Audit findings:** PT-01, verification portion.

**Purpose:** Prove camera-domain correctness independently of denoiser or reservoir output.

**Why this is a separate pass:** Diagnostic plumbing and GPU fixtures deserve their own review after
the packet is stable and must not conceal its implementation diff.

**Prerequisites:** S1-P1.

**Source scope:**

- **Primary implementation:** gated diagnostics around
  `ComputeTransmissionVirtualMotion` and `restir_di_temporal.hlsl` previous-receiver reconstruction.
- **Consumers/call sites:** existing PT diagnostic selection through `ScreenSpaceEffects`.
- **Tests/diagnostics:** `PtTransmissionVirtualMotionOnOrbit`; new off-origin static, pane-parallax,
  and current/previous ReSTIR-target checks in `d3d12-render-tests`.
- **Documentation:** AOV units/spaces and implementation-status evidence.

**Implementation contract:** Diagnostics expose current/previous camera positions, derived opaque and
transmission motion, and current/previous receiver/target agreement without modifying production
values. Static opaque geometry at a static off-origin camera yields zero motion. Pane motion follows
the declared current-minus-previous NDC convention and existing SDK scale. Diagnostic resources are
gated, versioned, and non-authoritative.

**Implementation outline:** Add the smallest AOV/counter outputs at existing calculations; extend the
tier-5 transmission fixture; add static ReSTIR agreement; capture static and orbit cases; compare
diagnostics-off resources/shaders.

**Non-goals:** Changing S1-P1 math, rejection thresholds, RR blending, ReSTIR weights, or persistent
diagnostic cost.

**Interim tests:** Shader build; `d3d12-render-tests.exe --tier=5`; the named orbit test; static
off-origin and lateral checker captures; diagnostics-off comparison.

**Pass completion gate:** PT-01's numeric/AOV gates pass: zero static opaque motion, expected pane
parallax, known checker reprojection, and static current/previous target agreement.

**Expected visible effect:** Diagnostic views only; production changes belong to S1-P1.

**Expected performance effect:** None with diagnostics disabled; enabled overhead is recorded.

**Regression surface:** Diagnostic permutations/resources, transmission motion, ReSTIR temporal
path, and both viewports.

**Stop and escalate conditions:** Stop if the AOV changes production math, requires undeclared
payload/RTPSO growth, materially perturbs shader liveness, or contradicts S1-P1 conventions.

**Handoff to next pass:** Numeric results, AOV images, manifest/hash, diagnostics-off comparison, and
the exact tier-5 result including any teardown issue.

**Suggested model:** `Terra High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-01, verification portion. Prerequisites: S1-P1.
> Implement only **S1-P2**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S1-P2** after S1-P1. Verify PT-01 with gated AOVs/tests around transmission virtual
> motion and ReSTIR previous-receiver reconstruction; preserve the camera packet and documented NDC
> convention. Extend/run `PtTransmissionVirtualMotionOnOrbit` plus static off-origin, lateral checker,
> and static current/previous target-agreement cases. Do not alter thresholds, ReSTIR weights, RR
> behavior, production math, or payload ABI. Require numeric/AOV proof and diagnostics-off
> equivalence. Record revision, changed files, exact commands/results, manifests, and screenshots;
> distinguish printed assertions from process teardown. Stop if diagnostics perturb the contract or
> require ABI growth. Do not begin S1-P3.

### S1-P3 - Application-scoped Streamline frame tokens

**Parent stage:** S1.

**Audit findings:** PT-10.

**Purpose:** Use one Streamline token for every feature evaluation in one application frame while
keeping viewport histories separate.

**Why this is a separate pass:** SDK temporal identity is independent of camera data and the later
history-compatibility policy.

**Prerequisites:** S1-P2 and the S0-P2 cadence traces.

**Source scope:**

- **Primary implementation:** application/renderer `BeginFrame` ownership and
  `DlssContext::Evaluate` token acquisition/consumption.
- **Consumers/call sites:** `DlssResolvePass` evaluations for Scene and Game viewports.
- **Tests/diagnostics:** S0-P2 single/dual/skipped/failed-evaluation cadence assertions and SDK logs.
- **Documentation:** token versus viewport ownership and status evidence.

**Implementation contract:** Acquire/cache one SDK token per application `BeginFrame`, pass it to all
DLSS/RR evaluations in that frame, and advance once at the next application frame. Stable viewport
IDs/resources remain distinct. Skipped viewports and evaluation order do not consume extra tokens;
the global per-evaluation counter is not the default temporal-frame proxy.

**Implementation outline:** Move token ownership to application-frame state; pass it explicitly;
remove or fence off implicit `Evaluate` advancement; add cadence tests; compare SDK debug logs.

**Non-goals:** Merging viewport resources, changing compatibility keys, sizing, camera resets, or
frame/fence scheduling.

**Interim tests:** Debug/core tests; single, simultaneous, skipped, reordered, and failed-evaluation
cases; SDK and D3D12 validation.

**Pass completion gate:** All evaluations in an application frame use the same token; tokens advance
monotonically once per application frame; viewport IDs/resources remain distinct and outputs remain
valid in one- and two-viewport runs.

**Expected visible effect:** Multi-viewport temporal cadence may improve; no promised single-view
change.

**Expected performance effect:** Neutral.

**Regression surface:** Scene/Game scheduling, skipped viewports, evaluation failure, direct/DLSS/RR,
and present failure.

**Stop and escalate conditions:** Stop if the official SDK token lifetime differs from the audit,
token acquisition requires unavailable submission identity, or ownership affects present/fences.

**Handoff to next pass:** Before/after cadence logs, SDK validation, viewport captures, revision, and
commands/results.

**Suggested model:** `Sol Medium`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-10. Prerequisites: S1-P2 and the S0-P2 cadence traces.
> Implement only **S1-P3**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S1-P3** after S1-P2, using PT-10 and S0-P2 logs as authoritative. Acquire/cache one
> Streamline token per application `BeginFrame`, pass it explicitly to all Scene/Game DLSS or RR
> evaluations in that frame, retain distinct viewport IDs/resources, and remove per-evaluation
> advancement as the default. Do not change history compatibility, camera logic, sizing, or frame
> scheduling. Test single, dual, skipped, reordered, and failed-evaluation cases with SDK/D3D12
> diagnostics. Require one shared token per application frame and one-step frame cadence. Record
> revision, files, commands, results, and logs. Stop on SDK-contract or ownership conflict. Do not
> begin S1-P4.

### S1-P4 - Per-viewport history compatibility and reset propagation

**Parent stage:** S1.

**Audit findings:** PT-11.

**Purpose:** Reuse temporal history only when producer, guide, reconstruction, extent, camera, and
diagnostic semantics are compatible.

**Why this is a separate pass:** It converts S0-P3 observations into one lifecycle invariant without
mixing the separate dynamic-transmission scope.

**Prerequisites:** S1-P3 and the S0-P3 reset-owner inventory.

**Source scope:**

- **Primary implementation:** `ScreenSpaceEffects`, including `SetDxrPathTracerDisplay`,
  `SetPtRrBundleMode`, AA/RR/resize setters, and `DlssResolvePass` camera-cut/reset flow.
- **Consumers/call sites:** PT accumulation, ReSTIR, TAA/DLSS/RR, and bloom histories.
- **Tests/diagnostics:** deterministic key tests and the transition matrix.
- **Documentation:** key fields, owner-specific reset policy, and pass evidence.

**Implementation contract:** A per-viewport key contains render producer, guide producer/version,
reconstruction feature/quality, render/output extent, camera cut or camera-packet validity, and
diagnostic signal. Compare with the previous compatible rendered frame. Each incompatible transition
schedules exactly one next-use reset for affected owners; compatible frames do not reset. Commit the
new key only after its relevant frame renders. First frame/incomplete camera is invalid, and DLSS/RR
feature changes reset even at identical quality/extent.

**Implementation outline:** Define key/reasons; build it at authoritative evaluation boundaries;
route one reset to each affected owner; commit after render; add key and transition tests.

**Non-goals:** PT-31 rejection, threshold tuning, SDK extent policy, or deleting histories.

**Interim tests:** Key equality/difference matrix; raster/hybrid/PT, DLSS/RR, DLAA/SR, guide
diagnostic, resize, camera-cut, first-frame, and steady-state sequences.

**Pass completion gate:** Every incompatible transition resets exactly once; compatible steady frames
do not; no state crosses viewports; reset logs match key differences and owner policy.

**Expected visible effect:** Stale blocks/trails caused by semantic switches should be removed;
legitimate reset frames may be noisier.

**Expected performance effect:** Neutral steady state; reset frames lose reuse transiently.

**Regression surface:** All supported modes/qualities, Scene/Game, first frame, camera cuts, resize,
and diagnostic AOVs.

**Stop and escalate conditions:** Stop if a history owner is undocumented, a key field has multiple
authorities, a local reset affects another viewport, or correctness requires an unrelated algorithm.

**Handoff to next pass:** Key schema/tests, transition/reset logs, owner map, captures, and exact
commands/results.

**Suggested model:** `Sol High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-11. Prerequisites: S1-P3 and the S0-P3 reset-owner inventory.
> Implement only **S1-P4**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S1-P4** after S1-P3, using PT-11 and S0-P3's owner inventory. Add a per-viewport
> compatibility key for producer, guide/version, feature/quality, render/output extent, camera
> validity/cut, and diagnostic signal. Incompatible changes schedule exactly one owner-appropriate
> reset; compatible frames do not; commit identity only after rendering. Cover raster/hybrid/PT,
> DLSS/RR, DLAA/SR, resize, camera-cut, first-frame, and diagnostic transitions. Do not implement
> PT-31, sizing, or heuristics. Build/test and provide key-unit results and reset logs. Record revision,
> files, commands, results, and captures. Stop on ambiguous or cross-viewport ownership. Do not begin
> S1-P5.

### S1-P5 - Conservative dynamic-transmission history rejection

**Parent stage:** S1.

**Audit findings:** PT-31, rejection policy only.

**Purpose:** Reject transmission history whenever current-TLAS replay cannot represent the previous
optical path.

**Why this is a separate pass:** It relies on the camera/history contract but deliberately excludes
the much larger previous-geometry implementation.

**Prerequisites:** S1-P1 and S1-P4.

**Source scope:**

- **Primary implementation:** `path_tracer.hlsl::ComputeTransmissionVirtualMotion` and
  `pt_dielectric.hlsli::TraceTransmissionGuide`.
- **Consumers/call sites:** PT motion/guide outputs and S1-P4's rejection/reset owner.
- **Tests/diagnostics:** moving checker behind static pane, moving pane, moving receiver, camera-only
  motion, and `PtTransmissionVirtualMotionOnOrbit`.
- **Documentation:** explicitly supported dynamic-transmission scope and fallback.

**Implementation contract:** Camera-only replay may remain valid through geometry proven static over
the compatible interval. If a participating optical boundary or reconstructed receiver/background
has unrepresentable motion, mark reuse invalid through the documented per-pixel rejection or the
smallest safe viewport reset. Never call current-geometry replay exact previous motion. Existing
motion/static metadata may be consumed; no previous TLAS is introduced.

**Implementation outline:** Identify available static/motion metadata; propagate guide validity;
connect invalidity to the history owner; add the four motion cases; document supported scope.

**Non-goals:** Previous TLAS/transforms for arbitrary geometry, skinned/deforming replay, motion
heuristic tuning, or dielectric estimator changes.

**Interim tests:** Tier-5 transmission tests; four-case matrix; camera cut; static-case reuse rate;
D3D12 validation.

**Pass completion gate:** Supported camera-only/static cases retain reuse; moving pane and moving
receiver/background either use real prior geometry already available or reject deterministically,
never silently reuse current geometry.

**Expected visible effect:** Dynamic trails may become noisier rejected frames; correct arbitrary
dynamic optical replay is not promised.

**Expected performance effect:** Neutral to lower convergence in rejected cases; no measured claim.

**Regression surface:** Thin/solid glass, moving emitters/receivers, static panes, camera motion,
DLSS/RR, and both viewports.

**Stop and escalate conditions:** Stop if detection requires a previous AS/new scene-wide motion
system, the SDK cannot express safe rejection, or unrelated modes/viewports would reset.

**Handoff to next pass:** Supported-scope statement, validity traces, four-case outputs, reuse/reset
statistics, and exact test evidence.

**Suggested model:** `Sol High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-31, rejection policy only. Prerequisites: S1-P1 and S1-P4.
> Implement only **S1-P5**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S1-P5** after S1-P1 and S1-P4. Address PT-31 only through conservative rejection:
> camera-only replay may remain valid for geometry proven static, but any moving optical boundary or
> receiver/background not representable in the previous domain must invalidate reuse. Use the S1
> history contract and document per-pixel or smallest safe reset. Do not build a previous TLAS, add
> media, or tune thresholds. Run the named orbit test and moving-checker/static-pane, moving-pane,
> moving-receiver, and camera-only cases. Record revision, files, commands, results, traces, and
> captures. Stop if prior geometry/new motion infrastructure or unrelated resets are required. Do
> not begin S1-P6.

### S1-P6 - Integrated temporal-lifecycle validation

**Parent stage:** S1.

**Audit findings:** PT-01, PT-10, PT-11, and PT-31 verification.

**Purpose:** Prove the completed S1 contract across cameras, viewports, modes, cuts, and motion.

**Why this is a separate pass:** This is a cross-system review gate, not an opportunity for new code
or heuristic changes.

**Prerequisites:** S1-P1 through S1-P5.

**Source scope:**

- **Primary implementation:** test/capture orchestration; production edits only to diagnose and
  correct an S1 gate failure within its owning pass.
- **Consumers/call sites:** complete camera/token/history/transmission path.
- **Tests/diagnostics:** all S1 focused tests and canonical S0 manifests, including Cornell output set.
- **Documentation:** S1 evidence bundle and pass statuses.

**Implementation contract:** No new rendering invariant. Validate same-viewport/compatible-frame
previous state, one application token, one reset per incompatibility, deterministic unsupported-
transmission rejection, and unchanged steady opaque reuse.

**Implementation outline:** Run CPU/shader/D3D tests; exercise static off-origin, orbit, cut,
dual-view, transition, and dynamic-transmission matrices; collect reset/reuse/token metrics; capture
the identical Cornell pose; diagnose or recommend rollback without tuning.

**Non-goals:** S2 exposure/sizing, S3 surfaces, ReSTIR tuning, or performance-improvement claims.

**Interim tests:** Audit build/CTest/tier-5 commands, SDK/D3D validation, and fresh-process manifests.

**Pass completion gate:** All PT-01/10/11 gates and PT-31 rejection cases pass; there is no cross-
viewport state; static opaque reuse/frame time stays within repeatability noise; the identical-pose
Cornell set archives raw real-time PT radiance, RR guides, final RR output, and accumulated reference.

**Expected visible effect:** S1 may remove deterministic trails; residual blur is reported without
assigning a single cause.

**Expected performance effect:** Steady state should be neutral; reset/rejection frames lose reuse.
Timing here proves non-regression only.

**Regression surface:** Raster, hybrid, full PT, direct, all DLSS/RR qualities, reference PT, and
Scene/Game.

**Stop and escalate conditions:** Stop on SDK/D3D validation errors, unresolved cross-viewport state,
contradictory camera/token evidence, or a regression requiring work outside S1.

**Handoff to next pass:** Full commands/results, logs, manifests, Cornell four-output set, regression
matrix, and any rollback recommendation.

**Suggested model:** `Sol High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-01, PT-10, PT-11, and PT-31 verification. Prerequisites: S1-P1 through S1-P5.
> Implement only **S1-P6**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Execute only validation pass **S1-P6** after S1-P1 through S1-P5. Do not implement S2 or tune
> temporal heuristics. Build and run the audit tests; validate static off-origin, pane orbit, camera
> cut, Scene+Game, producer/feature/quality/guide transitions, and all dynamic-transmission cases with
> SDK/D3D diagnostics. Require PT-01/10/11 gates, deterministic PT-31 rejection, no cross-viewport
> state, and steady opaque reuse within repeatability noise. Capture identical-pose Cornell raw real-
> time PT radiance, RR guides, final RR, and accumulated reference. Record revision, files, exact
> commands/results, logs, manifests, screenshots, and evidence categories. Diagnose or recommend
> rollback on failure. Stop before S2-P1.

### 11.4 S2 - DLSS, RR, exposure, sizing, and allocation passes

### S2-P1 - Separate reconstruction guidance from display exposure

**Parent stage:** S2.

**Audit findings:** PT-09.

**Purpose:** Apply authored display EV exactly once in every output path while sending only supported
exposure guidance to reconstruction.

**Why this is a separate pass:** Exposure math and color-space ownership can be proven independently
of extent/allocation restructuring.

**Prerequisites:** S1-P6.

**Source scope:**

- **Primary implementation:** `DlssResolvePass` exposure setup and bloom/tonemap handoff.
- **Consumers/call sites:** `DlssContext` DLSS/RR evaluation options, direct output, bloom, and
  renderer tonemapper.
- **Tests/diagnostics:** static HDR patches at EV `-2`, `0`, and `+2` across the mode matrix.
- **Documentation:** reconstruction/pre-exposure/display-EV contract and status evidence.

**Implementation contract:** Reconstruction guidance/pre-exposure and authored display EV are
distinct. Ordinary DLSS receives only supported guidance; the integrated RR path receives no
unsupported exposure. Direct and every successful Streamline branch apply display EV exactly once
after reconstruction. Bloom retains its declared HDR working space. No success path forces authored
EV to zero on the assumption that reconstruction applied display exposure.

**Implementation outline:** Name/separate values; restrict SDK inputs by feature; remove post-
evaluation EV suppression; preserve bloom/tonemap order and fallback; add crossed exposure tests.

**Non-goals:** Optimal settings, allocation, jitter, auto-exposure redesign, grading, or PT radiance.

**Interim tests:** Debug/shader build; deterministic HDR patches; direct, DLSS+DLAA/SR, and
RR+DLAA/SR smoke tests after history settling.

**Pass completion gate:** EV `-2` to `+2` has the declared four-stop relation in direct, DLSS with
all supported qualities, and RR with all supported qualities; history settling does not change final
exposure and no branch applies EV twice.

**Expected visible effect:** Manual exposure becomes mode-consistent; currently wrong branches may
visibly change brightness.

**Expected performance effect:** Neutral.

**Regression surface:** Direct, DLAA/SR, RR, bloom, tonemap, manual EV, and evaluation fallback.

**Stop and escalate conditions:** Stop if the official SDK contract conflicts with the audit,
bloom's working space is undefined, or correction requires a wider color-pipeline redesign.

**Handoff to next pass:** Exposure diagram, numeric results, settled screenshots, SDK logs, revision,
changed files, and commands/results.

**Suggested model:** `Sol Medium`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-09. Prerequisites: S1-P6.
> Implement only **S2-P1**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S2-P1** after S1-P6, addressing PT-09. Separate reconstruction guidance/pre-
> exposure from authored display EV in `DlssResolvePass`; send guidance only where the selected SDK
> feature supports it, send no unsupported RR exposure, and apply display EV exactly once after
> direct/DLSS/RR output while preserving bloom's declared HDR space. Do not touch sizing, jitter,
> allocation, auto exposure, or PT radiance. Test EV `-2/0/+2` across direct, DLSS+DLAA/all SR, and
> RR+DLAA/supported SR after settling. Record revision, files, exact commands/results, numeric values,
> screenshots, and SDK validation. Stop on SDK or bloom-space contradiction. Do not begin S2-P2.

### S2-P2 - SDK recommendation query and extent-owner model

**Parent stage:** S2.

**Audit findings:** PT-30, query/data-model portion.

**Purpose:** Make a cached SDK recommendation, rather than a fixed ratio, the canonical planned
render extent for each viewport/feature/quality tuple.

**Why this is a separate pass:** Query correctness can be exercised in shadow mode before resources
consume the result.

**Prerequisites:** S2-P1.

**Source scope:**

- **Primary implementation:** the optimal-settings entry points in `DlssContext` and a per-viewport
  recommendation/cache model.
- **Consumers/call sites:** the fixed-ratio policy in `ScreenSpaceEffects` and reconstruction state.
- **Tests/diagnostics:** feature/quality/output-size recommendation matrix, cache, and forced failure.
- **Documentation:** cache key, fallback, extent ownership, and RR no-DRS policy.

**Implementation contract:** Query/cache recommendations by viewport, output extent, DLSS versus RR,
and DLAA/SR quality. Record recommended extent and supported range. DLAA remains native and RR obeys
its no-arbitrary-DRS rule. Failure yields an explicit logged fallback, never a silent ratio. This
pass compares planned values in shadow mode; S2-P4 alone activates allocation.

**Implementation outline:** Define recommendation/key; call the loaded DLSS and corresponding RR
paths; validate results; replace scattered planning authority with one API; add cache/failure logs.

**Non-goals:** Reallocation, tags, jitter, exposure, or PT-24 resource preparation changes.

**Interim tests:** Pure key/cache tests; every feature/quality at several sizes; forced failure;
different Scene/Game output sizes.

**Pass completion gate:** Every exposed tuple reports a valid recommendation or explicit fallback;
cache invalidation follows its key; fixed ratios are no longer represented as SDK recommendations;
active rendering remains unchanged.

**Expected visible effect:** None until S2-P4.

**Expected performance effect:** Negligible query/cache overhead; no GPU change yet.

**Regression surface:** SDK init/failure, mode enumeration, output resize, and dual viewport.

**Stop and escalate conditions:** Stop if a required SDK query is unavailable, returned ranges do not
support an exposed mode, or planned and active extent ownership cannot be separated safely.

**Handoff to next pass:** Recommendation matrix, cache tests, fallback logs, API contract, revision,
and commands/results.

**Suggested model:** `Sol High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-30, query/data-model portion. Prerequisites: S2-P1.
> Implement only **S2-P2**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S2-P2** after S2-P1, addressing PT-30's query/model portion. Query and cache SDK
> extents/ranges per viewport, output extent, DLSS versus RR, and DLAA/SR quality; expose one planned-
> extent object and log explicit fallback. Keep active allocation unchanged in shadow mode until
> S2-P4 and preserve RR's no-arbitrary-DRS policy. Do not change jitter, tags, resources, exposure,
> or PT-24 preparation. Test every tuple, cache invalidation, two viewport sizes, and forced failure.
> Record revision, files, commands/results, the matrix, and SDK logs. Stop on SDK contradiction or if
> planning cannot be isolated. Do not begin S2-P3.

### S2-P3 - Feature/quality jitter-period contract

**Parent stage:** S2.

**Audit findings:** PT-30, Ultra Performance jitter subfinding and matrix portion.

**Purpose:** Give every feature/quality tuple a sufficient per-viewport jitter cycle with explicit
current/previous ownership.

**Why this is a separate pass:** Jitter cadence is temporal input math and should not be hidden by
resource recreation.

**Prerequisites:** S2-P2 and S1-P4.

**Source scope:**

- **Primary implementation:** jitter sequence/selection in `ScreenSpaceEffects` and
  `DlssResolvePass` camera input flow.
- **Consumers/call sites:** primary-ray jitter, motion constants, and DLSS/RR options.
- **Tests/diagnostics:** pure period/wrap/phase tests for every tuple and reset transition.
- **Documentation:** tuple-to-period table, phase owner, and motion convention.

**Implementation contract:** Ordinary DLSS supports the audited periods 8/18/24/32/72 for
DLAA/Quality/Balanced/Performance/Ultra Performance. RR supplies at least 32 phases; the current 64
may remain. Phase is per viewport, advances once per compatible rendered application frame, and
previous jitter belongs to the previous compatible frame. Feature/quality/extent incompatibility
resets it once via S1. Existing motion sign/scale remains unless an official contradiction is found.

**Implementation outline:** Centralize tuple/period; ensure sequence capacity; bind phase advancement
to application-frame/viewport history; reset via the compatibility key; add all wrap/transition tests.

**Non-goals:** New jitter-pattern research, allocation, motion-vector redesign, or denoiser tuning.

**Interim tests:** Period/index wraps including 72; dual/skipped viewport; feature, quality, extent,
and camera-cut transitions; shader/build checks.

**Pass completion gate:** Every tuple cycles the required period without truncation/out-of-range;
current/previous phase advances per compatible rendered frame and resets exactly once on change.

**Expected visible effect:** DLSS Ultra Performance stability may change; other modes retain intended
cadence.

**Expected performance effect:** Neutral.

**Regression surface:** Every DLSS/RR quality, motion, camera cuts, and Scene/Game cadence.

**Stop and escalate conditions:** Stop if SDK period requirements conflict, the sequence cannot grow
without changing the camera contract, or phase ownership conflicts with S1.

**Handoff to next pass:** Period matrix, phase traces, transition output, revision, and exact tests.

**Suggested model:** `Sol High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-30, Ultra Performance jitter subfinding and matrix portion. Prerequisites: S2-P2 and S1-P4.
> Implement only **S2-P3**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S2-P3** after S2-P2. Address PT-30's jitter matrix: ordinary DLSS uses audited
> periods 8/18/24/32/72 and RR at least 32, with per-viewport phase advancing once per compatible
> rendered application frame and previous jitter owned by the prior compatible frame. Reset through
> S1 on feature/quality/extent changes. Preserve motion sign/scale; do not redesign patterns,
> allocation, or tags. Add full period/wrap, dual/skipped-view, and transition tests. Record revision,
> files, commands/results, and phase traces. Stop on SDK/S1 ownership contradiction. Do not begin
> S2-P4.

### S2-P4 - Commit SDK extents to allocation, tags, and resets

**Parent stage:** S2.

**Audit findings:** PT-30 completion; PT-24 format/state prerequisite only.

**Purpose:** Make per-viewport resources and SDK tags match the queried feature/quality extent through
resize and transitions.

**Why this is a separate pass:** It is the controlled consumer conversion after query and jitter are
independently proven.

**Prerequisites:** S2-P1 through S2-P3 and S1-P4.

**Source scope:**

- **Primary implementation:** `ScreenSpaceEffects` allocation/recreation/resize,
  `DlssResolvePass` resource selection/tagging, and `DlssContext` evaluation options.
- **Consumers/call sites:** PT HDR/depth/motion/albedo/normal-roughness and RR-preparation resources.
- **Tests/diagnostics:** SDK tag validation, state/extent logs, resize, and the audited motion-format A/B.
- **Documentation:** active extent, format, tag, state, reset, and lifetime contract.

**Implementation contract:** S2-P2 owns internal extent; the viewport owns output extent. Recreate all
inputs coherently, update motion/depth scale and tag extents/states, and reset once via S1. DLAA is
native, SR uses recommendations, and RR receives no arbitrary DRS. Tags use tracked D3D12 states and
the verified lifetime. Resolve the RGBA16F motion uncertainty with SDK validation or a controlled
supported-format conversion. Retain RR preparation until S7-P1.

**Implementation outline:** Convert allocations; update resize/per-viewport ownership; bind tag
formats/states/scales; connect one reset; run SDK/debug and motion-format A/B validation.

**Non-goals:** PT direct tagging, five-pass removal, exposure/jitter redesign, or raster/hybrid removal.

**Interim tests:** Every feature/quality; resize/aspect; different viewport sizes; fallback; resource-
state validation; motion/depth AOVs and SDK logs.

**Pass completion gate:** Each tuple allocates/dispatches at supported extents; SDK accepts settings
and tags; transitions reset once; no crop/cross-viewport allocation occurs; motion-format behavior is
evidenced rather than assumed.

**Expected visible effect:** SR sharpness/stability or crop behavior may change; DLAA stays native.

**Expected performance effect:** GPU/memory cost can rise or fall with SDK-selected extent.

**Regression surface:** Direct, DLSS/RR x DLAA/SR, resize/aspect, states, motion/depth scale, and both
viewports.

**Stop and escalate conditions:** Stop on unsupported SDK tag/format, undocumented resource
consumer, state/lifetime hazard, or a need to remove PT-24 resources prematurely.

**Handoff to next pass:** Extent/tag matrix, SDK logs, resize traces, format evidence, D3D validation,
revision, and exact commands/results.

**Suggested model:** `Sol High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-30 completion; PT-24 format/state prerequisite only. Prerequisites: S2-P1 through S2-P3 and S1-P4.
> Implement only **S2-P4**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S2-P4** after S2-P1 through S2-P3 and S1 history work. Complete PT-30 by making
> S2-P2 recommendations own per-viewport allocation, resize, tag extents/states, motion/depth scaling,
> and one reset per change. DLAA stays native, SR uses SDK extents, and RR has no arbitrary DRS.
> Resolve RGBA16F motion-tag uncertainty by SDK validation or a controlled supported-format path;
> retain RR preparation for S7. Test every feature/quality, two viewport sizes, resize/aspect,
> fallback, state validation, and AOVs. Record revision, files, commands/results, logs, and captures.
> Stop on format, lifetime, hidden-consumer, or SDK contradiction. Do not begin S2-P5.

### S2-P5 - Full reconstruction mode-matrix validation

**Parent stage:** S2.

**Audit findings:** PT-09 and PT-30 verification; PT-24 prerequisites.

**Purpose:** Prove exposure, extent, jitter, tags, allocation, and reset behavior in every supported
output combination.

**Why this is a separate pass:** It is a cross-system gate and must not absorb optimization or
sharpening work.

**Prerequisites:** S2-P1 through S2-P4.

**Source scope:**

- **Primary implementation:** test/capture orchestration only; failures route to their owning pass.
- **Consumers/call sites:** direct, DLSS/RR feature-quality paths, bloom/tonemap, and both viewports.
- **Tests/diagnostics:** HDR patches, fine geometry, subpixel motion, resize/aspect, and mismatched
  Scene/Game sizes.
- **Documentation:** complete S2 evidence matrix and pass statuses.

**Implementation contract:** No new production invariant. Validate direct; DLSS+DLAA; DLSS+every
exposed SR quality; RR+DLAA; RR+every supported SR quality. Record extents, jitter period, token/view,
reset reason, tags/states, EV output, and SDK diagnostics in every cell.

**Implementation outline:** Build/run CPU, shader, D3D, and SDK checks; execute the matrix; exercise
resize/aspect/dual sizes; compare invariants; diagnose or roll back failures without tuning.

**Non-goals:** PT-24 optimization, S3 material/guide changes, sharpening tweaks, or speedup claims.

**Interim tests:** Audit build/CTest/D3D commands, fresh manifests, and direct/reconstruction images.

**Pass completion gate:** PT-09/PT-30 gates pass in every supported cell; all transitions reset once;
there is no lost exposure, crop, jitter error, invalid tag, or cross-viewport allocation.

**Expected visible effect:** Documents S2 changes without claiming transport symptom resolution.

**Expected performance effect:** Timing detects regression only; no optimization claim.

**Regression surface:** All preserved modes, qualities, viewports, resize/aspect, and fallback.

**Stop and escalate conditions:** Stop on SDK validation failure, unsupported exposed cell, out-of-S2
mode regression, or incompatible extent/format requirement.

**Handoff to next pass:** Complete matrix, manifests, screenshots, SDK/D3D logs, revision, commands,
results, and timing classification.

**Suggested model:** `Sol High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-09 and PT-30 verification; PT-24 prerequisites. Prerequisites: S2-P1 through S2-P4.
> Implement only **S2-P5**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Execute only **S2-P5** after S2-P1 through S2-P4. Do not implement S3 or optimize PT-24. Validate
> direct, DLSS+DLAA/all exposed SR, and RR+DLAA/all supported SR across exposure patches, fine
> geometry, subpixel motion, resize/aspect, and differently sized Scene/Game viewports. Record
> extents, jitter period, frame token/view ID, reset reason, tags/states, EV, and SDK diagnostics.
> Require every PT-09/PT-30 gate and exactly one transition reset. Record revision, exact commands/
> results, manifests, logs, screenshots, and evidence categories. Diagnose or recommend rollback on
> failure; do not tune. Stop before S3-P1.

### 11.5 S3 - surface-frame, footprint, and RR-guide passes

### S3-P1 - Canonical geometric and shading surface frames

**Parent stage:** S3.

**Audit findings:** PT-03.

**Purpose:** Carry true world-space geometric normal `Ng` separately from hemisphere-consistent
shading normal `Ns` and assign every consumer explicitly.

**Why this is a separate pass:** It establishes vocabulary and may change the shader ABI before
transforms, dielectric events, guides, and footprints depend on it.

**Prerequisites:** S2-P5.

**Source scope:**

- **Primary implementation:** `path_tracer.hlsl` closest hit/payload `normalOct` flow and PT surface-
  record declarations.
- **Consumers/call sites:** offsets, front face/sidedness, boundaries/media, ReSTIR surfaces,
  emissive PDF/Jacobian reconstruction, and BSDF inputs.
- **Tests/diagnostics:** `Ng`/`Ns` AOVs; flat/smooth triangles, mirrored winding, back-face hits, and
  emissive-PDF reconstruction.
- **Documentation:** `Ng`, `Ns`, tangent frame, orientation, offset, and optical macro-normal policy.

**Implementation contract:** `Ng` comes from true surface geometry under an explicit front-face
orientation; `Ns` is interpolated/normal-mapped BSDF normal made hemisphere-consistent with `Ng`.
Both are world-space and never aliased under ambiguous names. `Ng` owns offsets, sidedness,
boundaries/media, geometry terms, PDFs, and Jacobians; `Ns` owns BSDF shading. Declare the interface
macro-normal policy for smooth authored glass. Any payload/surface-record change updates all
producers/consumers and the matching SBT/RTPSO payload declarations.

**Implementation outline:** Define fields/conventions; populate true `Ng` and initial `Ns`; convert
geometric consumers; keep BSDF consumers on `Ns`; add AOV/tests and verify ABI sizes.

**Non-goals:** PT-05 transform correction beyond mechanical field introduction, normal-map LOD,
dielectric sharing, rough-dielectric math, or offset-algorithm research.

**Interim tests:** Shader compilation; payload/RTPSO size checks; flat/smooth/mirrored/back-face AOVs;
emissive PDF reconstruction; search for misleading geometric-normal use.

**Pass completion gate:** `Ng` and `Ns` remain distinct through every active record; each audited
consumer has declared ownership; no unscaled analytic ray offsets through the wrong side; all ABI
declarations agree.

**Expected visible effect:** Surface/boundary behavior may change; final glass/RR improvement is not
yet claimed.

**Expected performance effect:** Possible register/payload cost, to be measured at S3-P7.

**Regression surface:** Smooth glass, backfaces, emissive MIS, ReSTIR surfaces, SBT/payload, shared
raster/hybrid helpers.

**Stop and escalate conditions:** Stop if the macro-normal policy is not defined, payload growth
exceeds declarations, or a geometric-normal consumer cannot be classified.

**Handoff to next pass:** Consumer map, conventions, AOVs, payload/RTPSO sizes, revision, and exact
commands/results.

**Suggested model:** `Sol High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-03. Prerequisites: S2-P5.
> Implement only **S3-P1**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S3-P1** after S2-P5, addressing PT-03. Introduce distinct world-space true `Ng`
> and hemisphere-consistent `Ns` in the audited closest-hit/payload/surface flow; use `Ng` for
> offsets, sidedness, boundaries/media, geometric PDFs/Jacobians and identity, and `Ns` for BSDF
> shading. Declare the smooth-glass macro/interface-normal policy and update all required SBT/RTPSO/
> payload declarations. Do not fix inverse-transpose scale, LOD, rough dielectric, or hit distance
> except for mechanical compatibility. Add AOVs and flat/smooth/mirrored/back-face/emissive-PDF tests.
> Record revision, files, commands/results, AOVs, and ABI sizes. Stop on policy, consumer, or ABI
> ambiguity. Do not begin S3-P2.

### S3-P2 - Shared inverse-transpose normal transformation

**Parent stage:** S3.

**Audit findings:** PT-05.

**Purpose:** Transform object-space normals correctly under non-uniform scale, shear, reflection,
and orientation changes.

**Why this is a separate pass:** It corrects one geometric invariant after `Ng`/`Ns` ownership is
explicit.

**Prerequisites:** S3-P1.

**Source scope:**

- **Primary implementation:** normal transform in `hit_shading.hlsli`, including
  `ComputeWorldShadingNormal`/normal-map frame inputs.
- **Consumers/call sites:** PT and shared hit shaders; raster `hlsl_common.hlsl`/`lit.vs.hlsl` as the
  accepted parity reference.
- **Tests/diagnostics:** non-uniform sphere/capsule, sheared mesh, negative/mirrored transform AOVs.
- **Documentation:** inverse-transpose, normalization, determinant, and orientation convention.

**Implementation contract:** Transform object normals by the available world-to-object transpose/
inverse-transpose, normalize, and handle mirrored tangent/frame orientation consistently. S3-P1
continues to own true geometry orientation and shading-hemisphere correction. Raster and ray paths
agree for identical geometry without changing raster output.

**Implementation outline:** Centralize helper; replace object-to-world 3x3 normal use; convert all
required hit consumers; validate mirrored tangent orientation; compare analytic and raster normals.

**Non-goals:** Surface-vocabulary redesign, texture filtering, material changes, or offset redesign.

**Interim tests:** Shader build; analytic angular-error checks; scaled sphere/capsule, shear, negative
scale, and raster/PT AOV comparison.

**Pass completion gate:** Analytic normals meet tolerance; PT/raster AOVs agree under non-uniform
scale/shear; mirrored orientation is consistent; no required old transform consumer remains.

**Expected visible effect:** Scaled/sheared shading and guides may visibly correct.

**Expected performance effect:** Neutral.

**Regression surface:** Instancing, negative scale, tangent normal maps, and raster/hybrid shared code.

**Stop and escalate conditions:** Stop if world-to-object data is unavailable, the raster convention
conflicts, or helper conversion changes an unrelated renderer contract.

**Handoff to next pass:** Helper contract, consumer search, angular errors, parity AOVs, revision, and
commands/results.

**Suggested model:** `Sol High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-05. Prerequisites: S3-P1.
> Implement only **S3-P2**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S3-P2** after S3-P1, addressing PT-05. Centralize a robust inverse-transpose
> object-normal-to-world helper in `hit_shading.hlsli`, including normalization and mirrored/
> determinant orientation, and convert all required hit-shader consumers while treating raster
> `hlsl_common.hlsl`/`lit.vs.hlsl` as parity reference. Do not change surface ownership, LOD,
> dielectric, material, or offset models. Compile shaders and test non-uniform sphere/capsule, shear,
> negative scale, and raster/PT `Ng`/`Ns` agreement. Record revision, files, commands/results, and
> AOVs. Stop if transform data or shared-helper scope conflicts. Do not begin S3-P3.

### S3-P3 - Shared first dielectric event for radiance and guides

**Parent stage:** S3.

**Audit findings:** PT-04.

**Purpose:** Compute one first-interface dielectric event and consume it in both transport and its
transmission guide.

**Why this is a separate pass:** It depends on the S3 surface contract but must not be combined with
S4A's rough-dielectric estimator rewrite.

**Prerequisites:** S3-P1 and S3-P2.

**Source scope:**

- **Primary implementation:** `path_tracer.hlsl` material-bounce/primary-guide flow and
  `pt_dielectric.hlsli::TraceTransmissionGuide`.
- **Consumers/call sites:** first radiance direction, virtual motion, guide albedo, and event metadata.
- **Tests/diagnostics:** first-event direction/type AOVs, grazing/normal-mapped panes, and the two
  named tier-5 transmission tests.
- **Documentation:** incident/direction/eta/macro-normal/event conventions and layout impact.

**Implementation contract:** At the primary dielectric hit, create one event containing incident
point, oriented interface macro normal, eta pair, reflection/refraction/TIR type, sampled world-space
outgoing direction, and validity. Radiance and guide generation consume it rather than recompute with
different normals. Preserve current probabilities/weights until PT-02/PT-07; event sharing alone is
not an estimator fix.

**Implementation outline:** Define local/shared record; compute after finalized surface policy; route
to both consumers; remove duplicate guide direction selection; add equality tests/AOVs.

**Non-goals:** Rough BSDF/PDF, mixture weighting, media stack, or later-bounce guide redesign.

**Interim tests:** Shader compilation; `PtTransmissionGuideAlbedoBands`;
`PtTransmissionVirtualMotionOnOrbit`; normal-mapped/grazing reflection/refraction/TIR comparisons.

**Pass completion gate:** Radiance/guide first-event type and direction agree within documented
epsilon for supported thin/solid/TIR cases; no hidden path-specific interface normal remains.

**Expected visible effect:** Glass guide/radiance alignment may improve; rough-glass energy remains
unresolved.

**Expected performance effect:** Neutral or slight duplicate-work saving.

**Regression surface:** Thin/solid glass, normal mapping, grazing/TIR, virtual motion, guide albedo.

**Stop and escalate conditions:** Stop if sharing requires the S4A estimator, macro-normal policy is
unresolved, or event storage violates payload/RTPSO limits.

**Handoff to next pass:** Event layout, equality results/AOVs, supported scope, revision, and exact
test output.

**Suggested model:** `Sol High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-04. Prerequisites: S3-P1 and S3-P2.
> Implement only **S3-P3**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S3-P3** after S3-P1/P2, addressing PT-04. Compute one first dielectric event--hit
> point, oriented macro normal, eta pair, reflection/refraction/TIR type, outgoing world direction,
> and validity--and use it for both material-bounce radiance and `TraceTransmissionGuide`. Preserve
> existing sampling probabilities/weights; do not implement PT-02/PT-07, media, or later-bounce
> changes. Compile shaders and run both named transmission tests plus grazing, normal-mapped, and TIR
> event AOV comparisons. Record revision, files, commands/results, and AOVs. Stop if sharing requires
> rough-BSDF work or breaks ABI. Do not begin S3-P4.

### S3-P4 - Actual-event RR specular hit distance

**Parent stage:** S3.

**Audit findings:** PT-08.

**Purpose:** Report only the actual sampled supported specular-event distance, otherwise the SDK
omission convention.

**Why this is a separate pass:** It consumes S3-P3 event identity and can remove the detached trace
without mixing footprint work.

**Prerequisites:** S3-P3.

**Source scope:**

- **Primary implementation:** `path_tracer.hlsl` primary guide block and detached ideal-mirror trace.
- **Consumers/call sites:** specular-guide alpha, RR hit-distance extraction, and specular/virtual motion.
- **Tests/diagnostics:** mirror, rough-metal, mixed-lobe, thin-glass, and foreground-disocclusion A/B.
- **Documentation:** hit-distance origin/units, supported-event, miss, and omission conventions.

**Implementation contract:** Hit distance is world-space distance from the primary surface to the
first actual sampled supported secondary specular event. Non-specular, unsupported, miss, and invalid
events use the documented SDK omitted/non-specular representation. Never blend `maxDistance` with an
unrelated mirror hit. Remove the detached mirror traversal if no other consumer exists and pair the
distance with motion from the same event. Keep extraction/layout until S7-P1.

**Implementation outline:** Capture result from S3-P3; encode supported/omitted cases; remove trace/
roughness blend after consumer search; retain bundle extraction; run guide/motion/image/timing A/B.

**Non-goals:** PT direct resource tagging, RR tuning, rough dielectric, or a replacement synthetic trace.

**Interim tests:** Shader build; distance AOV; all five scenes; SDK validation; narrow PT GPU timing.

**Pass completion gate:** Every non-omitted value maps to the actual event; omissions follow the SDK;
the detached traversal has no remaining consumer; RR temporal metrics improve or match without false
distance, with PT GPU time lower/equal within measurement confidence.

**Expected visible effect:** Rough/mixed-specular RR stability may change; blur removal is not promised.

**Expected performance effect:** Structural traversal removal should be non-negative; timing is
required before claiming a saving.

**Regression surface:** Mirror, rough metal, mixed lobes, thin glass, motion, RR+DLAA/SR.

**Stop and escalate conditions:** Stop on SDK omission ambiguity, payload overflow, or an undocumented
consumer of the mirror trace.

**Handoff to next pass:** A/B guides/images/metrics/timing, SDK log, consumer search, revision, and
commands/results.

**Suggested model:** `Sol High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-08. Prerequisites: S3-P3.
> Implement only **S3-P4**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S3-P4** after S3-P3, addressing PT-08. Replace the detached mirror/blended guide
> with world-space distance from the primary surface to the actual first sampled supported specular
> event; use the SDK omission convention otherwise and pair it with that event's motion. Remove the
> extra traversal only after proving no consumer; keep bundle extraction for S7. Test mirror, rough
> metal, mixed lobe, thin glass, and disocclusion with AOVs, SDK validation, temporal metrics, and
> narrow PT timing. Record revision, files, commands/results, captures, and search. Stop on SDK,
> payload, or hidden-consumer conflict. Do not begin S3-P5.

### S3-P5 - Ray-footprint and normal-map LOD foundation

**Parent stage:** S3.

**Audit findings:** PT-06, initial footprint portion.

**Purpose:** Make a finite ray footprint part of the hit contract and use it for normal-map filtering
instead of fixed mip 0.

**Why this is a separate pass:** It fixes the immediate proven defect with a simple footprint before
the research-heavier scattering-aware propagation.

**Prerequisites:** S3-P4.

**Source scope:**

- **Primary implementation:** ray-cone state and closest-hit `ApplyWorldNormalMap(..., 0.0)` in
  `path_tracer.hlsl` plus footprint helpers in `hit_shading.hlsli`.
- **Consumers/call sites:** material texture LOD calculations and surface-frame finalization.
- **Tests/diagnostics:** footprint/selected-LOD AOVs and frequency/distance/roughness baseline sweep.
- **Documentation:** cone representation/units, UV conversion, finite fallback, and ABI impact.

**Implementation contract:** Carry finite non-negative cone width/spread in the existing world/ray
convention to the hit, convert through the established triangle/UV footprint, and use explicit LOD
for normal maps and compatible maps. Camera spread plus travel distance forms this foundation. Clamp
only to mip bounds; singular/non-finite input takes a documented conservative finite fallback. Any
layout change matches payload/SBT/RTPSO declarations.

**Implementation outline:** Define/validate representation; propagate existing camera/travel
footprint; replace normal mip 0; add fallback/AOVs; validate monotonic behavior and ABI.

**Non-goals:** Diffuse/GGX/refraction widening, anisotropic footprint research, or material-energy/
roughness changes.

**Interim tests:** Shader compilation; payload sizes; flat-map equivalence; high-frequency normal map
over distance; finite/monotonic AOV checks.

**Pass completion gate:** No active normal-map path is hardwired to mip 0; LOD coarsens monotonically
with basic footprint; flat/low-frequency materials remain equivalent; cone state and ABI are valid.

**Expected visible effect:** Distant normal-map aliasing may reduce; secondary rough-path stability
awaits S3-P6.

**Expected performance effect:** Small LOD ALU cost; possible cache benefit, unmeasured.

**Regression surface:** Normal-mapped materials, primary/secondary hits, texture boundaries, payload/
register pressure.

**Stop and escalate conditions:** Stop on duplicate/contradictory footprint representation, missing
UV data, or payload growth that invalidates RTPSO declarations.

**Handoff to next pass:** Representation, AOV sweep, ABI sizes, shader/test logs, revision, and files.

**Suggested model:** `Sol High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-06, initial footprint portion. Prerequisites: S3-P4.
> Implement only **S3-P5**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S3-P5** after S3-P4, addressing PT-06's foundation. Carry a finite non-negative
> camera/travel ray cone to closest hit, convert it through the existing world/triangle/UV footprint,
> and replace normal-map mip 0 with explicit footprint LOD. Add finite fallback, mip clamping,
> footprint/LOD AOVs, and ABI checks. Do not add diffuse/GGX/refraction widening or tune materials.
> Compile shaders and test flat-map equivalence plus high-frequency maps over distance with monotonic
> finite LOD. Record revision, files, commands/results, AOVs, and payload sizes. Stop on representation,
> UV-data, or RTPSO conflict. Do not begin S3-P6.

### S3-P6 - Scattering-aware ray-cone propagation

**Parent stage:** S3.

**Audit findings:** PT-06, extended propagation portion.

**Purpose:** Conservatively widen footprints through ideal reflection, rough/diffuse scattering, and
refraction without singular/non-finite behavior.

**Why this is a separate pass:** Its approximation and overblur risk require independent mathematical
and visual review after fixed mip 0 is gone.

**Prerequisites:** S3-P5.

**Source scope:**

- **Primary implementation:** bounce-loop cone state and material-event outputs in `path_tracer.hlsl`
  and dielectric event data in `pt_dielectric.hlsli`.
- **Consumers/call sites:** S3-P5 hit footprint/LOD contract.
- **Tests/diagnostics:** roughness/distance/event sweeps at grazing, eta/TIR, diffuse, mirror, metal,
  and glass.
- **Documentation:** propagation equations/approximations, finite bounds, and filtering-bias policy.

**Implementation contract:** Preserve S3-P5 representation/units. Ideal reflection does not gain
artificial rough widening; GGX/diffuse use a documented conservative spread; refraction accounts for
eta/interface geometry; grazing/singular/TIR cases remain finite/bounded. Rougher scattering cannot
produce a narrower footprint than its ideal counterpart. This is a filtering approximation, not a
BSDF/PDF change, and its blur bias is declared.

**Implementation outline:** Specify/test event equations; apply after each event; add finite bounds;
extend AOV sweeps; compare temporal stability, blur, energy, and shader pressure.

**Non-goals:** PT-07, anisotropic footprint redesign, denoiser tuning, or arbitrary LOD bias.

**Interim tests:** Deterministic formulas; shader build; mirror/diffuse/GGX/refraction/TIR sweeps;
high-frequency temporal sequence.

**Pass completion gate:** Formula tests pass; cone state is finite/non-negative/monotonic; shimmer
decreases without unacceptable overblur or energy drift; the mirror limit remains stable.

**Expected visible effect:** Secondary texture shimmer may reduce; overblur is an explicit risk.

**Expected performance effect:** Added ALU/register pressure with possible cache benefit; both timed.

**Regression surface:** High roughness, grazing, eta extremes, TIR, normal maps, bounce depth, and
shader pressure.

**Stop and escalate conditions:** Stop if no validated rule fits the representation, tests contradict
the approximation, or only symptom-tuned bias satisfies images.

**Handoff to next pass:** Equations, unit output, AOV/temporal comparisons, shader metrics, revision,
and exact commands/results.

**Suggested model:** `Sol High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-06, extended propagation portion. Prerequisites: S3-P5.
> Implement only **S3-P6**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S3-P6** after S3-P5, completing PT-06 propagation. Define/test cone updates for
> ideal reflection, GGX/diffuse, refraction, grazing, and TIR in S3-P5 units. Keep values finite/non-
> negative, ensure rougher events do not narrow below ideal, and document filtering bias. Do not
> change BSDF sampling/PDFs, implement PT-07, or tune denoiser thresholds. Run deterministic math,
> shader compilation, event/roughness/eta sweeps, AOVs, temporal high-frequency comparisons, and
> shader-pressure/timing checks. Record revision, files, equations, commands/results, and captures.
> Stop if tests reject the model or arbitrary bias is required. Do not begin S3-P7.

### S3-P7 - Integrated surface, guide, and RR validation

**Parent stage:** S3.

**Audit findings:** PT-03, PT-04, PT-05, PT-06, and PT-08 verification.

**Purpose:** Prove radiance, geometry, footprint, event guide, and RR inputs describe the same surface
and sampled event.

**Why this is a separate pass:** It is the stage review gate and must not hide transport or heuristic
work.

**Prerequisites:** S3-P1 through S3-P6.

**Source scope:**

- **Primary implementation:** test/capture orchestration; only diagnosed S3 corrections route back to
  the owning pass.
- **Consumers/call sites:** raw PT radiance, surface/ReSTIR records, RR guide bundle, and final output.
- **Tests/diagnostics:** all S3 analytic scenes and identical-pose Cornell comparison.
- **Documentation:** complete S3 evidence, timing categories, and unresolved observations.

**Implementation contract:** No new invariant. Validate `Ng`/`Ns`, transform parity, shared dielectric
event, exact/omitted distance, footprint/LOD, motion, payload/SBT/RTPSO ABI, and raster/hybrid
preservation. The user-observed mirror blur remains evidence requiring verification, not proof of one
cause.

**Implementation outline:** Run CPU/shader/D3D/ABI checks; exercise geometry/pane/mirror/normal-map/
disocclusion scenes; compare radiance with guide direction/distance/motion; collect RR matrix and
narrow timing; recapture the saved Cornell pose and report each boundary separately.

**Non-goals:** PT-02/PT-07, ReSTIR tuning, blur thresholds, S4 implementation, or making the Cornell
seam leak a finding.

**Interim tests:** Audit build/test commands; all S3 AOVs; D3D12 validation; raster/hybrid normal
goldens; RR+DLAA/SR captures.

**Pass completion gate:** Every PT-03/04/05/06/08 analytic/AOV gate passes; guide and radiance events
agree; no false hit distance; ABI/D3D validation are clean; raster/hybrid normals do not regress; RR
metrics improve or match. Archive the identical-pose raw real-time PT radiance, RR guides, final RR,
and accumulated reference, with mirror sharpness reported separately.

**Expected visible effect:** Consolidates S3 corrections; residual blur remains verification evidence.

**Expected performance effect:** Net direction unknown; separate removed-trace saving and cone cost
only when measured.

**Regression surface:** Raster, hybrid, full PT, direct, DLSS/RR qualities, reference PT, relevant
materials, and both viewports.

**Stop and escalate conditions:** Stop on ABI/validation failure, radiance-guide disagreement, raster/
hybrid regression, or a required fix outside S3. Do not promote the seam observation to a finding.

**Handoff to next pass:** Full matrix, manifests, AOVs, Cornell four-output set, timing, ABI/D3D logs,
revision, and unresolved evidence.

**Suggested model:** `Sol Ultra`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-03, PT-04, PT-05, PT-06, and PT-08 verification. Prerequisites: S3-P1 through S3-P6.
> Implement only **S3-P7**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Execute only **S3-P7** after S3-P1 through S3-P6; do not implement S4. Validate PT-03/04/05/06/08
> across flat/smooth/mirrored/scaled/sheared geometry, panes, mirror/roughness, high-frequency normal
> maps, mixed lobes, and disocclusion. Require consistent `Ng/Ns`, transform parity, one radiance/
> guide dielectric event, actual-or-omitted distance, finite monotonic footprint, clean ABI/D3D, and
> no raster/hybrid normal regression. Capture the identical Cornell pose with raw real-time PT, RR
> guides, final RR, and accumulated reference; report mirror sharpness without assigning one cause.
> Do not add the seam leak as a finding or tune heuristics. Record revision, commands/results,
> manifests, AOVs, screenshots, and timing categories. Stop on any out-of-stage dependency.

### 11.6 S4A - core reference-estimator passes

### S4A-P1 - Separate finite sanitation from optional clamping

**Parent stage:** S4A.

**Audit findings:** PT-15.

**Purpose:** Make finite-value sanitation unconditional while keeping the firefly clamp a separately
measured, explicitly biased option.

**Why this is a separate pass:** This robustness invariant is independent of estimator math and
establishes safe values for later outlier tracing.

**Prerequisites:** S0-P5 and S3-P7.

**Source scope:**

- **Primary implementation:** `hit_shading.hlsli::ClampRadiance` and PT accumulation/output writes in
  `path_tracer.hlsl`.
- **Consumers/call sites:** clamp settings, PT/RR-facing outputs, and dispatch/readback plumbing.
- **Tests/diagnostics:** focused finite-value helper tests and GPU injection of NaN, Inf, overflow,
  and ordinary HDR.
- **Documentation:** separate sanitation/clamp manifest fields and status evidence.

**Implementation contract:** Split always-on sanitation from optional luminance clamp without
changing the replacement policy. Sanitation precedes the clamp and every accumulation, guide, or
reconstruction-facing write. Clamp-disabled finite HDR stays bitwise or epsilon equivalent. Count
non-finite replacements and clamp events separately with safe, non-perturbing ownership.

**Implementation outline:** Extract sanitation; call it independently at audited boundaries; clamp
afterward only when enabled; add separate counters and injections; label logs unambiguously.

**Non-goals:** Threshold tuning, estimator fixes, RR heuristics, accumulation formats, or unrelated
raster/hybrid sanitation.

**Interim tests:** Shader/Debug build; helper tests; clamp-on/off injection; relevant tier-5 PT tests.

**Pass completion gate:** NaN/Inf/overflow never reaches accumulation or RR-facing output in either
clamp state; sanitation alone leaves ordinary finite HDR unchanged; counters discriminate events.

**Expected visible effect:** None for valid finite scenes; poison propagation may disappear where it
existed.

**Expected performance effect:** Negligible sanitation overhead; no measured saving.

**Regression surface:** Reference/real-time PT, direct/RR, clamp on/off, FP16 guides, accumulation reset.

**Stop and escalate conditions:** Stop on unaudited non-PT consumers, unsafe counter/readback
ownership, or unexpected change to finite values.

**Handoff to next pass:** Injection matrix, counter example, revision, files, commands/results, and
diagnostics-off comparison.

**Suggested model:** `Terra High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-15. Prerequisites: S0-P5 and S3-P7.
> Implement only **S4A-P1**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S4A-P1** after S0/S3 gates. Address PT-15 by splitting `ClampRadiance` into
> always-on finite sanitation and an independently enabled biased luminance clamp; sanitize before
> all PT accumulation and RR-facing writes, preserve finite HDR, and count sanitation/clamp events
> separately. Do not tune thresholds, change estimators, or touch unrelated modes. Build Debug,
> compile shaders, inject NaN/Inf/overflow with clamp on/off, and run relevant PT tests. Require
> poison-free outputs and unchanged ordinary HDR. Record revision, files, commands/results, and
> status. Stop on an unaudited consumer, unsafe counter lifetime, or finite-value change. Do not begin
> S4A-P2.

### S4A-P2 - Deterministic persistent-sample path tracing

**Parent stage:** S4A.

**Audit findings:** PT-02, PT-07, PT-14, and PT-15 partially; section-9 persistent over-bright-pixel
observation.

**Purpose:** Trace a representative offending sample from branch selection to final accumulation
without changing estimator behavior.

**Why this is a separate pass:** Baseline path evidence must predate mathematical fixes and any
classification of persistent pixels.

**Prerequisites:** S4A-P1 and S0-P5.

**Source scope:**

- **Primary implementation:** bounce, direct/MIS, environment, emission, sanitation, clamp, and
  accumulation paths in `path_tracer.hlsl`; dielectric event data in `pt_dielectric.hlsli`.
- **Consumers/call sites:** `DxrPathTracerDispatch` constants and existing diagnostic/readback flow.
- **Tests/diagnostics:** deterministic viewport/pixel/sample/path/bounce selector, bounded trace
  buffer, overflow flag, and disabled equivalence.
- **Documentation:** trace schema/version and capture-manifest fields.

**Implementation contract:** Record throughput before/after updates, branch probability, event and
direction, BSDF/BTDF value/directional PDF, light-selection/directional PDFs, MIS weights, emission/
environment, eta/TIR, sanitation, clamp input/output, and final accumulation value. Disabled mode
cannot alter RNG consumption, control flow, bindings, or output. Use existing fence-safe readback.

**Implementation outline:** Define bounded record/selector; instrument audited boundaries; report
overflow/truncation; prove disabled equivalence; capture a reported pixel if available or preserve an
exact reproduction procedure.

**Non-goals:** Fixing the path, calling it an ordinary firefly, changing clamp placement, building a
general GPU debugger, or investigating the seam leak.

**Interim tests:** Layout tests; shader build; repeated trace determinism; diagnostics-off hash;
buffer-overflow behavior; live reference capture when available.

**Pass completion gate:** A selected path is traceable through every required field to its final
write; disabled output is unchanged; no transient-firefly conclusion is made without mechanism-level
evidence.

**Expected visible effect:** None; diagnostics only.

**Expected performance effect:** None disabled; enabled overhead is recorded.

**Regression surface:** Reference accumulation, clamp/CDF states, viewport, resize/reset, readback lifetime.

**Stop and escalate conditions:** Stop if instrumentation changes RNG/control flow, cannot identify
the final sample, requires unsafe ownership, or exposes an emitter-specific path; route the latter to S5.

**Handoff to next pass:** Trace schema, config hash, baseline trace, disabled equivalence, revision,
commands/results, and emitter-specific handoff if present.

**Suggested model:** `Sol High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-02, PT-07, PT-14, and PT-15 partially; section-9 persistent over-bright-pixel observation. Prerequisites: S4A-P1 and S0-P5.
> Implement only **S4A-P2**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S4A-P2** after S4A-P1 and S0-P5. Add a deterministic bounded trace for selected
> reference-PT samples covering throughput, branch probability, event/direction, BSDF/BTDF and PDF,
> light PDFs, MIS, emission/environment, eta/TIR, sanitation, clamp, and final accumulation. Disabled
> diagnostics must not change RNG, control flow, bindings, or output; use fence-safe readback. Do not
> fix math, classify >20,000-sample bright pixels as ordinary fireflies, or investigate the seam.
> Build/compile, prove disabled equivalence, test overflow, and capture a representative path if
> available. Record revision, files, exact results, and artifacts. Stop if instrumentation perturbs
> the estimator or needs out-of-scope ownership. Do not begin S4A-P3.

### S4A-P3 - Correct the partial-transmission mixture

**Parent stage:** S4A.

**Audit findings:** PT-02.

**Purpose:** Make the reference mixture expectation equal the authored dielectric/opaque weighted sum.

**Why this is a separate pass:** The discrete-mixture defect has a small independent proof and must
not be obscured by rough-dielectric work.

**Prerequisites:** S4A-P2.

**Source scope:**

- **Primary implementation:** `path_tracer.hlsl::SampleMaterialBounce` reference branch.
- **Consumers/call sites:** reference throughput update, branch-selection helpers, and path trace.
- **Tests/diagnostics:** deterministic/Monte Carlo weights `0`, `.01`, `.5`, `.99`, and `1`.
- **Documentation:** reference versus named real-time mixture policy and pass evidence.

**Implementation contract:** Keep branch probability separate from conditional directional PDF.
Each branch contributes `(coefficient * sampled_lobe) / selection_probability`; where coefficient
equals probability, no extra inverse factor remains. Endpoints do not divide by zero. The named
real-time glass heuristic remains unchanged.

**Implementation outline:** Add pure expected-value fixture; expose coefficients/probabilities;
remove duplicate division or express both branches canonically; trace pre/post compensation; run
endpoint/convergence checks.

**Non-goals:** PT-07, real-time correction, roulette/clamp tuning, emissive, or environment changes.

**Interim tests:** CPU expectation; seeded confidence interval; shader build; lobe sweep; trace review.

**Pass completion gate:** All weights match the explicit sum; endpoint variance is finite; traces
show no unintended `1/w` or `1/(1-w)` amplification.

**Expected visible effect:** Reference energy corrects and rare partial-transmission weights may
reduce; this does not prove all persistent pixels are solved.

**Expected performance effect:** Neutral mean cost; variance may improve.

**Regression surface:** Reference transmission endpoints, mixed metallic/transmission, path length,
and real-time/reference separation.

**Stop and escalate conditions:** Stop if source coefficients differ from the audit, another consumer
requires compensated weights, or Monte Carlo tests contradict the derivation.

**Handoff to next pass:** Formula, deterministic/statistical results, endpoint traces, revision, and
confirmation that real-time output was untouched.

**Suggested model:** `Sol Medium`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-02. Prerequisites: S4A-P2.
> Implement only **S4A-P3**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S4A-P3** after S4A-P2. Correct PT-02 in `SampleMaterialBounce`: represent authored
> coefficients and discrete selection probabilities explicitly, apply `(coefficient * sampled_lobe)
> / selection_probability`, remove duplicate inverse-probability amplification, and handle endpoints.
> Keep conditional directional PDFs separate and the real-time heuristic unchanged. Test deterministic
> and seeded Monte Carlo weights `0/.01/.5/.99/1`; inspect persistent-path throughput. Build/compile
> and require the correct expectation with finite endpoint variance. Record revision, files, commands/
> results, traces, and status. Stop if source or tests contradict the derivation. Do not begin S4A-P4.

### S4A-P4 - Freeze the rough-dielectric mathematical contract

**Parent stage:** S4A.

**Audit findings:** PT-07 partially.

**Purpose:** Establish a reviewed, testable rough-dielectric oracle before production shader conversion.

**Why this is a separate pass:** Rough BTDF conventions are consequence-heavy and must be proven
independently from shader plumbing.

**Prerequisites:** S3-P3 and S4A-P3.

**Source scope:**

- **Primary implementation:** contract specification for `pt_dielectric.hlsli` and a focused CPU/
  reference math fixture; no active-renderer switch.
- **Consumers/call sites:** future sample/evaluate/PDF helpers and MIS conversion.
- **Tests/diagnostics:** furnace, sample/PDF, reciprocity/adjoint, eta, grazing, TIR, roughness grid.
- **Documentation:** selected Walter/PBRT/Heitz convention, equations, measures, and tolerances.

**Implementation contract:** Declare direction orientation, interface frame, GGX/VNDF, `D`, `G`,
Fresnel, reflection/refraction half vectors and Jacobians, radiance transport/eta scaling, discrete
Fresnel event probability, solid-angle PDFs, TIR, and smooth-delta threshold. A sample carries event,
direction, value/weight, conditional directional PDF, discrete probability, eta, and delta status.
Continuous and delta measures never mix.

**Implementation outline:** Write conventions/equations; build independent fixture; add full grid;
review identities/units; freeze the contract before production code.

**Non-goals:** Production behavior, material retuning, MIS wiring, medium stacks, or visual approval.

**Interim tests:** CPU math, numerical integration, sampled histogram versus analytic PDF, and
direction-swap/adjoint checks.

**Pass completion gate:** Equations/tests agree over the grid within stated confidence and no eta,
measure, transport-mode, TIR, or delta ambiguity remains.

**Expected visible effect:** None.

**Expected performance effect:** None.

**Regression surface:** Future reflection/refraction, grazing/TIR, eta pairs, and smooth transition.

**Stop and escalate conditions:** Stop if cited conventions cannot reconcile with project direction/
frame rules, furnace/PDF tests fail, or S3's interface policy must change.

**Handoff to next pass:** Reviewed equations, test vectors/tolerances, design decisions, revision, and
exact commands/results.

**Suggested model:** `Sol Ultra`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-07 partially. Prerequisites: S3-P3 and S4A-P3.
> Implement only **S4A-P4**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S4A-P4** after S3's surface/event contract and S4A-P3. Treat the audit and its
> Walter/PBRT/Heitz references as authoritative; change no production renderer. Specify/test one GGX
> rough dielectric: direction/frame, D/G/F, reflection/refraction Jacobians, radiance eta scaling,
> discrete Fresnel choice, solid-angle PDFs, TIR, and smooth-delta threshold. Add deterministic,
> integration, histogram, furnace, adjoint/reciprocity, eta, grazing, TIR, and roughness-grid tests.
> Require matched sample/evaluate/PDF math with tolerances. Record revision, files, equations, and
> exact results. Stop on a reference/project contradiction or failed math. Do not begin S4A-P5.

### S4A-P5 - Canonical dielectric data and pure helpers

**Parent stage:** S4A.

**Audit findings:** PT-07 partially.

**Purpose:** Give production shaders one typed event representation and one set of pure microfacet
helpers without activating the new estimator.

**Why this is a separate pass:** API/layout changes can compile and be reviewed before sampling or
rendering behavior changes.

**Prerequisites:** S4A-P4.

**Source scope:**

- **Primary implementation:** canonical types and pure helpers in `pt_dielectric.hlsli`.
- **Consumers/call sites:** declarations in `path_tracer.hlsl` and the CPU oracle mirror only.
- **Tests/diagnostics:** S4A-P4 vectors, shader-layout/static checks, all PT permutations.
- **Documentation:** types, units/measures, direction rules, and ABI assessment.

**Implementation contract:** Add canonical inputs/results and pure D/G/F, half-vector, Jacobian, eta,
and validity helpers matching P4 and S3 interface semantics. Delta/continuous are explicit tags. No
production call switches. Avoid payload/SBT growth unless essential; every ABI change must match RTPSO.

**Implementation outline:** Add types; implement helpers against vectors; compile permutations; add
layout checks; retain the existing production sampler.

**Non-goals:** Sampling activation, evaluate/PDF wiring, MIS, threshold tuning, or convenient payload
expansion.

**Interim tests:** Oracle vectors; all shader libraries; Debug build; payload/RTPSO declaration review;
active-output equivalence.

**Pass completion gate:** Helpers match the oracle; all permutations compile; active output and ABI
remain unchanged or explicitly validated.

**Expected visible effect:** None.

**Expected performance effect:** None in the active path.

**Regression surface:** Shader ABI, helper naming, direction/normal orientation, PT permutations.

**Stop and escalate conditions:** Stop if helpers require undeclared payload/SBT changes, existing
consumers use incompatible directions, or output changes before activation.

**Handoff to next pass:** API/layout diff, vector output, compile logs, ABI sizes, revision, and files.

**Suggested model:** `Sol High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-07 partially. Prerequisites: S4A-P4.
> Implement only **S4A-P5**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S4A-P5** after S4A-P4. Add canonical rough-dielectric input/event types and pure
> GGX D/G/F, eta, half-vector, refraction-Jacobian, and validity helpers in `pt_dielectric.hlsli`, with
> explicit delta/continuous tags and S3 interface semantics. Use P4 vectors as oracle. Do not switch
> production sampling/evaluation, change MIS, tune materials, or grow payload/SBT state without
> escalation. Compile every permutation, run math/layout tests and Debug build, and prove active
> output unchanged. Record revision, files, commands/results, and ABI assessment. Stop on ABI or
> direction conflict. Do not begin S4A-P6.

### S4A-P6 - Canonical rough-dielectric sampling

**Parent stage:** S4A.

**Audit findings:** PT-07 partially.

**Purpose:** Implement the sampling half of the frozen continuous model in isolation.

**Why this is a separate pass:** Distribution and RNG behavior must match the oracle before analytic
evaluation or production MIS changes.

**Prerequisites:** S4A-P5.

**Source scope:**

- **Primary implementation:** canonical replacement/helper for
  `pt_dielectric.hlsli::SampleDielectricInterface`.
- **Consumers/call sites:** isolated shader test path only; active production remains old.
- **Tests/diagnostics:** direction/event histograms, Fresnel frequency, TIR/eta/grazing/invalid cases.
- **Documentation:** sampling representation, RNG dimensions, and partial PT-07 status.

**Implementation contract:** Sample the declared GGX VNDF, choose reflection/refraction with the
frozen Fresnel convention, return valid local/world directions and the complete event, and handle TIR
deterministically. PDFs use P4's solid-angle/discrete convention; rough continuous events never use
the old delta sentinel.

**Implementation outline:** Implement transform/event selection; populate record/validity; compare
histograms; test singular fallback; keep production gated to old path.

**Non-goals:** Evaluate/PDF implementation, MIS, smooth activation, or variance tuning.

**Interim tests:** Seeded histograms; event ratios; eta/TIR vectors; invalid counts; shader/Debug build.

**Pass completion gate:** Event/direction frequencies agree with P4 within confidence; no unexplained
invalid sample; no rough event reports a delta sentinel; production output is unchanged.

**Expected visible effect:** None.

**Expected performance effect:** None until activation.

**Regression surface:** RNG dimensions, TIR, eta orientation, grazing validity, compilation.

**Stop and escalate conditions:** Stop if the oracle cannot be matched, RNG dimensions collide with
undocumented consumers, or production MIS must change to test the sampler.

**Handoff to next pass:** Histogram report, RNG note, invalid counts, compiled artifacts, revision,
and exact commands/results.

**Suggested model:** `Sol High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-07 partially. Prerequisites: S4A-P5.
> Implement only **S4A-P6**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S4A-P6** after S4A-P5. Add canonical GGX VNDF rough-dielectric sampling per P4:
> explicit reflection/refraction choice, eta, TIR, continuous directional PDF fields, and no rough-
> event delta sentinel. Validate through an isolated path; do not activate production, add evaluate/
> PDF, change MIS, or tune variance. Run seeded direction/event histograms, grazing/TIR/eta vectors,
> invalid counts, shader compilation, and Debug build. Require oracle agreement and unchanged active
> output. Record revision, files, RNG usage, commands/results, and status. Stop on oracle/RNG/consumer
> conflict. Do not begin S4A-P7.

### S4A-P7 - Matched rough-dielectric evaluation and PDF

**Parent stage:** S4A.

**Audit findings:** PT-07 partially.

**Purpose:** Complete matched continuous evaluate/PDF functions for the canonical sampler.

**Why this is a separate pass:** It isolates analytic evaluation and measure conversion from active
MIS integration.

**Prerequisites:** S4A-P6.

**Source scope:**

- **Primary implementation:** canonical evaluation/PDF helpers in `pt_dielectric.hlsli`.
- **Consumers/call sites:** focused tests and isolated shader test path only.
- **Tests/diagnostics:** sample-versus-PDF, evaluate/sample, integration, furnace, adjoint, eta/TIR.
- **Documentation:** active equations, numerical fallbacks, and partial status.

**Implementation contract:** Reflection/transmission evaluation includes frozen D/G/F, cosines,
eta/radiance scaling, and directional Jacobian. PDF combines discrete Fresnel probability and
conditional solid-angle density. Delta events use discrete-mass semantics, never fabricated
continuous density.

**Implementation outline:** Implement reflection then refraction; handle invalid half vectors,
hemispheres, and TIR; cross-check sampled events; integrate numerically.

**Non-goals:** Active call-site conversion, direct/MIS, smooth-threshold changes, or medium tracking.

**Interim tests:** P4 vectors; histograms; integration/furnace; direction swap; shader/Debug build.

**Pass completion gate:** Sample, evaluate, and PDF agree throughout roughness/eta space within stated
confidence and remain finite at grazing/TIR boundaries; active output is unchanged.

**Expected visible effect:** None.

**Expected performance effect:** None until activation.

**Regression surface:** PDF measure, eta, front/back orientation, roughness extremes, TIR.

**Stop and escalate conditions:** Stop if furnace/integration or adjoint tests fail, or any convention
differs from P4.

**Handoff to next pass:** Agreement tables, integration output, bounded fallbacks, revision, and
commands/results.

**Suggested model:** `Sol High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-07 partially. Prerequisites: S4A-P6.
> Implement only **S4A-P7**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S4A-P7** after S4A-P6. Add matched reflection/refraction evaluate and PDF helpers
> using P4 D/G/F, eta/radiance, half-vector, Jacobian, discrete-event, and solid-angle conventions.
> Rough events are continuous; smooth delta remains discrete. Do not convert production or change
> MIS. Run oracle vectors, sample/PDF histograms, integration, furnace, adjoint/reciprocity, grazing,
> eta, and TIR tests; compile/build and prove active output unchanged. Record revision, files, exact
> results, and evidence. Stop on any mathematical contradiction. Do not begin S4A-P8.

### S4A-P8 - Integrate rough dielectric with throughput and MIS

**Parent stage:** S4A.

**Audit findings:** PT-07 partially.

**Purpose:** Replace active rough-dielectric delta fiction with the canonical continuous event across
transport and MIS consumers.

**Why this is a separate pass:** This estimator-changing conversion must be atomic across consumers
and follows completed math/helpers.

**Prerequisites:** S4A-P7.

**Source scope:**

- **Primary implementation:** `pt_dielectric.hlsli`, `path_tracer.hlsl::SampleMaterialBounce`,
  throughput, direct lighting, and BSDF MIS.
- **Consumers/call sites:** emissive/environment hit-side MIS and persistent-path diagnostics.
- **Tests/diagnostics:** focused PT-07 suite, all shader permutations, rough-glass reference scenes.
- **Documentation:** active event contract and reference versus named real-time policy.

**Implementation contract:** Rough reflection/refraction uses canonical event value/PDF exactly once
in throughput and every MIS weight. Direct and hit-side evaluators use the same function. Smooth
events remain delta and do not enter continuous competing PDFs. S3's interface normal, eta, direction,
and guide event remain shared. Real-time approximations remain separate.

**Implementation outline:** Convert sampler/throughput; convert direct/hit competing PDFs; remove
rough unit-weight/sentinel assumptions; update trace; search old consumers; compile/test all paths.

**Non-goals:** Smooth-threshold selection, media, reciprocal opaque materials, or ReSTIR tuning.

**Interim tests:** Sentinel/duplicate search; math suite; shader/Debug/core build; rough-glass GPU checks.

**Pass completion gate:** No active rough event is delta; every sample/evaluate/PDF/MIS consumer
agrees; focused energy/PDF tests remain green.

**Expected visible effect:** Rough-glass energy/noise may change materially; symptom closure awaits validation.

**Expected performance effect:** More ALU/divergence is possible and not yet accepted.

**Regression surface:** Rough glass, direct/emissive/environment lighting, TIR, guides, and both PT modes.

**Stop and escalate conditions:** Stop on an undocumented evaluator, need to change S5 ReSTIR target,
or payload/RTPSO invalidation.

**Handoff to next pass:** Consumer inventory/search, tests, initial images, shader metrics readiness,
revision, and exact results.

**Suggested model:** `Sol High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-07 partially. Prerequisites: S4A-P7.
> Implement only **S4A-P8**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S4A-P8** after S4A-P7. Activate canonical rough-dielectric sample/evaluate/PDF
> across `SampleMaterialBounce`, throughput, direct MIS, and emissive/environment hit MIS; apply value
> and PDF exactly once, with smooth events discrete and rough events continuous. Retain S3 interface
> data and separate real-time policy. Do not tune the threshold, add media, change opaque materials,
> or tune ReSTIR. Remove all active rough-delta assumptions. Build/compile all permutations and run
> PT-07 math/furnace plus focused GPU checks. Record revision, files, searches, commands/results, and
> captures. Stop on hidden evaluator, ABI, or S5 dependency. Do not begin S4A-P9.

### S4A-P9 - Establish the smooth-delta transition

**Parent stage:** S4A.

**Audit findings:** PT-07 partially.

**Purpose:** Make the smooth/rough boundary explicit, gap-free, and consistent in value, event, and MIS.

**Why this is a separate pass:** Threshold behavior can conceal correct continuous math and needs an
isolated sweep rather than material retuning.

**Prerequisites:** S4A-P8.

**Source scope:**

- **Primary implementation:** threshold/event dispatch in `pt_dielectric.hlsli` and material bounce.
- **Consumers/call sites:** delta/continuous MIS classification and guide event metadata.
- **Tests/diagnostics:** dense roughness sweep around the frozen threshold with eta/TIR variations.
- **Documentation:** threshold and approximation policy.

**Implementation contract:** Exactly one representation is active at every roughness. Below the
declared threshold use exact discrete smooth dielectric; above use continuous rough evaluation/PDF.
Sampler, evaluator, PDF, MIS, and guide flags use one predicate. No double count or PDF gap. Any
inherent visible discontinuity is measured, not hidden by remapping authored material.

**Implementation outline:** Centralize threshold; convert all predicates; sweep both sides with fixed
RNG/converged statistics; verify guide/radiance event; record policy.

**Non-goals:** Artistic remapping, temporal filtering, clamp tuning, or changing P4 math.

**Interim tests:** Dense sweep; classification; furnace/PDF; guide AOV; shader build.

**Pass completion gate:** All consumers classify identically; no overlap/gap; energy stays within the
declared statistical tolerance across transition.

**Expected visible effect:** Stable smooth-to-rough transition; exact boundary may visibly change.

**Expected performance effect:** Delta/continuous workload mix may change; no claim before timing.

**Regression surface:** Near-zero roughness, TIR, grazing, guide direction, and MIS.

**Stop and escalate conditions:** Stop if no threshold satisfies the contract, material semantics need
a product choice, or guides cannot represent the same event.

**Handoff to next pass:** Threshold sweep, event AOVs, confidence intervals, revision, and declaration.

**Suggested model:** `Sol High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-07 partially. Prerequisites: S4A-P8.
> Implement only **S4A-P9**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S4A-P9** after S4A-P8. Centralize/enforce the smooth-delta threshold across
> sampling, evaluation, PDF, MIS, and guide event metadata: exact discrete smooth semantics below,
> continuous canonical rough model above, with no gap/overlap. Do not remap authored roughness, tune
> materials/filters, or change the model. Run dense roughness, normal/grazing, eta/TIR, furnace/PDF,
> guide, shader, and Debug tests. Require consistent classification and statistically stable energy.
> Record revision, files, commands/results, plots/AOVs, and policy. Stop if a product material choice
> is required. Do not begin S4A-P10.

### S4A-P10 - Validate rough-dielectric energy and statistics

**Parent stage:** S4A.

**Audit findings:** PT-07.

**Purpose:** Close PT-07 with independent analytic, statistical, visual, and timing evidence.

**Why this is a separate pass:** Validation must assess the frozen active model without opportunistic
model/threshold changes.

**Prerequisites:** S4A-P9.

**Source scope:**

- **Primary implementation:** test/capture harness only; no intended estimator edit.
- **Consumers/call sites:** reference rough-glass scenes and PT megakernel marker.
- **Tests/diagnostics:** furnace, sample/PDF, eta/TIR, adjoint, transition, roughness, temporal variance.
- **Documentation:** PT-07 evidence, confidence, known numerical bounds, and performance category.

**Implementation contract:** Validate the exact active model, separating CPU math, shader build, GPU
visual, and timing evidence. Reference is the oracle; real-time deviations are distinct. A failed gate
causes diagnosis/rollback, not tuning.

**Implementation outline:** Run full suite; capture matched images/variance; time relevant GPU scope;
inspect persistent trace; close or return to the owning pass based on evidence.

**Non-goals:** Algorithm changes, temporal/spatial tuning, clamp tuning, or S4B work.

**Interim tests:** All PT-07 tests, Debug/core/shader, D3D validation, matched live captures.

**Pass completion gate:** The full P4 gate passes with confidence intervals; no unexplained energy,
TIR, or sample/PDF failure; the quality/performance trade is explicitly accepted.

**Expected visible effect:** Validation only; images document prior changes.

**Expected performance effect:** Measured direction recorded without an assumed win.

**Regression surface:** Rough/smooth glass, eta, grazing, path length, real-time/reference split.

**Stop and escalate conditions:** Stop on failed math, unstable variance, unexplained regression, or
unavailable required live evidence; do not patch thresholds.

**Handoff to next pass:** Complete PT-07 bundle or diagnosed failure/rollback recommendation.

**Suggested model:** `Sol Ultra`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-07. Prerequisites: S4A-P9.
> Implement only **S4A-P10**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Execute only **S4A-P10** after S4A-P9; make no estimator change. Validate PT-07 with white furnace,
> sample/evaluate/PDF, adjoint/reciprocity, eta/TIR, grazing, nested eta pair, transition, and
> roughness-grid tests; compile/build; run matched rough-glass images, temporal variance, and isolated
> GPU timing; inspect path-trace fields. Separate CPU, shader, visual, and timing evidence. Require
> every math gate and an accepted trade. Record revision, commands/results, artifacts, and status.
> Diagnose or recommend rollback on failure; do not tune. Do not begin S4A-P11.

### S4A-P11 - Partition the reference environment estimator

**Parent stage:** S4A.

**Audit findings:** PT-14.

**Purpose:** Remove overlapping terminal environment contributions and double filtering from reference PT.

**Why this is a separate pass:** Environment path partitioning is independent once BSDF PDFs are trustworthy.

**Prerequisites:** S4A-P10.

**Source scope:**

- **Primary implementation:** environment NEE, miss/no-CDF, and bounce-cap tail in `path_tracer.hlsl`.
- **Consumers/call sites:** environment/BSDF MIS and persistent path trace.
- **Tests/diagnostics:** uniform/bright-texel environment, bounce sweep, CDF on/off, furnace.
- **Documentation:** unbiased reference partition versus named finite-depth product approximation.

**Implementation contract:** Reference uses a non-overlapping partition. A BSDF-sampled direction
reads unfiltered environment radiance. Remove the terminal prefiltered tail from reference unless a
complementary estimator is derived/tested. Prefiltered tails may remain only in a named real-time
approximation. All proposals, PDFs, MIS, and terminal contributions remain traceable.

**Implementation outline:** Inventory contributions; remove/complement reference overlap; use
unfiltered reference miss radiance; preserve/label product path; run bounce/CDF invariance.

**Non-goals:** PT-12, PT-20, environment tuning, or deleting product approximations.

**Interim tests:** Uniform/bright environment; furnace; bounce cap; CDF on/off; shader build; trace.

**Pass completion gate:** Reference energy is invariant within Monte Carlo confidence across the old
terminal boundary and CDF on/off converges to the same value.

**Expected visible effect:** Finite-depth reference energy may change; bright samples are not presumed solved.

**Expected performance effect:** Duplicate-tail removal may save work; magnitude unmeasured.

**Regression surface:** Misses, cap, environment NEE/MIS, transmission/reflection tails, real-time policy.

**Stop and escalate conditions:** Stop if product/reference tails cannot be separated, unfiltered
radiance is unavailable in one path, or an undocumented partition appears.

**Handoff to next pass:** Contribution inventory, invariance statistics, traces, product confirmation,
revision, and commands/results.

**Suggested model:** `Sol High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-14. Prerequisites: S4A-P10.
> Implement only **S4A-P11**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S4A-P11** after S4A-P10. Correct PT-14 in reference PT: make environment NEE,
> BSDF miss, and bounce cap non-overlapping; use unfiltered environment radiance for BSDF directions;
> remove the prefiltered terminal tail unless a complementary tested estimator exists. Preserve named
> real-time approximation. Do not fix PT-12/PT-20 or tune quality. Run uniform/bright environment,
> furnace, bounce, CDF, shader/build, and path-trace checks; require energy invariance within
> confidence. Record revision, files, commands/results, and path inventory. Stop on undocumented or
> inseparable behavior. Do not begin S4A-P12.

### S4A-P12 - Core-estimator and persistent-outlier closure

**Parent stage:** S4A.

**Audit findings:** PT-02, PT-07, PT-14, PT-15, and section-9 persistent over-bright pixels.

**Purpose:** Decide S4A completion from integrated evidence and mechanistically classify representative
persistent samples.

**Why this is a separate pass:** Stage closure reviews all core contracts without introducing another fix.

**Prerequisites:** S4A-P1 through S4A-P11.

**Source scope:**

- **Primary implementation:** validation/capture only; no intended estimator change.
- **Consumers/call sites:** fixed-pose reference accumulation, real-time/reference comparison, timing scopes.
- **Tests/diagnostics:** full mixture/dielectric/environment/sanitation suite and >20,000-sample
  clamp-on/off reproduction.
- **Documentation:** signed S4A status, evidence manifest, and downstream sample routing.

**Implementation contract:** Trace representative pixels through every required field and classify
only from path evidence. Emitter-specific cases transfer to PT-13/S5 and are not labelled ordinary
fireflies. S4A may close if its contracts pass and residuals are explicitly isolated downstream;
S4B cannot block closure or visual evaluation.

**Implementation outline:** Run all analytic/statistical gates; reproduce fixed pose clamp on/off;
compare baseline/corrected traces; assign demonstrated mechanisms or explicit downstream inputs;
record images, timing, confidence separately.

**Non-goals:** New fixes, S4B, PT-13 implementation, seam investigation, or tuning.

**Interim tests:** Full Debug/core/shader/D3D; fixed accumulation; path trace; representative timing.

**Pass completion gate:** PT-02/07/14/15 gates pass; bounce/CDF invariance holds; all trace fields
exist; samples are mechanistically classified or transferred to S5; performance trade is accepted.

**Expected visible effect:** None new; validates accumulated changes.

**Expected performance effect:** Integrated measurement only.

**Regression surface:** Reference core, clamp, CDF, transmission, accumulation precision.

**Stop and escalate conditions:** Stop on any S4A math gate, missing trace field, visual-only sample
guess, or need to enter S4B/S5.

**Handoff to next pass:** Signed S4A bundle and explicit S5 sample inputs; core visual evaluation may begin.

**Suggested model:** `Sol Ultra`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-02, PT-07, PT-14, PT-15, and section-9 persistent over-bright pixels. Prerequisites: S4A-P1 through S4A-P11.
> Implement only **S4A-P12**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Execute only **S4A-P12** after S4A-P1 through P11; make no new rendering change. Run PT-02/PT-07/
> PT-14/PT-15 gates, fixed-pose reference accumulation beyond 20,000 samples with clamp on/off, and
> representative timing. Trace offending samples through throughput, branch probability, BSDF/light
> PDFs, MIS, emission/environment, sanitation, clamp, and final write. Do not call them ordinary
> fireflies without mechanism evidence; transfer emitter cases to PT-13/S5. S4B must not block S4A
> closure. Record revision, exact commands/results, images, traces, timing, and status. Stop on any
> failed/missing gate; do not fix another stage or automatically begin S4B-P1.

### 11.7 S4B - extended physical-transport passes

### S4B-P1 - Declare curved-interface and medium product scope

**Parent stage:** S4B.

**Audit findings:** PT-17 and PT-18 partially.

**Purpose:** Fix the supported optical scope before selecting algorithms or path-state layouts.

**Why this is a separate pass:** Curved specular connection and nested-media support are product
decisions; implementing first would create an unapproved subsystem.

**Prerequisites:** S4A-P12.

**Source scope:**

- **Primary implementation:** product-scope contract for `TraceTransmissiveVisibility` and the
  `pathInMedium` state; no renderer implementation expected.
- **Consumers/call sites:** future PT-17/PT-18 code, UI diagnostics, scene support matrix.
- **Tests/diagnostics:** plane/parallel, wedge, curved lens, camera-inside, nested, overlap, and
  unmatched-boundary fixture definitions.
- **Documentation:** accepted support/fallback matrix and payload/RTPSO assessment.

**Implementation contract:** Decide separately whether curved solid directional NEE is omitted or
supported by a dedicated solver, preserving thin/parallel specialization. Decide whether only one
air/solid/air medium is supported or a bounded stack is required. Stack support declares depth,
boundary key, eta/absorption, camera initialization, and mismatch recovery; unsupported scope declares
deterministic fallback/diagnostics.

**Implementation outline:** Record decisions/rationale; define support matrix; specify diagnostics/
fixtures; assess path state/ABI; obtain product-owner acceptance.

**Non-goals:** Manifold solver, medium implementation, RGB visibility, or renderer changes.

**Interim tests:** Documentation consistency, fixture review, and payload-capacity assessment.

**Pass completion gate:** The product owner accepts unambiguous supported/unsupported behavior and
fallback for every fixture.

**Expected visible effect:** None.

**Expected performance effect:** None.

**Regression surface:** Future glass NEE, camera-inside/nested scenes, and payload pressure.

**Stop and escalate conditions:** Stop if no decision exists or if chosen solver/stack needs further
architecture decomposition.

**Handoff to next pass:** Signed scope matrix, ABI assessment, fixtures, and chosen branches.

**Suggested model:** `Sol High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-17 and PT-18 partially. Prerequisites: S4A-P12.
> Implement only **S4B-P1**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Execute only **S4B-P1** after S4A closes; make no renderer change. Using PT-17/PT-18, obtain and
> document two decisions: curved-solid directional NEE is either omitted with valid thin/parallel
> specialization or assigned a separately designed connection solver; nested/camera-inside/overlap
> media are either rejected diagnostically or supported by a bounded keyed eta/absorption stack with
> initialization/capacity/recovery. Define all named fixture gates and assess payload/RTPSO impact.
> Record revision and decision evidence. Stop if scope is not chosen or needs more decomposition. Do
> not begin S4B-P2.

### S4B-P2 - Preserve RGB transmissive visibility

**Parent stage:** S4B.

**Audit findings:** PT-16.

**Purpose:** Preserve per-channel Beer attenuation across supported direct-light visibility paths.

**Why this is a separate pass:** It is a bounded representation correction independent of curved
connection and medium layout.

**Prerequisites:** S4B-P1.

**Source scope:**

- **Primary implementation:** `TraceTransmissiveVisibility` in `pt_dielectric.hlsli` and integration
  in `path_tracer.hlsl`.
- **Consumers/call sites:** analytic/emissive direct-light evaluation and binary opaque fast path.
- **Tests/diagnostics:** analytic RGB slabs over thickness/boundary counts and live comparison.
- **Documentation:** visibility/transmittance return contract and status evidence.

**Implementation contract:** Return `float3` transmittance instead of an average scalar and accumulate
`exp(-sigma * distance)` per channel through each supported boundary. Preserve binary opaque
specialization. Do not change directional-connection validity or product medium scope.

**Implementation outline:** Change type/flow atomically; update consumers; retain opaque path; add
slab fixtures; compile and validate.

**Non-goals:** PT-17, PT-18, medium-stack introduction, or tint retuning.

**Interim tests:** Analytic values; white/colored slab; shader/Debug build; opaque-shadow parity.

**Pass completion gate:** Camera and shadow paths match analytic RGB attenuation; opaque visibility
remains unchanged.

**Expected visible effect:** Colored glass casts colored direct shadows.

**Expected performance effect:** Small RGB ALU/bandwidth increase is possible.

**Regression surface:** Analytic/emissive direct light, multiple boundaries, saturation, opaque fast path.

**Stop and escalate conditions:** Stop if a scalar ABI crosses an undocumented boundary or matching
camera attenuation requires unscheduled medium work.

**Handoff to next pass:** Analytic table, call-site inventory, images, revision, and tests/timing note.

**Suggested model:** `Sol Medium`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-16. Prerequisites: S4B-P1.
> Implement only **S4B-P2**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S4B-P2** after S4B-P1. Correct PT-16 by making
> `TraceTransmissiveVisibility` and analytic/emissive consumers preserve `float3` Beer transmittance
> through supported boundaries; retain the binary opaque fast path. Do not alter curved connection,
> medium scope, or tint. Run analytic slab tests over channels/thickness/boundaries, opaque parity,
> shader/Debug build, and live slab validation. Require camera/shadow agreement with
> `exp(-sigma*distance)`. Record revision, files, exact results, and images. Stop on hidden scalar ABI
> or unscheduled medium dependency. Do not begin S4B-P3.

### S4B-P3 - Enforce the directional-light connection policy

**Parent stage:** S4B.

**Audit findings:** PT-17.

**Purpose:** Prevent invented directional-light energy through unsupported curved solid specular paths.

**Why this is a separate pass:** Connection validity is distinct from RGB attenuation and medium storage.

**Prerequisites:** S4B-P2 and the S4B-P1 PT-17 decision.

**Source scope:**

- **Primary implementation:** `pt_dielectric.hlsli::TraceTransmissiveVisibility`.
- **Consumers/call sites:** sun/directional NEE and diagnostic reason output.
- **Tests/diagnostics:** parallel slab, wedge, curved lens, and ordinary opaque shadow.
- **Documentation:** supported specialization, omitted path, and fallback reason.

**Implementation contract:** Preserve the accepted thin/parallel specialization. For unsupported
curved solid paths, omit directional NEE and emit the declared diagnostic; never accept a miss after
refraction without checking final light-direction support. If a dedicated solver was selected, stop
and decompose it rather than improvising it here.

**Implementation outline:** Encode selected policy; validate final directional support; add reasons;
run all fixtures.

**Non-goals:** Manifold solver, medium stack, RGB work, or general caustics.

**Interim tests:** Direction comparison; plane/wedge/lens; opaque parity; shader build.

**Pass completion gate:** Plane-parallel support remains valid; wedge/lens do not invent sun energy;
unsupported paths diagnose explicitly.

**Expected visible effect:** Invalid curved-glass sun contributions may disappear.

**Expected performance effect:** Omission may save work; unmeasured.

**Regression surface:** Sun shadows, thin/solid glass, and ordinary visibility.

**Stop and escalate conditions:** Stop if a solver was selected, parallel specialization cannot be
identified safely, or ordinary shadow behavior must change.

**Handoff to next pass:** Implemented policy, fixture results/images, reasons, revision, and commands.

**Suggested model:** `Sol High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-17. Prerequisites: S4B-P2 and the S4B-P1 PT-17 decision.
> Implement only **S4B-P3**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S4B-P3** using the S4B-P1 PT-17 policy. Preserve declared thin/parallel NEE but
> never accept a directional-light connection merely because a refracted path misses after changing
> direction; omit unsupported curved-solid NEE with its diagnostic. Do not implement a manifold
> solver, media, or caustics. Run plane, wedge, curved lens, ordinary shadow, shader, and Debug tests;
> require no invented energy and no slab regression. Record revision, files, results, diagnostics,
> and images. Stop for solver design or unsafe specialization. Do not begin S4B-P4.

### S4B-P4 - Introduce the declared medium-state model

**Parent stage:** S4B.

**Audit findings:** PT-18 partially.

**Purpose:** Establish either a bounded typed medium state or explicit single-solid rejection state
without converting all transport consumers.

**Why this is a separate pass:** Layout/ABI must be reviewed before boundary logic migrates.

**Prerequisites:** S4B-P3 and the S4B-P1 PT-18 decision.

**Source scope:**

- **Primary implementation:** `path_tracer.hlsl` state currently represented by `pathInMedium`, tint,
  and absorption; shared dielectric boundary types.
- **Consumers/call sites:** declarations/initialization for camera and transmissive-shadow state only.
- **Tests/diagnostics:** push/pop, initialization, capacity, mismatch, and rejection fixtures.
- **Documentation:** medium state diagram, selected policy, layout, and ABI sizes.

**Implementation contract:** If nested support is chosen, introduce a bounded instance/material-keyed
stack with eta/absorption, camera initialization, maximum depth, overflow, and mismatch recovery. If
single-solid scope is chosen, encapsulate the fast state and typed unsupported detection/reasons. Do
not silently toggle through overlaps. Every payload/SBT change matches RTPSO.

**Implementation outline:** Implement selected data model; pure transition/recovery helpers;
initialization/layout tests; compile checks; leave full consumer conversion to P5.

**Non-goals:** Full boundary migration, dielectric math, or unbounded participating media.

**Interim tests:** Push/pop/mismatch, camera init, overflow/rejection, shader permutations, ABI sizes.

**Pass completion gate:** Every declared state fixture is deterministic and all shader/RTPSO layouts
remain consistent; production consumers are not partially switched.

**Expected visible effect:** None or diagnostics only.

**Expected performance effect:** Stack support can add state; not measured yet.

**Regression surface:** Payload, camera/shadow rays, single-solid fast path.

**Stop and escalate conditions:** Stop on payload overflow, unstable boundary keys, undefined mismatch
policy, or need for a larger volume architecture.

**Handoff to next pass:** State diagram, layout sizes, transition results, recovery rules, revision,
and commands/results.

**Suggested model:** `Sol High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-18 partially. Prerequisites: S4B-P3 and the S4B-P1 PT-18 decision.
> Implement only **S4B-P4**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S4B-P4** using the S4B-P1 medium decision. Replace ambiguous state declarations
> with either a bounded instance/material-keyed eta/absorption stack with camera initialization,
> capacity, overflow and mismatch recovery, or an encapsulated single-solid state with deterministic
> unsupported diagnostics. Add pure transition/layout tests, but do not convert all boundaries or
> change dielectric math. Compile every shader and verify payload/SBT/RTPSO. Record revision, files,
> sizes, and results. Stop if keys, recovery, capacity, or ABI are unresolved. Do not begin S4B-P5.

### S4B-P5 - Convert medium-boundary producers and consumers

**Parent stage:** S4B.

**Audit findings:** PT-18.

**Purpose:** Route every supported dielectric crossing through the declared medium policy.

**Why this is a separate pass:** Consumer migration can be searched/tested independently from state
layout introduction.

**Prerequisites:** S4B-P4.

**Source scope:**

- **Primary implementation:** boundary crossings in `path_tracer.hlsl` and `pt_dielectric.hlsli`.
- **Consumers/call sites:** camera/shadow paths, eta, absorption, TIR, and diagnostic output.
- **Tests/diagnostics:** air/glass/air, camera-inside, nested liquid/glass, overlap, unmatched boundary.
- **Documentation:** consumer inventory and supported/unsupported results.

**Implementation contract:** No consumer toggles an unaudited Boolean. All eta/absorption transitions
use canonical helpers. Supported scenes follow the selected state exactly; unsupported scenes use the
declared diagnostic/fallback without invented transport. Single-solid remains a fast specialization.

**Implementation outline:** Convert all crossings/consumers; search raw toggles; run fixtures; verify
payload/trace; remove obsolete direct mutation.

**Non-goals:** General participating media, manifold NEE, material redesign, or performance-driven
stack-depth tuning.

**Interim tests:** Transition unit tests; source search; shader build; GPU fixtures; S4A regression.

**Pass completion gate:** Every fixture renders correctly or produces its declared fallback; no raw
toggle remains; S4A invariants stay green.

**Expected visible effect:** Supported camera-inside/nested scenes may correct; unsupported scenes are explicit.

**Expected performance effect:** Stack may add divergence; single-solid fast path stays narrow.

**Regression surface:** Glass, TIR, absorption, camera-inside, shadow visibility, payload pressure.

**Stop and escalate conditions:** Stop on undocumented consumers, ambiguous boundary identity, S4A
regression, or unsafe ABI change.

**Handoff to next pass:** Consumer search/inventory, fixture captures, diagnostics, payload/timing
note, revision, and exact results.

**Suggested model:** `Sol High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-18. Prerequisites: S4B-P4.
> Implement only **S4B-P5**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S4B-P5** after S4B-P4. Convert every audited camera/shadow boundary, eta choice,
> and absorption update to canonical medium helpers; remove direct `pathInMedium` toggles outside the
> encapsulated fast path. Supported scenes use the chosen state; unsupported camera-inside/nested/
> overlap/unmatched cases produce the declared diagnostic. Do not add general volumes or manifold
> NEE. Run transition tests, searches, shader/build, all fixtures, and S4A regression. Record revision,
> files, exact results, images, and payload impact. Stop on ambiguity, hidden consumer, ABI hazard, or
> S4A failure. Do not begin S4B-P6.

### S4B-P6 - Unify the reciprocal opaque material model

**Parent stage:** S4B.

**Audit findings:** PT-19.

**Purpose:** Make opaque scattering reciprocal and identical across sampling, direct/hit evaluation,
and ReSTIR targets.

**Why this is a separate pass:** All duplicate evaluators must change atomically, without transmission
or environment work in the same diff.

**Prerequisites:** S4B-P5 and the S4A BSDF/MIS foundation.

**Source scope:**

- **Primary implementation:** duplicated opaque evaluations in `path_tracer.hlsl` near the audited
  sampler/direct locations and a shared canonical helper.
- **Consumers/call sites:** PT sample/evaluate/direct, emissive-hit MIS, ReSTIR target copies, and
  raster comparison fixture.
- **Tests/diagnostics:** direction-swap reciprocity, sample/evaluate, furnace, metallic/roughness/color grid.
- **Documentation:** selected reciprocal energy-compensation model and duplicate inventory.

**Implementation contract:** Select/document one reciprocal diffuse/specular compensation model.
Use S3 `Ng` for geometry and `Ns` for BSDF. Direction swap obeys the declared reciprocal/adjoint
convention. Every PT/ReSTIR copy calls one helper or proves algebraic identity; a one-copy fix fails.

**Implementation outline:** Freeze model; add tests; centralize helper; convert all consumers; search
duplicates; compare raster/PT grid.

**Non-goals:** Raster shader redesign, artistic retuning, rough dielectric, or reuse tuning.

**Interim tests:** Reciprocity/furnace/sample agreement; duplicate search; shader build; raster/PT images.

**Pass completion gate:** Direction-swap/furnace tests pass; every duplicate agrees; raster/PT
appearance trade is explicitly accepted.

**Expected visible effect:** Opaque energy and raster/PT consistency may change.

**Expected performance effect:** Neutral to small shader-model change.

**Regression surface:** Diffuse/metallic materials, roughness, direct/emissive MIS, ReSTIR targets.

**Stop and escalate conditions:** Stop if no model is approved, a hidden evaluator remains, or raster
matching requires unrelated raster changes.

**Handoff to next pass:** Model, inventory/search, tests, comparison grid, revision. Execute S5-P1 next.

**Suggested model:** `Sol High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-19. Prerequisites: S4B-P5 and the S4A BSDF/MIS foundation.
> Implement only **S4B-P6**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S4B-P6** after S4B-P5. Correct PT-19 by selecting the documented reciprocal opaque
> model and applying it atomically to PT sampling/evaluation/direct, emissive-hit MIS, and every
> ReSTIR target copy, preserving S3 `Ng/Ns`. Do not redesign raster shading, retune materials, alter
> rough dielectric, or tune reuse. Add reciprocity, sample/evaluate, furnace, and material-grid tests;
> search old formulas; compile/build and capture raster/PT comparison. Require all copies/tests to
> agree. Record revision, files, results, and accepted appearance trade. Stop on unapproved model or
> hidden consumer. Do not begin S4B-P7; execute S5-P1.

### S4B-P7 - Exact environment within-cell sampling and PDF

**Parent stage:** S4B.

**Audit findings:** PT-20.

**Purpose:** Make the environment sampler's within-cell distribution exactly match its reported
solid-angle PDF.

**Why this is a separate pass:** It is an isolated low-severity correction but must follow PT-12's
canonical representation.

**Prerequisites:** S4B-P6 and S5-P1.

**Source scope:**

- **Primary implementation:** CDF within-cell sampling/PDF in `pt_env_light.hlsli` and the matching
  CPU distribution normalization.
- **Consumers/call sites:** PT environment proposals, reservoir replay, and PDF queries.
- **Tests/diagnostics:** PDF integration and observed-frequency histograms including poles/coarse CDFs.
- **Documentation:** exact within-cell measure and CPU/GPU normalization convention.

**Implementation contract:** Either sample uniformly in sine-latitude measure or report the exact
piecewise density of the retained sampler. CPU/GPU normalize identically. Preserve S5-P1's canonical
unrotated UV. Do not change temporal rotation, target, visibility, bias, or terminal partition.

**Implementation outline:** Freeze measure; implement inverse mapping/PDF; mirror CPU convention; add
integration/histograms; exercise poles, low resolutions, and rotations.

**Non-goals:** PT-12, CDF weighting redesign, PT-14, or visual tuning.

**Interim tests:** Integral one; observed versus expected bins; poles/coarse grids/rotations; shader
build; S4A environment regression.

**Pass completion gate:** PDF integrates to one and histograms match expected probabilities including
polar rows within confidence.

**Expected visible effect:** Small polar-bias correction.

**Expected performance effect:** Neutral.

**Regression surface:** Importance sampling, poles, coarse CDF, rotations, CDF on/off.

**Stop and escalate conditions:** Stop if CPU/GPU cannot share the measure or S5-P1 representation
would have to change.

**Handoff to next pass:** Formula, integration/histograms, regressions, revision, and exact results.

**Suggested model:** `Sol High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-20. Prerequisites: S4B-P6 and S5-P1.
> Implement only **S4B-P7**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S4B-P7** after S5-P1 and S4B-P6. Correct PT-20 in `pt_env_light.hlsli` and CPU
> distribution by using one exact within-cell solid-angle convention--uniform sine latitude or the
> exact density of the retained sampler. Preserve canonical unrotated UV and do not change rotation,
> target, terminal estimation, or tuning. Add integration and frequency histograms for ordinary,
> coarse, polar, and rotated cases; compile/build and rerun S4A environment gates. Require integral
> one and histogram agreement. Record revision, files, formulas, commands/results, and evidence.
> Stop if CPU/GPU or representation cannot reconcile. Do not begin S4B-P8.

### S4B-P8 - Validate the declared extended transport scope

**Parent stage:** S4B.

**Audit findings:** PT-16, PT-17, PT-18, PT-19, and PT-20.

**Purpose:** Close S4B without reopening or blocking the completed S4A core.

**Why this is a separate pass:** Integrated validation cannot smuggle in new optics or tuning.

**Prerequisites:** S4B-P1 through S4B-P7, including the S5-P1 bridge.

**Source scope:**

- **Primary implementation:** validation/capture only; no intended estimator edit.
- **Consumers/call sites:** all retained S4B transport/material/environment contracts.
- **Tests/diagnostics:** RGB slab; plane/wedge/lens; medium fixtures; reciprocity grid; polar PDF;
  S4A regression.
- **Documentation:** signed support matrix, fallbacks, evidence categories, and accepted trades.

**Implementation contract:** All fixtures follow the declared support matrix; unsupported cases
produce explicit diagnostics; every S4A invariant remains green. Individual deferred extensions do
not revoke S4A completion.

**Implementation outline:** Run each focused gate; execute combined scenes; compare supported/
fallback behavior; record visual/timing evidence; close or explicitly defer each extension.

**Non-goals:** Scope expansion, solver research, estimator tuning, or S4A reclassification.

**Interim tests:** Full fixtures; S4A suite; Debug/shader/D3D; representative images/timing.

**Pass completion gate:** Retained S4B contracts pass for declared scope; fallbacks are explicit;
S4A is unchanged; image/performance trades are accepted.

**Expected visible effect:** Validation only.

**Expected performance effect:** Integrated evidence only.

**Regression surface:** Transmission, opaque materials, environment sampling, raster/PT comparison.

**Stop and escalate conditions:** Stop on scope contradiction, S4A regression, or need for an
unselected algorithm.

**Handoff to next pass:** Signed S4B bundle; resume at S5-P2 because S5-P1 is already complete.

**Suggested model:** `Sol Ultra`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-16, PT-17, PT-18, PT-19, and PT-20. Prerequisites: S4B-P1 through S4B-P7, including the S5-P1 bridge.
> Implement only **S4B-P8**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Execute only **S4B-P8** after S4B-P1 through P7; add no feature/tuning. Validate PT-16/17/18/19/20
> with RGB slabs, plane/wedge/lens, air/glass/camera-inside/nested/overlap/unmatched boundaries,
> reciprocal/furnace/material grids, and polar histograms. Unsupported scope must diagnose per policy.
> Rerun S4A and separate CPU, shader, visual, and timing evidence. Require every retained S4B contract
> and unchanged S4A. Record revision, commands/results, images, timing, diagnostics, and status. Stop
> on any failed contract or scope expansion. Do not automatically begin S5-P2.

### 11.8 S5 - light-sampling and ReSTIR estimator passes

### S5-P1 - Canonical environment reservoir representation

**Parent stage:** S5.

**Audit findings:** PT-12.

**Purpose:** Make initial environment sampling and temporal replay encode the same light sample at
every rotation.

**Why this is a separate pass:** Representation must precede PDF refinement and all reuse policy work;
it is also the required bridge to S4B-P7.

**Prerequisites:** S1-P6, S3-P7, S4A-P12, and S4B-P6.

**Source scope:**

- **Primary implementation:** environment encoding in `path_tracer.hlsl`, replay in
  `restir_di_temporal.hlsl`, and shared helpers in `restir_di.hlsli`.
- **Consumers/call sites:** reservoir serialization, environment radiance/PDF, history/reset key.
- **Tests/diagnostics:** single-bright-texel rotation round trip and layout/version tests.
- **Documentation:** representation/domain diagram and environment history compatibility.

**Implementation contract:** Store canonical unrotated CDF texture UV. Decode to environment-local
direction and apply world rotation once; reverse mapping removes rotation once before UV/PDF lookup.
Proposal remains the environment CDF plus current within-cell distribution; target/visibility/bias do
not change. Stored representation is texture UV; current/previous use the same domain and invalidate
on environment version change. Encode/decode adds no Jacobian; solid-angle density stays in the PDF.
PT-20 remains open.

**Implementation outline:** Add one encode/decode helper; convert producer/replay; update layout/key;
run zero/nonzero rotations.

**Non-goals:** PT-20, target/Jacobian/visibility changes, or reuse tuning.

**Interim tests:** Bright texel rotations; UV/direction/radiance/PDF round trip; shader build; reset.

**Pass completion gate:** Initial sample to reservoir to replay returns identical UV, world direction,
radiance, and PDF at every tested rotation.

**Expected visible effect:** Rotated-HDR temporal drift may disappear.

**Expected performance effect:** Neutral.

**Regression surface:** Environment rotation, CDF, temporal replay, history invalidation.

**Stop and escalate conditions:** Stop on undocumented reservoir consumers, domain conflicts, or a
requirement to solve PT-20 in the same diff.

**Handoff to next pass:** Representation diagram, rotation table, layout/reset evidence, revision;
execute S4B-P7 then S4B-P8 before S5-P2.

**Suggested model:** `Sol High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-12. Prerequisites: S1-P6, S3-P7, S4A-P12, and S4B-P6.
> Implement only **S5-P1**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S5-P1** after S1/S3/S4A and S4B-P6. Correct PT-12 by storing canonical unrotated
> environment-CDF UV, decoding to world direction with rotation exactly once, and using one helper in
> `path_tracer.hlsl`, `restir_di_temporal.hlsl`, and `restir_di.hlsli`. Keep proposal, target,
> visibility, bias, and within-cell PT-20 behavior unchanged; update environment history compatibility.
> Run bright-texel round trips over rotations, layout/reset, shader, and Debug tests. Require identical
> UV/direction/radiance/PDF. Record revision, files, commands/results, and docs. Stop on hidden
> consumers/domain conflict. Do not implement PT-20; hand off to S4B-P7.

### S5-P2 - Canonical emissive-light contract and fixtures

**Parent stage:** S5.

**Audit findings:** PT-13 partially.

**Purpose:** Freeze one emitter definition shared by proposal construction, NEE, hit evaluation, and MIS.

**Why this is a separate pass:** The texture-aware-versus-excluded choice and fixtures must precede
production conversion.

**Prerequisites:** S5-P1, S3-P7, S4B-P6, and S4B-P8.

**Source scope:**

- **Primary implementation:** contract for `SceneRenderer::UploadEmissiveLights` and PT/ReSTIR
  emissive sample/evaluation helpers; no active conversion.
- **Consumers/call sites:** future CPU upload, NEE, hit PDF/MIS, and reservoir targets.
- **Tests/diagnostics:** constant/checker/UV-scaled, one/two-sided, flat/smooth-normal emitters.
- **Documentation:** selected texture policy, sample representation, PDFs, orientation, and invalidation.

**Implementation contract:** Choose texture-aware importance with matched proposal or exclude textured
emitters with unbiased BSDF-hit fallback. Proposal is triangle selection times conditional surface
sampling; target evaluates identical radiance/sidedness; stable emitter identity and point are stored;
area/solid-angle uses true face `Ng`; history invalidates on generation change. State current/previous
domains, Jacobian, visibility, and bias explicitly; neither branch introduces estimator bias.

**Implementation outline:** Record product/math choice; define formulas/units; create fixtures/CPU
expected values; specify identity/versioning; approve before conversion.

**Non-goals:** Production conversion, PT-26 caching, H1-H4, or texture-system redesign.

**Interim tests:** Fixture creation; selection/conditional normalization; sidedness/face-normal tests.

**Pass completion gate:** The approved policy and every proposal/target/PDF/domain/orientation
convention are unambiguous; fixtures reproduce the old mismatch.

**Expected visible effect:** None.

**Expected performance effect:** None.

**Regression surface:** Textured/sided emitters, UV scale, smooth mesh, motion/editing.

**Stop and escalate conditions:** Stop if policy is not approved, stable identity is absent, or CPU/
GPU cannot evaluate the same approximation.

**Handoff to next pass:** Signed contract, fixtures/expected values, chosen branch, revision, and results.

**Suggested model:** `Sol High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-13 partially. Prerequisites: S5-P1, S3-P7, S4B-P6, and S4B-P8.
> Implement only **S5-P2**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Execute only **S5-P2** after S5-P1 and S3/S4B gates; change no production renderer. For PT-13,
> obtain/document one policy: shared texture-aware proposal approximation, or explicit textured-
> proposal exclusion with unbiased BSDF-hit fallback. Define triangle/conditional PDFs, emitted-
> radiance target, true-face-`Ng` area/solid-angle conversion, sidedness, identity/version, current/
> previous domains, visibility, Jacobian, and bias. Add all named fixtures/expected tests. Record
> revision, files, commands/results, and decision. Stop if policy/identity/CPU-GPU representation is
> unresolved. Do not begin S5-P3.

### S5-P3 - Convert emissive producers, consumers, and MIS

**Parent stage:** S5.

**Audit findings:** PT-13.

**Purpose:** Make CPU light construction, NEE, hit evaluation, and ReSTIR use one canonical emitter.

**Why this is a separate pass:** This is the estimator-changing atomic conversion after the contract.

**Prerequisites:** S5-P2.

**Source scope:**

- **Primary implementation:** `SceneRenderer::UploadEmissiveLights` and emissive sampling/hit paths
  in `path_tracer.hlsl`.
- **Consumers/call sites:** selection/conditional PDF, hit reconstruction/MIS, ReSTIR target copies,
  and emitter generation/reset.
- **Tests/diagnostics:** S5-P2 fixtures and persistent-sample emission trace.
- **Documentation:** active emitter policy and PT-26 preservation requirement.

**Implementation contract:** Implement only the approved branch. NEE/hit evaluate identical radiance,
sidedness, face orientation, and PDFs. Excluded textured emitters have zero proposal probability and
no competing light PDF at a BSDF hit. ReSTIR targets use the same definition. Relevant motion,
material, or texture change invalidates the generation. Visibility policy remains unchanged.

**Implementation outline:** Convert CPU construction, GPU sample, hit PDF/MIS, ReSTIR copies, and
invalidation atomically; search old formulas; run fixtures/outlier trace.

**Non-goals:** PT-26 caching, H1-H4, reuse tuning, or emitter filtering.

**Interim tests:** PDF histograms; sample/hit equality; reference energy; shader/Debug; moving reset.

**Pass completion gate:** All fixtures match sample/hit PDFs and stable energy; no old duplicate
definition remains; emission outlier traces have coherent PDFs/MIS.

**Expected visible effect:** Textured-emitter energy/ghosting may change.

**Expected performance effect:** Texture-aware construction may cost more; exclusion may raise variance.

**Regression surface:** Constant/textured/sided emitters, UVs, motion/edit, ReSTIR off/P5/P6/P7.

**Stop and escalate conditions:** Stop on hidden emitter consumer, biased fallback, or pressure to
fold PT-26 optimization into correctness.

**Handoff to next pass:** Inventory/search, fixtures, emission traces, generation/reset evidence,
revision, and exact results.

**Suggested model:** `Sol High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-13. Prerequisites: S5-P2.
> Implement only **S5-P3**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S5-P3** using the approved S5-P2 contract. Convert
> `SceneRenderer::UploadEmissiveLights`, PT NEE, emissive hit PDF/MIS, and all ReSTIR target copies to
> identical radiance, true face `Ng`, sidedness, selection and conditional PDFs; excluded textured
> emitters use zero proposal and unbiased hit fallback. Add generation invalidation. Do not implement
> PT-26 or change H1-H4/tuning. Run all fixtures, PDF/reference-energy tests, moving resets, shader/
> build, and persistent emission traces. Record revision, files, commands/results, and evidence. Stop
> on hidden consumer or bias. Do not begin S5-P4.

### S5-P4 - Remove redundant native-connection visibility work

**Parent stage:** S5.

**Audit findings:** PT-21 H1.

**Purpose:** Treat native P5 segments as visibility-known while retaining visibility for shifted/
reused samples.

**Why this is a separate pass:** It changes one provenance classification/ray count without temporal
normalization changes.

**Prerequisites:** S5-P3 and S1-P6.

**Source scope:**

- **Primary implementation:** fresh/native GI target and original-input MIS paths in
  `restir_di.hlsli` and active temporal/final shaders.
- **Consumers/call sites:** P5/P6/P7 shading and visibility diagnostics.
- **Tests/diagnostics:** known-valid/retrace-disagrees counter/AOV and floor-contact fixture.
- **Documentation:** native versus shifted proposal/target/visibility/bias contract.

**Implementation contract:** Proposal is the native P5 primary directional sample in solid angle;
target and stored reservoir stay unchanged; current native mapping has identity Jacobian. Native
current-domain segment visibility is known by construction, so skip only its redundant fresh-target
and original-input re-traces. Reconnected/history and final receiver winners retain visibility. No
new bias is expected.

**Implementation outline:** Add provenance/disagreement diagnostics; bypass exactly two native rays;
preserve shifted visibility; run P5/P6/P7 images, mean, and ray counts.

**Non-goals:** H2-H4, offset changes, candidates/radius/history/boiling tuning, or target redesign.

**Interim tests:** Counter/AOV; floor-contact static/orbit; ray inventory; shader/Debug build.

**Pass completion gate:** Native disagreement is eliminated by construction; shifted visibility
remains active; P5/P6 mean energy stays within tolerance.

**Expected visible effect:** Floor-contact grain may reduce; no promised symptom closure.

**Expected performance effect:** Structural ray reduction; isolated timing closes in S5-P9.

**Regression surface:** Native P5, input MIS, shifted P6/P7, floor contact, and occlusion.

**Stop and escalate conditions:** Stop if native/shifted provenance is ambiguous or mean changes show
a target-domain coupling.

**Handoff to next pass:** Diagnostics, ray inventory, P5/P6/P7 images/means, revision, and results.

**Suggested model:** `Sol High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-21 H1. Prerequisites: S5-P3 and S1-P6.
> Implement only **S5-P4**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S5-P4** after S5-P3. Correct PT-21 H1: treat the native P5 traced segment as
> visible by construction and skip only its redundant fresh-target and original-input-MIS visibility
> rays; retain visibility for reconnected/history samples and final receiver winners. Proposal,
> stored reservoir, target, Jacobian, and H2-H4 remain unchanged. Add provenance/disagreement AOVs and
> test floor-contact static/orbit P5/P6/P7, energy, ray count, shader, and Debug. Require exact native/
> shifted separation and stable mean. Record revision, files, results, diagnostics, and images. Stop
> on ambiguity or target coupling. Do not begin S5-P5.

### S5-P5 - Restore previous-domain BASIC temporal normalization

**Parent stage:** S5.

**Audit findings:** PT-21 H2.

**Purpose:** Normalize temporal reservoirs across their actual current and previous receiver domains.

**Why this is a separate pass:** It repairs one estimator equation while retaining current Jacobian
and visibility policies for later isolation.

**Prerequisites:** S5-P4 and S1-P6.

**Source scope:**

- **Primary implementation:** `restir_di_temporal.hlsl::GiTemporalResample` and shared target/BASIC
  helpers in `restir_di.hlsli`.
- **Consumers/call sites:** previous surface/material/camera inputs, diagnostics, reservoir finalize.
- **Tests/diagnostics:** asymmetric two-domain CPU test; target-ratio/source/normalization AOVs.
- **Documentation:** proposal, target, stored representation, current/previous, Jacobian, visibility,
  and residual-bias contract.

**Implementation contract:** Proposal set contains current fresh and prior reservoir sources with
source `M`. Stored sample remains the P5 secondary-point/raw-radiance representation. Evaluate one
target definition at current and previous primary receivers using S1 previous data. Apply RTXDI BASIC
`pi/piSum` normalization rather than same-domain `wSum/(M*selectedTarget)`. Keep H3 Jacobian and H4
visibility unchanged and explicitly label their remaining bounded bias.

**Implementation outline:** Activate validated previous inputs; add ratios; implement BASIC; preserve
fresh fallback; test P6 static/orbit before spatial reuse.

**Non-goals:** H2b, Jacobian/visibility, candidates, radius/history, or tuning.

**Interim tests:** Asymmetric/equal domains; zero support; selection invariance; shader/build; P6
static/orbit mean/diagnostics.

**Pass completion gate:** Expected-value tests pass; equal domains reduce correctly; orbit uses
nontrivial prior targets without mean drift/non-finite values.

**Expected visible effect:** Metallic temporal behavior may improve or reveal a diagnosable boundary.

**Expected performance effect:** Additional target reevaluation; unmeasured until S5-P9.

**Regression surface:** Static/orbit P6, view dependence, history fallback, zero support, P7 input.

**Stop and escalate conditions:** Stop if S1 previous data is invalid, math tests fail, or a boundary
cannot be explained without threshold tuning.

**Handoff to next pass:** Ratio/normalization reports, tests, P6 images/mean, revision, and commands.

**Suggested model:** `Sol Ultra`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-21 H2. Prerequisites: S5-P4 and S1-P6.
> Implement only **S5-P5**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S5-P5** after S5-P4 and S1. Correct H2 in `GiTemporalResample`: preserve source
> `M`, reevaluate the stored secondary-point/raw-radiance sample at current and authoritative previous
> receivers, and apply RTXDI BASIC `pi/piSum` instead of the same-domain shortcut. Keep H3 Jacobian
> and H4 visibility unchanged. Add previous/current target ratio, source, and normalization AOVs. Run
> asymmetric/equal-domain, zero-support, selection, shader/build, and P6 static/orbit mean tests before
> P7. Record revision, files, results, diagnostics, and images. Stop on invalid prior data, failed math,
> or unexplained boundaries; do not tune. Do not begin S5-P6.

### S5-P6 - Continue temporal reprojection search after GI rejection

**Parent stage:** S5.

**Audit findings:** PT-21 H2b.

**Purpose:** Select a genuinely GI-compatible prior candidate instead of stopping at the first generic match.

**Why this is a separate pass:** Search control can be proven without changing weights, Jacobians,
visibility, or thresholds.

**Prerequisites:** S5-P5.

**Source scope:**

- **Primary implementation:** candidate loop in `restir_di_temporal.hlsl`.
- **Consumers/call sites:** prior-depth/reservoir validation and rejection diagnostics.
- **Tests/diagnostics:** two-depth neighborhood fixture and first-fails/later-passes counters.
- **Documentation:** candidate order, acceptance predicates, and unchanged estimator contract.

**Implementation contract:** Proposal, target, stored reservoir, BASIC, Jacobian, visibility, and bias
stay fixed. Current motion projects into previous frame; every candidate must pass generic surface and
GI expected-depth/reservoir validity before acceptance. Failure continues the existing search order.

**Implementation outline:** Move GI checks into loop; continue on failure; add counters; test curved/
two-depth neighborhoods; compare P5+P6/P7.

**Non-goals:** Randomized order, threshold changes, H3/H4, radius/history tuning.

**Interim tests:** Loop unit fixture; counters; curved orbit P6; shader/Debug build.

**Pass completion gate:** Every fixture where a later candidate passes accepts it; an invalid first
candidate never terminates search; mean energy is stable.

**Expected visible effect:** Curved-surface motion grain may reduce.

**Expected performance effect:** More candidates may be inspected; unmeasured.

**Regression surface:** Disocclusion, curved surfaces, motion reprojection, temporal acceptance.

**Stop and escalate conditions:** Stop if candidates lack needed identity data or continuation changes
unrelated validation.

**Handoff to next pass:** Counts, two-depth tests, comparative images/means, revision, and exact results.

**Suggested model:** `Sol High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-21 H2b. Prerequisites: S5-P5.
> Implement only **S5-P6**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S5-P6** after S5-P5. Correct H2b by performing GI depth/reservoir validity inside
> the temporal candidate loop and continuing after failure. Do not alter proposal/target/BASIC,
> Jacobian, visibility, thresholds, search order, or tuning. Add first-fails/later-passes counters and
> a two-depth/curved fixture. Run focused loop tests, P5+P6/P7 static/orbit, shader, and Debug; require
> later valid candidate selection and stable mean. Record revision, files, commands/results, counters,
> and images. Stop if candidate identity is unavailable or scope expands. Do not begin S5-P7.

### S5-P7 - Unify the ReSTIR reconnection Jacobian convention

**Parent stage:** S5.

**Audit findings:** PT-21 H3.

**Purpose:** Apply one validated reconnection mapping/Jacobian policy in temporal and spatial reuse.

**Why this is a separate pass:** Jacobian math and bounded bias require independent consequence-heavy review.

**Prerequisites:** S5-P6.

**Source scope:**

- **Primary implementation:** temporal/spatial Jacobian helpers in `restir_di.hlsli` and active shaders.
- **Consumers/call sites:** reservoir streaming, UCW transformation, BASIC domain evaluation, diagnostics.
- **Tests/diagnostics:** analytic equal/distance/normal/zero-support cases and reject/clamp counters.
- **Documentation:** proposal/target/stored representation/domain mapping, units, visibility, and bias.

**Implementation contract:** Proposal is the P5 secondary-point solid-angle distribution; stored raw
radiance contains no inverse PDF and fresh `weightSum=1/q_omega`. Target is receiver-domain under the
unchanged H4 policy. Apply the receiver-to-secondary Jacobian exactly once to shifted proposal/UCW,
preserve source `M`, and use the same temporal/spatial convention. Per the recorded RTXDI policy,
reject raw Jacobian outside `[0.1,10]`, then clamp accepted values to `[1/3,3]`; record bounded bias.

**Implementation outline:** Freeze formula placement; unify helpers; replace old raw window; add
counters/AOVs; run analytic/B0 comparison.

**Non-goals:** H4, neighbors/radius/history/boiling, or target changes.

**Interim tests:** Analytic Jacobians; temporal/spatial equivalence; invalid support; counters;
shader/build; B0 metrics.

**Pass completion gate:** Analytic tests pass; Jacobian is applied once; temporal/spatial match;
reject/clamp rates and quality deltas are recorded.

**Expected visible effect:** Contact amplification/correlation may decrease.

**Expected performance effect:** Structurally neutral; quality/timing close later.

**Regression surface:** Distance/cosine extremes, nearby secondary geometry, zero support, both reuse stages.

**Stop and escalate conditions:** Stop if mapping tests fail, a hidden second application appears, or
bounds introduce an unreviewed bias policy.

**Handoff to next pass:** Formula/placement diagram, tests, counters, B0 deltas, revision, and results.

**Suggested model:** `Sol Ultra`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-21 H3. Prerequisites: S5-P6.
> Implement only **S5-P7**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S5-P7** after S5-P6. Correct H3 by unifying temporal/spatial reconnection under the
> P5 secondary-point solid-angle proposal and receiver target; apply its Jacobian exactly once,
> preserve source `M`, reject raw values outside `[0.1,10]`, then clamp accepted values to `[1/3,3]`.
> Keep H4 and all tuning unchanged; document bounded bias. Add analytic equal/distance/normal/zero-
> support tests, reject/clamp counters, B0 comparison, shader, and Debug. Record revision, files,
> formulas, commands/results, and metrics. Stop on math contradiction or hidden second application.
> Do not begin S5-P8.

### S5-P8 - Select and enforce an explicit ReSTIR visibility policy

**Parent stage:** S5.

**Audit findings:** PT-21 H4.

**Purpose:** Separate target support, optional source correction, and final receiver visibility under
a documented bias policy.

**Why this is a separate pass:** Visibility changes support and ray count and must follow H1-H3.

**Prerequisites:** S5-P7.

**Source scope:**

- **Primary implementation:** GI target/BASIC/final shading in `restir_di.hlsli` and temporal/spatial/
  final shaders.
- **Consumers/call sites:** native, historical, spatial samples; visibility AOVs/ray counters.
- **Tests/diagnostics:** unshadowed BASIC, unshadowed plus source correction, and current shadowed-target matrix.
- **Documentation:** proposal/target/representation/domains/Jacobian/visibility and selected bias/fallback.

**Implementation contract:** Proposal/stored reservoir stay fixed; current/previous targets use one
definition; final receiver visibility always runs. Unbiased fallback is unshadowed target+BASIC+final
visibility. Any conservative source-domain correction enters normalization consistently and is
labelled bounded bias. Binary visibility cannot be silently folded into targets then retraced. S5-P7
Jacobian is unchanged.

**Implementation outline:** Add isolated policy modes; run matrix after H1-H3; select/document one;
remove ambiguous mixed behavior; preserve final visibility.

**Non-goals:** Radius/history/candidates/boiling/filter/offset tuning.

**Interim tests:** Expected support; source/final counters; occluders; P5/P6/P7 energy/leaks; ray/timing.

**Pass completion gate:** Selected policy has explicit estimator semantics, stable mean, no unacceptable
leaks, recorded rays/timing, and a passing unbiased fallback.

**Expected visible effect:** Broad patches/floor churn may change.

**Expected performance effect:** Ray count may rise/fall and must be measured.

**Regression surface:** Occluders, disocclusion, floor contact, source support, final P5/P6/P7 shading.

**Stop and escalate conditions:** Stop if no policy passes energy/leak gates, visibility cannot be
separated from target, or a threshold workaround is proposed.

**Handoff to next pass:** Policy matrix, selected/fallback contract, energy/leaks, ray timing, revision.

**Suggested model:** `Sol Ultra`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-21 H4. Prerequisites: S5-P7.
> Implement only **S5-P8**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S5-P8** after H1-H3. Correct H4 by making target support, optional source-domain
> visibility correction, and final receiver visibility explicit. Preserve proposal/stored reservoir
> and S5-P7 Jacobian. Compare unshadowed target+BASIC+final visibility; that policy plus consistent
> conservative source correction; and current shadowed-target behavior. Always keep final visibility
> and an unbiased fallback. Do not tune radius/history/candidates/boiling/filters. Require expected-
> value, mean-energy, leak, ray, and timing evidence. Record revision, files, commands/results, images,
> and policy. Stop if no policy passes or concepts cannot separate. Do not begin S5-P9.

### S5-P9 - Integrated P5/P6/P7 quality and timing gate

**Parent stage:** S5.

**Audit findings:** PT-12, PT-13, PT-21 H1-H4, and emitter-related persistent samples.

**Purpose:** Accept or reject the repaired light/ReSTIR estimator using canonical quality, bias,
motion, and isolated timing evidence.

**Why this is a separate pass:** Integrated validation may not include parameter tuning or another fix.

**Prerequisites:** S5-P1 through S5-P8 and S4B-P7/P8.

**Source scope:**

- **Primary implementation:** existing P7 capture/metric/timing harness only.
- **Consumers/call sites:** P5+P6, filter-only, spatial-only, full P7, RR off/on.
- **Tests/diagnostics:** canonical `p7-diagnostics-tracker.md` suite and named reports/timestamps.
- **Documentation:** status companion, ReSTIR roadmap, P7 tracker, and evidence manifest.

**Implementation contract:** Freeze P1-P8 proposal, target, representation, domain, Jacobian,
visibility, and bias contracts. RR off evaluates estimator correctness; RR on evaluates product
quality. Compare P5+P6, filter-only, spatial-only, full P7 at identical config. Record mean energy,
fine noise, low-frequency variance, blurred-hot coverage, neighbor correlation, temporal chroma,
leaks, and isolated temporal/spatial/filter GPU timing. Retrace emitter samples through emission,
light/directional/BSDF PDFs, and MIS.

**Implementation outline:** Build/test; canonical warmup/static/orbit captures; collect reports and
isolated timing; compare declared gates; update ledgers without tuning.

**Non-goals:** Radius/history/candidate/boiling/filter tuning, hints, new source fixes, or hiding errors with RR.

**Interim tests:** CPU expected values; shader/Debug/D3D; canonical warm-up and fixed ROI/camera runs.

**Pass completion gate:** Round trips/PDF-target equalities pass; P5/P6 mean is stable; P7 improves
fine/local noise without unacceptable broad correlation/chroma/leaks/mean error; isolated timings
meet the production budget; emitter outliers are retraced.

**Expected visible effect:** Validation of prior corrections only.

**Expected performance effect:** Measured isolated costs, not wall-clock-only inference.

**Regression surface:** Environment rotation, emitters, static/orbit, occlusion, rough metal, RR,
and P5/P6/P7 toggles.

**Stop and escalate conditions:** Stop on estimator gate failure, noncanonical config, missing isolated
timing, or temptation to tune before diagnosis.

**Handoff to next pass:** Signed S5 bundle and explicit deferred tuning candidates.

**Suggested model:** `Sol Ultra`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-12, PT-13, PT-21 H1-H4, and emitter-related persistent samples. Prerequisites: S5-P1 through S5-P8 and S4B-P7/P8.
> Implement only **S5-P9**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Execute only **S5-P9** after S5-P1 through P8 and S4B validation; make no estimator/tuning change.
> Run CPU/math, shader/build, then canonical P5+P6, filter-only, spatial-only, and full-P7 static/orbit
> with RR off for estimator correctness and on for product quality. Record mean, fine noise, low-
> frequency variance, blurred-hot coverage, neighbor correlation, temporal chroma, leaks, and isolated
> temporal/spatial/filter GPU timestamps. Retrace PT-13 emitter samples through emission/light/BSDF
> PDFs and MIS. Require audit quality gates and production timing budget. Record revision, exact
> commands/results, reports, images, timing, and status. Diagnose/rollback; do not tune or start S6-P1.

### 11.9 S6 - frame-overlap and PT/raster-isolation passes

### S6-P1 - Inventory global-drain ownership

**Parent stage:** S6.

**Audit findings:** PT-22 partially.

**Purpose:** Establish the owner, last-use fence, reuse condition, and baseline wait diagnostics for
every resource whose safety currently inherits the global drain.

**Why this is a separate pass:** Changing the wait before the inventory risks silent in-flight reuse.

**Prerequisites:** S0-P4 and S0-P5; S5-P9.

**Source scope:**

- **Primary implementation:** `GfxContext::BeginFrame`/end/present, frame/submission fences, upload
  ring, descriptor capacity/generation paths, and `DxrDispatchContext` AS lifetimes.
- **Consumers/call sites:** `Renderer`, `Application` viewport loop, `SceneRenderer::RecordDxrPass`,
  histories/readbacks.
- **Tests/diagnostics:** named test targets, CPU/GPU timeline, wait-reason and generation counters.
- **Documentation:** authoritative resource-ownership table and capture manifest.

**Implementation contract:** For allocators, frame resources, upload ranges, descriptor tables,
TLAS/BLAS scratch/results, readbacks, histories, resize and capacity growth, record owner, slot/range,
queue, last reader/writer fence, and reuse rule. Split selected-frame, latest-global, upload-conflict,
and present/pacing waits. Do not change scheduling.

**Implementation outline:** Instrument current wait; build ownership table; add counters for wait and
generation changes; capture single/dual-view baselines.

**Non-goals:** Shortening waits, changing allocations/AS, PT/raster isolation, or estimator work.

**Interim tests:** Audit build/core tests; two slots, dual view, resize, scene/descriptor growth,
upload wrap under D3D debug; diagnostics-off comparison.

**Pass completion gate:** Every PT-22 resource category has a known owner/reuse fence or explicit
unresolved escalation; wait components are attributable; output/config hashes remain unchanged.

**Expected visible effect:** None.

**Expected performance effect:** None; optional diagnostic overhead only.

**Regression surface:** Startup/shutdown, viewports, resize/growth, uploads, AS rebuild, all modes.

**Stop and escalate conditions:** Stop on unknown queue, uncoordinated writers, unidentifiable fence,
or instrumentation that changes submission order.

**Handoff to next pass:** Ownership table, unresolved items, baseline timeline/hash, revision, files,
and exact commands/results.

**Suggested model:** `Sol Ultra`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-22 partially. Prerequisites: S0-P4 and S0-P5; S5-P9.
> Implement only **S6-P1**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S6-P1** after S0 markers/manifest and S5. Address PT-22 only by inventorying and
> instrumenting resources protected by the global drain; alter no wait. Record owner, slot/range,
> queue, last-use fence, reuse, and growth handling for allocators, uploads, descriptors, AS,
> readbacks, and histories; split frame-context/global/upload/present waits. Build the audit targets,
> run core tests, and capture dual-view/resize/growth/wrap under D3D debug. Require a complete table,
> attributable baseline, and unchanged output/hash. Record revision, files, exact evidence categories.
> Stop on any unreconciled owner/fence. Do not begin S6-P2.

### S6-P2 - Fence frame contexts and command allocators

**Parent stage:** S6.

**Audit findings:** PT-22 partially.

**Purpose:** Make each frame context and command allocator independently safe to reuse.

**Why this is a separate pass:** Allocator lifetime is the smallest foundational ownership transition
and can land while the global wait still masks concurrency.

**Prerequisites:** S6-P1.

**Source scope:**

- **Primary implementation:** `GfxContext` frame-context structure, `BeginFrame`, fence assignment,
  and allocator reset.
- **Consumers/call sites:** renderer/application frame entry/exit and command-list recording.
- **Tests/diagnostics:** pure fence-decision tests and debug/GBV multi-frame rotation.
- **Documentation:** ownership table and pass evidence.

**Implementation contract:** Each slot stores the submission fence covering all commands recorded by
its allocators; reset only after that fence completes in the correct queue domain. Keep the latest-
global wait intact through S6-P5.

**Implementation outline:** Normalize per-slot fence; assign at submission; gate reset/reuse; add
assertions/tests; verify current serialized output.

**Non-goals:** Upload/descriptors/AS, global-wait removal, or frame-count changes.

**Interim tests:** Complete/incomplete/out-of-order slot cases; Debug/core build; repeated rotation,
minimize/skip, dual viewport, D3D debug and GBV.

**Pass completion gate:** No allocator/context resets early; independent slot decisions are tested;
serialized output remains unchanged.

**Expected visible effect:** None.

**Expected performance effect:** None until S6-P6.

**Regression surface:** Frame index, skip/minimize, dual viewport, device loss/shutdown.

**Stop and escalate conditions:** Stop on allocator sharing across slots, ambiguous queue fences, or
need for scheduler redesign.

**Handoff to next pass:** Layout, fence tests, validation log, updated table, revision, and results.

**Suggested model:** `Sol High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-22 partially. Prerequisites: S6-P1.
> Implement only **S6-P2**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S6-P2** after S6-P1. Make frame-context/command-allocator reuse depend on the fence
> of the submission that last used that slot; keep the global wait. Do not change uploads,
> descriptors, AS, frame count, or rendering. Add fence-decision tests; build/run core tests and
> repeated slot rotation under D3D debug/GBV. Require proof no allocator resets early and serialized
> output is unchanged. Record revision, files, commands/results, and validation. Stop on cross-slot/
> queue ambiguity. Do not begin S6-P3.

### S6-P3 - Fence upload-ring ranges

**Parent stage:** S6.

**Audit findings:** PT-22 partially.

**Purpose:** Protect every upload allocation range by the submission fence that consumes it.

**Why this is a separate pass:** Upload wrap/growth hazards differ from allocator reuse and need
isolated pressure tests.

**Prerequisites:** S6-P2 and S6-P1's upload inventory.

**Source scope:**

- **Primary implementation:** `GfxContext` upload allocator allocation/wrap/growth/retirement/wait.
- **Consumers/call sites:** scene/material, previous transforms, emissive data, constants, DXR uploads.
- **Tests/diagnostics:** boundary/exact-fit/wrap/pressure/growth tests and allocation/fence trace.
- **Documentation:** ring generation/ownership model and pass evidence.

**Implementation contract:** Each byte range is immutable until consuming fence completion. Wrap
reuses only non-overlapping retired ranges. Pressure waits on the oldest conflict or uses explicit
fence-safe growth, never latest-global by implication. Cross-queue use declares synchronization.

**Implementation outline:** Attach offset/size/generation/fence; reclaim completed ranges; make wrap/
growth testable; convert audited consumers without data changes; stress with global wait still on.

**Non-goals:** PT-26 caching, upload format, descriptors, or removing global wait.

**Interim tests:** Unit boundaries/wrap/growth; large uploads, animation, dual view, resize, D3D/GBV.

**Pass completion gate:** Forced wrap/growth has no overwrite/stale data/validation error; every
audited range carries a retirement fence.

**Expected visible effect:** None.

**Expected performance effect:** None until overlap; possible bookkeeping overhead.

**Regression surface:** Large scenes, emissive uploads, transforms, resize, exhaustion, dual view.

**Stop and escalate conditions:** Stop if consuming fence is unknowable, queue ownership is implicit,
or safe growth requires an unplanned allocator architecture.

**Handoff to next pass:** Unit/stress output, allocation trace, updated ownership list, revision.

**Suggested model:** `Sol High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-22 partially. Prerequisites: S6-P2 and S6-P1's upload inventory.
> Implement only **S6-P3**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S6-P3** after S6-P2. Fence upload-ring byte ranges to consuming submissions;
> preserve data and global drain. Reuse only completed non-overlapping ranges; pressure waits on the
> oldest conflict or uses explicit safe growth. Add boundary/wrap/pressure/growth tests and stress
> large uploads, animation, resize, dual view with D3D debug/GBV. Require complete per-range ownership
> with no stale/overwritten data. Do not implement emissive caching, descriptors, or S6-P6. Record
> revision, files, exact results, and traces. Stop if queue/fence ownership is unavailable. Do not
> begin S6-P4.

### S6-P4 - Fence descriptor tables and capacity growth

**Parent stage:** S6.

**Audit findings:** PT-22 partially.

**Purpose:** Keep descriptor-table generations immutable and live while referenced in flight.

**Why this is a separate pass:** Capacity growth rewrites ring slots today and has a distinct
use-after-rewrite hazard.

**Prerequisites:** S6-P3 and S6-P1's descriptor inventory.

**Source scope:**

- **Primary implementation:** renderer descriptor heap/table allocation, slot tables, growth/recreate.
- **Consumers/call sites:** `DxrDispatchContext`, `ScreenSpaceEffects`, scene/material, PT/hybrid bindings.
- **Tests/diagnostics:** `descriptor-heap-tests`, forced growth, generation/fence trace.
- **Documentation:** descriptor-generation ownership table and status.

**Implementation contract:** A submitted table/heap generation remains immutable/live until its last-
use fence. Growth creates a future generation, never rewrites an in-flight slot. Reclaim old
generations only after completion. PT-32 TLAS index reclamation remains S9.

**Implementation outline:** Identify generations; record last use; build future tables separately;
defer completed generations; force growth across slots/viewports.

**Non-goals:** Layout/bindless/root changes or PT-32 reclamation.

**Interim tests:** Descriptor tests; forced growth/recreate; resize/mode/dual view; D3D/GBV.

**Pass completion gate:** No submitted table changes or retires early; growth is validation-clean;
indices/output are equivalent.

**Expected visible effect:** None.

**Expected performance effect:** None until overlap; temporary generations can raise peak use.

**Regression surface:** Scene/material growth, resize, viewports, every descriptor consumer.

**Stop and escalate conditions:** Stop on vendor-owned undocumented lifetime, inability to coexist
generations, or root-layout requirement.

**Handoff to next pass:** Generation trace, peak/live counts, tests, residual inventory, revision.

**Suggested model:** `Sol High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-22 partially. Prerequisites: S6-P3 and S6-P1's descriptor inventory.
> Implement only **S6-P4**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S6-P4** after S6-P3. Make descriptor tables/capacity generations fence-owned;
> never rewrite or reclaim data referenced by incomplete submissions. Preserve layouts/indices/modes
> and defer PT-32. Run `descriptor-heap-tests`, core tests, forced growth, resize, dual-view, and mode
> stress under D3D/GBV. Require immutable in-flight generations, completed-fence reclamation,
> equivalent output, and clean validation. Record revision, files, commands/results, counts, and
> trace. Stop on root/SDK/generation conflict. Do not begin S6-P5.

### S6-P5 - Fence acceleration-structure generations

**Parent stage:** S6.

**Audit findings:** PT-22 partially.

**Purpose:** Give BLAS/TLAS scratch, result, update, and bound-SRV generations explicit lifetimes.

**Why this is a separate pass:** AS build/update/read ordering is high consequence and independent of
general frame resources.

**Prerequisites:** S6-P4 and S6-P1's AS inventory.

**Source scope:**

- **Primary implementation:** `DxrDispatchContext` AS build/update, scratch/result, and TLAS SRV generation.
- **Consumers/call sites:** PT/hybrid dispatch, SBT/root bindings referencing TLAS.
- **Tests/diagnostics:** static skip, rebuild/refit, relocation, animation, D3D/GBV.
- **Documentation:** AS build-complete versus trace-last-use fence table.

**Implementation contract:** Scratch reuses after build/update completion. Result and descriptor stay
live through every trace submission. Preserve update source/destination ordering, barriers, flags,
unchanged-build skipping, and cleared SBT/payload conclusions. Pass last-use fence to PT-32 records
without reclaiming them here.

**Implementation outline:** Attach generation/build/use fences; separate build eligibility/trace
lifetime; preserve barriers; propagate relocation last use; stress all paths.

**Non-goals:** AS tuning/compaction, payload/SBT repacking, PT-32 reclaim, or global-wait removal.

**Interim tests:** Build/tests; static/dynamic/rebuild/relocation/mode transition; D3D/GBV.

**Pass completion gate:** No AS resource/descriptor reuses before its governing fence; all inventory
items close or escalate; validation is clean.

**Expected visible effect:** None.

**Expected performance effect:** None until S6-P6; temporary retention expected.

**Regression surface:** Static/dynamic geometry, hybrid/full/reference PT, growth, resize, view alternation.

**Stop and escalate conditions:** Stop on undocumented queue overlap, unidentifiable last trace use,
or barrier assumptions invalid under generations.

**Handoff to next pass:** AS timeline/trace, validation, signed ownership checklist, revision/results.

**Suggested model:** `Sol High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-22 partially. Prerequisites: S6-P4 and S6-P1's AS inventory.
> Implement only **S6-P5**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S6-P5** after S6-P4. Fence BLAS/TLAS scratch, result, update, and bound-SRV
> generations to their actual build and trace submissions. Preserve flags, barriers, ABI, and build
> skipping; pass the last-use fence to PT-32 but reclaim nothing yet; keep global wait. Exercise static
> skip, animation/refit, rebuild, relocation, hybrid/PT modes, and growth with D3D/GBV. Require correct
> survival through all fences and no validation error. Record revision, files, exact results, and
> traces. Stop on cross-queue/AS contradiction. Do not begin S6-P6.

### S6-P6 - Remove the latest-global-submission wait

**Parent stage:** S6.

**Audit findings:** PT-22 substantially.

**Purpose:** Allow frame N+1 CPU recording to overlap frame N GPU execution using explicit ownership.

**Why this is a separate pass:** It is the single scheduling transition after resource contracts,
not a place for more ownership redesign.

**Prerequisites:** S6-P1 through S6-P5 with no unresolved drain-protected resource.

**Source scope:**

- **Primary implementation:** `GfxContext::BeginFrame` wait selection/submission bookkeeping.
- **Consumers/call sites:** all slot/ring/descriptor/AS owners and renderer/application frame flow.
- **Tests/diagnostics:** wait-decision tests, D3D/GBV stress, deterministic output, overlap timeline.
- **Documentation:** new wait contract and before/after evidence.

**Implementation contract:** `BeginFrame` waits only for the selected frame slot and explicit
conflicting resource/ring fences, not merely a newer unrelated global submission. Present/pacing is
separate. No owner may regain implicit global-drain safety.

**Implementation outline:** Assert checklist; remove global max component; retain explicit waits;
exercise all pressure paths; capture overlap.

**Non-goals:** Frame-count/present changes, producer suppression, tuning, or cleanup.

**Interim tests:** Wait-unit tests; build/core; slot rotation, ring wrap, descriptor growth, AS rebuild,
resize, dual view, D3D/GBV; deterministic comparison.

**Pass completion gate:** Timeline proves overlap; focused stress has no early reuse/validation error;
output is equivalent. Long acceptance remains S6-P7.

**Expected visible effect:** None.

**Expected performance effect:** Potential throughput/pacing improvement; magnitude unproven.

**Regression surface:** All in-flight resources, present, skip/minimize, resize/growth/load, dual view.

**Stop and escalate conditions:** Stop on unresolved owner, GBV hazard, need to alter present/unrelated
behavior, or output change.

**Handoff to next pass:** Timelines, wait distributions, hashes, validation, assertions, revision/results.

**Suggested model:** `Sol Ultra`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-22 substantially. Prerequisites: S6-P1 through S6-P5 with no unresolved drain-protected resource.
> Implement only **S6-P6**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S6-P6** after S6-P1 through P5 have no unresolved owner. Remove the unconditional
> latest-global-submission component from `GfxContext::BeginFrame`; retain selected-slot and explicit
> conflict waits. Do not change frame count, present, algorithms, or PT/raster production. Build/test
> and stress slot/ring/descriptor/AS/resize/dual-view under D3D/GBV; capture frame N+1 CPU overlap and
> compare deterministic outputs. Record revision, files, exact validation/visual/timing evidence.
> Stop on any ownership hazard or output change. Do not begin S6-P7.

### S6-P7 - Prove long-running frame overlap

**Parent stage:** S6.

**Audit findings:** PT-22 completion validation.

**Purpose:** Prove the overlap model is correct, bounded, and worthwhile in long production stress.

**Why this is a separate pass:** One no-crash run cannot establish ownership safety or pacing benefit.

**Prerequisites:** S6-P6.

**Source scope:**

- **Primary implementation:** diagnostic/test harness only; no intended production change.
- **Consumers/call sites:** complete resource model under representative and geometry-heavy workloads.
- **Tests/diagnostics:** long D3D/GBV stress, CPU/GPU timelines, hashes/statistical images, resource peaks.
- **Documentation:** final PT-22 evidence or rollback recommendation.

**Implementation contract:** Exercise dual view, resize, scene growth, ring wrap, descriptor growth,
AS update/rebuild, mode cuts, and present-unblocked timing. Separate CPU record, each wait, present,
GPU median/p95, and pacing.

**Implementation outline:** Define stress manifest; run long transitions; capture before/after
timeline/distributions; compare output/history; accept or recommend rollback.

**Non-goals:** Fixing unrelated exposed defects, changing quality, increasing waits, or PT/raster work.

**Interim tests:** Full named suite; D3D/GBV; checkpoints with generation counts and hashes.

**Pass completion gate:** No validation error/unbounded growth; sustained overlap; equivalent outputs;
p95/pacing/throughput improvement justifies complexity.

**Expected visible effect:** None.

**Expected performance effect:** Repeatable measured improvement required for acceptance.

**Regression surface:** Full renderer, long editor sessions, pressure, and present behavior.

**Stop and escalate conditions:** Stop on an ownership class outside P2-P5, need to restore a drain,
or neutral/negative result outside noise.

**Handoff to next pass:** Long manifests/logs, captures/stats, resource peaks, and accept/rollback decision.

**Suggested model:** `Sol Ultra`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-22 completion validation. Prerequisites: S6-P6.
> Implement only **S6-P7**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Execute only **S6-P7** after S6-P6. Run long deterministic D3D/GBV stress over representative/
> geometry-heavy scenes, dual view, resize/growth, upload wrap, descriptor pressure, AS changes, mode
> cuts, and present-unblocked operation. Capture CPU record, each fence wait, present, GPU median/p95,
> pacing, resource peaks, and sustained overlap; compare deterministic/statistical outputs. Require
> clean validation, bounded resources, preserved output, and a repeatable whole-frame/pacing benefit.
> Diagnose or recommend rollback; do not hide failures with waits/thresholds. Record revision and all
> artifacts. Stop before S6-P8.

### S6-P8 - Dedicated full-PT input and root contract

**Parent stage:** S6.

**Audit findings:** PT-23 partially.

**Purpose:** Make full PT and its reconstruction bundle independent of unused raster attachments while
preserving shared scene truth.

**Why this is a separate pass:** Dependencies must be removed before any raster producer is skipped.

**Prerequisites:** S6-P7, S1-P6, and S2-P5.

**Source scope:**

- **Primary implementation:** `DxrPathTracerDispatch::FrameInputs`, PT root/binding, and PT-owned HDR/
  depth/motion/albedo/normal-roughness/surface bundle.
- **Consumers/call sites:** `SceneRenderer::RecordDxrPass`, `ScreenSpaceEffects`, `DlssResolvePass`,
  diagnostic/fallback bundle selection.
- **Tests/diagnostics:** reflection/source search, absent-raster-input PT run, RTPSO/root/SBT validation.
- **Documentation:** required/optional PT input table and mode dependency inventory.

**Implementation contract:** Full PT receives GPU scene/material/instance, environment, AS, and PT-
owned outputs. Raster depth/normal/material/direct/shadow/indirect/velocity are not required for PT
radiance. Diagnostics/fallbacks declare optional dependencies. Every root-layout change keeps all
RTPSOs/associations ABI-consistent.

**Implementation outline:** Define required versus optional; convert validation/calls; bind deliberate
resources; version bundle; run with attachments absent while producers remain on.

**Non-goals:** Skipping CSM/geometry, PT-24 direct tagging, guide changes, or deleting raster/hybrid layouts.

**Interim tests:** Shader/reflection; source search; PT direct/DLSS/RR/reference with absent raster;
raster/hybrid regression.

**Pass completion gate:** Full PT starts/renders without raster scene attachments; no hidden read or
RTPSO/root mismatch; non-PT modes remain intact.

**Expected visible effect:** None.

**Expected performance effect:** None until S6-P9.

**Regression surface:** All PT outputs/diagnostics, raster/hybrid shared layout, both viewports.

**Stop and escalate conditions:** Stop on hidden production consumer, ABI/root limit, or SDK tag
contract contradiction.

**Handoff to next pass:** Required/optional table, search/reflection, mode images/hashes, RTPSO logs.

**Suggested model:** `Sol High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-23 partially. Prerequisites: S6-P7, S1-P6, and S2-P5.
> Implement only **S6-P8**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S6-P8** after S6-P7/S1/S2. Correct PT-23 by defining dedicated full-PT inputs/root
> and PT-owned reconstruction outputs; full PT must not require raster depth/normal/material/direct/
> shadow/indirect/velocity except explicitly selected diagnostics. Preserve shared scene/material/AS/
> environment, raster/hybrid layouts, and guide semantics. Do not disable raster/CSM or implement
> PT-24. Compile/reflection-search, validate RTPSO/root/SBT, and run PT direct/DLSS/RR/reference plus
> raster/hybrid with raster attachments absent from PT. Record revision, files, results, and evidence.
> Stop on hidden consumer/ABI/SDK conflict. Do not begin S6-P9.

### S6-P9 - Suppress raster scene and CSM production in full PT

**Parent stage:** S6.

**Audit findings:** PT-23 substantially.

**Purpose:** Stop scene-object G-buffer and CSM work only when active full PT has no consumer.

**Why this is a separate pass:** Producer gating follows proven consumer independence.

**Prerequisites:** S6-P8.

**Source scope:**

- **Primary implementation:** `SceneRenderer::Render`, `RenderShadowPass`, `RenderGeometryPass`, and
  full-PT dependency gates.
- **Consumers/call sites:** post process, overlays/grid/selection/picking/debug, PT/reconstruction.
- **Tests/diagnostics:** per-pass markers, geometry-heavy captures, full mode/editor matrix.
- **Documentation:** mode inventory and explicit diagnostic fallback policy.

**Implementation contract:** In active full PT, skip scene-object CSM/G-buffer unless a declared
consumer explicitly requests fallback. Preserve viewport targets, UI/editor overlays, grid, selection,
picking, raster/hybrid, and reference PT. Never consume stale raster resources.

**Implementation outline:** Gate producers from dependency declaration; preserve composition; make
optional diagnostics request/reject clearly; capture cost in representative/geometry-heavy scenes.

**Non-goals:** Global raster-resource removal, overlay deletion, PT shading, or hybrid changes.

**Interim tests:** PT direct/DLSS/RR/reference; raster/hybrid goldens; editor tools; dual view; resize/
mode cuts; D3D/GBV.

**Pass completion gate:** Full PT renders with producers disabled; no stale read; all supported/editor
functions remain; CPU/GPU result is non-negative.

**Expected visible effect:** None.

**Expected performance effect:** Structural work removal; magnitude scene-dependent and small in the
audited simple scene.

**Regression surface:** Editor tools, post/debug views, every non-PT mode, reconstruction variants.

**Stop and escalate conditions:** Stop on undeclared consumer, out-of-mode change, output difference,
or negative whole-frame result outside noise.

**Handoff to next pass:** Matrix, marker captures, timings, hashes/images, fallback list, revision.

**Suggested model:** `Sol High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-23 substantially. Prerequisites: S6-P8.
> Implement only **S6-P9**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S6-P9** after S6-P8. Skip scene-object G-buffer and CSM only in active full PT;
> preserve viewport targets, overlays/grid/selection/picking, raster, hybrid, reference, both
> viewports, and every reconstruction combination. Optional raster diagnostics request an explicit
> producer or fail clearly; never read stale attachments. Run the full editor/mode matrix with D3D/
> GBV and capture CPU/GPU markers in representative/heavy scenes. Require unchanged output, no stale
> access, preserved modes, and non-negative measurement. Record revision, files, visual/timing results.
> Stop on hidden consumer/cross-mode dependency. Do not begin S6-P10.

### S6-P10 - Cross-system S6 validation

**Parent stage:** S6.

**Audit findings:** PT-22 and PT-23 completion.

**Purpose:** Validate combined frame overlap and PT/raster isolation across the supported matrix.

**Why this is a separate pass:** It is a major cross-system review, not an implementation catch-all.

**Prerequisites:** S6-P9.

**Source scope:**

- **Primary implementation:** test/capture automation and necessary diagnostics only.
- **Consumers/call sites:** raster, hybrid, PT direct/DLSS/RR/reference, both viewports/editor.
- **Tests/diagnostics:** full tests, D3D/GBV, long stress, mode matrix, timing/resource bounds.
- **Documentation:** signed S6 evidence and known optional/unsupported diagnostics.

**Implementation contract:** Exercise mode cuts, resize, dual view, overlays, heavy scenes, AS/
descriptor/upload pressure, and long overlap; keep correctness, image equivalence, and timing separate.

**Implementation outline:** Run deterministic matrix; inspect markers/timelines; compare output;
record peaks; route failures to owning pass.

**Non-goals:** New fixes, quality tuning, cleanup, or SER.

**Interim tests:** Audit build/core; D3D all with known printed-result/teardown distinction; live matrix.

**Pass completion gate:** PT-22/PT-23 stage gates pass together without renderer/editor loss,
unbounded resources, or unsupported performance claims.

**Expected visible effect:** None.

**Expected performance effect:** Combined benefit reported only from measurements.

**Regression surface:** Entire renderer.

**Stop and escalate conditions:** Stop if failure cannot be assigned P2-P9, supported mode needs
implicit coupling, output differs, or pacing regresses.

**Handoff to next pass:** Signed matrix, captures, revision/results, resource peaks, limitations.

**Suggested model:** `Sol Ultra`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-22 and PT-23 completion. Prerequisites: S6-P9.
> Implement only **S6-P10**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Execute only **S6-P10** after S6-P9. Review PT-22/PT-23 without features/cleanup. Build/run audit
> tests, recording D3D printed assertions separately from teardown; run long D3D/GBV across raster,
> hybrid, PT direct/DLSS/RR/reference, all qualities, viewports, editor, resize/cuts, and resource/AS
> pressure. Capture equivalence, bounds, overlap/waits, median/p95, and producer savings separately.
> Require both stage gates. Route failures to their owner; record revision, commands/results, manifests,
> captures, and limitations. Stop before S7-P1.

### 11.10 S7 - proven redundant-work passes

### S7-P1 - Direct-tag PT resources for RR

**Parent stage:** S7.

**Audit findings:** PT-24.

**Purpose:** Remove RR preparation copies by tagging semantically correct PT-owned resources directly.

**Why this is a separate pass:** It is one resource-layout invariant and one measurable five-pass
removal, isolated from other optimizations.

**Prerequisites:** S6-P10, S2-P5, and S3-P7.

**Source scope:**

- **Primary implementation:** `ScreenSpaceEffects::PreparePathTracerRrBundle`, `DlssResolvePass` tags,
  and PT output layout in `DxrDispatchContext`.
- **Consumers/call sites:** Streamline RR, PT debug AOVs, raster/hybrid bundles.
- **Tests/diagnostics:** SDK/state validation, AOV equivalence, narrow preparation markers.
- **Documentation:** PT bundle formats/states/extents/lifetime and removed-pass ledger.

**Implementation contract:** Tag PT HDR, R32 depth, motion, albedos, signed world normal/linear
roughness directly at active extent/state/lifetime. If hit distance/specular albedo must be distinct,
produce it once in PT. Remove three guide blits, depth conversion, and extraction only after
equivalence. Preserve distinct raster/hybrid data.

**Implementation outline:** Validate formats; make layout tag-ready; convert tags; compare AOVs;
remove obsolete passes/resources/markers; retain intentional diagnostics.

**Non-goals:** PT-08 semantics, motion/exposure/sizing, estimator math, or raster/hybrid guide removal.

**Interim tests:** Build/core/shader; SDK validation; direct/DLSS/RR matrix; state capture; pixel/AOV.

**Pass completion gate:** Valid tags/states/lifetime; equivalent guides/output; all five preparation
passes absent; narrow time/bandwidth lower outside noise.

**Expected visible effect:** None.

**Expected performance effect:** Lower full-resolution bandwidth/pass overhead, measured here.

**Regression surface:** RR qualities, PT AOVs, resize/cuts, raster/hybrid reconstruction.

**Stop and escalate conditions:** Stop if SDK rejects format, copied data has a hidden consumer,
lifetime is unsafe, or equivalence fails.

**Handoff to next pass:** Tag table, capture/AOVs, timing, removed resource list, revision/results.

**Suggested model:** `Sol High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-24. Prerequisites: S6-P10, S2-P5, and S3-P7.
> Implement only **S7-P1**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S7-P1** after S6/S2/S3. Correct PT-24 by directly tagging PT-owned RR inputs with
> correct formats, extents, states, and lifetime; R32 depth is supported. Produce any required split
> hit-distance layout once in PT. Preserve raster/hybrid bundles and guide semantics. Remove the five
> prep passes only after SDK/state and AOV/final-output equivalence. Run the full mode matrix and
> narrow timing. Require equivalent data, clean validation, pass removal, and repeatable reduction.
> Record revision, files, commands/results, captures, and timing. Stop on SDK/consumer/lifetime conflict.
> Do not begin S7-P2.

### S7-P2 - Remove unused PT history textures

**Parent stage:** S7.

**Audit findings:** PT-25.

**Purpose:** Remove `m_ptPrevDepth` and `m_ptPrevNormalRoughness` only after confirming zero consumers.

**Why this is a separate pass:** It has a simple equivalence gate and must not share timing with
another optimization.

**Prerequisites:** S7-P1.

**Source scope:**

- **Primary implementation:** `DxrDispatchContext` allocation and history-copy paths.
- **Consumers/call sites:** getters, barriers, reset/resize, diagnostics/readbacks.
- **Tests/diagnostics:** repository/reflection search, captured reads, memory/copy markers.
- **Documentation:** removed-resource/API ledger and PT-25 status.

**Implementation contract:** If zero consumers, remove both allocations, copies, barriers, getters,
and stale API atomically. Preserve ReSTIR surface-record history. Any future consumer allocates lazily.

**Implementation outline:** Search/capture reads; remove storage/API; remove copies/transitions; update
reset/resize; verify memory/markers.

**Non-goals:** ReSTIR/history-policy changes or other cleanup.

**Interim tests:** Build/core; PT/ReSTIR/RR; reset/resize/cuts; resource capture.

**Pass completion gate:** No consumer remains; output/history is unchanged; resources/copies vanish.
The audited 42.19 MiB/84.38 MiB calculations remain calculations, not measured speedup.

**Expected visible effect:** None.

**Expected performance effect:** Lower memory/copy traffic; runtime result measured separately.

**Regression surface:** PT/ReSTIR/RR histories, diagnostics, resize, transitions.

**Stop and escalate conditions:** Stop if any runtime/debug/readback consumer appears or ReSTIR must change.

**Handoff to next pass:** Search/capture, memory/copy evidence, mode tests, revision/results.

**Suggested model:** `Terra High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-25. Prerequisites: S7-P1.
> Implement only **S7-P2**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S7-P2** after S7-P1. For PT-25, prove `m_ptPrevDepth` and
> `m_ptPrevNormalRoughness` have no runtime/diagnostic/readback consumer, then remove allocation,
> copies, barriers, getters, and reset/resize API atomically. Preserve ReSTIR histories. Build/test PT,
> ReSTIR, RR, resize, resets, and cuts; capture reads and markers. Require zero consumers, unchanged
> behavior, and structural resource/copy removal; do not call calculated traffic measured savings.
> Record revision, files, searches, results. Stop if a consumer exists. Do not begin S7-P3.

### S7-P3 - Cache emissive distributions by generation

**Parent stage:** S7.

**Audit findings:** PT-26.

**Purpose:** Rebuild the canonical emissive distribution only when its input fingerprint changes and
keep every in-flight slot coherent.

**Why this is a separate pass:** Dirty invalidation plus slot propagation is one measurable ownership
invariant and depends on completed correctness.

**Prerequisites:** S7-P2, S5-P3, and S6-P10.

**Source scope:**

- **Primary implementation:** `SceneRenderer::RenderPostProcessPass`, `UploadEmissiveLights`, five
  GPU uploads, and scene/material dirty generations.
- **Consumers/call sites:** PT emissive NEE/MIS and ReSTIR proposals.
- **Tests/diagnostics:** fingerprint/reason tests, static/moving/edit cases, slot generations, CPU/copy markers.
- **Documentation:** fingerprint/invalidation/slot contract and PT-26 status.

**Implementation contract:** Fingerprint canonical topology, material/emission, relevant transforms,
and light/environment policy. Rebuild only on change. Every in-flight slot receives a coherent
generation before use and is never overwritten in flight. Transform-only split requires separate
equivalence/benefit evidence.

**Implementation outline:** Define fingerprint/reasons; cache CPU derivation; propagate fence-safely;
convert call; instrument rebuild/count/bytes; test every invalidator.

**Non-goals:** PT-13 math, sampling/MIS, ReSTIR behavior, or broad scene caching.

**Interim tests:** Static warmup; topology/transform/material/emission edits; animation; dual view;
overlap; timing/equivalence.

**Pass completion gate:** Zero static rebuilds after warmup; each edit invalidates exact data; slots
agree; output equivalent; CPU/upload cost improves in emissive-heavy stress.

**Expected visible effect:** None.

**Expected performance effect:** Lower static CPU/upload work, scene-dependent.

**Regression surface:** All emitter types, motion/edits, load, viewports, frame slots.

**Stop and escalate conditions:** Stop if PT-13 is incomplete, dirty signal misses an input, safe
propagation needs a drain, or output changes.

**Handoff to next pass:** Schema, invalidation matrix, slot trace, equivalence/timing, revision/results.

**Suggested model:** `Sol High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-26. Prerequisites: S7-P2, S5-P3, and S6-P10.
> Implement only **S7-P3**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S7-P3** after S7-P2, PT-13, and S6. Correct PT-26 by fingerprinting canonical
> emissive topology, material/emission, transforms, and light/environment policy; rebuild/upload only
> on change and populate each in-flight slot fence-safely. Do not change emitter definition, PDFs,
> MIS, or ReSTIR. Test warmup, each edit, animation, dual view, and overlap; record reasons/counts/
> bytes, CPU/copy timing, and equivalence. Require zero static rebuilds, exact invalidation, coherent
> slots, unchanged output, and repeatable narrow saving. Record revision/files/results. Stop on
> invalidation/ownership ambiguity. Do not begin S7-P4.

### S7-P4 - Precompute the triangle LOD invariant

**Parent stage:** S7.

**Audit findings:** PT-27.

**Purpose:** Move the fixed mesh/triangle/material portion of
`ComputeTriangleAlbedoLodConstant` out of per-hit execution without changing LOD.

**Why this is a separate pass:** It changes one scene-upload representation and shader consumer only.

**Prerequisites:** S7-P3 and S3-P6.

**Source scope:**

- **Primary implementation:** scene upload data and `path_tracer.hlsl::ComputeTriangleAlbedoLodConstant`.
- **Consumers/call sites:** closest-hit filtering and any shared evaluator.
- **Tests/diagnostics:** old/new LOD AOV, static/scaled/animated/missing-UV, shader metrics/timing.
- **Documentation:** cached representation, transform correction, dirty policy, ABI, tolerance.

**Implementation contract:** Cache per-triangle or mesh/material object-space footprint plus exact
transform correction. Preserve non-uniform animation, UV degeneracy/missing UV, and dimensions. Any
GPU data ABI change updates declarations/bounds.

**Implementation outline:** Isolate invariant; define/upload cache; compute on correct dirty events;
convert shader with old/new AOV; remove old work only after equivalence.

**Non-goals:** Coarser heuristic, PT-06 cone change, filtering change, or unrelated payload packing.

**Interim tests:** Shader build; LOD AOV across transforms/UV cases; instruction/register/PT timing.

**Pass completion gate:** Old/new LOD agrees within declared tolerance; ABI/bounds clean; hit or frame
cost improves outside noise.

**Expected visible effect:** None.

**Expected performance effect:** Lower closest-hit work if cache trade is favorable.

**Regression surface:** Texture-heavy models, animation/scale, UV edge cases, upload memory.

**Stop and escalate conditions:** Stop if exact correction is not compact, equivalence fails, or cache
cost exceeds benefit without simplifying ownership.

**Handoff to next pass:** Layout/dirty policy, AOVs, shader metrics/timing, mode tests, revision.

**Suggested model:** `Sol High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-27. Prerequisites: S7-P3 and S3-P6.
> Implement only **S7-P4**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S7-P4** after S7-P3/S3. Correct PT-27 by precomputing the invariant part of
> `ComputeTriangleAlbedoLodConstant` in scene-upload data with exact transform correction. Preserve
> LOD for static, uniform/non-uniform, animated, degenerate/missing UV. Do not change cones, filtering,
> or unrelated payloads. Compile shaders, compare old/new LOD AOVs, validate ABI, and measure closest-
> hit/whole PT in texture-heavy stress. Require equivalence first and repeatable benefit second.
> Record revision, files, results, AOVs, timing. Stop if exactness/ABI cannot hold. Stop before S8-P1.

### 11.11 S8 - SER validation and policy passes

### S8-P1 - Correct the SER capability decision

**Parent stage:** S8.

**Audit findings:** PT-28.

**Purpose:** Gate SER on the actual DXR/SM/runtime contract rather than DXR Tier 1.2.

**Why this is a separate pass:** The pure decision can be exhaustively tested without UI or timing.

**Prerequisites:** S7-P4 and S0-P1.

**Source scope:**

- **Primary implementation:** `DxrCapabilities` decision/query implementation.
- **Consumers/call sites:** SER shader/RTPSO creation and fallback selection.
- **Tests/diagnostics:** RT tier x SM pure matrix, forced creation failure, capability log.
- **Documentation:** capability matrix and PT-28 status.

**Implementation contract:** Attempt SER only with RT tier >=1.0, SM 6.9, relevant runtime report,
and successful shader/PSO creation. Preserve hot functional `lib_6_6`/lower fallback.

**Implementation outline:** Extract pure decision; remove Tier-1.2-only condition; add matrix/failure;
preserve fallback reasons.

**Non-goals:** UI, default enablement, hints, benchmarks, or fallback removal.

**Interim tests:** RT absent and tiers 1.0/1.1/1.2 crossed with SM 6.8/6.9; PSO failure; build/shaders.

**Pass completion gate:** Only supported cells attempt SER; every unsupported/failure cell selects the
tested fallback.

**Expected visible effect:** None.

**Expected performance effect:** None intrinsically; enables later measurement on eligible hardware.

**Regression surface:** Capability detection, startup PSOs, fallback hardware.

**Stop and escalate conditions:** Stop if runtime/official contract cannot reconcile or fallback
prewarm/function regresses.

**Handoff to next pass:** Matrix, capability logs, PSO/fallback outcomes, revision/results.

**Suggested model:** `Terra High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-28. Prerequisites: S7-P4 and S0-P1.
> Implement only **S8-P1**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S8-P1** after S7/S0. Correct PT-28: gate SER on RT tier >=1.0, SM 6.9, runtime
> capability, and successful shader/RTPSO creation; remove the Tier-1.2 requirement and preserve
> `lib_6_6`/lower fallback. Add RT-absent and tier 1.0/1.1/1.2 x SM 6.8/6.9 plus failure tests. Build/
> compile and require correct attempt/fallback in every cell. Do not change UI/default/hints or
> benchmark. Record revision, files, commands/results, capability evidence. Stop on unreconciled
> runtime contract. Do not begin S8-P2.

### S8-P2 - Report truthful SER runtime state

**Parent stage:** S8.

**Audit findings:** PT-29 partially.

**Purpose:** Separate availability, requested policy, dispatched permutation, actual reorder, and
fallback reason in UI/logs.

**Why this is a separate pass:** Truthful state is required before interpreting benchmarks.

**Prerequisites:** S8-P1 and S0-P1.

**Source scope:**

- **Primary implementation:** `DxrPathTracerDispatch` status model, `m_activeSerPermutation`/
  `IsPathTracerSerActive` semantics, Options22 data.
- **Consumers/call sites:** `RayTracingSection`, logs, and manifest.
- **Tests/diagnostics:** status-state unit matrix and live capability capture.
- **Documentation:** precise SER state vocabulary and status evidence.

**Implementation contract:** Report adapter/runtime capabilities, compiler/PSO availability,
auto/off/on request, permutation dispatched this frame, actual-reorders where exposed, and fallback
reason separately. “Active” cannot mean permutation selected alone.

**Implementation outline:** Define status structure; populate at capability/creation/policy/dispatch;
update UI/log/manifest; test contradictory/fallback cases.

**Non-goals:** Speed claims, hints, default policy, or more capability logic.

**Interim tests:** Pure states; force off/on/unavailable/failure; live target adapter; fallback render.

**Pass completion gate:** UI/log/manifest agree and cannot label unsupported/fallback selection as
actual hardware reorder.

**Expected visible effect:** Diagnostic/UI wording only.

**Expected performance effect:** None.

**Regression surface:** Settings UI, startup logs, fallback, dispatch selection.

**Stop and escalate conditions:** Stop if expected runtime field is absent, UI forces state collapse,
or live dispatch contradicts status.

**Handoff to next pass:** Schema, screenshots/logs, manifest, forced-state results, revision.

**Suggested model:** `Terra High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-29 partially. Prerequisites: S8-P1 and S0-P1.
> Implement only **S8-P2**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S8-P2** after S8-P1. Correct PT-29 observability by reporting capability,
> compiler/PSO availability, requested auto/off/on, permutation dispatched this frame, actual-reorder
> status where exposed, and fallback reason distinctly. Narrow `IsPathTracerSerActive`/
> `m_activeSerPermutation`; never call selection actual reorder. Preserve fallback and do not benchmark
> or tune. Add status tests and live UI/log/manifest evidence. Require agreement/truthfulness. Record
> revision, files, exact results, screenshots/logs. Stop if runtime state cannot be represented. Do
> not begin S8-P3.

### S8-P3 - Automate the fixed SER benchmark protocol

**Parent stage:** S8.

**Audit findings:** PT-29 measurement preparation.

**Purpose:** Make section 7's force-off/on protocol reproducible without changing renderer quality.

**Why this is a separate pass:** Orchestration must be validated before evidence collection or hints.

**Prerequisites:** S8-P2 and S0-P5.

**Source scope:**

- **Primary implementation:** existing diagnostics/benchmark runner and SER policy invocation.
- **Consumers/call sites:** performance-roadmap artifact schema and manifest.
- **Tests/diagnostics:** dry-run schedule, incomplete-run detection, schema/hash tests.
- **Documentation:** exact section-7 invocation and artifact layout.

**Implementation contract:** Automate fresh sessions; 120-frame warm-up after each change; three
A-B-B-A 300-frame blocks and second session three B-A-A-B blocks; fixed ~1440p/1 spp/16 bounces/
RR+DLAA, seeds/extents/static camera. Canonical orbit is separate equivalence. Record narrow/enclosing
PT, GPU frame, CPU record/wait/present, median/p95/MAD, hash/images, and clocks/temperature if available.

**Implementation outline:** Encode order/warmups; validate manifest; detect partial runs; emit complete
schema; dry run.

**Non-goals:** Final experiment, renderer changes, lower quality, counters, or hints.

**Interim tests:** Schedule/window assertions; hash reproducibility; incomplete-run and schema validation.

**Pass completion gate:** Runner emits the exact sequence and metadata deterministically without
changing production configuration.

**Expected visible effect:** None.

**Expected performance effect:** None; tooling only.

**Regression surface:** Diagnostics configuration and artifacts.

**Stop and escalate conditions:** Stop if automation needs intrusive renderer changes, scenes/settings
cannot reproduce, or timestamps cannot separate.

**Handoff to next pass:** Runner command, dry schedule, schema, reference hash, revision/results.

**Suggested model:** `Terra High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-29 measurement preparation. Prerequisites: S8-P2 and S0-P5.
> Implement only **S8-P3**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S8-P3** after S8-P2/S0. Automate section 7 exactly: fresh processes; 120 warm-up
> frames after changes; three A-B-B-A 300-frame blocks and three reverse B-A-A-B blocks; fixed 1440p,
> 1 spp, 16 bounces, RR+DLAA, seed/extent/static camera; orbit only separate equivalence. Emit narrow/
> enclosing PT, GPU frame, CPU record/wait/present, median/p95/MAD, hash/images, and available clocks.
> Do not run final benchmark or alter quality/hints. Test schedule, warmup, partial-run detection,
> schema/hash. Record revision, files, results, dry artifacts. Stop if reproducibility requires renderer
> redesign. Do not begin S8-P4.

### S8-P4 - Run the base SER force-off/on experiment

**Parent stage:** S8.

**Audit findings:** PT-29 primary measurement.

**Purpose:** Determine whether the existing SER permutation preserves output and changes whole-frame
performance repeatably.

**Why this is a separate pass:** Base effect must be established before counters or hints.

**Prerequisites:** S8-P3 and S4A-P12.

**Source scope:**

- **Primary implementation:** no production code; validated benchmark/capture workflow.
- **Consumers/call sites:** divergence floor/interior, material/texture stress, rough dielectric,
  coherent sky, optional traversal control.
- **Tests/diagnostics:** full section-7 protocol and output equivalence.
- **Documentation:** raw windows, summaries, manifests, and evidence classification.

**Implementation contract:** Run identical force-off/on configs. Static camera is timing; orbit is
secondary equivalence. Fixed-seed outputs compare exactly where order permits, else by predeclared
epsilon/statistics. Do not mix scene or static/orbit samples.

**Implementation outline:** Capture capability; run both session orders; validate manifests; aggregate
per scene/policy; classify effect versus noise.

**Non-goals:** Hints, counters, default policy, resolution/quality tuning, or production edits.

**Interim tests:** Pilot window/manifest, equivalence, thermal/order review.

**Pass completion gate:** Both datasets are complete/valid; images equivalent; narrow and whole-frame
effects include spread. No speedup claim inside noise.

**Expected visible effect:** None beyond equivalent stochastic scheduling.

**Expected performance effect:** Unknown until measured.

**Regression surface:** SER/non-SER RTPSO, coherent/divergent workloads, live state/fallback.

**Stop and escalate conditions:** Stop on capability mismatch, permutation failure, output difference,
or invalid thermal/order protocol.

**Handoff to next pass:** Raw windows/summaries, images, manifests, logs, positive/neutral/negative classification.

**Suggested model:** `Sol High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-29 primary measurement. Prerequisites: S8-P3 and S4A-P12.
> Implement only **S8-P4**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Execute only **S8-P4** with S8-P3; change no code/hint. Run exact force-off/on A-B-B-A and reverse
> protocol on divergence, texture/material, rough dielectric, and sky control at fixed production
> settings/static camera; orbit separately. Require exact/predeclared statistical equivalence and
> report narrow/enclosing PT, whole frame, CPU wait/record/present, median/p95/MAD, and order/thermal
> effects. Stop on capability/permutation/output/protocol failure. Record commands, manifests, raw
> artifacts, and conclusions without speedup inside noise. Do not automatically begin S8-P5.

### S8-P5 - Diagnose a measured SER delta with GPU counters

**Parent stage:** S8.

**Audit findings:** PT-29 conditional diagnosis.

**Purpose:** Explain a repeatable S8-P4 delta with workload/occupancy evidence.

**Why this is a separate pass:** Counter capture is justified only after timing shows a real delta.

**Prerequisites:** S8-P4 with a non-neutral repeatable result; otherwise record not applicable.

**Source scope:**

- **Primary implementation:** no production code; PIX/Nsight-equivalent captures.
- **Consumers/call sites:** representative divergent and coherent-control frames.
- **Tests/diagnostics:** active lanes, divergence, registers/spills, occupancy, SM/RT utilization,
  hit/miss/material distribution, trace/reorder/invoke time.
- **Documentation:** counter diagnosis linked to exact manifests/windows.

**Implementation contract:** Capture matching frames/configs. A policy-relevant SER effect aligns with
counters/workload; mismatch makes the result unstable/unexplained, not a tuning basis.

**Implementation outline:** Select representative windows; capture matching policies; compare all
named counters; link evidence; classify causal or inconclusive.

**Non-goals:** Shader/hint/default/quality changes.

**Interim tests:** Manifest equality and capture-overhead perturbation check.

**Pass completion gate:** Consistent counter explanation, or explicit inconclusive classification
excluded from policy.

**Expected visible effect:** None.

**Expected performance effect:** None; diagnostics only.

**Regression surface:** Capture perturbation and representative-frame selection.

**Stop and escalate conditions:** Stop if capture reverses result, frames differ, counters unavailable,
or evidence contradicts.

**Handoff to next pass:** Captures/manifests and concise causal/inconclusive diagnosis.

**Suggested model:** `Sol High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-29 conditional diagnosis. Prerequisites: S8-P4 with a non-neutral repeatable result; otherwise record not applicable.
> Implement only **S8-P5**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Execute only **S8-P5** if S8-P4 found a repeatable non-neutral delta; otherwise mark N/A. Capture
> matched divergent/control frames and report lanes/occupancy, divergence, registers/spills, SM/RT,
> hit/miss/material, and trace/reorder/invoke timing. Change no shader/hint/default/quality. Require a
> counter explanation consistent with timing; otherwise call it inconclusive. Record capture commands,
> manifests, artifacts, and conclusion. Stop on perturbation/mismatch/unavailable evidence. Do not
> automatically begin S8-P6.

### S8-P6 - Evaluate isolated SER hint variants

**Parent stage:** S8.

**Audit findings:** PT-29 conditional optimization.

**Purpose:** Test narrowly isolated grouping changes against the proven base SER behavior.

**Why this is a separate pass:** Hint work is invalid until the base path and effect are proven.

**Prerequisites:** S8-P4; S8-P5 when diagnosis applies.

**Source scope:**

- **Primary implementation:** grouping around `HitObject::TraceRay`, `MaybeReorderThread`, and
  `Invoke` in `path_tracer.hlsl`; experimental permutations.
- **Consumers/call sites:** PT-only SM 6.9 RTPSO and fallback selection.
- **Tests/diagnostics:** S8 runner, image equivalence, register/spill/live-state metrics.
- **Documentation:** per-variant manifests/revisions and results.

**Implementation contract:** Test one at a time: current primary/continuation hint versus automatic;
optionally skip bounce 0; only then one small stable post-reorder work class. Record registers/spills.
Keep fallback hot and quality/estimator identical.

**Implementation outline:** Create one variant; compile; equivalence; full protocol/metrics; revert or
retain separately before next; never combine without isolated evidence.

**Non-goals:** Default policy, estimator/quality changes, or fallback removal.

**Interim tests:** Each RTPSO; output; shader live state; narrow/whole timing.

**Pass completion gate:** Every tried variant has equivalent output and complete timing/live-state;
no narrow-only win is retained when whole frame loses it.

**Expected visible effect:** None.

**Expected performance effect:** Unknown/workload-dependent.

**Regression surface:** SM 6.9 compile, live state, coherent scenes, fallback.

**Stop and escalate conditions:** Stop on output change, unjustified grouping, spill regression, or
unproven base operation.

**Handoff to next pass:** Variant revisions, manifests, images, timing/metrics, recommendation.

**Suggested model:** `Sol High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-29 conditional optimization. Prerequisites: S8-P4; S8-P5 when diagnosis applies.
> Implement only **S8-P6**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement/evaluate only **S8-P6** after the base experiment. Test isolated variants in order:
> current hint versus automatic; optional bounce-0 skip; then only if justified one small stable post-
> reorder work class. Never combine before isolated evidence. Preserve estimator/quality and fallback.
> Compile each RTPSO, record registers/spills/live state, run equivalence and full timing, and report
> narrow/whole results. Stop on output change, unjustified class, or live-state regression. Record
> files, per-variant revisions, commands/results, and recommendation. Do not begin S8-P7 automatically.

### S8-P7 - Set and document the SER policy

**Parent stage:** S8.

**Audit findings:** PT-29 completion.

**Purpose:** Convert validated capability/benchmark evidence into a truthful default or auto policy.

**Why this is a separate pass:** Product policy consumes evidence and cannot bias the experiment.

**Prerequisites:** S8-P4 and S8-P5/P6 when applicable.

**Source scope:**

- **Primary implementation:** `DxrSettings`, dispatch policy, capability fallback, proven hint if any.
- **Consumers/call sites:** `RayTracingSection`, logs/manifest, RTPSO prewarm.
- **Tests/diagnostics:** policy-state/fallback, equivalence, confirmation timing.
- **Documentation:** final policy with narrow/whole evidence and workload bounds.

**Implementation contract:** Default SER only for repeatable whole-frame benefit beyond noise without
material coherent-scene regression. Otherwise default off or use evidence-backed auto. Preserve
functional non-SER fallback and truthful state.

**Implementation outline:** Map evidence to policy; implement setting/fallback; include only proven
hint; test state/persistence/fallback; run confirmation.

**Non-goals:** New hints, quality changes, unsupported prediction, or marketing from narrow timing.

**Interim tests:** Policy matrix; startup/persistence; fallback; equivalence; representative timing.

**Pass completion gate:** Policy matches evidence; fallback/state tests pass; output equivalent;
documentation reports narrow and whole frame.

**Expected visible effect:** UI/default only; output equivalent.

**Expected performance effect:** Only the measured policy result; no generalization.

**Regression surface:** Startup/capability, persistence, coherent/divergent scenes, all PT outputs.

**Stop and escalate conditions:** Stop if evidence is inconclusive, fallback cannot remain, or a
supported mode changes.

**Handoff to next pass:** Final matrix/hint, confirmation, revision and evidence links.

**Suggested model:** `Sol High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-29 completion. Prerequisites: S8-P4 and S8-P5/P6 when applicable.
> Implement only **S8-P7**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S8-P7** from completed S8 evidence. Default/auto SER only for a repeatable whole-
> frame win beyond noise without coherent regression; otherwise default off/evidence-backed auto.
> Preserve non-SER fallback and truthful S8-P2 status; include only a passing hint. Run policy,
> persistence, fallback, equivalence, and confirmation timing. Do not add hints or change quality.
> Record revision, files, exact results, final policy, and narrow/whole evidence. Stop if evidence is
> inconclusive or a supported mode changes. Stop before S9-P1.

### 11.12 S9 - cleanup and lifetime-hardening passes

### S9-P1 - Fence-defer relocated TLAS descriptor reclamation

**Parent stage:** S9.

**Audit findings:** PT-32.

**Purpose:** Reclaim retired TLAS SRV indices after the last submission that can reference them.

**Why this is a separate pass:** Lifetime correctness precedes dedicated boundedness stress.

**Prerequisites:** S8-P7 and S6-P10.

**Source scope:**

- **Primary implementation:** `DxrDispatchContext` TLAS relocation/retired list and renderer deferred
  descriptor free.
- **Consumers/call sites:** TLAS root/descriptor bindings across in-flight slots.
- **Tests/diagnostics:** retirement decision tests, generation/count trace, D3D/GBV relocation.
- **Documentation:** PT-32 last-use/reclamation contract.

**Implementation contract:** Retired record contains index and last trace-submission fence. Enqueue in
existing deferred reclamation; free only after completion; destruction drains safely; no global stall.

**Implementation outline:** Propagate S6 last use; replace destruction-only retention; preserve
binding; instrument live/retired/reclaimed.

**Non-goals:** Heap redesign, unrelated reclamation, waits, or style cleanup.

**Interim tests:** Complete/incomplete decisions; repeated relocation; dual view/overlap; D3D/GBV.

**Pass completion gate:** Old indices remain valid while referenced and reclaim after completion; no
use-after-free or unbounded focused list.

**Expected visible effect:** None.

**Expected performance effect:** Negligible; bounded long-session descriptor use.

**Regression surface:** TLAS growth/rebuild, overlap, descriptor reuse, shutdown.

**Stop and escalate conditions:** Stop if last use/queue cannot be identified, deferred mechanism is
incompatible, or validation sees premature reuse.

**Handoff to next pass:** Retirement trace/counts, validation, docs, revision/results.

**Suggested model:** `Sol High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-32. Prerequisites: S8-P7 and S6-P10.
> Implement only **S9-P1**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S9-P1** after S6/S8. Correct PT-32 by replacing destruction-only retained TLAS
> SRVs with fence-deferred descriptor reclamation; each index carries the last trace fence and frees
> only after completion, with safe shutdown and no global stall. Run decision tests, repeated TLAS
> growth/relocation, dual view/overlap under D3D/GBV. Require no early reuse and a reclaiming list.
> Record revision, files, commands/results, counts, validation. Stop if last use/queue is unclear. Do
> not begin S9-P2.

### S9-P2 - Stress descriptor retirement

**Parent stage:** S9.

**Audit findings:** PT-32 completion validation.

**Purpose:** Prove descriptor usage remains bounded during long relocation and heap pressure.

**Why this is a separate pass:** Focused correctness does not prove long-session boundedness.

**Prerequisites:** S9-P1.

**Source scope:**

- **Primary implementation:** deterministic stress/count diagnostics only.
- **Consumers/call sites:** TLAS relocation, descriptor growth, all slots.
- **Tests/diagnostics:** repeated growth/rebuild, dual view, resize, scene churn, D3D/GBV.
- **Documentation:** stress manifest, peak/steady counts, and pass evidence.

**Implementation contract:** Force relocations across slots. Live+retired may rise with in-flight work
but returns to a stable bound after fences.

**Implementation outline:** Define manifest; churn/grow/resize; collect live/retired/reclaimed graph;
wait governing fences; verify bound.

**Non-goals:** Heap policy, other cleanup, or tuning.

**Interim tests:** Short dry run, long D3D/GBV, capacity pressure, shutdown.

**Pass completion gate:** No validation/exhaustion; counts show stable reclamation after completion.

**Expected visible effect:** None.

**Expected performance effect:** None intended.

**Regression surface:** Long editor sessions, churn, resize, dual view, overlap.

**Stop and escalate conditions:** Stop if counts grow monotonically, reclaim only at shutdown, or S6
ownership hazard appears.

**Handoff to next pass:** Manifest, graph, peak/baseline, validation, revision/results.

**Suggested model:** `Terra High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-32 completion validation. Prerequisites: S9-P1.
> Implement only **S9-P2**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Execute only **S9-P2** after S9-P1. Run deterministic long descriptor stress with repeated TLAS
> relocation/rebuild, scene churn/growth, resize, dual view, all slots, and capacity pressure under
> D3D/GBV. Record live/retired/reclaimed counts; after fences, use must return to a stable bound and
> never exhaust. Change no heap policy or unrelated lifetime. Record revision, commands, manifest,
> graphs, results. Stop on monotonic growth or S6 hazard. Do not begin S9-P3.

### S9-P3 - Complete and validate narrow GPU markers

**Parent stage:** S9.

**Audit findings:** PT-33.

**Purpose:** Ensure each surviving ReSTIR/RR preparation stage has exact GPU attribution.

**Why this is a separate pass:** Marker validation must not mix renderer cleanup; S0 may already have
completed the code.

**Prerequisites:** S9-P2 and S0-P4.

**Source scope:**

- **Primary implementation:** `DxrDispatchContext` temporal/spatial/boiling/surface-history and any
  surviving RR-preparation marker.
- **Consumers/call sites:** GPU captures and benchmark tooling.
- **Tests/diagnostics:** representative capture/marker-tree inspection.
- **Documentation:** marker owner/name checklist.

**Implementation contract:** Each phase has one correctly named scope enclosing only its commands. If
S0 already satisfies the checklist, this is validation/status only.

**Implementation outline:** Compare S0 checklist/current stages; add only missing scopes; capture;
verify nesting/ownership.

**Non-goals:** Dispatch movement, optimization, broad marker noise, or style naming.

**Interim tests:** Build/shaders; capture tree; diagnostics overhead check.

**Pass completion gate:** One capture proves exact ownership/nesting for every remaining checklist item.

**Expected visible effect:** None.

**Expected performance effect:** None meaningful.

**Regression surface:** Capture tooling and event nesting.

**Stop and escalate conditions:** Stop if scoping requires command ownership change or S0 conflicts.

**Handoff to next pass:** Marker capture/checklist and residual cleanup ledger.

**Suggested model:** `Terra Medium`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-33. Prerequisites: S9-P2 and S0-P4.
> Implement only **S9-P3**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement or validate only **S9-P3** after S0/S9-P2. Ensure separate correctly named scopes for
> ReSTIR temporal, spatial, boiling, surface history, and surviving RR preparation. If S0 already
> satisfies it, make no forced code change. Do not move dispatches, optimize, or style-refactor. Build/
> compile and inspect one capture tree; require exact ownership/nesting. Record revision, files if any,
> commands/results, capture/checklist. Stop if architecture must change. Do not begin S9-P4.

### S9-P4 - Remove only pass-proven stale APIs and permutations

**Parent stage:** S9.

**Audit findings:** Cleanup follow-through for PT-23, PT-24, PT-25, PT-28, PT-29, PT-32, PT-33 and
only additional items explicitly listed by earlier handoffs.

**Purpose:** Remove obsolete fields, getters, resources, and permutations proven stale by completed passes.

**Why this is a separate pass:** Mechanical cleanup follows validated replacements and stays apart
from lifetime/estimator changes.

**Prerequisites:** S9-P3 and completed S1-S8 stale-item ledgers.

**Source scope:**

- **Primary implementation:** only predeclared obsolete PT raster-presence fields, RR prep/history
  APIs, conflated SER status, retired-list leftovers, and proven shader/RTPSO registrations.
- **Consumers/call sites:** compiler/search-verified references and tests.
- **Tests/diagnostics:** source/reflection/permutation search, full build and mode matrix.
- **Documentation:** removed/retained symbol ledger.

**Implementation contract:** Remove only after replacement and source/reflection/runtime/test evidence
prove no consumer. Preserve non-SER fallback and every supported mode. If too large, stop and propose
additional passes.

**Implementation outline:** Import ledger; remove one related cluster at a time; compile/search after
each; run matrix; record retained compatibility.

**Non-goals:** Style refactor, speculative dead code, architecture, or supported fallback/debug removal.

**Interim tests:** Build all targets; core/D3D; source/reflection/permutation searches; mode matrix.

**Pass completion gate:** Every removed item was predeclared; no reference remains; full supported
matrix passes.

**Expected visible effect:** None.

**Expected performance effect:** None required; minor simplification possible.

**Regression surface:** Init/settings, root/RTPSO, fallback hardware, debug, all modes.

**Stop and escalate conditions:** Stop on hidden consumer, required supported permutation, or diff
becoming cross-system/unreviewable.

**Handoff to next pass:** Removed/retained ledger, searches, build/mode results, revision.

**Suggested model:** `Terra High`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: Cleanup follow-through for PT-23, PT-24, PT-25, PT-28, PT-29, PT-32, PT-33 and only additional items explicitly listed by earlier handoffs. Prerequisites: S9-P3 and completed S1-S8 stale-item ledgers.
> Implement only **S9-P4**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Implement only **S9-P4** from completed handoff ledgers; do not search for new cleanup. Remove only
> fields/getters/resources/call paths/shader permutations explicitly proven obsolete by S1-S8.
> Preserve every renderer/reconstruction/reference/viewport/debug mode and non-SER fallback. Build all
> targets, run core/D3D mode tests, and prove no source/reflection/permutation consumer. Stop if a
> hidden consumer appears, supported hardware needs it, or diff ceases to be reviewable; propose
> another pass. Record revision, files, exact results, removed/retained ledger. Do not begin S9-P5.

### S9-P5 - Finalize implementation evidence

**Parent stage:** S9.

**Audit findings:** PT-32/PT-33 plus status for every implemented finding.

**Purpose:** Close the roadmap with revision-linked evidence without rewriting the accepted snapshot.

**Why this is a separate pass:** Status must reflect completed gates rather than accompany code cleanup.

**Prerequisites:** S9-P4 and all accepted handoffs.

**Source scope:**

- **Primary implementation:** audit implementation-status companion or clearly designated status
  appendix; affected roadmaps/manifests.
- **Consumers/call sites:** future prompts and project review.
- **Tests/diagnostics:** artifact-link verification and final recorded mode matrix.
- **Documentation:** revisions, files, commands/results, unsupported scope, evidence categories.

**Implementation contract:** Preserve historical findings. Per pass record revision, changed files,
exact commands/results, CPU, shader, D3D/GPU regression, visual, timing, gate, and unsupported scope
separately. Never complete an unrun visual/performance gate.

**Implementation outline:** Ingest handoffs; verify links; reconcile statuses/conditionals; record final
matrix/limitations; sign artifact index.

**Non-goals:** Renderer changes, new findings, severity changes, or source reinvestigation.

**Interim tests:** Link checks, status-schema consistency, cross-reference/ID validation.

**Pass completion gate:** Every completed pass links reproducible evidence; incomplete/conditional
passes are explicit; final matrix/limitations are complete.

**Expected visible effect:** None.

**Expected performance effect:** None.

**Regression surface:** Documentation consistency only.

**Stop and escalate conditions:** Stop on missing/contradictory evidence or need to infer an unrun gate.

**Handoff to next pass:** None; signed catalog/status/artifact index.

**Suggested model:** `Terra Medium`.

**Paste-ready implementation prompt:**

> Treat `devdoc/dxr/pt/pipeline-audit-2026-07-15.md` at source snapshot `9b65f29127b818abaa142fbc851c157b409df02c` as authoritative. Findings: PT-32/PT-33 plus status for every implemented finding. Prerequisites: S9-P4 and all accepted handoffs.
> Implement only **S9-P5**. Apply this entry's implementation contract and non-goals; run its interim tests and satisfy its completion gate. Record the implementation revision, changed files (or none), exact commands/results, and CPU, shader, D3D12, GPU visual, and GPU timing evidence separately. Stop and report any listed escalation condition; do not begin the next pass.
>
> Execute only **S9-P5** as documentation finalization. Keep §§1-10 historical; update only the
> designated pass-status companion/appendix. For every completed pass record revision, files, exact
> commands/results, CPU tests, shader compilation, D3D/GPU regression, visual evidence, timing,
> gate, and unsupported scope separately. Do not add findings/change severity or infer unrun visual/
> performance success. Verify links and final mode matrix. Stop/report missing or contradictory
> evidence. Make no renderer change.

### 11.13 Compact pass catalog

“GPU visual” means a live image/AOV comparison is part of that pass's gate. “GPU timing” means GPU
timestamp or capture timing is required, not merely useful. “Estimator correctness” is reserved for
passes that change sampling, evaluation, PDF, MIS, target, visibility, or transport expectation;
temporal/resource/diagnostic correctness alone is marked No. Conditional passes remain stable IDs.

| Pass ID | Parent stage | PT findings | Primary contract | Prerequisites | Complexity | Suggested agent tier | GPU visual required | GPU timing required | Estimator correctness change | Resource concurrency change | Next pass ID |
|---|---|---|---|---|---|---|---:|---:|---:|---:|---|
| S0-P1 | S0 | PT-28/29 obs. | Versioned capability/runtime truth | Audit snapshot | Small | Terra High | No | No | No | No | S0-P2 |
| S0-P2 | S0 | PT-10 obs. | Frame/view/token cadence trace | S0-P1 | Small | Terra Medium | No | No | No | No | S0-P3 |
| S0-P3 | S0 | PT-01/11/31 obs. | Reset-owner/reason trace | S0-P2 | Medium | Terra High | No | No | No | No | S0-P4 |
| S0-P4 | S0 | PT-33 | Exact narrow GPU scopes | S0-P3 | Small | Terra Medium | No | No | No | No | S0-P5 |
| S0-P5 | S0 | PT-29; all prerequisite | Deterministic manifest/hash | S0-P1 through S0-P4 | Medium | Terra High | Yes | Yes | No | No | S1-P1 |
| S1-P1 | S1 | PT-01 | Per-view current/previous camera packet | S0-P5 | Large | Sol High | No | No | No | No | S1-P2 |
| S1-P2 | S1 | PT-01 | Camera-domain tests/AOVs | S1-P1 | Medium | Terra High | Yes | No | No | No | S1-P3 |
| S1-P3 | S1 | PT-10 | One token per application frame | S1-P2 | Medium | Sol Medium | Yes | No | No | No | S1-P4 |
| S1-P4 | S1 | PT-11 | Per-view compatibility/reset key | S1-P3 | Large | Sol High | Yes | No | No | No | S1-P5 |
| S1-P5 | S1 | PT-31 | Reject unrepresentable optical history | S1-P1, S1-P4 | Large | Sol High | Yes | No | No | No | S1-P6 |
| S1-P6 | S1 | PT-01/10/11/31 | Integrated temporal lifecycle gate | S1-P1 through S1-P5 | Large | Sol High | Yes | Yes | No | No | S2-P1 |
| S2-P1 | S2 | PT-09 | One authored display exposure | S1-P6 | Medium | Sol Medium | Yes | No | No | No | S2-P2 |
| S2-P2 | S2 | PT-30 | SDK recommendation/extent model | S2-P1 | Medium | Sol High | No | No | No | No | S2-P3 |
| S2-P3 | S2 | PT-30 | Per-tuple jitter period/phase | S2-P2, S1-P4 | Medium | Sol High | Yes | No | No | No | S2-P4 |
| S2-P4 | S2 | PT-30; PT-24 prereq. | Extent/allocation/tag/reset integration | S2-P1 through S2-P3 | Large | Sol High | Yes | No | No | No | S2-P5 |
| S2-P5 | S2 | PT-09/30 | Full reconstruction matrix gate | S2-P1 through S2-P4 | Large | Sol High | Yes | Yes | No | No | S3-P1 |
| S3-P1 | S3 | PT-03 | Distinct world-space Ng/Ns | S2-P5 | Large | Sol High | Yes | No | No | No | S3-P2 |
| S3-P2 | S3 | PT-05 | Shared inverse-transpose normals | S3-P1 | Medium | Sol High | Yes | No | No | No | S3-P3 |
| S3-P3 | S3 | PT-04 | One dielectric event for radiance/guide | S3-P1 and S3-P2 | Large | Sol High | Yes | No | No | No | S3-P4 |
| S3-P4 | S3 | PT-08 | Actual-event or omitted hit distance | S3-P3 | Large | Sol High | Yes | Yes | No | No | S3-P5 |
| S3-P5 | S3 | PT-06 | Hit footprint drives normal-map LOD | S3-P4 | Large | Sol High | Yes | No | No | No | S3-P6 |
| S3-P6 | S3 | PT-06 | Scattering-aware cone propagation | S3-P5 | Research-heavy | Sol High | Yes | Yes | No | No | S3-P7 |
| S3-P7 | S3 | PT-03/04/05/06/08 | Integrated surface/guide review | S3-P1 through S3-P6 | Large | Sol Ultra | Yes | Yes | No | No | S4A-P1 |
| S4A-P1 | S4A | PT-15 | Always sanitize; clamp optional | S3-P7 | Medium | Terra High | No | No | No | No | S4A-P2 |
| S4A-P2 | S4A | PT-02/07/14/15 obs. | Deterministic path trace | S4A-P1, S0-P5 | Large | Sol High | Yes | No | No | No | S4A-P3 |
| S4A-P3 | S4A | PT-02 | Correct discrete mixture weights | S4A-P2 | Medium | Sol Medium | No | No | Yes | No | S4A-P4 |
| S4A-P4 | S4A | PT-07 partial | Rough-dielectric oracle/contract | S4A-P3, S3 | Research-heavy | Sol Ultra | No | No | No | No | S4A-P5 |
| S4A-P5 | S4A | PT-07 partial | Canonical dielectric types/helpers | S4A-P4 | Medium | Sol High | No | No | No | No | S4A-P6 |
| S4A-P6 | S4A | PT-07 partial | Oracle-matched sampling | S4A-P5 | Large | Sol High | No | No | No | No | S4A-P7 |
| S4A-P7 | S4A | PT-07 partial | Matched evaluate/PDF | S4A-P6 | Large | Sol High | No | No | No | No | S4A-P8 |
| S4A-P8 | S4A | PT-07 partial | Activate throughput/MIS contract | S4A-P7 | Large | Sol High | Yes | No | Yes | No | S4A-P9 |
| S4A-P9 | S4A | PT-07 partial | Gap-free delta/continuous boundary | S4A-P8 | Medium | Sol High | Yes | No | Yes | No | S4A-P10 |
| S4A-P10 | S4A | PT-07 | Furnace/statistical closure | S4A-P9 | Large | Sol Ultra | Yes | Yes | No | No | S4A-P11 |
| S4A-P11 | S4A | PT-14 | Non-overlapping environment estimator | S4A-P10 | Large | Sol High | Yes | No | Yes | No | S4A-P12 |
| S4A-P12 | S4A | PT-02/07/14/15 | Core/outlier integrated gate | S4A-P1 through S4A-P11 | Large | Sol Ultra | Yes | Yes | No | No | S4B-P1 |
| S4B-P1 | S4B | PT-17/18 partial | Product optical-scope decision | S4A-P12 | Research-heavy | Sol High | No | No | No | No | S4B-P2 |
| S4B-P2 | S4B | PT-16 | RGB transmissive visibility | S4B-P1 | Medium | Sol Medium | Yes | No | Yes | No | S4B-P3 |
| S4B-P3 | S4B | PT-17 | Declared directional NEE policy | S4B-P2 | Large | Sol High | Yes | No | Yes | No | S4B-P4 |
| S4B-P4 | S4B | PT-18 partial | Typed medium/rejection state | S4B-P3 | Large | Sol High | No | No | No | No | S4B-P5 |
| S4B-P5 | S4B | PT-18 | Convert medium consumers | S4B-P4 | Large | Sol High | Yes | No | Yes | No | S4B-P6 |
| S4B-P6 | S4B | PT-19 | Reciprocal shared opaque model | S4B-P5 | Large | Sol High | Yes | No | Yes | No | S5-P1 |
| S5-P1 | S5 | PT-12 | Canonical unrotated environment UV | S4B-P6, S1/S3/S4A | Medium | Sol High | Yes | No | Yes | No | S4B-P7 |
| S4B-P7 | S4B | PT-20 | Exact within-cell PDF | S5-P1 | Medium | Sol High | No | No | Yes | No | S4B-P8 |
| S4B-P8 | S4B | PT-16/17/18/19/20 | Extended-scope integrated gate | S4B-P1 through S4B-P7 | Large | Sol Ultra | Yes | Yes | No | No | S5-P2 |
| S5-P2 | S5 | PT-13 partial | Canonical emitter contract | S5-P1, S3, S4B-P6 | Large | Sol High | No | No | No | No | S5-P3 |
| S5-P3 | S5 | PT-13 | Emitter producer/consumer/MIS identity | S5-P2 | Large | Sol High | Yes | No | Yes | No | S5-P4 |
| S5-P4 | S5 | PT-21 H1 | Native visibility known-valid | S5-P3 | Medium | Sol High | Yes | No | Yes | No | S5-P5 |
| S5-P5 | S5 | PT-21 H2 | Previous-domain BASIC normalization | S5-P4, S1 | Research-heavy | Sol Ultra | Yes | No | Yes | No | S5-P6 |
| S5-P6 | S5 | PT-21 H2b | Continue compatible reprojection search | S5-P5 | Medium | Sol High | Yes | No | Yes | No | S5-P7 |
| S5-P7 | S5 | PT-21 H3 | One reconnection Jacobian policy | S5-P6 | Research-heavy | Sol Ultra | Yes | No | Yes | No | S5-P8 |
| S5-P8 | S5 | PT-21 H4 | Explicit visibility/bias policy | S5-P7 | Research-heavy | Sol Ultra | Yes | Yes | Yes | No | S5-P9 |
| S5-P9 | S5 | PT-12/13/21 | P5/P6/P7 quality/timing gate | S5-P1 through S5-P8; S4B-P8 | Large | Sol Ultra | Yes | Yes | No | No | S6-P1 |
| S6-P1 | S6 | PT-22 partial | Complete ownership/wait inventory | S0-P4 and S0-P5; S5-P9 | Large | Sol Ultra | No | Yes | No | No | S6-P2 |
| S6-P2 | S6 | PT-22 partial | Frame-slot allocator fences | S6-P1 | Large | Sol High | No | No | No | Yes | S6-P3 |
| S6-P3 | S6 | PT-22 partial | Upload-range retirement fences | S6-P2 | Large | Sol High | No | No | No | Yes | S6-P4 |
| S6-P4 | S6 | PT-22 partial | Immutable descriptor generations | S6-P3 | Large | Sol High | No | No | No | Yes | S6-P5 |
| S6-P5 | S6 | PT-22 partial | AS build/use lifetimes | S6-P4 | Large | Sol High | No | No | No | Yes | S6-P6 |
| S6-P6 | S6 | PT-22 | Remove unrelated global wait | S6-P1 through S6-P5 | Large | Sol Ultra | Yes | Yes | No | Yes | S6-P7 |
| S6-P7 | S6 | PT-22 | Long overlap proof | S6-P6 | Large | Sol Ultra | Yes | Yes | No | No | S6-P8 |
| S6-P8 | S6 | PT-23 partial | Dedicated full-PT input/root | S6-P7, S1/S2 | Large | Sol High | Yes | No | No | No | S6-P9 |
| S6-P9 | S6 | PT-23 | Skip raster/CSM only in full PT | S6-P8 | Medium | Sol High | Yes | Yes | No | No | S6-P10 |
| S6-P10 | S6 | PT-22/23 | Cross-system S6 acceptance | S6-P9 | Large | Sol Ultra | Yes | Yes | No | No | S7-P1 |
| S7-P1 | S7 | PT-24 | Direct tag PT; remove five prep passes | S6-P10, S2/S3 | Large | Sol High | Yes | Yes | No | No | S7-P2 |
| S7-P2 | S7 | PT-25 | Remove zero-consumer histories | S7-P1 | Small | Terra High | Yes | Yes | No | No | S7-P3 |
| S7-P3 | S7 | PT-26 | Generation-based emissive rebuild | S7-P2, S5-P3, S6 | Large | Sol High | Yes | Yes | No | Yes | S7-P4 |
| S7-P4 | S7 | PT-27 | Exact cached triangle LOD invariant | S7-P3, S3 | Large | Sol High | Yes | Yes | No | No | S8-P1 |
| S8-P1 | S8 | PT-28 | Correct RT/SM/runtime decision | S7-P4, S0 | Medium | Terra High | No | No | No | No | S8-P2 |
| S8-P2 | S8 | PT-29 partial | Truthful runtime status | S8-P1 | Medium | Terra High | No | No | No | No | S8-P3 |
| S8-P3 | S8 | PT-29 prep. | Reproducible benchmark runner | S8-P2, S0-P5 | Medium | Terra High | No | No | No | No | S8-P4 |
| S8-P4 | S8 | PT-29 | Base force-off/on evidence | S8-P3, S4A | Research-heavy | Sol High | Yes | Yes | No | No | S8-P5 if delta; else S8-P6 |
| S8-P5 | S8 | PT-29 cond. | Counter explanation of delta | S8-P4 delta | Research-heavy | Sol High | No | Yes | No | No | S8-P6 |
| S8-P6 | S8 | PT-29 cond. | Isolated hint experiments | S8-P4; P5 if needed | Research-heavy | Sol High | Yes | Yes | No | No | S8-P7 |
| S8-P7 | S8 | PT-29 | Evidence-backed SER policy | S8-P4 through S8-P6 as applicable | Medium | Sol High | Yes | Yes | No | No | S9-P1 |
| S9-P1 | S9 | PT-32 | Fence-deferred TLAS SRV free | S8-P7, S6 | Large | Sol High | No | No | No | Yes | S9-P2 |
| S9-P2 | S9 | PT-32 | Bounded descriptor stress | S9-P1 | Medium | Terra High | No | No | No | No | S9-P3 |
| S9-P3 | S9 | PT-33 | Final marker ownership/nesting | S9-P2, S0-P4 | Small | Terra Medium | No | No | No | No | S9-P4 |
| S9-P4 | S9 | PT-23/24/25/28/29/32/33 | Remove only proven stale APIs/permutations | S9-P3, handoff ledgers | Medium | Terra High | Yes | No | No | No | S9-P5 |
| S9-P5 | S9 | PT-32/33; all status | Revision-linked final evidence | S9-P4, all handoffs | Small | Terra Medium | No | No | No | No | End |

## Primary contracts and technical references

- [DirectX Raytracing specification, current living specification](https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html): AS/SBT requirements, Shader Model 6.9 SER support, and Options22 reporting.
- [HLSL Shader Execution Reordering proposal](https://microsoft.github.io/hlsl-specs/proposals/0027-shader-execution-reordering/): `HitObject` and reordering semantics.
- [NVIDIA SER whitepaper](https://developer.nvidia.com/sites/default/files/akamai/gameworks/ser-whitepaper.pdf): intended workload/divergence behavior and measurement context.
- [Streamline Programming Guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuide.md): application frame tokens and multiple-viewport integration.
- [Streamline DLSS guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS.md) and [NVIDIA DLSS Programming Guide](https://github.com/NVIDIA/DLSS/blob/main/doc/DLSS_Programming_Guide_Release.pdf): optimal settings, jitter phases, exposure, reset, and resource contracts.
- [Streamline DLSS Ray Reconstruction guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_RR.md): RR inputs, depth formats, hit distance, motion, and dynamic-resolution restrictions.
- [NVIDIA RR sample](https://github.com/nvpro-samples/vk_denoise_dlssrr): first-secondary-event hit distance and specular-motion integration example.
- [Microsoft fence-based resource management](https://learn.microsoft.com/en-us/windows/win32/direct3d12/fence-based-resource-management): frame-resource reuse and fence ownership.
- [NVIDIA: solving self-intersection artifacts in DXR](https://developer.nvidia.com/blog/solving-self-intersection-artifacts-in-directx-raytracing/): inverse-transpose normal handling and robust ray offsets.
- [Walter et al., Microfacet Models for Refraction through Rough Surfaces](https://www.graphics.cornell.edu/~bjw/microfacetbsdf.pdf), [PBRT v4 dielectric BSDF](https://pbr-book.org/4ed/Reflection_Models/Dielectric_BSDF), and [Heitz, Sampling the GGX Distribution of Visible Normals](https://www.jcgt.org/published/0007/04/01/paper.pdf): rough dielectric sampling/evaluation/PDF contracts.
- [Veach and Guibas, Optimally Combining Sampling Techniques](https://www.cs.jhu.edu/~misha/ReadingSeminar/Papers/Veach95.pdf) and [Veach, non-symmetric scattering](https://graphics.stanford.edu/papers/non-symmetric/): MIS and reciprocity foundations.
- [NVIDIA RTXDI](https://github.com/NVIDIA-RTX/RTXDI): reference ReSTIR implementation and documentation; repository-specific deviations and experiments are tracked in [P7 diagnostics](p7-diagnostics-tracker.md).

These sources define contracts and expected algorithms; they do not substitute for the project-specific
completion gates above.

## Audit verification evidence

The audit made no renderer/shader implementation edits. At the audited revision:

- **T - build passed:**
  `cmake --build build --config Debug --target engine-tests descriptor-heap-tests d3d12-render-tests -- /m`.
- **T - core tests passed:**
  `ctest --test-dir build -C Debug --output-on-failure -R "^(engine-tests|descriptor-heap-tests)$"`
  reported 2/2 passing (`engine-tests` 14.57 s, `descriptor-heap-tests` 0.02 s).
- **T - D3D12 suite reached 29/30 passing:**
  `build\Debug\d3d12-render-tests.exe --all` printed one unrelated headless UI failure,
  `ImGuiMouseTracksGlfwCursor`, while both PT transmission tests passed. The wrapper timed out after
  300.7 s because the executable did not tear down after printing results; this is not a clean suite
  pass and should remain visible.
- **T - isolated PT transmission tier printed 2/2 passing:**
  `build\Debug\d3d12-render-tests.exe --tier=5` printed
  `PtTransmissionGuideAlbedoBands` and `PtTransmissionVirtualMotionOnOrbit` as passing, then likewise
  failed to terminate before the 180.6 s wrapper timeout. Treat the assertions as passed and teardown
  as unresolved.
- The Debug build/test path enabled the D3D12 debug layer and compiled the relevant shaders without a
  reported shader compilation failure. This does not replace GPU-based validation or a live capture.

Generated build/test outputs are outside this source-controlled audit artifact. The only intentional
repository change for this audit is this Markdown file.
