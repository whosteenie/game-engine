#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

#include "d3d12_test_harness.h"
#include "test_expect.h"
#include "d3d12_test_runner.h"
#include "pt_test_harness.h"

// MANUAL GPU TEST SUITE — DO NOT AUTO-RUN
// ---------------------------------------
// This executable spins up a real D3D12 device + GLFW window and runs many slow
// GPU integration tests. It is NOT part of CTest and is OFF by default in CMake.
//
// Agents / CI: never build or run this unless the user explicitly asks.
// Enable build: cmake -B build -DGAME_ENGINE_BUILD_D3D12_RENDER_TESTS=ON
// Run manually:  .\build\Debug\d3d12-render-tests.exe

#include "engine/camera/Camera.h"
#include "engine/lighting/CascadedShadowMap.h"
#include "engine/lighting/DirectionalShadowSettings.h"
#include "engine/lighting/IBL.h"
#include "engine/lighting/Light.h"
#include "engine/lighting/SceneLighting.h"
#include "engine/gizmos/GizmoDraw.h"
#include "engine/rendering/Constants.h"
#include "engine/rendering/GridRenderer.h"
#include "engine/rendering/Material.h"
#include "engine/rendering/MaterialTextures.h"
#include "engine/rendering/Mesh.h"
#include "engine/rendering/RenderDebug.h"
#include "engine/rendering/Shader.h"
#include "engine/rendering/ShaderCache.h"
#include "engine/rendering/Texture.h"
#include "engine/assets/TextureCache.h"
#include "engine/rhi/GfxContext.h"
#include "engine/rhi/d3d12/FixedDescriptorHeap.h"
#include "engine/rhi/d3d12/GpuBuffer.h"
#include "engine/raytracing/Blas.h"
#include "engine/raytracing/DxrContext.h"
#include "engine/raytracing/DxrDispatchContext.h"
#include "engine/raytracing/DxrGpuResource.h"
#include "engine/raytracing/DxrInstanceTransform.h"
#include "engine/raytracing/DxrPipeline.h"
#include "engine/raytracing/DxrRootSignature.h"
#include "engine/raytracing/ShaderBindingTable.h"
#include "engine/raytracing/Tlas.h"

#include "primitives/Cube.h"
#include "primitives/Plane.h"
#include "primitives/Sphere.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_dx12.h>

#include <d3d12.h>
#include <D3D12MemAlloc.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <iostream>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>
#include <vector>

namespace render_tests
{
    constexpr int kSmokeFramebufferSize = 32;
    constexpr int kPbrFramebufferSize = 256;
    constexpr int kPtFramebufferSize = 128;
    constexpr int kThinPrimitiveFramebufferSize = 256;
    int g_framebufferSize = kSmokeFramebufferSize;

    int FramebufferSize()
    {
        return g_framebufferSize;
    }

    int ScaledReadbackRadius(const int radiusAt256)
    {
        return std::max(2, (radiusAt256 * FramebufferSize()) / kPbrFramebufferSize);
    }

    void SetFramebufferSizeForTier(const int tier)
    {
        if (tier <= gpu_render_tests::kTierSmoke)
        {
            g_framebufferSize = kSmokeFramebufferSize;
        }
        else if (tier >= gpu_render_tests::kTierPathTracing)
        {
            g_framebufferSize = kPtFramebufferSize;
        }
        else
        {
            g_framebufferSize = kPbrFramebufferSize;
        }
    }


    Camera MakeSceneCamera()
    {
        Camera camera(glm::vec3(0.0f, 2.0f, 6.0f), -90.0f, -20.0f);
        camera.SetAspectFromFramebuffer(FramebufferSize(), FramebufferSize());
        return camera;
    }

    Camera MakeForwardCamera(const glm::vec3& position)
    {
        Camera camera(position, -90.0f, 0.0f);
        camera.SetAspectFromFramebuffer(FramebufferSize(), FramebufferSize());
        return camera;
    }

    Camera MakeTopDownCamera()
    {
        Camera camera(glm::vec3(0.0f, 8.0f, 0.01f), -90.0f, -89.0f);
        camera.SetAspectFromFramebuffer(FramebufferSize(), FramebufferSize());
        return camera;
    }

    float MaxChannelDistanceFromClear(const Framebuffer& framebuffer, int centerX, int centerY, int radius)
    {
        float bestDistance = 0.0f;
        for (int y = centerY - radius; y <= centerY + radius; ++y)
        {
            for (int x = centerX - radius; x <= centerX + radius; ++x)
            {
                float rgba[4]{};
                if (!ReadFramebufferPixel(framebuffer, x, y, rgba))
                {
                    continue;
                }

                bestDistance = std::max(bestDistance, ColorDistanceFromClear(rgba));
            }
        }

        return bestDistance;
    }

    Camera MakeOrbitCamera(
        const float angleRadians,
        const glm::vec3& target,
        const float radius,
        const float height)
    {
        const glm::vec3 eye(
            target.x + std::cos(angleRadians) * radius,
            height,
            target.z + std::sin(angleRadians) * radius);
        Camera camera(eye, 0.0f, 0.0f);
        camera.SetOrientationFromDirection(target - eye);
        camera.SetAspectFromFramebuffer(FramebufferSize(), FramebufferSize());
        return camera;
    }

    float PeakCenterLuminance(const Framebuffer& framebuffer, int radius = 12)
    {
        const int center = FramebufferSize() / 2;
        float best = 0.0f;
        for (int y = center - radius; y <= center + radius; ++y)
        {
            for (int x = center - radius; x <= center + radius; ++x)
            {
                float rgba[4]{};
                if (!ReadFramebufferPixel(framebuffer, x, y, rgba))
                {
                    continue;
                }

                const float luminance =
                    0.2126f * rgba[0] + 0.7152f * rgba[1] + 0.0722f * rgba[2];
                if (luminance > best)
                {
                    best = luminance;
                }
            }
        }

        return best;
    }

    float PeakFramebufferLuminance(const Framebuffer& framebuffer)
    {
        float best = 0.0f;
        const int step = (std::max)(1, FramebufferSize() / 32);
        for (int y = 0; y < FramebufferSize(); y += step)
        {
            for (int x = 0; x < FramebufferSize(); x += step)
            {
                float rgba[4]{};
                if (!ReadFramebufferPixel(framebuffer, x, y, rgba))
                {
                    continue;
                }

                const float luminance =
                    0.2126f * rgba[0] + 0.7152f * rgba[1] + 0.0722f * rgba[2];
                if (luminance > best)
                {
                    best = luminance;
                }
            }
        }

        return best;
    }

    void SetupEmptyShadowMapForPbr(
        CascadedShadowMap& shadowMap,
        const Camera& camera,
        const glm::vec3& sunDirection = glm::normalize(glm::vec3(-0.3f, -1.0f, -0.2f)))
    {
        shadowMap.BeginFrame(
            camera,
            sunDirection,
            glm::vec3(-2.0f),
            glm::vec3(2.0f),
            true,
            DirectionalShadowSettings{});
        shadowMap.EndFrame();
    }

    void PreparePbrFramebufferDraw(Framebuffer& framebuffer)
    {
        GfxContext::Get().SetBoundOutputFramebuffer(&framebuffer);
        framebuffer.BindDrawTarget(false);
    }

    float MaxChannelValue(
        const Framebuffer& framebuffer,
        int centerX,
        int centerY,
        int radius,
        int channel);

    void LogPixelSummary(const Framebuffer& framebuffer, const char* label);

    float MaxChannelSumNearCenter(
        const Framebuffer& framebuffer,
        const int centerX,
        const int centerY,
        const int radius);

    bool ResizePbrTestFramebuffer(Framebuffer& framebuffer)
    {
        return framebuffer.Resize(FramebufferSize(), FramebufferSize());
    }

    void ExpectVisibleShadedPixels(
        const Framebuffer& framebuffer,
        const char* label,
        const float minChannelSum = 0.12f,
        const int radius = 24)
    {
        const int center = FramebufferSize() / 2;
        const float channelSum = MaxChannelSumNearCenter(framebuffer, center, center, radius);
        if (channelSum <= minChannelSum)
        {
            LogPixelSummary(framebuffer, label);
        }

        test::ExpectTrue(
            channelSum > minChannelSum,
            "Shaded output should contain visible non-black pixels near the center");
    }

    Camera MakePbrTestCamera()
    {
        return MakeSceneCamera();
    }

    float MaxChannelSumNearCenter(
        const Framebuffer& framebuffer,
        const int centerX,
        const int centerY,
        const int radius)
    {
        return MaxChannelValue(framebuffer, centerX, centerY, radius, 0)
            + MaxChannelValue(framebuffer, centerX, centerY, radius, 1)
            + MaxChannelValue(framebuffer, centerX, centerY, radius, 2);
    }

    void DrawPbrCube(
        Framebuffer& framebuffer,
        Material& material,
        const Camera& camera,
        const SceneLighting& lighting,
        IBL& ibl,
        RenderDebugMode debugMode = RenderDebugMode::None,
        CascadedShadowMap* shadowMap = nullptr,
        const bool outputLinear = false);

    float LuminanceSpreadAcrossOrbitCameras(
        Framebuffer& framebuffer,
        Material& material,
        Mesh& mesh,
        const glm::mat4& modelMatrix,
        const SceneLighting& lighting,
        IBL& ibl,
        const glm::vec3& target,
        const RenderDebugMode debugMode,
        float* outMaxLuminance = nullptr,
        const int cameraCount = 8,
        const bool useFramePeakForSpread = false)
    {
        float minCenterLuminance = std::numeric_limits<float>::max();
        float maxCenterLuminance = 0.0f;
        float minSpreadLuminance = std::numeric_limits<float>::max();
        float maxSpreadLuminance = 0.0f;
        float maxFrameLuminance = 0.0f;
        CascadedShadowMap shadowMap;

        for (int step = 0; step < cameraCount; ++step)
        {
            const float angle = glm::two_pi<float>() * (static_cast<float>(step) / static_cast<float>(cameraCount));
            const Camera camera = MakeOrbitCamera(angle, target, 6.0f, 3.5f);

            BeginOffscreenPass(framebuffer);
            GfxContext::Get().ResetDrawSrvTable();
            SetupEmptyShadowMapForPbr(shadowMap, camera);
            GfxContext::Get().SetBoundOutputFramebuffer(&framebuffer);
            const float blackClear[] = {0.0f, 0.0f, 0.0f, 1.0f};
            framebuffer.BindDrawTarget(true, blackClear);
            material.Apply(
                camera,
                lighting,
                ibl,
                modelMatrix,
                &shadowMap,
                false,
                false,
                debugMode,
                DirectionalShadowSettings{});
            PreparePbrFramebufferDraw(framebuffer);
            mesh.Draw();
            EndOffscreenPass();

            const float centerLuminance =
                PeakCenterLuminance(framebuffer, ScaledReadbackRadius(24));
            minCenterLuminance = (std::min)(minCenterLuminance, centerLuminance);
            maxCenterLuminance = (std::max)(maxCenterLuminance, centerLuminance);
            const float frameLuminance = PeakFramebufferLuminance(framebuffer);
            if (frameLuminance > maxFrameLuminance)
            {
                maxFrameLuminance = frameLuminance;
            }

            const float spreadSample =
                useFramePeakForSpread ? frameLuminance : centerLuminance;
            minSpreadLuminance = (std::min)(minSpreadLuminance, spreadSample);
            maxSpreadLuminance = (std::max)(maxSpreadLuminance, spreadSample);
        }

        if (outMaxLuminance != nullptr)
        {
            *outMaxLuminance = maxFrameLuminance;
        }

        return maxSpreadLuminance - minSpreadLuminance;
    }

