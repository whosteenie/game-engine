#include "d3d12_test_harness.h"
#include "test_expect.h"

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
#include "engine/raytracing/DxrGpuResource.h"
#include "engine/raytracing/DxrInstanceTransform.h"
#include "engine/raytracing/Tlas.h"

#include "primitives/Cube.h"
#include "primitives/Plane.h"
#include "primitives/Sphere.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_dx12.h>

#include <d3d12.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <iostream>
#include <cmath>
#include <limits>
#include <sstream>
#include <vector>

namespace
{
    constexpr int kFramebufferSize = 256;

    Camera MakeSceneCamera()
    {
        Camera camera(glm::vec3(0.0f, 2.0f, 6.0f), -90.0f, -20.0f);
        camera.SetAspectFromFramebuffer(kFramebufferSize, kFramebufferSize);
        return camera;
    }

    Camera MakeForwardCamera(const glm::vec3& position)
    {
        Camera camera(position, -90.0f, 0.0f);
        camera.SetAspectFromFramebuffer(kFramebufferSize, kFramebufferSize);
        return camera;
    }

    Camera MakeTopDownCamera()
    {
        Camera camera(glm::vec3(0.0f, 8.0f, 0.01f), -90.0f, -89.0f);
        camera.SetAspectFromFramebuffer(kFramebufferSize, kFramebufferSize);
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
        camera.SetAspectFromFramebuffer(kFramebufferSize, kFramebufferSize);
        return camera;
    }

    float CenterLuminance(const Framebuffer& framebuffer, int radius = 12)
    {
        const int center = kFramebufferSize / 2;
        float sum = 0.0f;
        int count = 0;
        for (int y = center - radius; y <= center + radius; ++y)
        {
            for (int x = center - radius; x <= center + radius; ++x)
            {
                float rgba[4]{};
                if (!ReadFramebufferPixel(framebuffer, x, y, rgba))
                {
                    continue;
                }

                sum += 0.2126f * rgba[0] + 0.7152f * rgba[1] + 0.0722f * rgba[2];
                ++count;
            }
        }

        return count > 0 ? sum / static_cast<float>(count) : 0.0f;
    }

    float LuminanceSpreadAcrossOrbitCameras(
        Framebuffer& framebuffer,
        Material& material,
        Mesh& mesh,
        const glm::mat4& modelMatrix,
        const SceneLighting& lighting,
        IBL& ibl,
        const glm::vec3& target,
        const RenderDebugMode debugMode,
        const int cameraCount = 8)
    {
        float minLuminance = std::numeric_limits<float>::max();
        float maxLuminance = 0.0f;

        for (int step = 0; step < cameraCount; ++step)
        {
            const float angle = glm::two_pi<float>() * (static_cast<float>(step) / static_cast<float>(cameraCount));
            const Camera camera = MakeOrbitCamera(angle, target, 6.0f, 3.5f);

            BeginOffscreenPass(framebuffer);
            GfxContext::Get().SetBoundOutputFramebuffer(&framebuffer);
            framebuffer.Bind();
            material.Apply(
                camera,
                lighting,
                ibl,
                modelMatrix,
                nullptr,
                false,
                false,
                debugMode,
                DirectionalShadowSettings{});
            mesh.Draw();
            EndOffscreenPass();

            const float luminance = CenterLuminance(framebuffer);
            minLuminance = std::min(minLuminance, luminance);
            maxLuminance = std::max(maxLuminance, luminance);
        }

        return maxLuminance - minLuminance;
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
        const int center = kFramebufferSize / 2;
        const float maxDistance = MaxChannelDistanceFromClear(framebuffer, center, center, 24);
        const float maxR = MaxChannelValue(framebuffer, center, center, 24, 0);
        const float maxG = MaxChannelValue(framebuffer, center, center, 24, 1);
        const float maxB = MaxChannelValue(framebuffer, center, center, 24, 2);
        std::cerr << label << " maxDistance=" << maxDistance << " maxRGB=(" << maxR << ", " << maxG << ", " << maxB
                  << ")\n";
    }

    void TestTransientUploadAllocates()
    {
        D3d12TestContext context;
        test::ExpectTrue(context.Initialize(), "D3D12 test context should initialize");

        const float data[] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
        const GfxContext::TransientUploadAllocation upload =
            GfxContext::Get().AllocateTransientUpload(data, static_cast<std::uint32_t>(sizeof(data)));
        test::ExpectTrue(upload.gpuAddress != 0, "Transient upload should return a GPU address");
        test::ExpectTrue(upload.byteSize == sizeof(data), "Transient upload should preserve byte size");

        context.Shutdown();
    }

