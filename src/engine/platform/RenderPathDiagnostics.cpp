#include "engine/platform/RenderPathDiagnostics.h"

#include "engine/rendering/RenderDebug.h"
#include "engine/rendering/SsaoDiagnostics.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
    bool g_enabled = false;
    bool g_enabledInitialized = false;

    bool QueryEnabled()
    {
        if (!g_enabledInitialized)
        {
            g_enabledInitialized = true;
            const char* value = std::getenv("GAME_ENGINE_RENDER_DEBUG");
            g_enabled = value != nullptr && value[0] != '\0' && value[0] != '0';
        }

        return g_enabled;
    }

    void WriteLine(const char* category, const std::string& message)
    {
        std::fprintf(stderr, "[render:%s] %s\n", category, message.c_str());
        std::fflush(stderr);
    }
}

namespace RenderPathDiagnostics
{
    bool IsEnabled()
    {
        return QueryEnabled();
    }

    void Log(const char* category, const std::string& message)
    {
        if (!QueryEnabled() || category == nullptr)
        {
            return;
        }

        WriteLine(category, message);
    }

    void LogHdrToggled(const bool enabled)
    {
        if (!QueryEnabled())
        {
            return;
        }

        WriteLine("hdr", std::string("post-processing ") + (enabled ? "enabled" : "disabled"));
    }

    void LogSsaoToggled(const bool enabled)
    {
        if (!QueryEnabled())
        {
            return;
        }

        WriteLine("ssao", std::string("SSAO ") + (enabled ? "enabled" : "disabled"));
    }

    void LogSsaoApplySnapshot(const SsaoDiagnosticsSnapshot& snapshot)
    {
        if (!QueryEnabled())
        {
            return;
        }

        char buffer[1536];
        std::snprintf(
            buffer,
            sizeof(buffer),
            "apply frame=%llu enabled=%d pass=%d composite=%d compositeSsao=%d shadowComposite=%d split=%d geomNormals=%d "
            "pbrDebug=%d debugMode=%s hdrSrc=%s debugSrc=%s scene=%dx%d "
            "srv depth=0x%zx normal=0x%zx noise=0x%zx ssaoRaw=0x%zx ssaoBlur=0x%zx hdr=0x%zx shadow=0x%zx "
            "uniforms samples=%d kernelSize=%d kernelCount=%d kernel0=(%.4f,%.4f,%.4f) radius=%.3f bias=%.4f ao=%.2f power=%.2f "
            "gpuReadback=%d center hwDepth=%.4f normal=(%.3f,%.3f,%.3f) ssaoRaw=%.4f ssaoBlur=%.4f",
            static_cast<unsigned long long>(snapshot.captureFrame),
            snapshot.enabled ? 1 : 0,
            snapshot.passExecuted ? 1 : 0,
            snapshot.compositeRan ? 1 : 0,
            snapshot.compositeUsesSsao ? 1 : 0,
            snapshot.shadowComposite ? 1 : 0,
            snapshot.splitLighting ? 1 : 0,
            snapshot.geometryNormals ? 1 : 0,
            snapshot.pbrDebugActive ? 1 : 0,
            RenderDebugModeLabel(snapshot.debugMode),
            snapshot.hdrColorSource != nullptr ? snapshot.hdrColorSource : "null",
            snapshot.ssaoDebugViewSource != nullptr ? snapshot.ssaoDebugViewSource : "null",
            snapshot.sceneWidth,
            snapshot.sceneHeight,
            snapshot.depthSrv,
            snapshot.normalSrv,
            snapshot.noiseSrv,
            snapshot.ssaoRawSrv,
            snapshot.ssaoBlurSrv,
            snapshot.hdrColorSrv,
            snapshot.shadowFactorSrv,
            snapshot.hasUniformSamples ? 1 : 0,
            snapshot.hasUniformKernelSize ? 1 : 0,
            snapshot.kernelCount,
            snapshot.kernelSample0X,
            snapshot.kernelSample0Y,
            snapshot.kernelSample0Z,
            snapshot.radius,
            snapshot.bias,
            snapshot.aoStrength,
            snapshot.ssaoPower,
            snapshot.gpuReadbackValid ? 1 : 0,
            snapshot.centerHardwareDepth,
            snapshot.centerNormalR,
            snapshot.centerNormalG,
            snapshot.centerNormalB,
            snapshot.centerSsaoRaw,
            snapshot.centerSsaoBlur);
        WriteLine("ssao", buffer);
    }