    float PeakCenterChannelSpread(
        const Framebuffer& framebuffer,
        const int centerX,
        const int centerY,
        const int radius)
    {
        float minChannel = 1.0f;
        float maxChannel = 0.0f;
        for (int y = centerY - radius; y <= centerY + radius; ++y)
        {
            for (int x = centerX - radius; x <= centerX + radius; ++x)
            {
                float rgba[4]{};
                if (!ReadFramebufferPixel(framebuffer, x, y, rgba))
                {
                    continue;
                }

                const float peak = (std::max)(rgba[0], (std::max)(rgba[1], rgba[2]));
                minChannel = (std::min)(minChannel, peak);
                maxChannel = (std::max)(maxChannel, peak);
            }
        }

        return maxChannel - minChannel;
    }

    void DrawPbrMeshWithDebugMode(
        Framebuffer& framebuffer,
        Material& material,
        Mesh& mesh,
        const glm::mat4& modelMatrix,
        const Camera& camera,
        const SceneLighting& lighting,
        IBL& ibl,
        const RenderDebugMode debugMode)
    {
        CascadedShadowMap shadowMap;
        BeginOffscreenPass(framebuffer);
        GfxContext::Get().ResetDrawSrvTable();
        SetupEmptyShadowMapForPbr(shadowMap, camera);
        GfxContext::Get().SetBoundOutputFramebuffer(&framebuffer);
        const float blackClear[] = {0.0f, 0.0f, 0.0f, 1.0f};
        framebuffer.BindDrawTarget(true, blackClear);
        material.Apply(
            camera,
            lighting,
            ibl,
            modelMatrix,
            &shadowMap,
            false,
            false,
            debugMode,
            DirectionalShadowSettings{});
        PreparePbrFramebufferDraw(framebuffer);
        mesh.Draw();
        EndOffscreenPass();
    }

    float MaxChannelValue(
        const Framebuffer& framebuffer,
        int centerX,
        int centerY,
        int radius,
        int channel)
    {
        float bestValue = 0.0f;
        for (int y = centerY - radius; y <= centerY + radius; ++y)
        {
            for (int x = centerX - radius; x <= centerX + radius; ++x)
            {
                float rgba[4]{};
                if (!ReadFramebufferPixel(framebuffer, x, y, rgba))
                {
                    continue;
                }

                bestValue = std::max(bestValue, rgba[channel]);
            }
        }

        return bestValue;
    }

    void ExpectPixelDiffersFromClear(
        const Framebuffer& framebuffer,
        int x,
        int y,
        const char* message)
    {
        float rgba[4]{};
        test::ExpectTrue(ReadFramebufferPixel(framebuffer, x, y, rgba), "Pixel readback should succeed");
        if (test::FailureCount() > 0)
        {
            return;
        }

        if (IsNearClearColor(rgba))
        {
            std::ostringstream details;
            details << message << " (rgba="
                    << rgba[0] << ", " << rgba[1] << ", " << rgba[2] << ", " << rgba[3] << ")";
            test::ExpectTrue(false, details.str().c_str());
        }
    }

    void LogPixelSummary(const Framebuffer& framebuffer, const char* label)
    {
        const int center = FramebufferSize() / 2;
        const float maxDistance = MaxChannelDistanceFromClear(framebuffer, center, center, 24);
        const float maxR = MaxChannelValue(framebuffer, center, center, 24, 0);
        const float maxG = MaxChannelValue(framebuffer, center, center, 24, 1);
        const float maxB = MaxChannelValue(framebuffer, center, center, 24, 2);
        std::cerr << label << " maxDistance=" << maxDistance << " maxRGB=(" << maxR << ", " << maxG << ", " << maxB
                  << ")\n";
    }

    void TestTransientUploadAllocates()
    {

        const float data[] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
        const GfxContext::TransientUploadAllocation upload =
            GfxContext::Get().AllocateTransientUpload(data, static_cast<std::uint32_t>(sizeof(data)));
        test::ExpectTrue(upload.gpuAddress != 0, "Transient upload should return a GPU address");
        test::ExpectTrue(upload.byteSize == sizeof(data), "Transient upload should preserve byte size");
    }

