# game-engine

A Windows D3D12 scene editor built around a deferred PBR renderer. You import glTF models, place lights and objects in a docked ImGui UI, tune the render pipeline from the Renderer Tuning panel, and preview in Scene View or Game View. Play mode runs the scene with Jolt physics.

Most of the codebase is the renderer and the editor hooks that drive it.

## Build

Requires Windows 10/11, Visual Studio 2022, CMake 3.20+, and the Windows SDK (D3D12 and DXC).

```powershell
cmake -S . -B build
cmake --build build --config Debug --target game-engine
.\build\Debug\game-engine.exe
```

Optional: copy `dxcompiler.dll` and `dxil.dll` into `vendor/dxc/x64/` before building (see [vendor/dxc/README.md](vendor/dxc/README.md)).

Shaders are HLSL compiled at runtime with DXC (Shader Model 6.0). Albedo textures use `DXGI_FORMAT_R8G8B8A8_UNORM_SRGB`; linear maps (normal, roughness, AO) use `DXGI_FORMAT_R8G8B8A8_UNORM`.

## Renderer

### Core lighting

- Split direct/indirect deferred PBR with a multi-render-target G-buffer
- Image-based lighting from HDR environment maps (skybox or solid background, diffuse + specular)
- Directional sun with shadow maps (PCF and PCSS filtering options)
- HDR rendering with selectable tonemap (gamma, Reinhard, ACES)
- Bloom

### Screen space

- Ambient occlusion: SSAO or GTAO (edge-aware denoise on GTAO)
- Screen-space reflections (SSR) with trace quality scaling and SVGF spatial/temporal denoise
- Screen-space global illumination (SSGI) with noise injection, spatial blur, and temporal accumulation
- Anti-aliasing: none, TAA, FXAA, SMAA, SSAA, MSAA
- Motion vectors for temporal passes

### Ray tracing (DXR 1.1, optional hardware)

Hybrid mode keeps the raster base and adds RT passes on top:

- Specular reflections (stochastic trace, NRD RELAX specular denoise)
- Soft directional sun shadows (NRD SIGMA shadow denoise)
- One-bounce diffuse GI (NRD RELAX diffuse denoise; mutually exclusive with SSGI inject)

Path traced mode replaces the hybrid stack with a unified path tracer:

- Real-time (single frame, pairs with upscaling/denoise below)
- Reference (progressive accumulation)

### NVIDIA upscaling (Streamline, optional)

- DLSS Super Resolution and DLAA (native-res DLSS as pure AA)
- DLSS Ray Reconstruction (neural denoise + upscale over the RT/HDR signal when enabled)

DLSS features compile out or no-op on non-NVIDIA hardware.

### Debug

- Large set of G-buffer, lighting, AO, SSR, SSGI, and RT debug views
- GPU pass timings in the Performance panel
- Render diagnostics export from the Renderer Tuning panel

## Editor

- Docked panels: Scene View, Game View, Hierarchy, Inspector, Renderer Tuning, Project Files, Performance
- glTF mesh and material import
- Transform gizmos, multi-select, undo/redo
- Scene projects with serialized renderer settings
- Play mode with physics simulation