    void LogHdrApplySnapshot(
        const int sceneWidth,
        const int sceneHeight,
        const int viewportWidth,
        const int viewportHeight,
        const bool sceneFramebufferValid,
        const bool splitLighting,
        const bool runSsao,
        const bool useShadowFactorComposite,
        const bool outputTargetBound,
        const std::uintptr_t hdrColorSrvCpuHandle,
        const std::uintptr_t sceneDirectSrvCpuHandle,
        const std::uintptr_t sceneIndirectSrvCpuHandle,
        const std::uintptr_t sceneDepthSrvCpuHandle,
        const std::uint32_t srvDescriptorsUsed,
        const std::uint32_t srvDescriptorsCapacity)
    {
        if (!QueryEnabled())
        {
            return;
        }

        char buffer[1024];
        std::snprintf(
            buffer,
            sizeof(buffer),
            "first Apply after HDR toggle: scene=%dx%d viewport=%dx%d "
            "sceneFbValid=%d splitLighting=%d ssao=%d shadowComposite=%d outputBound=%d "
            "srvCpu hdrColor=0x%zx direct=0x%zx indirect=0x%zx depth=0x%zx "
            "srvHeap=%u/%u",
            sceneWidth,
            sceneHeight,
            viewportWidth,
            viewportHeight,
            sceneFramebufferValid ? 1 : 0,
            splitLighting ? 1 : 0,
            runSsao ? 1 : 0,
            useShadowFactorComposite ? 1 : 0,
            outputTargetBound ? 1 : 0,
            hdrColorSrvCpuHandle,
            sceneDirectSrvCpuHandle,
            sceneIndirectSrvCpuHandle,
            sceneDepthSrvCpuHandle,
            srvDescriptorsUsed,
            srvDescriptorsCapacity);
        WriteLine("hdr", buffer);
    }

    void LogImportBegin(const std::string& path, const int parentIndex)
    {
        if (!QueryEnabled())
        {
            return;
        }

        WriteLine(
            "import",
            "begin path=\"" + path + "\" parentIndex=" + std::to_string(parentIndex));
    }

    void LogImportTextureFailure(const int imageIndex, const std::string& detail)
    {
        if (!QueryEnabled())
        {
            return;
        }

        WriteLine(
            "import",
            "texture failure imageIndex=" + std::to_string(imageIndex) + " " + detail);
    }

    void LogImportComplete(
        const std::string& path,
        const int nodesImported,
        const int meshNodes,
        const int texturesCached,
        const int textureFailures,
        const float floorOffsetY,
        const std::uint32_t srvDescriptorsUsed,
        const std::uint32_t srvDescriptorsCapacity,
        const std::string& existingObjectSnapshot)
    {
        if (!QueryEnabled())
        {
            return;
        }

        WriteLine(
            "import",
            "complete path=\"" + path + "\" nodes=" + std::to_string(nodesImported) +
                " meshNodes=" + std::to_string(meshNodes) +
                " texturesCached=" + std::to_string(texturesCached) +
                " textureFailures=" + std::to_string(textureFailures) +
                " floorOffsetY=" + std::to_string(floorOffsetY) +
                " srvHeap=" + std::to_string(srvDescriptorsUsed) + "/" +
                std::to_string(srvDescriptorsCapacity));

        if (!existingObjectSnapshot.empty())
        {
            WriteLine("import", existingObjectSnapshot);
        }
    }
}
