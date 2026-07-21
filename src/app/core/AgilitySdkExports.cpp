// The inbox D3D12 loader reads these exports from the main executable before device creation.
// They must match the D3D12Core.dll copied by cmake/AgilitySdk.cmake exactly.
#if defined(_WIN32) && defined(GAME_ENGINE_AGILITY_SDK_VERSION)
#include <windows.h>

extern "C"
{
__declspec(dllexport) extern const UINT D3D12SDKVersion = GAME_ENGINE_AGILITY_SDK_VERSION;
__declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}
#endif
