#pragma once

#include <cstdint>
#include <string>

// Opt-in render/import/HDR diagnostics. Enable with environment variable:
//   GAME_ENGINE_RENDER_DEBUG=1
// Logs are event-driven (HDR toggle, model import, texture failures) — not per-frame.

namespace RenderPathDiagnostics
{
    bool IsEnabled();

    void Log(const char* category, const std::string& message);

    void LogHdrToggled(bool enabled);

    // Called once on the first post-process Apply after an HDR toggle.
    void LogHdrApplySnapshot(
        int sceneWidth,
        int sceneHeight,
        int viewportWidth,
        int viewportHeight,
        bool sceneFramebufferValid,
        bool splitLighting,
        bool runSsao,
        bool useShadowFactorComposite,
        bool outputTargetBound,
        std::uintptr_t hdrColorSrvCpuHandle,
        std::uintptr_t sceneDirectSrvCpuHandle,
        std::uintptr_t sceneIndirectSrvCpuHandle,
        std::uintptr_t sceneDepthSrvCpuHandle,
        std::uint32_t srvDescriptorsUsed,
        std::uint32_t srvDescriptorsCapacity);

    void LogImportBegin(const std::string& path, int parentIndex);

    void LogImportTextureFailure(int imageIndex, const std::string& detail);

    void LogImportComplete(
        const std::string& path,
        int nodesImported,
        int meshNodes,
        int texturesCached,
        int textureFailures,
        float floorOffsetY,
        std::uint32_t srvDescriptorsUsed,
        std::uint32_t srvDescriptorsCapacity,
        const std::string& existingObjectSnapshot);
}