    void TestOffscreenManualRedClear()
    {
        D3d12TestContext context;
        test::ExpectTrue(context.Initialize(), "D3D12 test context should initialize");

        {
            Framebuffer framebuffer;
            test::ExpectTrue(framebuffer.Resize(kFramebufferSize, kFramebufferSize), "Framebuffer resize should succeed");

            BeginOffscreenPass(framebuffer);
            D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
            rtv.ptr = framebuffer.GetColorRtvCpuHandle(0);
            const float redClear[] = {0.9f, 0.1f, 0.05f, 1.0f};
            auto* commandList = static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());
            commandList->ClearRenderTargetView(rtv, redClear, 0, nullptr);
            EndOffscreenPass();

            const float maxRed = MaxChannelValue(framebuffer, kFramebufferSize / 2, kFramebufferSize / 2, 4, 0);
            test::ExpectTrue(maxRed > 0.75f, "Manual red clear should raise the red channel near the center");

            (void)framebuffer.Resize(0, 0);
        }

        context.Shutdown();
    }

    void TestLineDrawWithoutDepthBinding()
    {
        D3d12TestContext context;
        test::ExpectTrue(context.Initialize(), "D3D12 test context should initialize");

        {
            Framebuffer framebuffer;
            test::ExpectTrue(framebuffer.Resize(kFramebufferSize, kFramebufferSize), "Framebuffer resize should succeed");

            {
                Shader shader(EngineConstants::GridVertexShader, EngineConstants::LineFragmentShader);
                const Camera camera = MakeForwardCamera(glm::vec3(0.0f, 0.0f, 4.0f));
                const std::vector<float> crossLines = {
                    -2.0f, 0.0f, 0.0f,
                     2.0f, 0.0f, 0.0f,
                     0.0f, -2.0f, 0.0f,
                     0.0f,  2.0f, 0.0f,
                };

                BeginOffscreenPass(framebuffer, false);
                shader.Use(false);
                GizmoDraw::DrawLineVertices(shader, camera, crossLines, glm::vec3(0.1f, 0.9f, 0.2f));
                EndOffscreenPass();

                const float maxGreen = MaxChannelValue(framebuffer, kFramebufferSize / 2, kFramebufferSize / 2, 12, 1);
                if (maxGreen <= 0.35f)
                {
                    LogPixelSummary(framebuffer, "Line draw without depth binding");
                }
                test::ExpectTrue(
                    maxGreen > 0.35f,
                    "Line draw without depth binding should produce visible green pixels near the center");
            }

            (void)framebuffer.Resize(0, 0);
        }

        context.Shutdown();
    }

    void TestGridMrtDraw()
    {
        D3d12TestContext context;
        test::ExpectTrue(context.Initialize(), "D3D12 test context should initialize");

        {
            Framebuffer framebuffer;
            test::ExpectTrue(
                framebuffer.Resize(kFramebufferSize, kFramebufferSize, FramebufferColorMode::SplitDirectIndirect),
                "MRT framebuffer resize should succeed");

            {
                GridRenderer grid;
                const Camera camera = MakeTopDownCamera();

                BeginOffscreenPass(framebuffer);
                grid.Draw(camera, true);
                EndOffscreenPass();
            }

            const float bestDistance =
                MaxChannelDistanceFromClear(framebuffer, kFramebufferSize / 2, kFramebufferSize / 2, 24);
            if (bestDistance <= 0.01f)
            {
                LogPixelSummary(framebuffer, "Grid MRT draw");
            }
            test::ExpectTrue(
                bestDistance > 0.01f,
                "Grid MRT draw should change pixels near the center away from the clear color");

            (void)framebuffer.Resize(0, 0);
        }

        context.Shutdown();
    }

    void TestShaderUniformRegistration()
    {
        D3d12TestContext context;
        test::ExpectTrue(context.Initialize(), "D3D12 test context should initialize");

        {
            Shader lineShader(EngineConstants::GridVertexShader, EngineConstants::LineFragmentShader);
            test::ExpectTrue(lineShader.IsLinked(), "Grid+line shader should link");
            test::ExpectTrue(lineShader.HasUniform("uView"), "Grid+line shader should expose uView");
            test::ExpectTrue(lineShader.HasUniform("uProjection"), "Grid+line shader should expose uProjection");
            test::ExpectTrue(lineShader.HasUniform("uColor"), "Grid+line shader should expose uColor");

            Shader maskShader(
                EngineConstants::SelectionMaskVertexShader,
                EngineConstants::LineFragmentShader);
            test::ExpectTrue(maskShader.IsLinked(), "Selection mask+line shader should link");
            test::ExpectTrue(maskShader.HasUniform("uModel"), "Selection mask shader should expose uModel");
            test::ExpectTrue(maskShader.HasUniform("uColor"), "Selection mask shader should expose uColor");
        }

        context.Shutdown();
    }

    void TestFramebufferClear()
    {
        D3d12TestContext context;
        test::ExpectTrue(context.Initialize(), "D3D12 test context should initialize");

        {
            Framebuffer framebuffer;
            test::ExpectTrue(framebuffer.Resize(kFramebufferSize, kFramebufferSize), "Framebuffer resize should succeed");

            BeginOffscreenPass(framebuffer);
            EndOffscreenPass();

            float rgba[4]{};
            test::ExpectTrue(
                ReadFramebufferPixel(framebuffer, kFramebufferSize / 2, kFramebufferSize / 2, rgba),
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

        context.Shutdown();
    }

    void TestGizmoLineDraw()
    {
        D3d12TestContext context;
        test::ExpectTrue(context.Initialize(), "D3D12 test context should initialize");

        {
            Framebuffer framebuffer;
            test::ExpectTrue(framebuffer.Resize(kFramebufferSize, kFramebufferSize), "Framebuffer resize should succeed");

            {
                Shader shader(EngineConstants::GridVertexShader, EngineConstants::LineFragmentShader);
                const Camera camera = MakeForwardCamera(glm::vec3(0.0f, 0.0f, 4.0f));
                const std::vector<float> crossLines = {
                    -2.0f, 0.0f, 0.0f,
                     2.0f, 0.0f, 0.0f,
                     0.0f, -2.0f, 0.0f,
                     0.0f,  2.0f, 0.0f,
                };

                BeginOffscreenPass(framebuffer);
                shader.Use(false);
                GizmoDraw::DrawLineVertices(shader, camera, crossLines, glm::vec3(0.1f, 0.9f, 0.2f));
                EndOffscreenPass();

                const float maxGreen = MaxChannelValue(framebuffer, kFramebufferSize / 2, kFramebufferSize / 2, 12, 1);
                if (maxGreen <= 0.35f)
                {
                    LogPixelSummary(framebuffer, "Line draw");
                }
                test::ExpectTrue(maxGreen > 0.35f, "Line draw should produce visible green pixels near the center");
            }

            (void)framebuffer.Resize(0, 0);
        }

        context.Shutdown();
    }

    void TestIdentityClipSpaceTriangle()
    {
        D3d12TestContext context;
        test::ExpectTrue(context.Initialize(), "D3D12 test context should initialize");

        {
            Framebuffer framebuffer;
            test::ExpectTrue(framebuffer.Resize(kFramebufferSize, kFramebufferSize), "Framebuffer resize should succeed");

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

                const float maxRed = MaxChannelValue(framebuffer, kFramebufferSize / 2, kFramebufferSize / 2, 32, 0);
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

        context.Shutdown();
    }

    void TestSolidTriangleDraw()
    {
        D3d12TestContext context;
        test::ExpectTrue(context.Initialize(), "D3D12 test context should initialize");

        {
            Framebuffer framebuffer;
            test::ExpectTrue(framebuffer.Resize(kFramebufferSize, kFramebufferSize), "Framebuffer resize should succeed");

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

                const float maxRed = MaxChannelValue(framebuffer, kFramebufferSize / 2, kFramebufferSize / 2, 16, 0);
                test::ExpectTrue(maxRed > 0.35f, "Triangle draw should produce visible red pixels near the center");
                test::ExpectTrue(
                    MaxChannelDistanceFromClear(framebuffer, kFramebufferSize / 2, kFramebufferSize / 2, 16) > 0.05f,
                    "Triangle draw should differ from the clear color near the center");

                vertexBuffer.Destroy();
                indexBuffer.Destroy();
            }

            (void)framebuffer.Resize(0, 0);
        }

        context.Shutdown();
    }

    void TestGridDraw()
    {
        D3d12TestContext context;
        test::ExpectTrue(context.Initialize(), "D3D12 test context should initialize");

        {
            Framebuffer framebuffer;
            test::ExpectTrue(framebuffer.Resize(kFramebufferSize, kFramebufferSize), "Framebuffer resize should succeed");

            {
                GridRenderer grid;
                const Camera camera = MakeSceneCamera();

                BeginOffscreenPass(framebuffer);
                grid.Draw(camera, false);
                EndOffscreenPass();
            }

            const float bestDistance =
                MaxChannelDistanceFromClear(framebuffer, kFramebufferSize / 2, kFramebufferSize / 2, 24);
            test::ExpectTrue(
                bestDistance > 0.05f,
                "Grid draw should change pixels near the center away from the clear color");

            (void)framebuffer.Resize(0, 0);
        }

        context.Shutdown();
    }

    void TestPbrCubeDraw()
    {
        D3d12TestContext context;
        test::ExpectTrue(context.Initialize(), "D3D12 test context should initialize");

        {
            Framebuffer framebuffer;
            test::ExpectTrue(framebuffer.Resize(kFramebufferSize, kFramebufferSize), "Framebuffer resize should succeed");
            test::ExpectTrue(framebuffer.GetDepthResource() != nullptr, "Viewport framebuffer should have a depth buffer");

            {
                std::unique_ptr<Mesh> cube = CreateCubeMesh();
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

                IBL ibl(EngineConstants::EnvironmentHdr);
                CascadedShadowMap shadowMap;
                const Camera camera = MakeSceneCamera();

                BeginOffscreenPass(framebuffer);

                shadowMap.BeginFrame(
                    camera,
                    glm::normalize(glm::vec3(-0.3f, -1.0f, -0.2f)),
                    glm::vec3(-2.0f),
                    glm::vec3(2.0f),
                    true,
                    DirectionalShadowSettings{});
                shadowMap.EndFrame();

                framebuffer.BindDrawTarget(false);

                material.Apply(
                    camera,
                    lighting,
                    ibl,
                    glm::scale(glm::mat4(1.0f), glm::vec3(2.0f)),
                    &shadowMap,
                    true,
                    false,
                    RenderDebugMode::None,
                    DirectionalShadowSettings{});
                cube->Draw();

                EndOffscreenPass();

                test::ExpectTrue(
                    MaxChannelDistanceFromClear(framebuffer, kFramebufferSize / 2, kFramebufferSize / 2, 24) > 0.05f,
                    "PBR cube should differ from the clear color near the center");
            }

            Material::ReleaseGlobalGpuResources();
            (void)framebuffer.Resize(0, 0);
        }

        context.Shutdown();
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
        D3d12TestContext context;
        test::ExpectTrue(context.Initialize(), "D3D12 test context should initialize");

        {
            Framebuffer framebuffer;
            test::ExpectTrue(framebuffer.Resize(kFramebufferSize, kFramebufferSize), "Framebuffer resize should succeed");

            BeginOffscreenPass(framebuffer);
            EndOffscreenPass(FrameSubmitMode::DirectSubmit);

            float directRgba[4]{};
            test::ExpectTrue(
                ReadFramebufferPixel(framebuffer, kFramebufferSize / 2, kFramebufferSize / 2, directRgba),
                "Direct-submit clear readback should succeed");
            test::ExpectTrue(
                IsNearClearColor(directRgba),
                "Direct-submit path should preserve the viewport clear color");

            BeginOffscreenPass(framebuffer);
            EndEditorPass(framebuffer, false);

            float editorRgba[4]{};
            test::ExpectTrue(
                ReadFramebufferPixel(framebuffer, kFramebufferSize / 2, kFramebufferSize / 2, editorRgba),
                "Editor-path clear readback should succeed");
            test::ExpectTrue(
                IsNearClearColor(editorRgba),
                "Editor EndFrame path should preserve the viewport clear color on readback");

            (void)framebuffer.Resize(0, 0);
        }

        context.Shutdown();
    }

    void TestEditorRenderingPath()
    {
        D3d12TestContext context;
        test::ExpectTrue(context.Initialize(640, 480), "D3D12 test context should initialize");

        {
            Framebuffer framebuffer;
            test::ExpectTrue(framebuffer.Resize(kFramebufferSize, kFramebufferSize), "Framebuffer resize should succeed");
            IdentityRedTriangleDraw triangle;

            BeginOffscreenPass(framebuffer);
            EndEditorPass(framebuffer, false);

            float clearReadback[4]{};
            test::ExpectTrue(
                ReadFramebufferPixel(framebuffer, kFramebufferSize / 2, kFramebufferSize / 2, clearReadback),
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
                context.Shutdown();
                return;
            }

            BeginOffscreenPass(framebuffer, false);
            triangle.Draw();
            EndEditorPass(framebuffer, false);

            const float maxRed = MaxChannelValue(framebuffer, kFramebufferSize / 2, kFramebufferSize / 2, 32, 0);
            test::ExpectTrue(maxRed > 0.35f, "Editor-path triangle should remain visible after EndFrame");
            test::ExpectTrue(
                maxRed > MaxChannelValue(framebuffer, kFramebufferSize / 2, kFramebufferSize / 2, 32, 1) + 0.1f,
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
                    ReadFramebufferPixel(framebuffer, kFramebufferSize / 2, kFramebufferSize / 2, rgba),
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
                MaxChannelValue(framebuffer, kFramebufferSize / 2, kFramebufferSize / 2, 24, 0) > 0.35f,
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
                    MaxChannelValue(framebuffer, kFramebufferSize / 2, kFramebufferSize / 2, 24, 0) > 0.35f,
                    "Viewport texture should still contain the drawn triangle before ImGui presents it");
            }

            if (test::FailureCount() == 0)
            {
                GfxContext::Get().Resize(800, 600);
                test::ExpectTrue(
                    framebuffer.Resize(kFramebufferSize, kFramebufferSize),
                    "Viewport framebuffer should resize after swapchain resize");
                BeginOffscreenPass(framebuffer, false);
                triangle.Draw();
                EndEditorPass(framebuffer, false);
                test::ExpectTrue(
                    MaxChannelValue(framebuffer, kFramebufferSize / 2, kFramebufferSize / 2, 24, 0) > 0.35f,
                    "Drawing after swapchain resize should still produce visible geometry");
            }

            (void)framebuffer.Resize(0, 0);
        }

        context.Shutdown();
    }

    void TestPbrAlbedoRetainsColor()
    {
        D3d12TestContext context;
        test::ExpectTrue(context.Initialize(), "D3D12 test context should initialize");

        {
            Framebuffer framebuffer;
            test::ExpectTrue(framebuffer.Resize(kFramebufferSize, kFramebufferSize), "Framebuffer resize should succeed");

            {
                std::unique_ptr<Mesh> cube = CreateCubeMesh();
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

                IBL ibl(EngineConstants::EnvironmentHdr);
                CascadedShadowMap shadowMap;
                const Camera camera = MakeSceneCamera();

                BeginOffscreenPass(framebuffer);
                BindViewportLikeSceneRenderer(framebuffer);

                shadowMap.BeginFrame(
                    camera,
                    glm::normalize(glm::vec3(-0.3f, -1.0f, -0.2f)),
                    glm::vec3(-2.0f),
                    glm::vec3(2.0f),
                    true,
                    DirectionalShadowSettings{});
                shadowMap.EndFrame();
                framebuffer.BindDrawTarget(false);

                material.Apply(
                    camera,
                    lighting,
                    ibl,
                    glm::scale(glm::mat4(1.0f), glm::vec3(2.0f)),
                    &shadowMap,
                    true,
                    false,
                    RenderDebugMode::None,
                    DirectionalShadowSettings{});
                cube->Draw();

                EndEditorPass(framebuffer, false);

                const int center = kFramebufferSize / 2;
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

        context.Shutdown();
    }

    void TestPbrWithRenderedShadowCascades()
    {
        D3d12TestContext context;
        test::ExpectTrue(context.Initialize(), "D3D12 test context should initialize");

        {
            Framebuffer framebuffer;
            test::ExpectTrue(framebuffer.Resize(kFramebufferSize, kFramebufferSize), "Framebuffer resize should succeed");

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

            IBL ibl(EngineConstants::EnvironmentHdr);
            CascadedShadowMap shadowMap;
            Shader shadowDepthShader(
                EngineConstants::ShadowDepthVertexShader,
                EngineConstants::ShadowDepthFragmentShader);
            const Camera camera = MakeSceneCamera();
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

            framebuffer.Bind();

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
            cube->Draw();

            EndOffscreenPass();

            const int center = kFramebufferSize / 2;
            const float maxRed = MaxChannelValue(framebuffer, center, center, 24, 0);
            const float maxGreen = MaxChannelValue(framebuffer, center, center, 24, 1);
            const float maxBlue = MaxChannelValue(framebuffer, center, center, 24, 2);

            test::ExpectTrue(
                MaxChannelDistanceFromClear(framebuffer, center, center, 24) > 0.08f,
                "PBR with rendered shadow cascades should differ from the clear color");
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

        context.Shutdown();
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
        const RenderDebugMode debugMode = RenderDebugMode::None,
        CascadedShadowMap* shadowMap = nullptr)
    {
        std::unique_ptr<Mesh> cube = CreateCubeMesh();
        const glm::mat4 modelMatrix = glm::scale(glm::mat4(1.0f), glm::vec3(2.0f));

        GfxContext::Get().SetBoundOutputFramebuffer(&framebuffer);
        framebuffer.Bind();

        material.Apply(
            camera,
            lighting,
            ibl,
            modelMatrix,
            shadowMap,
            shadowMap != nullptr,
            false,
            debugMode,
            DirectionalShadowSettings{});
        cube->Draw();
    }

    void TestMaterialLayerUniformAlbedo()
    {
        D3d12TestContext context;
        test::ExpectTrue(context.Initialize(), "D3D12 test context should initialize");

        {
            Framebuffer framebuffer;
            test::ExpectTrue(framebuffer.Resize(kFramebufferSize, kFramebufferSize), "Framebuffer resize should succeed");

            Material material(
                EngineConstants::LitVertexShader,
                EngineConstants::PbrFragmentShader,
                glm::vec3(0.92f, 0.18f, 0.12f),
                0.4f,
                0.0f);
            SceneLighting lighting = MakeDefaultTestLighting();
            IBL ibl(EngineConstants::EnvironmentHdr);
            const Camera camera = MakeSceneCamera();

            BeginOffscreenPass(framebuffer);
            DrawPbrCube(framebuffer, material, camera, lighting, ibl);
            EndOffscreenPass();

            const int center = kFramebufferSize / 2;
            const float maxRed = MaxChannelValue(framebuffer, center, center, 20, 0);
            test::ExpectTrue(
                maxRed > 0.12f,
                "Uniform-albedo PBR layer should write a visible red channel");
            test::ExpectTrue(
                MaxChannelDistanceFromClear(framebuffer, center, center, 20) > 0.06f,
                "Uniform-albedo PBR layer should differ from the viewport clear color");

            Material::ReleaseGlobalGpuResources();
            (void)framebuffer.Resize(0, 0);
        }

        context.Shutdown();
    }

    void TestMaterialLayerAmbientIbl()
    {
        D3d12TestContext context;
        test::ExpectTrue(context.Initialize(), "D3D12 test context should initialize");

        {
            Framebuffer framebuffer;
            test::ExpectTrue(framebuffer.Resize(kFramebufferSize, kFramebufferSize), "Framebuffer resize should succeed");

            Material material(
                EngineConstants::LitVertexShader,
                EngineConstants::PbrFragmentShader,
                glm::vec3(0.85f, 0.2f, 0.15f),
                0.45f,
                0.0f);
            SceneLighting lighting = MakeDefaultTestLighting();
            IBL ibl(EngineConstants::EnvironmentHdr);
            const Camera camera = MakeSceneCamera();

            BeginOffscreenPass(framebuffer);
            DrawPbrCube(
                framebuffer,
                material,
                camera,
                lighting,
                ibl,
                RenderDebugMode::AmbientIbl);
            EndOffscreenPass();

            const int center = kFramebufferSize / 2;
            test::ExpectTrue(
                MaxChannelDistanceFromClear(framebuffer, center, center, 20) > 0.05f,
                "Ambient IBL debug layer should produce non-clear output");
            test::ExpectTrue(
                MaxChannelValue(framebuffer, center, center, 20, 0)
                    + MaxChannelValue(framebuffer, center, center, 20, 1)
                    + MaxChannelValue(framebuffer, center, center, 20, 2)
                    > 0.12f,
                "Ambient IBL debug layer should not collapse to black");

            Material::ReleaseGlobalGpuResources();
            (void)framebuffer.Resize(0, 0);
        }

        context.Shutdown();
    }

    void TestDirectDiffuseGeomStableAcrossOrbitCameras()
    {
        D3d12TestContext context;
        test::ExpectTrue(context.Initialize(), "D3D12 test context should initialize");

        {
            Framebuffer framebuffer;
            test::ExpectTrue(framebuffer.Resize(kFramebufferSize, kFramebufferSize), "Framebuffer resize should succeed");

            std::unique_ptr<Mesh> cube = CreateCubeMesh();
            Material material(
                EngineConstants::LitVertexShader,
                EngineConstants::PbrFragmentShader,
                glm::vec3(0.75f, 0.75f, 0.75f),
                0.35f,
                0.0f);
            SceneLighting lighting = MakeDefaultTestLighting();
            IBL ibl(EngineConstants::EnvironmentHdr);
            const glm::mat4 modelMatrix = glm::scale(glm::mat4(1.0f), glm::vec3(2.0f));
            const glm::vec3 cubeTopTarget(0.0f, 2.0f, 0.0f);

            const float spread = LuminanceSpreadAcrossOrbitCameras(
                framebuffer,
                material,
                *cube,
                modelMatrix,
                lighting,
                ibl,
                cubeTopTarget,
                RenderDebugMode::DirectDiffuseGeom);

            test::ExpectTrue(
                spread < 0.08f,
                "Direct diffuse (geom N·L) on a uniform cube top should stay stable across orbit cameras");

            Material::ReleaseGlobalGpuResources();
            (void)framebuffer.Resize(0, 0);
        }

        context.Shutdown();
    }

    void TestDiffuseIblStableAtSpherePole()
    {
        D3d12TestContext context;
        test::ExpectTrue(context.Initialize(), "D3D12 test context should initialize");

        {
            Framebuffer framebuffer;
            test::ExpectTrue(framebuffer.Resize(kFramebufferSize, kFramebufferSize), "Framebuffer resize should succeed");

            std::unique_ptr<Mesh> sphere = CreateSphereMesh(0.5f, 32, 16);
            Material material(
                EngineConstants::LitVertexShader,
                EngineConstants::PbrFragmentShader,
                glm::vec3(0.75f, 0.75f, 0.75f),
                0.35f,
                0.0f);
            SceneLighting lighting = MakeDefaultTestLighting();
            IBL ibl(EngineConstants::EnvironmentHdr);
            const glm::mat4 modelMatrix = glm::scale(glm::mat4(1.0f), glm::vec3(2.0f));
            const glm::vec3 spherePoleTarget(0.0f, 1.0f, 0.0f);

            const float spread = LuminanceSpreadAcrossOrbitCameras(
                framebuffer,
                material,
                *sphere,
                modelMatrix,
                lighting,
                ibl,
                spherePoleTarget,
                RenderDebugMode::DiffuseIbl);

            test::ExpectTrue(
                spread < 0.10f,
                "Diffuse IBL at the sphere pole should stay stable across orbit cameras");

            Material::ReleaseGlobalGpuResources();
            (void)framebuffer.Resize(0, 0);
        }

        context.Shutdown();
    }

    void TestWoodCubeDirectLightingViewSensitivity()
    {
        D3d12TestContext context;
        test::ExpectTrue(context.Initialize(), "D3D12 test context should initialize");

        {
            Framebuffer framebuffer;
            test::ExpectTrue(framebuffer.Resize(kFramebufferSize, kFramebufferSize), "Framebuffer resize should succeed");

            std::unique_ptr<Mesh> cube = CreateCubeMesh();
            Material material(
                EngineConstants::LitVertexShader,
                EngineConstants::PbrFragmentShader,
                glm::vec3(0.9f, 0.15f, 0.1f),
                0.35f,
                0.0f);
            ApplyWoodTableMaterialMaps(material);

            SceneLighting lighting = MakeDefaultTestLighting();
            IBL ibl(EngineConstants::EnvironmentHdr);
            const glm::mat4 modelMatrix = glm::scale(glm::mat4(1.0f), glm::vec3(2.0f));
            const glm::vec3 cubeTopTarget(0.0f, 2.0f, 0.0f);

            const float geomSpread = LuminanceSpreadAcrossOrbitCameras(
                framebuffer,
                material,
                *cube,
                modelMatrix,
                lighting,
                ibl,
                cubeTopTarget,
                RenderDebugMode::DirectDiffuseGeom);
            const float directSpread = LuminanceSpreadAcrossOrbitCameras(
                framebuffer,
                material,
                *cube,
                modelMatrix,
                lighting,
                ibl,
                cubeTopTarget,
                RenderDebugMode::DirectLighting);

            test::ExpectTrue(
                geomSpread < 0.08f,
                "Wood cube geom N·L should stay stable across orbit cameras");
            test::ExpectTrue(
                directSpread > geomSpread + 0.04f,
                "Wood cube direct lighting should vary more than geom N·L across cameras (normal-map sensitivity)");

            Material::ReleaseGlobalGpuResources();
            (void)framebuffer.Resize(0, 0);
        }

        context.Shutdown();
    }

    void TestMultiFrameOffscreenSyncStability()
    {
        D3d12TestContext context;
        test::ExpectTrue(context.Initialize(), "D3D12 test context should initialize");

        {
            Framebuffer framebuffer;
            test::ExpectTrue(framebuffer.Resize(kFramebufferSize, kFramebufferSize), "Framebuffer resize should succeed");

            Material material(
                EngineConstants::LitVertexShader,
                EngineConstants::PbrFragmentShader,
                glm::vec3(0.92f, 0.18f, 0.12f),
                0.4f,
                0.0f);
            SceneLighting lighting = MakeDefaultTestLighting();
            IBL ibl(EngineConstants::EnvironmentHdr);
            const Camera camera = MakeSceneCamera();

            float previousRgba[3]{};
            bool hasPrevious = false;
            for (int frameIndex = 0; frameIndex < 5; ++frameIndex)
            {
                BeginOffscreenPass(framebuffer, true, true);
                DrawPbrCube(framebuffer, material, camera, lighting, ibl);
                EndOffscreenPass();

                const int center = kFramebufferSize / 2;
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

        context.Shutdown();
    }

    void TestDescriptorBudget()
    {
        D3d12TestContext context;
        test::ExpectTrue(context.Initialize(), "D3D12 test context should initialize");

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

        context.Shutdown();
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
        D3d12TestContext context;
        test::ExpectTrue(context.Initialize(640, 480), "D3D12 test context should initialize");

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
                framebuffer.Resize(kFramebufferSize, kFramebufferSize),
                "Framebuffer resize should succeed");

            Material material(
                EngineConstants::LitVertexShader,
                EngineConstants::PbrFragmentShader,
                glm::vec3(0.85f, 0.2f, 0.15f),
                0.45f,
                0.0f);
            ApplyWoodTableMaterialMaps(material);

            SceneLighting lighting = MakeDefaultTestLighting();
            IBL ibl(EngineConstants::EnvironmentHdr);
            CascadedShadowMap shadowMap;
            Shader shadowDepthShader(
                EngineConstants::ShadowDepthVertexShader,
                EngineConstants::ShadowDepthFragmentShader);
            DirectionalShadowSettings shadowSettings;
            shadowSettings.SetShadowMapResolution(1024);
            shadowSettings.SetCascadeCount(2);
            const Camera camera = MakeSceneCamera();

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
                MaxChannelDistanceFromClear(framebuffer, kFramebufferSize / 2, kFramebufferSize / 2, 24);
            test::ExpectTrue(
                maxSignal > 0.04f,
                "Application-style render loop should produce visible scene pixels after bulk mesh upload");

            Material::ReleaseGlobalGpuResources();
            (void)framebuffer.Resize(0, 0);
        }

        context.Shutdown();
    }

    void TestSwapchainPresentLoopStability()
    {
        D3d12TestContext context;
        test::ExpectTrue(context.Initialize(640, 480), "D3D12 test context should initialize");

        for (int frameIndex = 0; frameIndex < 30; ++frameIndex)
        {
            PresentEditorSwapchainFrame();
            GfxContext::Get().WaitForGpuIdle();
            glfwPollEvents();
        }

        context.Shutdown();
    }

    void TestResizeDuringSwapchainPresentLoop()
    {
        D3d12TestContext context;
        test::ExpectTrue(context.Initialize(640, 480), "D3D12 test context should initialize");

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

        context.Shutdown();
    }

    void TestImGuiMouseTracksGlfwCursor()
    {
        D3d12TestContext context;
        test::ExpectTrue(context.Initialize(640, 480), "D3D12 test context should initialize");

        glfwFocusWindow(context.window);
        glfwPollEvents();

        glfwSetCursorPos(context.window, 100.0, 120.0);
        glfwPollEvents();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        const ImVec2 firstMousePos = ImGui::GetIO().MousePos;
        ImGui::Render();

        glfwSetCursorPos(context.window, 280.0, 220.0);
        glfwPollEvents();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        const ImVec2 secondMousePos = ImGui::GetIO().MousePos;
        ImGui::Render();

        test::ExpectNear(firstMousePos.x, 100.0f, 2.0f, "ImGui mouse X should track GLFW cursor");
        test::ExpectNear(firstMousePos.y, 120.0f, 2.0f, "ImGui mouse Y should track GLFW cursor");
        test::ExpectNear(secondMousePos.x, 280.0f, 2.0f, "ImGui mouse X should update after GLFW cursor move");
        test::ExpectNear(secondMousePos.y, 220.0f, 2.0f, "ImGui mouse Y should update after GLFW cursor move");
        test::ExpectTrue(
            std::abs(secondMousePos.x - firstMousePos.x) > 50.0f
                || std::abs(secondMousePos.y - firstMousePos.y) > 50.0f,
            "ImGui mouse position should change when GLFW cursor moves");

        context.Shutdown();
    }

    void TestDxrAccelerationStructuresSmoke()
    {
        D3d12TestContext context;
        test::ExpectTrue(context.Initialize(), "D3D12 test context should initialize");

        if (!GfxContext::Get().IsRaytracingSupported())
        {
            std::cout << "SKIP: DXR acceleration structure smoke (no RTX tier)\n";
            context.Shutdown();
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
        context.Shutdown();
    }
}

int main()
{
    test::ResetFailures();
    TestFramebufferClear();
    TestShaderUniformRegistration();
    TestTransientUploadAllocates();
    TestOffscreenManualRedClear();
    TestClearReadbackDirectVsEditor();
    TestGizmoLineDraw();
    TestLineDrawWithoutDepthBinding();
    TestIdentityClipSpaceTriangle();
    TestSolidTriangleDraw();
    TestGridDraw();
    TestGridMrtDraw();
    TestPbrCubeDraw();
    TestPbrAlbedoRetainsColor();
    TestMaterialLayerUniformAlbedo();
    TestMaterialLayerAmbientIbl();
    TestDirectDiffuseGeomStableAcrossOrbitCameras();
    TestDiffuseIblStableAtSpherePole();
    TestWoodCubeDirectLightingViewSensitivity();
    TestMultiFrameOffscreenSyncStability();
    TestPbrWithRenderedShadowCascades();
    TestEditorRenderingPath();
    TestApplicationPathAfterBulkMeshUpload();
    TestDescriptorBudget();
    TestSwapchainPresentLoopStability();
    TestResizeDuringSwapchainPresentLoop();
    TestImGuiMouseTracksGlfwCursor();
    TestDxrAccelerationStructuresSmoke();

    if (test::FailureCount() == 0)
    {
        std::cout << "All D3D12 render tests passed.\n";
    }

    return test::ExitCode();
}
