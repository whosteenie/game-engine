# game-engine

D3D12 scene editor with PBR rendering, glTF import, and ImGui tooling.

## Build

Requires **Windows 10/11**, **Visual Studio 2022**, **CMake 3.20+**, and the **Windows SDK** (provides D3D12 and **DXC** — `dxcompiler.dll`).

```powershell
cmake -S . -B build
cmake --build build --config Debug --target game-engine
.\build\Debug\game-engine.exe
```

Optional: copy `dxcompiler.dll` and `dxil.dll` into `vendor/dxc/x64/` before building (see [vendor/dxc/README.md](vendor/dxc/README.md)).

### Toolchain

| Component | Choice |
| --------- | ------ |
| **Shaders** | HLSL compiled at runtime via **DXC** (SM 6.0); `dxcompiler.dll` + `dxil.dll` from `vendor/dxc/x64/` or Windows SDK redist |
| **Albedo textures** | `DXGI_FORMAT_R8G8B8A8_UNORM_SRGB` — GPU sRGB→linear decode on sample (no `pow` in PBR shader) |
| **Linear textures** | `DXGI_FORMAT_R8G8B8A8_UNORM` — normals, roughness, AO |

## Documentation

- [D3D12 cleanup plan](devdoc/d3d12-cleanup.md) — phased roadmap to a single-convention, OpenGL-free renderer.
- [SSGI groundwork](devdoc/ssgi-groundwork.md) — prerequisites and phased plan before production screen-space GI.
- [SSGI quality tracker](devdoc/ssgi-quality-tracker.md) — open quality bugs, tuning notes, and fix order.
- [SSR groundwork](devdoc/ssr-groundwork.md) — phased plan for screen-space reflections (specular IBL replacement).