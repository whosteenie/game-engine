# DXC runtime (local)

PF7 pins DXC **1.9.2602.24** (`dxcompiler.dll` / `dxil.dll`) for builds and local testing. This is
the retail compiler release that supports Shader Model 6.9 and native DXR 1.2 SER. Place
the matching **x64** runtime DLLs here:

```
vendor/dxc/x64/dxcompiler.dll
vendor/dxc/x64/dxil.dll
```

CMake copies these next to `game-engine.exe` and `d3d12-render-tests.exe` on POST_BUILD when present.
The build currently accepts the Windows SDK `Redist/D3D/x64` copies as a development fallback, but
release/CI should provision this directory from the pinned compiler package so shader output does
not vary with the installed SDK.

Typical source: the official [DirectXShaderCompiler v1.9.2602.24 release](https://github.com/microsoft/DirectXShaderCompiler/releases/tag/v1.9.2602.24).

DLLs are gitignored by default (`*.dll`); keep them local or commit intentionally if your team prefers vendoring.
