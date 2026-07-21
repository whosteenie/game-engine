# DirectX 12 Agility SDK (PF5)

The selected runtime is **Microsoft.Direct3D.D3D12 1.619.4**, with exported
`D3D12SDKVersion` **619**. Release/CI provisioning must extract that exact NuGet package to:

```text
vendor/agility-sdk/1.619.4/
```

so this file exists:

```text
vendor/agility-sdk/1.619.4/build/native/bin/x64/D3D12Core.dll
```

CMake detects this layout and enables `GAME_ENGINE_USE_AGILITY_SDK` by default. It copies the
runtime to `D3D12/` beside `game-engine.exe` and exports the matching loader symbols from the
executable. If the package is absent, the option stays off and the legacy inbox-D3D12 path remains
fully functional.
