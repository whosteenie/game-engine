# S1-P1 implementation evidence

Authoritative audit: [`pipeline-audit-2026-07-15.md`](../../pipeline-audit-2026-07-15.md), source snapshot `9b65f29127b818abaa142fbc851c157b409df02c`. This record covers only S1-P1; S1-P2 was not started.

## Revision and changed files

- Implementation worktree base revision: `2a08a63f9b283c5fd1dba613a0c297600ab88390`.
- No implementation commit was created.
- Changed files:
  - `CMakeLists.txt`
  - `src/app/scene/SceneRenderer.cpp`
  - `src/engine/raytracing/DxrPathTracerDispatch.cpp`
  - `src/engine/raytracing/DxrPathTracerDispatch.h`
  - `src/engine/raytracing/DxrRootSignature.h`
  - `src/engine/rendering/MotionVectorFrameState.h`
  - `src/engine/rendering/ScreenSpaceEffects.cpp`
  - `src/engine/rendering/TemporalCameraPacket.h`
  - `tests/engine_tests_main.cpp`
  - `tests/temporal_camera_packet_test.cpp`
  - this evidence record

## Packet and coordinate contract

`ScreenSpaceEffects` is already instantiated separately for Scene and Game viewports. Its motion state now owns the previous complete camera state, and `SceneRenderer` creates one current/previous packet from the active viewport owner for each PT dispatch. Previous means the previous compatible rendered frame for that viewport.

The packet uses GLM column-major matrices, the renderer's left-handed zero-to-one clip space, world-space camera position, and NDC jitter. Its projection and inverse view-projection are unjittered; jitter is stored separately as the offset applied to `projection[2].xy`. Real-time PT primaries derive a jittered projection and inverse from those members. Motion output remains unjittered `currentNDC - previousNDC`. Transmission replay intentionally combines the previous view with the current effective projection so static-camera jitter cancels, preserving the established shader convention.

Retained raster motion matrices are asserted against the packet in `SceneRenderer`; the retained `Camera` dispatch parameter is asserted against the current packet in PT and ReSTIR. An incomplete current state skips PT and invalidates ReSTIR history. An incomplete or disagreeing previous state sets both PT and ReSTIR history validity false. Current-camera values are packed as non-consumable safety values in that invalid case, so identity/zero is never substituted as valid previous history.

`ReflectionDispatchConstants` remains 704 bytes with the established shader offsets: unjittered view-projection 448, previous view-projection 512, previous replay inverse view-projection 592, and previous camera position 656. Compile-time assertions now guard these offsets.

## Commands and results

- `cmake --build build --config Debug --target engine-tests -- /m` — first attempt compiled the new test but failed to link `TryBuildCameraConstants` because `engine-tests` intentionally links `engine-render`, not `engine-dxr`. The pure CPU packing helper was then made inline; no library dependency was added.
- `cmake --build build --config Debug --target engine-tests -- /m` — passed after that correction.
- `build\Debug\engine-tests.exe --filter=temporal_camera_packet` — passed 1/1; covers complete packet packing, explicit jitter conventions, previous replay construction, byte copies, incomplete inverse/jitter invalidation, current-state safe packing, and incomplete-current rejection.
- `cmake --build build --config Debug --target game-engine engine-tests descriptor-heap-tests d3d12-render-tests -- /m` — passed. Rebuilt and linked the application, render/DXR libraries, CPU tests, descriptor tests, and D3D12 render tests; build-time NRD DXIL compilation reported 159/159 tasks successful.
- `build\Debug\engine-tests.exe --filter=shader_compile` — passed 1/1, including the PT/ReSTIR shader set.
- `ctest --test-dir build -C Debug --output-on-failure -R "^(engine-tests|descriptor-heap-tests)$"` — passed 2/2: `engine-tests` 14.39 s and `descriptor-heap-tests` 0.02 s.
- `build\Debug\d3d12-render-tests.exe --tier=1` — printed 9/9 passing D3D12 smoke tests, then did not tear down before the 120.6-second wrapper timeout (exit 124). This is assertion-pass evidence, not a clean process/suite pass.
- `Select-String -Path build/CMakeCache.txt -Pattern '^GAME_ENGINE_D3D12_DEBUG_LAYER:'` — reported `GAME_ENGINE_D3D12_DEBUG_LAYER:BOOL=ON`.
- `rg -n "prevViewProjection\{1\.0f\}|prevView\{1\.0f\}|prevCameraPos\{0\.0f\}|motionHistoryValid|constants\.historyValid\s*=\s*1u|m_lastPrevCameraPos" src/app/scene src/engine/raytracing -g '*.h' -g '*.cpp'` — no matches. The old valid-history identity/zero fields and unconditional ReSTIR history-valid assignment are absent from the audited PT path.
- `git -c safe.directory=C:/Users/justi/Documents/GitHub/game-engine diff --check` — passed; only Git's existing LF-to-CRLF working-copy warnings were printed.

## Evidence status

- CPU: focused packet/packing/invalidity test passed 1/1; complete core/descriptor tests passed 2/2.
- Shader: application build compiled NRD DXIL successfully (159/159); focused shader compilation test passed 1/1 for the PT/ReSTIR shader set. Shader source and cbuffer fields were not changed.
- D3D12: Debug layer is enabled. Tier 1 printed 9/9 passing with no reported debug-layer error, but teardown timed out, so this is not recorded as a clean executable pass.
- GPU visual: not run and not required by the S1-P1 gate. No symptom-removal claim is made; that belongs to S1-P2/S1-P6.
- GPU timing: not run and not required by the S1-P1 gate. Expected performance effect remains neutral.

## Gate status

S1-P1's implementation gate is satisfied for the source/CPU/shader contract: every PT temporal dispatch receives a complete packet from the active viewport owner, incomplete previous data invalidates PT/ReSTIR history, CPU packing and declared GPU offsets agree, and the former valid identity/zero defaults are gone. D3D12 tier-1 assertions passed with the known unresolved teardown timeout. No ownership, convention, audited-data-flow, or GPU-layout conflict was encountered. S1-P2 was not begun.
