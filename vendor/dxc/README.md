# DXC runtime (local)

PF5 pins DXC **1.8.2505.28** (`dxcompiler.dll` / `dxil.dll`) for builds and local testing. Place
the matching **x64** runtime DLLs here:

```
vendor/dxc/x64/dxcompiler.dll
vendor/dxc/x64/dxil.dll
```

CMake copies these next to `game-engine.exe` and `d3d12-render-tests.exe` on POST_BUILD when present.
The build currently accepts the Windows SDK `Redist/D3D/x64` copies as a development fallback, but
release/CI should provision this directory from the pinned compiler package so shader output does
not vary with the installed SDK.

Typical source (Windows SDK):

`C:\Program Files (x86)\Windows Kits\10\Redist\D3D\x64\`

DLLs are gitignored by default (`*.dll`); keep them local or commit intentionally if your team prefers vendoring.
