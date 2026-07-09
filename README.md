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

### D3D12 debug layer (optional)

The D3D12 debug layer is **off by default**. It only activates in **Debug** builds when enabled at configure time:

```powershell
# Enable (reconfigure, then rebuild Debug)
cmake -S . -B build -DGAME_ENGINE_D3D12_DEBUG_LAYER=ON
cmake --build build --config Debug --target game-engine

# Disable again
cmake -S . -B build -DGAME_ENGINE_D3D12_DEBUG_LAYER=OFF
cmake --build build --config Debug --target game-engine
```

Validation messages and GPU-based validation slow things down noticeably; use this when debugging D3D12 lifetime or barrier issues. Release builds ignore the flag.

## Tests

### Interactive runner (recommended)

```powershell
.\scripts\run-tests.ps1

# Non-interactive (useful for scripts / quick runs):
.\scripts\run-tests.ps1 -Run Cpu
.\scripts\run-tests.ps1 -Run Gpu -Tier 1
.\scripts\run-tests.ps1 -Run List
```

Menu-driven runner with green `[PASS]` / red `[FAIL]` output and a final `x/y tests passed in …` summary. Supports all CPU tests, GPU tiers, individual test selection, and listing what's available.

### CPU tests (fast — run these routinely)

Registered with CTest; no GPU window required.

```powershell
cmake --build build --config Debug --target engine-tests descriptor-heap-tests

# Run directly (stdout shows pass/fail lines)
.\build\Debug\engine-tests.exe
.\build\Debug\engine-tests.exe --list
.\build\Debug\engine-tests.exe --filter=material
.\build\Debug\descriptor-heap-tests.exe

# Or via CTest from the build directory (CPU only by default)
cd build
ctest -C Debug -L cpu --output-on-failure
```

`engine-tests` covers shadow math, lighting probes, materials, guide encoding, shader compile smoke, and related CPU-side checks. `descriptor-heap-tests` covers the fixed descriptor heap allocator.

### GPU render tests (opt-in)

Requires a real D3D12 device and RTX for tier 4 (DXR). **Not** part of default `ctest` — use the `gpu` label explicitly.

```powershell
cmake -S . -B build -DGAME_ENGINE_BUILD_D3D12_RENDER_TESTS=ON
cmake --build build --config Debug --target d3d12-render-tests

# Tier 1 smoke (default) — shared D3D12 session, 32×32 offscreen for boolean draw tests
.\build\Debug\d3d12-render-tests.exe
.\build\Debug\d3d12-render-tests.exe --tier=1
.\build\Debug\d3d12-render-tests.exe --filter=TestPbr*
.\build\Debug\d3d12-render-tests.exe --list
.\build\Debug\d3d12-render-tests.exe --all          # every tier (slow)

# Via CTest (from build directory; never runs unless you pass -L gpu)
cd build
ctest -C Debug -L gpu-smoke --output-on-failure
ctest -C Debug -L gpu --output-on-failure
```

**Tiers:** `1` gpu-smoke (clear/draw/upload), `2` gpu-pbr (PBR/IBL/shadows), `3` gpu-editor (present/ImGui), `4` gpu-dxr (BLAS/TLAS/dispatch). `--tier=N` runs tiers 1 through N.

On success you should see `[PASS]` lines per test and `N/N tests passed.` with exit code `0`. Failures print `[FAIL]` and assertion details to stderr. DXR tests print `SKIP: ... (no RTX tier)` and exit `0` when hardware lacks ray tracing.

See [devdoc/testing/renderer-tests-plan.md](devdoc/testing/renderer-tests-plan.md) for the full inventory and roadmap.

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
