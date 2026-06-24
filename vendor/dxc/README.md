# DXC runtime (local)

Place the **x64** shader compiler runtime DLLs here for builds and local testing:

```
vendor/dxc/x64/dxcompiler.dll
vendor/dxc/x64/dxil.dll
```

CMake copies these next to `game-engine.exe` and `d3d12-render-tests.exe` on POST_BUILD when present.
If this folder is empty, the build falls back to the Windows SDK `Redist/D3D/x64` copies.

Typical source (Windows SDK):

`C:\Program Files (x86)\Windows Kits\10\Redist\D3D\x64\`

DLLs are gitignored by default (`*.dll`); keep them local or commit intentionally if your team prefers vendoring.