    void TestOffscreenManualRedClear()
    {

        {
            Framebuffer framebuffer;
            test::ExpectTrue(framebuffer.Resize(FramebufferSize(), FramebufferSize()), "Framebuffer resize should succeed");

            BeginOffscreenPass(framebuffer);
            D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
            rtv.ptr = framebuffer.GetColorRtvCpuHandle(0);
            const float redClear[] = {0.9f, 0.1f, 0.05f, 1.0f};
            auto* commandList = static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());
            commandList->ClearRenderTargetView(rtv, redClear, 0, nullptr);
            EndOffscreenPass();

            const float maxRed = MaxChannelValue(framebuffer, FramebufferSize() / 2, FramebufferSize() / 2, 4, 0);
            test::ExpectTrue(maxRed > 0.75f, "Manual red clear should raise the red channel near the center");

            (void)framebuffer.Resize(0, 0);
        }
    }

    void TestLineDrawWithoutDepthBinding()
    {

        {
            Framebuffer framebuffer;
            test::ExpectTrue(
                framebuffer.Resize(kThinPrimitiveFramebufferSize, kThinPrimitiveFramebufferSize),
                "Framebuffer resize should succeed");

            {
                Shader shader(EngineConstants::GizmoLineVertexShader, EngineConstants::LineFragmentShader);
                Camera camera = MakeForwardCamera(glm::vec3(0.0f, 0.0f, 4.0f));
                camera.SetAspectFromFramebuffer(kThinPrimitiveFramebufferSize, kThinPrimitiveFramebufferSize);
                const std::vector<float> crossLines = {
                    -2.0f, 0.0f, 0.0f,
                     2.0f, 0.0f, 0.0f,
                     0.0f, -2.0f, 0.0f,
                     0.0f,  2.0f, 0.0f,
                };

                BeginOffscreenPass(framebuffer, false);
                GizmoDraw::DrawLineVertices(
                    shader, camera, crossLines, glm::vec3(0.1f, 0.9f, 0.2f), false);
                EndOffscreenPass();

                const int center = kThinPrimitiveFramebufferSize / 2;
                const float bestDistance =
                    MaxChannelDistanceFromClear(framebuffer, center, center, 24);
                if (bestDistance <= 0.05f)
                {
                    LogPixelSummary(framebuffer, "Line draw without depth binding");
                }
                test::ExpectTrue(
                    bestDistance > 0.05f,
                    "Line draw without depth binding should produce visible green pixels near the center");
            }

            (void)framebuffer.Resize(0, 0);
        }
    }

    void TestGridMrtDraw()
    {

        {
            Framebuffer framebuffer;
            test::ExpectTrue(
                framebuffer.Resize(FramebufferSize(), FramebufferSize(), FramebufferColorMode::SplitDirectIndirect),
                "MRT framebuffer resize should succeed");

            {
                GridRenderer grid;
                const Camera camera = MakeTopDownCamera();

                BeginOffscreenPass(framebuffer);
                grid.Draw(camera, true);
                EndOffscreenPass();
            }

            const float bestDistance =
                MaxChannelDistanceFromClear(framebuffer, FramebufferSize() / 2, FramebufferSize() / 2, 24);
            if (bestDistance <= 0.01f)
            {
                LogPixelSummary(framebuffer, "Grid MRT draw");
            }
            test::ExpectTrue(
                bestDistance > 0.01f,
                "Grid MRT draw should change pixels near the center away from the clear color");

            (void)framebuffer.Resize(0, 0);
        }
    }

    void TestShaderUniformRegistration()
    {

        {
            Shader lineShader(EngineConstants::GizmoLineVertexShader, EngineConstants::LineFragmentShader);
            test::ExpectTrue(lineShader.IsLinked(), "Gizmo line shader should link");
            test::ExpectTrue(lineShader.HasUniform("uView"), "Gizmo line shader should expose uView");
            test::ExpectTrue(lineShader.HasUniform("uProjection"), "Gizmo line shader should expose uProjection");
            test::ExpectTrue(lineShader.HasUniform("uColor"), "Gizmo line shader should expose uColor");

            Shader maskShader(
                EngineConstants::SelectionMaskVertexShader,
                EngineConstants::LineFragmentShader);
            test::ExpectTrue(maskShader.IsLinked(), "Selection mask+line shader should link");
            test::ExpectTrue(maskShader.HasUniform("uModel"), "Selection mask shader should expose uModel");
            test::ExpectTrue(maskShader.HasUniform("uColor"), "Selection mask shader should expose uColor");
        }
    }

    void TestFramebufferClear()
    {

        {
            Framebuffer framebuffer;
            test::ExpectTrue(framebuffer.Resize(FramebufferSize(), FramebufferSize()), "Framebuffer resize should succeed");

            BeginOffscreenPass(framebuffer);
            EndOffscreenPass();

            float rgba[4]{};
            test::ExpectTrue(
                ReadFramebufferPixel(framebuffer, FramebufferSize() / 2, FramebufferSize() / 2, rgba),
                "Framebuffer readback should succeed");
            for (int channel = 0; channel < 3; ++channel)
            {
                test::ExpectNear(
                    rgba[channel],
                    kViewportClearColor[channel],
                    0.02f,
                    "Cleared framebuffer channel should match clear color");
            }

            (void)framebuffer.Resize(0, 0);
        }
    }

    void TestGizmoLineDraw()
    {

        {
            Framebuffer framebuffer;
            test::ExpectTrue(
                framebuffer.Resize(kThinPrimitiveFramebufferSize, kThinPrimitiveFramebufferSize),
                "Framebuffer resize should succeed");

            {
                Shader shader(EngineConstants::GizmoLineVertexShader, EngineConstants::LineFragmentShader);
                Camera camera = MakeForwardCamera(glm::vec3(0.0f, 0.0f, 4.0f));
                camera.SetAspectFromFramebuffer(kThinPrimitiveFramebufferSize, kThinPrimitiveFramebufferSize);
                const std::vector<float> crossLines = {
                    -2.0f, 0.0f, 0.0f,
                     2.0f, 0.0f, 0.0f,
                     0.0f, -2.0f, 0.0f,
                     0.0f,  2.0f, 0.0f,
                };

                BeginOffscreenPass(framebuffer);
                GizmoDraw::DrawLineVertices(
                    shader, camera, crossLines, glm::vec3(0.1f, 0.9f, 0.2f), false);
                EndOffscreenPass();

                const int center = kThinPrimitiveFramebufferSize / 2;
                const float bestDistance =
                    MaxChannelDistanceFromClear(framebuffer, center, center, 24);
                if (bestDistance <= 0.05f)
                {
                    LogPixelSummary(framebuffer, "Line draw");
                }
                test::ExpectTrue(
                    bestDistance > 0.05f,
                    "Line draw should produce visible green pixels near the center");
            }

            (void)framebuffer.Resize(0, 0);
        }
    }

    void TestIdentityClipSpaceTriangle()
    {

        {
            Framebuffer framebuffer;
            test::ExpectTrue(framebuffer.Resize(FramebufferSize(), FramebufferSize()), "Framebuffer resize should succeed");

            {
                const float vertices[] = {
                    -0.9f, -0.9f, 0.5f,
                     0.9f, -0.9f, 0.5f,
                     0.0f,  0.9f, 0.5f,
                };
                const unsigned int indices[] = {0, 1, 2};

                GpuBuffer vertexBuffer;
                GpuBuffer indexBuffer;
                vertexBuffer.Create(
                    GpuBuffer::Type::Vertex,
                    vertices,
                    static_cast<std::uint32_t>(sizeof(vertices)));
                indexBuffer.Create(
                    GpuBuffer::Type::Index,
                    indices,
                    static_cast<std::uint32_t>(sizeof(indices)));

                Shader shader(
                    EngineConstants::SelectionMaskVertexShader,
                    EngineConstants::LineFragmentShader);

                const glm::mat4 identity(1.0f);

                BeginOffscreenPass(framebuffer, false);
                shader.Use(false);
                shader.SetMat4("uModel", identity);
                shader.SetMat4("uView", identity);
                shader.SetMat4("uProjection", identity);
                shader.SetVec3("uColor", glm::vec3(0.95f, 0.2f, 0.1f));
                shader.FlushUniforms();

                auto* commandList = static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());
                commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                vertexBuffer.BindVertex(0, 3 * static_cast<std::uint32_t>(sizeof(float)));
                indexBuffer.BindIndex();
                commandList->DrawIndexedInstanced(3, 1, 0, 0, 0);
                EndOffscreenPass();

                const float maxRed = MaxChannelValue(framebuffer, FramebufferSize() / 2, FramebufferSize() / 2, 32, 0);
                if (maxRed <= 0.35f)
                {
                    LogPixelSummary(framebuffer, "Identity clip-space triangle");
                }
                test::ExpectTrue(
                    maxRed > 0.35f,
                    "Identity clip-space triangle should produce visible red pixels near the center");

                vertexBuffer.Destroy();
                indexBuffer.Destroy();
            }

            (void)framebuffer.Resize(0, 0);
        }
    }

    void TestSolidTriangleDraw()
    {

        {
            Framebuffer framebuffer;
            test::ExpectTrue(framebuffer.Resize(FramebufferSize(), FramebufferSize()), "Framebuffer resize should succeed");

            {
                const float vertices[] = {
                    -0.9f, -0.9f, 0.5f,
                     0.9f, -0.9f, 0.5f,
                     0.0f,  0.9f, 0.5f,
                };
                const unsigned int indices[] = {0, 1, 2};

                GpuBuffer vertexBuffer;
                GpuBuffer indexBuffer;
                vertexBuffer.Create(
                    GpuBuffer::Type::Vertex,
                    vertices,
                    static_cast<std::uint32_t>(sizeof(vertices)));
                indexBuffer.Create(
                    GpuBuffer::Type::Index,
                    indices,
                    static_cast<std::uint32_t>(sizeof(indices)));

                Shader shader(
                    EngineConstants::SelectionMaskVertexShader,
                    EngineConstants::LineFragmentShader);

                const glm::mat4 identity(1.0f);

                BeginOffscreenPass(framebuffer, false);

                shader.Use(false);
                shader.SetMat4("uModel", identity);
                shader.SetMat4("uView", identity);
                shader.SetMat4("uProjection", identity);
                shader.SetVec3("uColor", glm::vec3(0.9f, 0.15f, 0.1f));
                shader.FlushUniforms();

                auto* commandList = static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());
                commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                vertexBuffer.BindVertex(0, 3 * static_cast<std::uint32_t>(sizeof(float)));
                indexBuffer.BindIndex();
                commandList->DrawIndexedInstanced(3, 1, 0, 0, 0);

                EndOffscreenPass();

                const float maxRed = MaxChannelValue(framebuffer, FramebufferSize() / 2, FramebufferSize() / 2, 16, 0);
                test::ExpectTrue(maxRed > 0.35f, "Triangle draw should produce visible red pixels near the center");
                test::ExpectTrue(
                    MaxChannelDistanceFromClear(framebuffer, FramebufferSize() / 2, FramebufferSize() / 2, 16) > 0.05f,
                    "Triangle draw should differ from the clear color near the center");

                vertexBuffer.Destroy();
                indexBuffer.Destroy();
            }

            (void)framebuffer.Resize(0, 0);
        }
    }

    void TestGridDraw()
    {

        {
            Framebuffer framebuffer;
            test::ExpectTrue(framebuffer.Resize(FramebufferSize(), FramebufferSize()), "Framebuffer resize should succeed");

            {
                GridRenderer grid;
                const Camera camera = MakePbrTestCamera();

                BeginOffscreenPass(framebuffer);
                grid.Draw(camera, false);
                EndOffscreenPass();
            }

            const float bestDistance =
                MaxChannelDistanceFromClear(framebuffer, FramebufferSize() / 2, FramebufferSize() / 2, 24);
            test::ExpectTrue(
                bestDistance > 0.05f,
                "Grid draw should change pixels near the center away from the clear color");

            (void)framebuffer.Resize(0, 0);
        }
    }

    void TestPbrCubeDraw()
    {

        {
            Framebuffer framebuffer;
            test::ExpectTrue(ResizePbrTestFramebuffer(framebuffer), "Framebuffer resize should succeed");
            test::ExpectTrue(framebuffer.GetDepthResource() != nullptr, "Viewport framebuffer should have a depth buffer");

            {
                Material material(
                    EngineConstants::LitVertexShader,
                    EngineConstants::PbrFragmentShader,
                    glm::vec3(0.85f, 0.2f, 0.15f),
                    0.45f,
                    0.0f);

                SceneLighting lighting;
                lighting.AddLight(Light::MakeDirectional(
                    glm::normalize(glm::vec3(-0.3f, -1.0f, -0.2f)),
                    glm::vec3(1.0f),
                    3.0f));
                lighting.SetShadowLightIndex(0);

                IBL& ibl = GetD3d12TestSession().GetEnvironmentIbl();
                const Camera camera = MakePbrTestCamera();

                BeginOffscreenPass(framebuffer);
                DrawPbrCube(framebuffer, material, camera, lighting, ibl);
                EndOffscreenPass();

                ExpectVisibleShadedPixels(framebuffer, "PBR cube draw");
            }

            Material::ReleaseGlobalGpuResources();
            (void)framebuffer.Resize(0, 0);
        }
    }

    struct IdentityRedTriangleDraw
    {
        GpuBuffer vertexBuffer;
        GpuBuffer indexBuffer;
        Shader shader;

        IdentityRedTriangleDraw()
            : shader(EngineConstants::SelectionMaskVertexShader, EngineConstants::LineFragmentShader)
        {
            const float vertices[] = {
                -0.9f, -0.9f, 0.5f,
                 0.9f, -0.9f, 0.5f,
                 0.0f,  0.9f, 0.5f,
            };
            const unsigned int indices[] = {0, 1, 2};

            vertexBuffer.Create(
                GpuBuffer::Type::Vertex,
                vertices,
                static_cast<std::uint32_t>(sizeof(vertices)));
            indexBuffer.Create(
                GpuBuffer::Type::Index,
                indices,
                static_cast<std::uint32_t>(sizeof(indices)));
        }

        void Draw() const
        {
            const glm::mat4 identity(1.0f);
            shader.Use(false);
            shader.SetMat4("uModel", identity);
            shader.SetMat4("uView", identity);
            shader.SetMat4("uProjection", identity);
            shader.SetVec3("uColor", glm::vec3(0.95f, 0.12f, 0.08f));
            shader.FlushUniforms();

            auto* commandList = static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());
            commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            vertexBuffer.BindVertex(0, 3 * static_cast<std::uint32_t>(sizeof(float)));
            indexBuffer.BindIndex();
            commandList->DrawIndexedInstanced(3, 1, 0, 0, 0);
        }
    };

    void TestClearReadbackDirectVsEditor()
    {

        {
            Framebuffer framebuffer;
            test::ExpectTrue(framebuffer.Resize(FramebufferSize(), FramebufferSize()), "Framebuffer resize should succeed");

            BeginOffscreenPass(framebuffer);
            EndOffscreenPass(FrameSubmitMode::DirectSubmit);

            float directRgba[4]{};
            test::ExpectTrue(
                ReadFramebufferPixel(framebuffer, FramebufferSize() / 2, FramebufferSize() / 2, directRgba),
                "Direct-submit clear readback should succeed");
            test::ExpectTrue(
                IsNearClearColor(directRgba),
                "Direct-submit path should preserve the viewport clear color");

            BeginOffscreenPass(framebuffer);
            EndEditorPass(framebuffer, false);

            float editorRgba[4]{};
            test::ExpectTrue(
                ReadFramebufferPixel(framebuffer, FramebufferSize() / 2, FramebufferSize() / 2, editorRgba),
                "Editor-path clear readback should succeed");
            test::ExpectTrue(
                IsNearClearColor(editorRgba),
                "Editor EndFrame path should preserve the viewport clear color on readback");

            (void)framebuffer.Resize(0, 0);
        }
    }

    void TestEditorRenderingPath()
    {

        {
            Framebuffer framebuffer;
            test::ExpectTrue(framebuffer.Resize(FramebufferSize(), FramebufferSize()), "Framebuffer resize should succeed");
            IdentityRedTriangleDraw triangle;

            BeginOffscreenPass(framebuffer);
            EndEditorPass(framebuffer, false);

            float clearReadback[4]{};
            test::ExpectTrue(
                ReadFramebufferPixel(framebuffer, FramebufferSize() / 2, FramebufferSize() / 2, clearReadback),
                "Editor-path clear readback should succeed");
            test::ExpectTrue(
                clearReadback[0] > 0.03f || clearReadback[1] > 0.03f || clearReadback[2] > 0.03f,
                "Editor-path clear should not read back as pure black");
            test::ExpectTrue(
                IsNearClearColor(clearReadback),
                "Editor-path clear should match the expected viewport clear color");

            if (test::FailureCount() > 0)
            {
                (void)framebuffer.Resize(0, 0);
                return;
            }

            BeginOffscreenPass(framebuffer, false);
            triangle.Draw();
            EndEditorPass(framebuffer, false);

            const float maxRed = MaxChannelValue(framebuffer, FramebufferSize() / 2, FramebufferSize() / 2, 32, 0);
            test::ExpectTrue(maxRed > 0.35f, "Editor-path triangle should remain visible after EndFrame");
            test::ExpectTrue(
                maxRed > MaxChannelValue(framebuffer, FramebufferSize() / 2, FramebufferSize() / 2, 32, 1) + 0.1f,
                "Editor-path triangle should keep a dominant red channel");

            float previousRgba[4]{};
            bool hasPrevious = false;
            for (int frameIndex = 0; frameIndex < 5; ++frameIndex)
            {
                BeginOffscreenPass(framebuffer, false);
                triangle.Draw();
                EndEditorPass(framebuffer, false);

                float rgba[4]{};
                test::ExpectTrue(
                    ReadFramebufferPixel(framebuffer, FramebufferSize() / 2, FramebufferSize() / 2, rgba),
                    "Stable-frame readback should succeed");

                if (hasPrevious)
                {
                    for (int channel = 0; channel < 3; ++channel)
                    {
                        test::ExpectNear(
                            rgba[channel],
                            previousRgba[channel],
                            0.04f,
                            "Viewport color should stay stable across editor frames");
                    }
                }

                previousRgba[0] = rgba[0];
                previousRgba[1] = rgba[1];
                previousRgba[2] = rgba[2];
                previousRgba[3] = rgba[3];
                hasPrevious = true;
            }

            BeginOffscreenPass(framebuffer);
            BindViewportLikeSceneRenderer(framebuffer);
            triangle.Draw();
            EndEditorPass(framebuffer, false);
            test::ExpectTrue(
                MaxChannelValue(framebuffer, FramebufferSize() / 2, FramebufferSize() / 2, 24, 0) > 0.35f,
                "SceneRenderer-style double Bind should still produce visible geometry");

            if (test::FailureCount() == 0)
            {
                BeginOffscreenPass(framebuffer, false);
                triangle.Draw();
                EndEditorPass(framebuffer, true);

                const ImDrawData* drawData = ImGui::GetDrawData();
                test::ExpectTrue(
                    drawData != nullptr && drawData->TotalVtxCount > 0,
                    "ImGui viewport composite should emit draw geometry");

                const ImTextureID viewportTextureId =
                    static_cast<ImTextureID>(framebuffer.GetColorTexture());
                bool foundViewportTexture = false;
                if (drawData != nullptr)
                {
                    for (int listIndex = 0; listIndex < drawData->CmdListsCount; ++listIndex)
                    {
                        const ImDrawList* drawList = drawData->CmdLists[listIndex];
                        for (int cmdIndex = 0; cmdIndex < drawList->CmdBuffer.Size; ++cmdIndex)
                        {
                            const ImDrawCmd& command = drawList->CmdBuffer[cmdIndex];
                            if (command.UserCallback == nullptr && command.GetTexID() == viewportTextureId)
                            {
                                foundViewportTexture = true;
                                break;
                            }
                        }

                        if (foundViewportTexture)
                        {
                            break;
                        }
                    }
                }

                test::ExpectTrue(
                    foundViewportTexture,
                    "ImGui viewport composite should reference the offscreen viewport SRV");
                test::ExpectTrue(
                    MaxChannelValue(framebuffer, FramebufferSize() / 2, FramebufferSize() / 2, 24, 0) > 0.35f,
                    "Viewport texture should still contain the drawn triangle before ImGui presents it");
            }

            if (test::FailureCount() == 0)
            {
                GfxContext::Get().Resize(800, 600);
                test::ExpectTrue(
                    framebuffer.Resize(FramebufferSize(), FramebufferSize()),
                    "Viewport framebuffer should resize after swapchain resize");
                BeginOffscreenPass(framebuffer, false);
                triangle.Draw();
                EndEditorPass(framebuffer, false);
                test::ExpectTrue(
                    MaxChannelValue(framebuffer, FramebufferSize() / 2, FramebufferSize() / 2, 24, 0) > 0.35f,
                    "Drawing after swapchain resize should still produce visible geometry");
            }

            (void)framebuffer.Resize(0, 0);
        }
    }

    void TestPbrAlbedoRetainsColor()
    {

        {
            Framebuffer framebuffer;
            test::ExpectTrue(ResizePbrTestFramebuffer(framebuffer), "Framebuffer resize should succeed");

            {
                Material material(
                    EngineConstants::LitVertexShader,
                    EngineConstants::PbrFragmentShader,
                    glm::vec3(0.9f, 0.15f, 0.1f),
                    0.35f,
                    0.0f);

                SceneLighting lighting;
                lighting.AddLight(Light::MakeDirectional(
                    glm::normalize(glm::vec3(-0.3f, -1.0f, -0.2f)),
                    glm::vec3(1.0f),
                    4.0f));
                lighting.SetShadowLightIndex(0);

                IBL& ibl = GetD3d12TestSession().GetEnvironmentIbl();
                const Camera camera = MakePbrTestCamera();

                BeginOffscreenPass(framebuffer);
                DrawPbrCube(framebuffer, material, camera, lighting, ibl);
                EndOffscreenPass();

                ExpectVisibleShadedPixels(framebuffer, "PBR red albedo");

                const int center = FramebufferSize() / 2;
                const float maxRed = MaxChannelValue(framebuffer, center, center, 24, 0);
                const float maxGreen = MaxChannelValue(framebuffer, center, center, 24, 1);
                const float maxBlue = MaxChannelValue(framebuffer, center, center, 24, 2);

                float sampleRgba[4]{};
                if (ReadFramebufferPixel(framebuffer, center, center, sampleRgba))
                {
                    test::ExpectTrue(
                        !IsApproximatelyGrayscale(sampleRgba, 0.05f),
                        "PBR output with red albedo should not collapse to grayscale");
                }

                test::ExpectTrue(maxRed > maxGreen + 0.05f, "PBR red albedo should dominate green channel");
                test::ExpectTrue(maxRed > maxBlue + 0.05f, "PBR red albedo should dominate blue channel");
            }

            Material::ReleaseGlobalGpuResources();
            (void)framebuffer.Resize(0, 0);
        }
    }

    void TestPbrWithRenderedShadowCascades()
    {

        {
            Framebuffer framebuffer;
            test::ExpectTrue(ResizePbrTestFramebuffer(framebuffer), "Framebuffer resize should succeed");

            std::unique_ptr<Mesh> cube = CreateCubeMesh();
            Material material(
                EngineConstants::LitVertexShader,
                EngineConstants::PbrFragmentShader,
                glm::vec3(0.9f, 0.15f, 0.1f),
                0.35f,
                0.0f);
            ApplyWoodTableMaterialMaps(material);

            SceneLighting lighting;
            lighting.AddLight(Light::MakeDirectional(
                glm::normalize(glm::vec3(-0.3f, -1.0f, -0.2f)),
                glm::vec3(1.0f),
                4.0f));
            lighting.SetShadowLightIndex(0);

            IBL& ibl = GetD3d12TestSession().GetEnvironmentIbl();
            CascadedShadowMap shadowMap;
            Shader shadowDepthShader(
                EngineConstants::ShadowDepthVertexShader,
                EngineConstants::ShadowDepthFragmentShader);
            const Camera camera = MakePbrTestCamera();
            const glm::mat4 modelMatrix = glm::scale(glm::mat4(1.0f), glm::vec3(2.0f));
            const glm::vec3 sunDirection = glm::normalize(glm::vec3(-0.3f, -1.0f, -0.2f));

            BeginOffscreenPass(framebuffer);
            GfxContext::Get().SetBoundOutputFramebuffer(&framebuffer);

            shadowMap.BeginFrame(
                camera,
                sunDirection,
                glm::vec3(-2.0f),
                glm::vec3(2.0f),
                true,
                DirectionalShadowSettings{});
            for (int cascadeIndex = 0; cascadeIndex < shadowMap.GetActiveCascadeCount(); ++cascadeIndex)
            {
                shadowMap.BeginCascade(cascadeIndex);
                shadowDepthShader.Use();
                shadowDepthShader.SetMat4("uLightSpaceMatrix", shadowMap.GetLightSpaceMatrix(cascadeIndex));
                shadowDepthShader.SetMat4("uModel", modelMatrix);
                shadowDepthShader.FlushUniforms();
                cube->Draw();
            }
            shadowMap.EndFrame();

            test::ExpectTrue(shadowMap.HasRenderedDepth(), "Shadow pass should record rendered depth");

            PreparePbrFramebufferDraw(framebuffer);

            material.Apply(
                camera,
                lighting,
                ibl,
                modelMatrix,
                &shadowMap,
                true,
                false,
                RenderDebugMode::None,
                DirectionalShadowSettings{});
            PreparePbrFramebufferDraw(framebuffer);
            cube->Draw();

            EndOffscreenPass();

            const int center = FramebufferSize() / 2;
            const float maxRed = MaxChannelValue(framebuffer, center, center, 24, 0);
            const float maxGreen = MaxChannelValue(framebuffer, center, center, 24, 1);
            const float maxBlue = MaxChannelValue(framebuffer, center, center, 24, 2);

            test::ExpectTrue(
                maxRed + maxGreen + maxBlue > 0.18f,
                "PBR with rendered shadow cascades should not collapse to black");
            test::ExpectTrue(
                maxRed > maxGreen + 0.03f,
                "Textured red-leaning PBR should retain a dominant red channel with shadow depth rendered");

            Material::ReleaseGlobalGpuResources();
            TextureCache::Get().Clear();
            ShaderCache::Clear();
            (void)framebuffer.Resize(0, 0);

            {
                std::vector<std::unique_ptr<Material>> retainedMaterials;
                retainedMaterials.push_back(std::make_unique<Material>(
                    EngineConstants::LitVertexShader,
                    EngineConstants::PbrFragmentShader,
                    glm::vec3(0.85f, 0.2f, 0.15f),
                    0.45f,
                    0.0f));
                ApplyWoodTableMaterialMaps(*retainedMaterials.back());
                retainedMaterials.clear();
            }

            TextureCache::Get().Clear();
            ShaderCache::Clear();
            Material::ReleaseGlobalGpuResources();
        }
    }

    SceneLighting MakeDefaultTestLighting()
    {
        SceneLighting lighting;
        lighting.AddLight(Light::MakeDirectional(
            glm::normalize(glm::vec3(-0.3f, -1.0f, -0.2f)),
            glm::vec3(1.0f),
            4.0f));
        lighting.SetShadowLightIndex(0);
        return lighting;
    }

    void DrawPbrCube(
        Framebuffer& framebuffer,
        Material& material,
        const Camera& camera,
        const SceneLighting& lighting,
        IBL& ibl,
        const RenderDebugMode debugMode,
        CascadedShadowMap* shadowMap,
        const bool outputLinear)
    {
        GfxContext::Get().ResetDrawSrvTable();

        std::unique_ptr<Mesh> cube = CreateCubeMesh();
        const glm::mat4 modelMatrix = glm::scale(glm::mat4(1.0f), glm::vec3(2.0f));

        CascadedShadowMap localShadowMap;
        CascadedShadowMap* activeShadowMap = shadowMap;
        if (activeShadowMap == nullptr)
        {
            activeShadowMap = &localShadowMap;
            SetupEmptyShadowMapForPbr(*activeShadowMap, camera);
        }

        GfxContext::Get().SetBoundOutputFramebuffer(&framebuffer);
        const float blackClear[] = {0.0f, 0.0f, 0.0f, 1.0f};
        framebuffer.BindDrawTarget(true, blackClear);

        test::ExpectTrue(
            ibl.IsReady(),
            ibl.GetLoadError().empty() ? "Environment IBL should be ready before PBR draw"
                                       : ibl.GetLoadError().c_str());

        material.Apply(
            camera,
            lighting,
            ibl,
            modelMatrix,
            activeShadowMap,
            activeShadowMap != nullptr && activeShadowMap->HasRenderedDepth(),
            outputLinear,
            debugMode,
            DirectionalShadowSettings{});
        PreparePbrFramebufferDraw(framebuffer);
        cube->Draw();
    }

    void TestMaterialLayerUniformAlbedo()
    {

        {
            Framebuffer framebuffer;
            test::ExpectTrue(ResizePbrTestFramebuffer(framebuffer), "Framebuffer resize should succeed");

            Material material(
                EngineConstants::LitVertexShader,
                EngineConstants::PbrFragmentShader,
                glm::vec3(0.92f, 0.18f, 0.12f),
                0.4f,
                0.0f);
            SceneLighting lighting = MakeDefaultTestLighting();
            IBL& ibl = GetD3d12TestSession().GetEnvironmentIbl();
            const Camera camera = MakePbrTestCamera();

            BeginOffscreenPass(framebuffer);
            DrawPbrCube(framebuffer, material, camera, lighting, ibl);
            EndOffscreenPass();

            const int center = FramebufferSize() / 2;
            const float maxRed = MaxChannelValue(framebuffer, center, center, 20, 0);
            ExpectVisibleShadedPixels(framebuffer, "Uniform-albedo PBR layer", 0.12f, 20);
            test::ExpectTrue(
                maxRed > 0.12f,
                "Uniform-albedo PBR layer should write a visible red channel");

            Material::ReleaseGlobalGpuResources();
            (void)framebuffer.Resize(0, 0);
        }
    }

    void TestMaterialLayerAmbientIbl()
    {

        {
            Framebuffer framebuffer;
            test::ExpectTrue(ResizePbrTestFramebuffer(framebuffer), "Framebuffer resize should succeed");

            Material material(
                EngineConstants::LitVertexShader,
                EngineConstants::PbrFragmentShader,
                glm::vec3(0.85f, 0.2f, 0.15f),
                0.45f,
                0.0f);
            SceneLighting lighting = MakeDefaultTestLighting();
            IBL& ibl = GetD3d12TestSession().GetEnvironmentIbl();
            const Camera camera = MakePbrTestCamera();

            BeginOffscreenPass(framebuffer);
            DrawPbrCube(
                framebuffer,
                material,
                camera,
                lighting,
                ibl,
                RenderDebugMode::AmbientIbl);
            EndOffscreenPass();

            const int center = FramebufferSize() / 2;
            ExpectVisibleShadedPixels(framebuffer, "Ambient IBL debug layer", 0.12f, 20);
            test::ExpectTrue(
                MaxChannelDistanceFromClear(framebuffer, center, center, 20) > 0.05f,
                "Ambient IBL debug layer should produce non-clear output");

            Material::ReleaseGlobalGpuResources();
            (void)framebuffer.Resize(0, 0);
        }
    }

    void TestDirectDiffuseGeomStableAcrossOrbitCameras()
    {

        {
            Framebuffer framebuffer;
            test::ExpectTrue(ResizePbrTestFramebuffer(framebuffer), "Framebuffer resize should succeed");

            std::unique_ptr<Mesh> cube = CreateCubeMesh();
            Material material(
                EngineConstants::LitVertexShader,
                EngineConstants::PbrFragmentShader,
                glm::vec3(0.75f, 0.75f, 0.75f),
                0.35f,
                0.0f);
            SceneLighting lighting = MakeDefaultTestLighting();
            IBL& ibl = GetD3d12TestSession().GetEnvironmentIbl();
            const glm::mat4 modelMatrix = glm::scale(glm::mat4(1.0f), glm::vec3(2.0f));
            const glm::vec3 cubeTopTarget(0.0f, 2.0f, 0.0f);

            float maxLuminance = 0.0f;
            const float spread = LuminanceSpreadAcrossOrbitCameras(
                framebuffer,
                material,
                *cube,
                modelMatrix,
                lighting,
                ibl,
                cubeTopTarget,
                RenderDebugMode::DirectDiffuseGeom,
                &maxLuminance);

            test::ExpectTrue(
                maxLuminance > 0.08f,
                "Direct diffuse orbit cameras should produce visible shaded output");
            test::ExpectTrue(
                spread < 0.08f,
                "Direct diffuse (geom N·L) on a uniform cube top should stay stable across orbit cameras");

            Material::ReleaseGlobalGpuResources();
            (void)framebuffer.Resize(0, 0);
        }
    }

    void TestDiffuseIblStableAtSpherePole()
    {

        {
            Framebuffer framebuffer;
            test::ExpectTrue(ResizePbrTestFramebuffer(framebuffer), "Framebuffer resize should succeed");

            std::unique_ptr<Mesh> sphere = CreateSphereMesh(0.5f, 32, 16);
            Material material(
                EngineConstants::LitVertexShader,
                EngineConstants::PbrFragmentShader,
                glm::vec3(0.75f, 0.75f, 0.75f),
                0.35f,
                0.0f);
            SceneLighting lighting = MakeDefaultTestLighting();
            IBL& ibl = GetD3d12TestSession().GetEnvironmentIbl();
            const glm::mat4 modelMatrix = glm::scale(glm::mat4(1.0f), glm::vec3(2.0f));
            const glm::vec3 spherePoleTarget(0.0f, 1.0f, 0.0f);

            float maxLuminance = 0.0f;
            const float spread = LuminanceSpreadAcrossOrbitCameras(
                framebuffer,
                material,
                *sphere,
                modelMatrix,
                lighting,
                ibl,
                spherePoleTarget,
                RenderDebugMode::DiffuseIbl,
                &maxLuminance);

            test::ExpectTrue(
                maxLuminance > 0.08f,
                "Diffuse IBL orbit cameras should produce visible shaded output");
            test::ExpectTrue(
                spread < 0.10f,
                "Diffuse IBL at the sphere pole should stay stable across orbit cameras");

            Material::ReleaseGlobalGpuResources();
            (void)framebuffer.Resize(0, 0);
        }
    }

    void TestWoodCubeDirectLightingViewSensitivity()
    {

        {
            Framebuffer framebuffer;
            test::ExpectTrue(ResizePbrTestFramebuffer(framebuffer), "Framebuffer resize should succeed");

            std::unique_ptr<Mesh> cube = CreateCubeMesh();
            Material material(
                EngineConstants::LitVertexShader,
                EngineConstants::PbrFragmentShader,
                glm::vec3(0.9f, 0.15f, 0.1f),
                0.35f,
                0.0f);
            ApplyWoodTableMaterialMaps(material);
            test::ExpectTrue(material.HasNormalMap(), "Wood cube material should load a normal map");

            SceneLighting lighting = MakeDefaultTestLighting();
            IBL& ibl = GetD3d12TestSession().GetEnvironmentIbl();
            const glm::mat4 modelMatrix = glm::scale(glm::mat4(1.0f), glm::vec3(2.0f));
            const Camera camera = MakeForwardCamera(glm::vec3(0.0f, 0.0f, 6.0f));
            const int center = FramebufferSize() / 2;
            const int sampleRadius = ScaledReadbackRadius(16);

            DrawPbrMeshWithDebugMode(
                framebuffer,
                material,
                *cube,
                modelMatrix,
                camera,
                lighting,
                ibl,
                RenderDebugMode::GeometricNormal);
            const float geomNormalSpread =
                PeakCenterChannelSpread(framebuffer, center, center, sampleRadius);

            DrawPbrMeshWithDebugMode(
                framebuffer,
                material,
                *cube,
                modelMatrix,
                camera,
                lighting,
                ibl,
                RenderDebugMode::ShadedNormal);
            const float shadedNormalSpread =
                PeakCenterChannelSpread(framebuffer, center, center, sampleRadius);

            ExpectVisibleShadedPixels(framebuffer, "Wood cube shaded-normal debug draw", 0.12f, sampleRadius);
            test::ExpectTrue(
                geomNormalSpread < 0.08f,
                "Wood cube geometric-normal debug view should stay relatively uniform on a single face");
            test::ExpectTrue(
                shadedNormalSpread > 0.005f,
                "Wood cube shaded-normal debug view should show normal-map perturbation");
            test::ExpectTrue(
                shadedNormalSpread > geomNormalSpread + 0.005f,
                "Wood cube shaded-normal debug view should vary more than geometric normals (normal-map sensitivity)");

            Material::ReleaseGlobalGpuResources();
            (void)framebuffer.Resize(0, 0);
        }
    }

    void TestMultiFrameOffscreenSyncStability()
    {

        {
            Framebuffer framebuffer;
            test::ExpectTrue(ResizePbrTestFramebuffer(framebuffer), "Framebuffer resize should succeed");

            Material material(
                EngineConstants::LitVertexShader,
                EngineConstants::PbrFragmentShader,
                glm::vec3(0.92f, 0.18f, 0.12f),
                0.4f,
                0.0f);
            SceneLighting lighting = MakeDefaultTestLighting();
            IBL& ibl = GetD3d12TestSession().GetEnvironmentIbl();
            const Camera camera = MakePbrTestCamera();

            float previousRgba[3]{};
            bool hasPrevious = false;
            for (int frameIndex = 0; frameIndex < 5; ++frameIndex)
            {
                BeginOffscreenPass(framebuffer, true, true);
                DrawPbrCube(framebuffer, material, camera, lighting, ibl);
                EndOffscreenPass();

                const int center = FramebufferSize() / 2;
                float rgba[4]{};
                test::ExpectTrue(
                    ReadFramebufferPixel(framebuffer, center, center, rgba),
                    "Multi-frame offscreen readback should succeed");
                test::ExpectTrue(
                    rgba[0] + rgba[1] + rgba[2] > 0.12f,
                    "Synced multi-frame offscreen pass should not read back black");

                if (hasPrevious)
                {
                    for (int channel = 0; channel < 3; ++channel)
                    {
                        test::ExpectNear(
                            rgba[channel],
                            previousRgba[channel],
                            0.05f,
                            "Synced multi-frame offscreen output should stay stable");
                    }
                }

                previousRgba[0] = rgba[0];
                previousRgba[1] = rgba[1];
                previousRgba[2] = rgba[2];
                hasPrevious = true;
            }

            Material::ReleaseGlobalGpuResources();
            (void)framebuffer.Resize(0, 0);
        }
    }

    void TestDescriptorBudget()
    {

        std::vector<std::uint32_t> allocated;
        for (;;)
        {
            const std::uint32_t index = GfxContext::Get().AllocateOffscreenSrv();
            if (index == FixedDescriptorHeap::kInvalid)
            {
                break;
            }

            allocated.push_back(index);
            test::ExpectTrue(
                GfxContext::Get().GetSrvCpuHandle(index) != 0,
                "Allocated SRV index should produce a valid CPU handle");
        }

        test::ExpectTrue(!allocated.empty(), "Descriptor heap should allocate at least one SRV before exhaustion");
        test::ExpectTrue(
            GfxContext::Get().GetSrvCpuHandle(FixedDescriptorHeap::kInvalid) == 0,
            "Invalid SRV index must not produce a CPU handle");

        for (const std::uint32_t index : allocated)
        {
            GfxContext::Get().FreeOffscreenSrv(index);
        }

        const std::uint32_t reallocated = GfxContext::Get().AllocateOffscreenSrv();
        test::ExpectTrue(reallocated != FixedDescriptorHeap::kInvalid, "Freed descriptors should become available again");
        GfxContext::Get().FreeOffscreenSrv(reallocated);
    }

    void RenderImportedSceneShadowPass(
        Framebuffer& framebuffer,
        const Camera& camera,
        const std::vector<std::unique_ptr<Mesh>>& meshes,
        Material& material,
        SceneLighting& lighting,
        IBL& ibl,
        CascadedShadowMap& shadowMap,
        Shader& shadowDepthShader,
        DirectionalShadowSettings& shadowSettings)
    {
        const glm::vec3 boundsMin(-4.0f);
        const glm::vec3 boundsMax(4.0f);
        const glm::vec3 sunDirection = glm::normalize(glm::vec3(-0.3f, -1.0f, -0.2f));

        GfxContext::Get().SetBoundOutputFramebuffer(&framebuffer);

        shadowMap.BeginFrame(
            camera,
            sunDirection,
            boundsMin,
            boundsMax,
            true,
            shadowSettings);
        for (int cascadeIndex = 0; cascadeIndex < shadowMap.GetActiveCascadeCount(); ++cascadeIndex)
        {
            shadowMap.BeginCascade(cascadeIndex);
            shadowDepthShader.Use();
            shadowDepthShader.SetMat4(
                "uLightSpaceMatrix",
                shadowMap.GetLightSpaceMatrix(cascadeIndex));

            for (std::size_t meshIndex = 0; meshIndex < meshes.size(); ++meshIndex)
            {
                const glm::mat4 modelMatrix = glm::translate(
                    glm::mat4(1.0f),
                    glm::vec3(static_cast<float>(meshIndex) * 0.15f, 0.0f, 0.0f));
                shadowDepthShader.SetMat4("uModel", modelMatrix);
                shadowDepthShader.FlushUniforms();
                meshes[meshIndex]->Draw();
            }
        }
        shadowMap.EndFrame();

        framebuffer.Bind();
        for (std::size_t meshIndex = 0; meshIndex < meshes.size(); ++meshIndex)
        {
            const glm::mat4 modelMatrix = glm::translate(
                glm::mat4(1.0f),
                glm::vec3(static_cast<float>(meshIndex) * 0.15f, 0.0f, 0.0f));
            material.Apply(
                camera,
                lighting,
                ibl,
                modelMatrix,
                &shadowMap,
                true,
                false,
                RenderDebugMode::None,
                shadowSettings);
            meshes[meshIndex]->Draw();
        }
        framebuffer.Unbind();
    }

    void TestApplicationPathAfterBulkMeshUpload()
    {

        {
            std::vector<std::unique_ptr<Mesh>> importedMeshes;
            constexpr int kImportedMeshCount = 24;
            importedMeshes.reserve(kImportedMeshCount);
            for (int meshIndex = 0; meshIndex < kImportedMeshCount; ++meshIndex)
            {
                importedMeshes.push_back(CreateCubeMesh());
            }

            Framebuffer framebuffer;
            test::ExpectTrue(
                framebuffer.Resize(FramebufferSize(), FramebufferSize()),
                "Framebuffer resize should succeed");

            Material material(
                EngineConstants::LitVertexShader,
                EngineConstants::PbrFragmentShader,
                glm::vec3(0.85f, 0.2f, 0.15f),
                0.45f,
                0.0f);
            ApplyWoodTableMaterialMaps(material);

            SceneLighting lighting = MakeDefaultTestLighting();
            IBL& ibl = GetD3d12TestSession().GetEnvironmentIbl();
            CascadedShadowMap shadowMap;
            Shader shadowDepthShader(
                EngineConstants::ShadowDepthVertexShader,
                EngineConstants::ShadowDepthFragmentShader);
            DirectionalShadowSettings shadowSettings;
            shadowSettings.SetShadowMapResolution(1024);
            shadowSettings.SetCascadeCount(2);
            const Camera camera = MakePbrTestCamera();

            for (int frameIndex = 0; frameIndex < 5; ++frameIndex)
            {
                GfxContext::Get().BeginFrame();
                GfxContext::Get().WaitForSwapchainFrames();

                if (frameIndex == 0)
                {
                    const unsigned char pinkPixel[] = {255, 128, 192, 255};
                    (void)Texture::CreateFromPixels(pinkPixel, 1, 1, 4, TextureColorSpace::SRGB);
                }

                RenderImportedSceneShadowPass(
                    framebuffer,
                    camera,
                    importedMeshes,
                    material,
                    lighting,
                    ibl,
                    shadowMap,
                    shadowDepthShader,
                    shadowSettings);

                ImGui_ImplDX12_NewFrame();
                ImGui_ImplGlfw_NewFrame();
                if (ImGuiIO* io = &ImGui::GetIO())
                {
                    io->DisplaySize = ImVec2(
                        static_cast<float>(GfxContext::Get().GetWidth()),
                        static_cast<float>(GfxContext::Get().GetHeight()));
                    io->DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
                }
                ImGui::NewFrame();
                ImGui::Render();
                GfxContext::Get().EndFrame();
            }

            GfxContext::Get().WaitForGpuIdle();

            const float maxSignal =
                MaxChannelDistanceFromClear(framebuffer, FramebufferSize() / 2, FramebufferSize() / 2, 24);
            test::ExpectTrue(
                maxSignal > 0.04f,
                "Application-style render loop should produce visible scene pixels after bulk mesh upload");

            Material::ReleaseGlobalGpuResources();
            (void)framebuffer.Resize(0, 0);
        }
    }

    void TestSwapchainPresentLoopStability()
    {

        for (int frameIndex = 0; frameIndex < 30; ++frameIndex)
        {
            PresentEditorSwapchainFrame();
            GfxContext::Get().WaitForGpuIdle();
            glfwPollEvents();
        }
    }

    void TestResizeDuringSwapchainPresentLoop()
    {

        for (int frameIndex = 0; frameIndex < 20; ++frameIndex)
        {
            if (frameIndex == 5)
            {
                GfxContext::Get().Resize(800, 600);
            }
            else if (frameIndex == 12)
            {
                GfxContext::Get().Resize(640, 480);
            }

            PresentEditorSwapchainFrame();
            GfxContext::Get().WaitForGpuIdle();
            glfwPollEvents();
        }

        test::ExpectTrue(GfxContext::Get().GetWidth() == 640, "Swapchain width should settle after deferred resize");
        test::ExpectTrue(GfxContext::Get().GetHeight() == 480, "Swapchain height should settle after deferred resize");
    }

    void TestImGuiMouseTracksGlfwCursor()
    {
        GLFWwindow* window = GetD3d12TestSession().context.window;

        // ImGui_ImplGlfw_UpdateMouseData only polls glfwGetCursorPos while the GLFW window
        // is focused. Long swapchain-present loops can leave stale io.MousePos from the
        // physical cursor when focus is lost on the hidden test window.
        glfwShowWindow(window);
        for (int attempt = 0; attempt < 10; ++attempt)
        {
            glfwFocusWindow(window);
            glfwPollEvents();
            if (glfwGetWindowAttrib(window, GLFW_FOCUSED) != 0)
            {
                break;
            }

            ImGui_ImplGlfw_Sleep(20);
        }

        ImGui_ImplGlfw_WindowFocusCallback(window, 1);
        ImGui_ImplGlfw_CursorEnterCallback(window, 0);
        glfwPollEvents();

        const auto sampleImGuiMouse = [&](const double x, const double y) -> ImVec2 {
            glfwSetCursorPos(window, x, y);
            glfwPollEvents();

            ImGui_ImplDX12_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            if (ImGuiIO* io = &ImGui::GetIO())
            {
                io->DisplaySize = ImVec2(
                    static_cast<float>(GfxContext::Get().GetWidth()),
                    static_cast<float>(GfxContext::Get().GetHeight()));
                io->DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
            }
            ImGui::NewFrame();
            const ImVec2 mousePos = ImGui::GetIO().MousePos;
            ImGui::Render();
            return mousePos;
        };

        const ImVec2 firstMousePos = sampleImGuiMouse(100.0, 120.0);
        const ImVec2 secondMousePos = sampleImGuiMouse(280.0, 220.0);

        test::ExpectTrue(
            glfwGetWindowAttrib(window, GLFW_FOCUSED) != 0,
            "GLFW test window should be focused for ImGui mouse polling");

        double glfwX = 0.0;
        double glfwY = 0.0;
        glfwGetCursorPos(window, &glfwX, &glfwY);
        test::ExpectNear(static_cast<float>(glfwX), 280.0f, 2.0f, "GLFW cursor X should reflect SetCursorPos");
        test::ExpectNear(static_cast<float>(glfwY), 220.0f, 2.0f, "GLFW cursor Y should reflect SetCursorPos");

        glfwHideWindow(window);

        test::ExpectNear(firstMousePos.x, 100.0f, 2.0f, "ImGui mouse X should track GLFW cursor");
        test::ExpectNear(firstMousePos.y, 120.0f, 2.0f, "ImGui mouse Y should track GLFW cursor");
        test::ExpectNear(secondMousePos.x, 280.0f, 2.0f, "ImGui mouse X should update after GLFW cursor move");
        test::ExpectNear(secondMousePos.y, 220.0f, 2.0f, "ImGui mouse Y should update after GLFW cursor move");
        test::ExpectTrue(
            std::abs(secondMousePos.x - firstMousePos.x) > 50.0f
                || std::abs(secondMousePos.y - firstMousePos.y) > 50.0f,
            "ImGui mouse position should change when GLFW cursor moves");
    }

    void TestDxrAccelerationStructuresSmoke()
    {

        if (!GfxContext::Get().IsRaytracingSupported())
        {
            std::cout << "SKIP: DXR acceleration structure smoke (no RTX tier)\n";
            return;
        }

        std::unique_ptr<Mesh> plane = CreatePlaneMesh(2.0f);
        std::unique_ptr<Mesh> cube = CreateCubeMesh();
        test::ExpectTrue(plane != nullptr && cube != nullptr, "Primitive meshes should be created");

        plane->EnsureGpuResources();
        cube->EnsureGpuResources();
        test::ExpectTrue(plane->GetVertexBuffer().IsValid(), "Plane VB should be valid");
        test::ExpectTrue(cube->GetVertexBuffer().IsValid(), "Cube VB should be valid");

        Framebuffer framebuffer;
        test::ExpectTrue(framebuffer.Resize(64, 64), "Framebuffer resize should succeed");
        BeginOffscreenPass(framebuffer, false);

        auto* commandList4 = DxrContext::Get().QueryCommandList4(GfxContext::Get().GetCommandList());
        test::ExpectTrue(commandList4 != nullptr, "Command list 4 should be available");

        DxrGpuResource scratch{};
        test::ExpectTrue(CreateDxrDefaultBuffer(16ull * 1024ull * 1024ull, true, scratch), "Scratch buffer alloc");

        Blas planeBlas;
        Blas cubeBlas;
        std::string buildError;
        test::ExpectTrue(planeBlas.Build(commandList4, plane.get(), scratch, buildError), buildError.c_str());
        test::ExpectTrue(cubeBlas.Build(commandList4, cube.get(), scratch, buildError), buildError.c_str());
        test::ExpectTrue(planeBlas.GetGpuVirtualAddress() != 0, "Plane BLAS GPU VA");
        test::ExpectTrue(cubeBlas.GetGpuVirtualAddress() != 0, "Cube BLAS GPU VA");

        std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instances(2);
        WriteD3D12InstanceTransform(glm::mat4(1.0f), reinterpret_cast<float*>(instances[0].Transform));
        instances[0].InstanceID = 0;
        instances[0].InstanceMask = 0xFF;
        instances[0].AccelerationStructure = planeBlas.GetGpuVirtualAddress();

        const glm::mat4 cubeTransform = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        WriteD3D12InstanceTransform(cubeTransform, reinterpret_cast<float*>(instances[1].Transform));
        instances[1].InstanceID = 1;
        instances[1].InstanceMask = 0xFF;
        instances[1].AccelerationStructure = cubeBlas.GetGpuVirtualAddress();

        Tlas tlas;
        test::ExpectTrue(tlas.Build(commandList4, instances, scratch, buildError), buildError.c_str());
        test::ExpectTrue(tlas.GetGpuVirtualAddress() != 0, "TLAS GPU VA");

        const std::uint64_t expectedTriangles =
            static_cast<std::uint64_t>(plane->GetIndices().size() + cube->GetIndices().size()) / 3u;
        test::ExpectTrue(
            planeBlas.GetTriangleCount() + cubeBlas.GetTriangleCount() == expectedTriangles,
            "BLAS triangle counts should match CPU index counts");

        EndOffscreenPass();
        (void)framebuffer.Resize(0, 0);

        tlas.Release();
        cubeBlas.Release();
        planeBlas.Release();
        scratch.Release();
        plane.reset();
        cube.reset();
        DrainDeferredTestGpuResources();
    }

    float HalfToFloat(const std::uint16_t half)
    {
        const std::uint32_t sign = static_cast<std::uint32_t>(half & 0x8000u) << 16;
        const std::uint32_t exponent = (half & 0x7C00u) >> 10;
        const std::uint32_t mantissa = half & 0x03FFu;
        std::uint32_t bits = 0;
        if (exponent == 0)
        {
            bits = mantissa == 0 ? sign : sign;
        }
        else if (exponent == 0x1Fu)
        {
            bits = sign | 0x7F800000u | (mantissa << 13);
        }
        else
        {
            bits = sign |
                static_cast<std::uint32_t>((exponent + (127 - 15)) << 23) |
                (mantissa << 13);
        }

        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    bool ReadbackRgba16FTextureCenter(
        ID3D12Resource* textureResource,
        const int width,
        const int height,
        float outRgba[4])
    {
        if (textureResource == nullptr || outRgba == nullptr || width <= 0 || height <= 0)
        {
            return false;
        }

        D3D12MA::Allocator* allocator = GfxContext::Get().GetMemoryAllocator();
        constexpr UINT64 kReadbackSize = sizeof(std::uint16_t) * 4ull;

        D3D12_RESOURCE_DESC readbackDesc{};
        readbackDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        readbackDesc.Width = kReadbackSize;
        readbackDesc.Height = 1;
        readbackDesc.DepthOrArraySize = 1;
        readbackDesc.MipLevels = 1;
        readbackDesc.Format = DXGI_FORMAT_UNKNOWN;
        readbackDesc.SampleDesc.Count = 1;
        readbackDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        D3D12MA::ALLOCATION_DESC readbackAllocationDesc{};
        readbackAllocationDesc.HeapType = D3D12_HEAP_TYPE_READBACK;

        ID3D12Resource* readbackResource = nullptr;
        D3D12MA::Allocation* readbackAllocation = nullptr;
        if (FAILED(allocator->CreateResource(
                &readbackAllocationDesc,
                &readbackDesc,
                D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,
                &readbackAllocation,
                IID_PPV_ARGS(&readbackResource))))
        {
            return false;
        }

        const int centerX = width / 2;
        const int centerY = height / 2;

        GfxContext::Get().ExecuteImmediate([&](void* commandListPtr) {
            auto* commandList = static_cast<ID3D12GraphicsCommandList*>(commandListPtr);

            D3D12_RESOURCE_BARRIER toCopy{};
            toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toCopy.Transition.pResource = textureResource;
            toCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            toCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            toCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            commandList->ResourceBarrier(1, &toCopy);

            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = textureResource;
            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            source.SubresourceIndex = 0;

            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = readbackResource;
            destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            destination.PlacedFootprint.Offset = 0;
            destination.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            destination.PlacedFootprint.Footprint.Width = 1;
            destination.PlacedFootprint.Footprint.Height = 1;
            destination.PlacedFootprint.Footprint.Depth = 1;
            destination.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(kReadbackSize);

            D3D12_BOX sourceBox{};
            sourceBox.left = static_cast<UINT>(centerX);
            sourceBox.top = static_cast<UINT>(centerY);
            sourceBox.front = 0;
            sourceBox.right = static_cast<UINT>(centerX + 1);
            sourceBox.bottom = static_cast<UINT>(centerY + 1);
            sourceBox.back = 1;
            commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, &sourceBox);

            D3D12_RESOURCE_BARRIER fromCopy{};
            fromCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            fromCopy.Transition.pResource = textureResource;
            fromCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            fromCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            fromCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            commandList->ResourceBarrier(1, &fromCopy);
        });

        D3D12_RANGE readRange{0, static_cast<SIZE_T>(kReadbackSize)};
        void* mapped = nullptr;
        if (FAILED(readbackResource->Map(0, &readRange, &mapped)))
        {
            readbackAllocation->Release();
            readbackResource->Release();
            return false;
        }

        const auto* halfChannels = static_cast<const std::uint16_t*>(mapped);
        for (int channel = 0; channel < 4; ++channel)
        {
            outRgba[channel] = HalfToFloat(halfChannels[channel]);
        }

        readbackResource->Unmap(0, nullptr);
        readbackAllocation->Release();
        readbackResource->Release();
        return true;
    }

    Camera MakePtGlassCamera(const float yawDegrees = -90.0f)
    {
        Camera camera(glm::vec3(0.0f, 0.0f, 4.0f), yawDegrees, 0.0f);
        camera.SetAspectFromFramebuffer(kPtFramebufferSize, kPtFramebufferSize);
        return camera;
    }

    struct PtGlassTestFixture
    {
        MinimalPtGlassScene scene;
        PtDummyGbufferBindings gbuffer;
        PtDispatchStack stack;
        DxrGpuResource scratch{};
        IBL* environmentIbl = nullptr;
        std::string lastError;

        bool Setup(const bool includeGlassPane = true)
        {
            lastError.clear();
            if (!GfxContext::Get().IsRaytracingSupported())
            {
                return false;
            }

            environmentIbl = &GetD3d12TestSession().GetEnvironmentIbl();
            test::ExpectTrue(environmentIbl->IsReady(), "PT glass tests require warmed environment IBL");

            Framebuffer framebuffer;
            test::ExpectTrue(
                framebuffer.Resize(kPtFramebufferSize, kPtFramebufferSize),
                "PT glass framebuffer resize should succeed");
            BeginOffscreenPass(framebuffer, false);

            auto* commandList4 = DxrContext::Get().QueryCommandList4(GfxContext::Get().GetCommandList());
            test::ExpectTrue(commandList4 != nullptr, "Command list 4 should be available for PT glass tests");
            test::ExpectTrue(
                CreateDxrDefaultBuffer(16ull * 1024ull * 1024ull, true, scratch),
                "PT glass scratch buffer alloc");

            test::ExpectTrue(scene.Build(commandList4, scratch, includeGlassPane, lastError), lastError.c_str());
            test::ExpectTrue(gbuffer.Create(lastError), lastError.c_str());
            test::ExpectTrue(stack.EnsureReady(lastError), lastError.c_str());
            return true;
        }

        void Teardown()
        {
            stack.Release();
            gbuffer.Release();
            scene.Release();
            scratch.Release();
            DrainDeferredTestGpuResources();
        }
    };

    bool DispatchPtGlassFrame(
        PtGlassTestFixture& fixture,
        Camera& camera,
        const glm::mat4& prevViewProjection,
        const glm::mat4& prevView,
        const glm::vec3& prevCameraPos,
        const bool motionHistoryValid,
        const std::uint32_t frameIndex)
    {
        PtFrameDispatchParams params{};
        params.scene = &fixture.scene;
        params.gbuffer = &fixture.gbuffer;
        params.stack = &fixture.stack;
        params.environmentIbl = fixture.environmentIbl;
        params.camera = &camera;
        params.width = kPtFramebufferSize;
        params.height = kPtFramebufferSize;
        params.prevViewProjection = prevViewProjection;
        params.prevView = prevView;
        params.prevCameraPos = prevCameraPos;
        params.motionHistoryValid = motionHistoryValid;
        params.frameIndex = frameIndex;

        std::string dispatchError;
        const bool dispatched = DispatchMinimalPathTracerFrame(params, dispatchError);
        test::ExpectTrue(dispatched, dispatchError.c_str());
        return dispatched;
    }

    void TestPtTransmissionGuideAlbedoBands()
    {
        if (!GfxContext::Get().IsRaytracingSupported())
        {
            std::cout << "SKIP: PT transmission guide albedo bands (no RTX tier)\n";
            return;
        }

        float baselineDiffuseR = 0.0f;
        {
            PtGlassTestFixture baseline;
            if (baseline.Setup(false))
            {
                Camera camera(glm::vec3(0.0f, 0.0f, 2.0f), -90.0f, 0.0f);
                camera.SetAspectFromFramebuffer(kPtFramebufferSize, kPtFramebufferSize);
                const glm::mat4 unjitteredViewProj =
                    camera.GetUnjitteredProjectionMatrix() * camera.GetViewMatrix();
                if (DispatchPtGlassFrame(
                        baseline,
                        camera,
                        unjitteredViewProj,
                        camera.GetViewMatrix(),
                        camera.GetPosition(),
                        false,
                        0u))
                {
                    EndOffscreenPass();
                    float diffuseGuide[4]{};
                    if (ReadbackPtGuideCenterPixel(
                            baseline.stack.dispatchContext.GetPathTracerDiffuseAlbedoResource(),
                            baseline.stack.dispatchContext.GetPathTracerDiffuseAlbedoResourceState(),
                            kPtFramebufferSize,
                            kPtFramebufferSize,
                            DXGI_FORMAT_R8G8B8A8_UNORM,
                            diffuseGuide))
                    {
                        baselineDiffuseR = diffuseGuide[0];
                        test::ExpectTrue(
                            diffuseGuide[0] > 0.55f,
                            "Opaque backdrop baseline should produce red diffuse guide band");
                        test::ExpectTrue(
                            diffuseGuide[1] < 0.25f,
                            "Opaque backdrop baseline should keep diffuse guide G low");
                    }
                }
                else
                {
                    EndOffscreenPass();
                }
            }
            baseline.Teardown();
        }

        PtGlassTestFixture fixture;
        if (!fixture.Setup())
        {
            fixture.Teardown();
            return;
        }

        Camera camera = MakePtGlassCamera();
        const glm::mat4 unjitteredViewProj =
            camera.GetUnjitteredProjectionMatrix() * camera.GetViewMatrix();
        if (!DispatchPtGlassFrame(
                fixture,
                camera,
                unjitteredViewProj,
                camera.GetViewMatrix(),
                camera.GetPosition(),
                false,
                0u))
        {
            EndOffscreenPass();
            fixture.Teardown();
            return;
        }

        EndOffscreenPass();

        float diffuseGuide[4]{};
        test::ExpectTrue(
            ReadbackPtGuideCenterPixel(
                fixture.stack.dispatchContext.GetPathTracerDiffuseAlbedoResource(),
                fixture.stack.dispatchContext.GetPathTracerDiffuseAlbedoResourceState(),
                kPtFramebufferSize,
                kPtFramebufferSize,
                DXGI_FORMAT_R8G8B8A8_UNORM,
                diffuseGuide),
            "Diffuse albedo guide readback should succeed");

        float normalGuide[4]{};
        test::ExpectTrue(
            ReadbackPtGuideCenterPixel(
                fixture.stack.dispatchContext.GetPathTracerNormalRoughnessResource(),
                fixture.stack.dispatchContext.GetPathTracerNormalRoughnessResourceState(),
                kPtFramebufferSize,
                kPtFramebufferSize,
                DXGI_FORMAT_R16G16B16A16_FLOAT,
                normalGuide),
            "Normal-roughness guide readback should succeed");

        // PSR (transmission-rr-guides.md): through a clear head-on pane transmitWeight ~= 1 - Fresnel
        // ~= 0.96, so the guides describe the BACKGROUND surface. The roughness therefore blends toward
        // the backdrop's (0.35), not the glass pane's (0.02). (Before the S1 refraction fix the thin-slab
        // guide trace produced a mirrored direction that missed the backdrop and left glass roughness.)
        test::ExpectTrue(
            normalGuide[3] > 0.2f,
            "Through clear glass, guide roughness should blend toward the backdrop (PSR)");

        // The refracted guide trace hits the backdrop, so diffuse picks up its red albedo band. If the
        // thin-shell retry ever misses (transmission-rr-guides.md), the neutral sky fallback is accepted.
        const bool backdropGuideBand =
            diffuseGuide[0] > 0.55f && diffuseGuide[1] < 0.25f;
        const bool skyFallbackBand =
            diffuseGuide[0] > 0.42f && diffuseGuide[0] < 0.52f
            && std::abs(diffuseGuide[0] - diffuseGuide[1]) < 0.03f;
        test::ExpectTrue(
            backdropGuideBand || skyFallbackBand,
            "Through glass, diffuse guide should be backdrop red or neutral sky fallback");
        if (baselineDiffuseR > 0.0f && backdropGuideBand)
        {
            // PSR at head-on makes the through-glass guide track the backdrop, so it should closely
            // match the direct-backdrop baseline (minus the small Fresnel-reflected fraction), not
            // diverge from it as the pre-fix mirrored/sky-fallback behavior did.
            test::ExpectTrue(
                diffuseGuide[0] > baselineDiffuseR - 0.15f,
                "Through clear glass, diffuse guide should closely match the backdrop (PSR)");
        }
        test::ExpectTrue(
            normalGuide[2] > 0.5f,
            "Through glass, normal guide Z should stay camera-facing on the pane");

        fixture.Teardown();
    }

    void TestPtTransmissionVirtualMotionOnOrbit()
    {
        if (!GfxContext::Get().IsRaytracingSupported())
        {
            std::cout << "SKIP: PT transmission virtual motion (no RTX tier)\n";
            return;
        }

        PtGlassTestFixture fixture;
        if (!fixture.Setup())
        {
            fixture.Teardown();
            return;
        }

        Camera prevCamera = MakePtGlassCamera(-90.0f);
        const glm::mat4 prevViewProj =
            prevCamera.GetUnjitteredProjectionMatrix() * prevCamera.GetViewMatrix();

        Camera camera = MakePtGlassCamera(-75.0f);
        if (!DispatchPtGlassFrame(
                fixture,
                camera,
                prevViewProj,
                prevCamera.GetViewMatrix(),
                prevCamera.GetPosition(),
                true,
                1u))
        {
            EndOffscreenPass();
            fixture.Teardown();
            return;
        }

        EndOffscreenPass();

        float motionGuide[4]{};
        test::ExpectTrue(
            ReadbackPtGuideCenterPixel(
                fixture.stack.dispatchContext.GetPathTracerMotionResource(),
                fixture.stack.dispatchContext.GetPathTracerMotionResourceState(),
                kPtFramebufferSize,
                kPtFramebufferSize,
                DXGI_FORMAT_R16G16B16A16_FLOAT,
                motionGuide),
            "Motion guide readback should succeed");

        const float motionMagnitude = std::sqrt(motionGuide[0] * motionGuide[0] + motionGuide[1] * motionGuide[1]);
        test::ExpectTrue(
            motionMagnitude > 0.002f,
            "Virtual refracted motion should exceed threshold when camera orbits through glass pane");

        fixture.Teardown();
    }

    void TestDxrDispatchSmoke()
    {

        if (!GfxContext::Get().IsRaytracingSupported())
        {
            std::cout << "SKIP: DXR dispatch smoke (no RTX tier)\n";
            return;
        }

        std::unique_ptr<Mesh> plane = CreatePlaneMesh(2.0f);
        test::ExpectTrue(plane != nullptr, "Plane mesh should be created");
        plane->EnsureGpuResources();

        Framebuffer framebuffer;
        test::ExpectTrue(framebuffer.Resize(64, 64), "Framebuffer resize should succeed");
        BeginOffscreenPass(framebuffer, false);

        auto* commandList4 = DxrContext::Get().QueryCommandList4(GfxContext::Get().GetCommandList());
        test::ExpectTrue(commandList4 != nullptr, "Command list 4 should be available");

        DxrGpuResource scratch{};
        test::ExpectTrue(CreateDxrDefaultBuffer(16ull * 1024ull * 1024ull, true, scratch), "Scratch buffer alloc");

        Blas planeBlas;
        std::string buildError;
        test::ExpectTrue(planeBlas.Build(commandList4, plane.get(), scratch, buildError), buildError.c_str());

        std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instances(1);
        WriteD3D12InstanceTransform(glm::mat4(1.0f), reinterpret_cast<float*>(instances[0].Transform));
        instances[0].InstanceID = 0;
        instances[0].InstanceMask = 0xFF;
        instances[0].AccelerationStructure = planeBlas.GetGpuVirtualAddress();

        Tlas tlas;
        test::ExpectTrue(tlas.Build(commandList4, instances, scratch, buildError), buildError.c_str());

        DxrPipeline pipeline;
        ShaderBindingTable shaderBindingTable;
        DxrDispatchContext dispatchContext;
        test::ExpectTrue(pipeline.CreateSmokePipeline(buildError), buildError.c_str());
        test::ExpectTrue(
            shaderBindingTable.BuildSmokeTable(pipeline.GetProperties(), buildError),
            buildError.c_str());

        DxrRootSignature::DispatchConstants constants{};
        constants.outputWidth = 64;
        constants.outputHeight = 64;
        constants.clearColor[0] = 1.0f;
        constants.clearColor[1] = 0.0f;
        constants.clearColor[2] = 1.0f;
        constants.clearColor[3] = 1.0f;

        test::ExpectTrue(
            dispatchContext.DispatchSmoke(
                commandList4,
                pipeline.GetStateObject(),
                pipeline.GetGlobalRootSignature(),
                shaderBindingTable,
                tlas.GetResultResource(),
                tlas.GetGpuVirtualAddress(),
                64,
                64,
                constants,
                buildError),
            buildError.c_str());

        float rgba[4]{};
        EndOffscreenPass();

        test::ExpectTrue(
            ReadbackRgba16FTextureCenter(dispatchContext.GetOutputResource(), 64, 64, rgba),
            "DXR smoke output readback should succeed");
        test::ExpectNear(rgba[0], 1.0f, 0.05f, "DXR smoke output R should match magenta");
        test::ExpectNear(rgba[1], 0.0f, 0.05f, "DXR smoke output G should match magenta");
        test::ExpectNear(rgba[2], 1.0f, 0.05f, "DXR smoke output B should match magenta");

        // Release before GfxContext shutdown — these destructors defer D3D12MA frees and run
        // after D3d12TestContext::Shutdown() if we rely on scope exit alone.
        dispatchContext.Release();
        shaderBindingTable.Release();
        pipeline.Release();
        tlas.Release();
        planeBlas.Release();
        scratch.Release();
        plane.reset();
        (void)framebuffer.Resize(0, 0);
        DrainDeferredTestGpuResources();
    }

    void RegisterGpuRenderTests(std::vector<gpu_render_tests::TestEntry>& outTests)
    {
        auto add = [&](const char* name, const int tier, const char* label, const auto& fn) {
            outTests.push_back(gpu_render_tests::TestEntry{name, tier, label, fn});
        };

        add("TransientUploadAllocates", gpu_render_tests::kTierSmoke, "gpu-smoke", TestTransientUploadAllocates);
        add("OffscreenManualRedClear", gpu_render_tests::kTierSmoke, "gpu-smoke", TestOffscreenManualRedClear);
        add("GridMrtDraw", gpu_render_tests::kTierSmoke, "gpu-smoke", TestGridMrtDraw);
        add("ShaderUniformRegistration", gpu_render_tests::kTierSmoke, "gpu-smoke", TestShaderUniformRegistration);
        add("FramebufferClear", gpu_render_tests::kTierSmoke, "gpu-smoke", TestFramebufferClear);
        add("IdentityClipSpaceTriangle", gpu_render_tests::kTierSmoke, "gpu-smoke", TestIdentityClipSpaceTriangle);
        add("SolidTriangleDraw", gpu_render_tests::kTierSmoke, "gpu-smoke", TestSolidTriangleDraw);
        add("GridDraw", gpu_render_tests::kTierSmoke, "gpu-smoke", TestGridDraw);
        add("LineDrawWithoutDepthBinding", gpu_render_tests::kTierPbr, "gpu-pbr", TestLineDrawWithoutDepthBinding);
        add("GizmoLineDraw", gpu_render_tests::kTierPbr, "gpu-pbr", TestGizmoLineDraw);
        add("ClearReadbackDirectVsEditor", gpu_render_tests::kTierSmoke, "gpu-smoke", TestClearReadbackDirectVsEditor);

        add("PbrCubeDraw", gpu_render_tests::kTierPbr, "gpu-pbr", TestPbrCubeDraw);
        add("PbrAlbedoRetainsColor", gpu_render_tests::kTierPbr, "gpu-pbr", TestPbrAlbedoRetainsColor);
        add("PbrWithRenderedShadowCascades", gpu_render_tests::kTierPbr, "gpu-pbr", TestPbrWithRenderedShadowCascades);
        add("MaterialLayerUniformAlbedo", gpu_render_tests::kTierPbr, "gpu-pbr", TestMaterialLayerUniformAlbedo);
        add("MaterialLayerAmbientIbl", gpu_render_tests::kTierPbr, "gpu-pbr", TestMaterialLayerAmbientIbl);
        add("DirectDiffuseGeomStableAcrossOrbitCameras", gpu_render_tests::kTierPbr, "gpu-pbr", TestDirectDiffuseGeomStableAcrossOrbitCameras);
        add("DiffuseIblStableAtSpherePole", gpu_render_tests::kTierPbr, "gpu-pbr", TestDiffuseIblStableAtSpherePole);
        add("WoodCubeDirectLightingViewSensitivity", gpu_render_tests::kTierPbr, "gpu-pbr", TestWoodCubeDirectLightingViewSensitivity);
        add("MultiFrameOffscreenSyncStability", gpu_render_tests::kTierPbr, "gpu-pbr", TestMultiFrameOffscreenSyncStability);

        add("EditorRenderingPath", gpu_render_tests::kTierEditor, "gpu-editor", TestEditorRenderingPath);
        add("ApplicationPathAfterBulkMeshUpload", gpu_render_tests::kTierEditor, "gpu-editor", TestApplicationPathAfterBulkMeshUpload);
        add("SwapchainPresentLoopStability", gpu_render_tests::kTierEditor, "gpu-editor", TestSwapchainPresentLoopStability);
        add("ResizeDuringSwapchainPresentLoop", gpu_render_tests::kTierEditor, "gpu-editor", TestResizeDuringSwapchainPresentLoop);
        add("ImGuiMouseTracksGlfwCursor", gpu_render_tests::kTierEditor, "gpu-editor", TestImGuiMouseTracksGlfwCursor);
        add("DescriptorBudget", gpu_render_tests::kTierEditor, "gpu-editor", TestDescriptorBudget);

        add("DxrAccelerationStructuresSmoke", gpu_render_tests::kTierDxr, "gpu-dxr", TestDxrAccelerationStructuresSmoke);
        add("DxrDispatchSmoke", gpu_render_tests::kTierDxr, "gpu-dxr", TestDxrDispatchSmoke);

        add("PtTransmissionGuideAlbedoBands", gpu_render_tests::kTierPathTracing, "gpu-dxr-pt", TestPtTransmissionGuideAlbedoBands);
        add("PtTransmissionVirtualMotionOnOrbit", gpu_render_tests::kTierPathTracing, "gpu-dxr-pt", TestPtTransmissionVirtualMotionOnOrbit);
    }
}
