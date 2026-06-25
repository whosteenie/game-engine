#include "app/scene/RenderDiagnostics.h"

#include "app/scene/Scene.h"
#include "app/scene/SceneRenderer.h"
#include "engine/camera/Camera.h"
#include "engine/lighting/Light.h"
#include "engine/rendering/RenderDebug.h"
#include "engine/lighting/SceneLighting.h"
#include "engine/scene/SceneObject.h"
#include "engine/rendering/ScreenSpaceEffects.h"
#include "engine/lighting/CascadedShadowMap.h"
#include "engine/lighting/DirectionalShadowSettings.h"
#include "engine/lighting/ShadowMapMath.h"
#include "engine/lighting/LightingProbe.h"

#include "engine/rhi/GfxContext.h"

#include <glm/gtc/matrix_transform.hpp>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>

namespace
{
    std::string FormatVec3(const glm::vec3& value)
    {
        std::ostringstream stream;
        stream << std::fixed << std::setprecision(4)
               << "(" << value.x << ", " << value.y << ", " << value.z << ")";
        return stream.str();
    }

    glm::vec3 GetPrimaryLightDirection(const Scene& scene)
    {
        const SceneLighting& lighting = scene.GetRenderer().GetLighting();
        const int shadowLightIndex = lighting.GetShadowLightIndex();
        if (shadowLightIndex >= 0 &&
            static_cast<std::size_t>(shadowLightIndex) < lighting.GetLightCount())
        {
            return lighting.GetLights()[static_cast<std::size_t>(shadowLightIndex)].GetDirection();
        }

        for (const Light& light : lighting.GetLights())
        {
            if (light.GetType() == LightType::Directional)
            {
                return light.GetDirection();
            }
        }

        return glm::vec3(0.0f, 1.0f, 0.0f);
    }

    void ComputeShadowSceneBounds(const Scene& scene, glm::vec3& boundsMin, glm::vec3& boundsMax)
    {
        if (!scene.GetRenderer().ComputeShadowCasterBounds(scene, boundsMin, boundsMax))
        {
            boundsMin = glm::vec3(0.0f);
            boundsMax = glm::vec3(0.0f);
        }
    }

    void WriteObjectShadowAnalysis(
        std::ostream& out,
        const Scene& scene,
        int objectIndex,
        const glm::mat4& lightSpaceMatrix,
        const glm::vec3& lightDirection)
    {
        const SceneObject& object = scene.GetObject(static_cast<std::size_t>(objectIndex));
        glm::vec3 boundsMin;
        glm::vec3 boundsMax;
        scene.GetWorldBounds(objectIndex, boundsMin, boundsMax);

        out << "Object: " << object.GetName() << " (index " << objectIndex << ")\n";
        out << "  World bounds min: " << FormatVec3(boundsMin) << "\n";
        out << "  World bounds max: " << FormatVec3(boundsMax) << "\n";
        out << "  Casts shadow: " << (object.CastsShadow() ? "yes" : "no") << "\n";
        out << "  Receives shadow: " << (object.ReceivesShadow() ? "yes" : "no") << "\n";
        out << "  Albedo: " << FormatVec3(object.GetMaterial().GetAlbedo()) << "\n";
        out << "  Roughness: " << object.GetMaterial().GetRoughness() << "\n";
        out << "  Has AO map: " << (object.GetMaterial().HasAoMap() ? "yes" : "no") << "\n";
        out << "  Has normal map: " << (object.GetMaterial().HasNormalMap() ? "yes" : "no") << "\n";

        const std::array<glm::vec3, 8> corners = {
            glm::vec3(boundsMin.x, boundsMin.y, boundsMin.z),
            glm::vec3(boundsMax.x, boundsMin.y, boundsMin.z),
            glm::vec3(boundsMin.x, boundsMax.y, boundsMin.z),
            glm::vec3(boundsMax.x, boundsMax.y, boundsMin.z),
            glm::vec3(boundsMin.x, boundsMin.y, boundsMax.z),
            glm::vec3(boundsMax.x, boundsMin.y, boundsMax.z),
            glm::vec3(boundsMin.x, boundsMax.y, boundsMax.z),
            glm::vec3(boundsMax.x, boundsMax.y, boundsMax.z),
        };

        glm::vec3 uvMin(std::numeric_limits<float>::max());
        glm::vec3 uvMax(std::numeric_limits<float>::lowest());
        out << "  Corner light-space NDC (xyz = uv + depth):\n";
        for (std::size_t cornerIndex = 0; cornerIndex < corners.size(); ++cornerIndex)
        {
            const glm::vec3 shadowNdc = WorldToShadowNdc(lightSpaceMatrix, corners[cornerIndex]);
            uvMin = glm::min(uvMin, shadowNdc);
            uvMax = glm::max(uvMax, shadowNdc);
            out << "    [" << cornerIndex << "] " << FormatVec3(shadowNdc) << "\n";
        }

        out << "  Shadow UV range: min " << FormatVec3(uvMin) << " max " << FormatVec3(uvMax) << "\n";

        const glm::vec3 faceNormals[6] = {
            glm::vec3(0.0f, 0.0f, -1.0f),
            glm::vec3(0.0f, 0.0f, 1.0f),
            glm::vec3(-1.0f, 0.0f, 0.0f),
            glm::vec3(1.0f, 0.0f, 0.0f),
            glm::vec3(0.0f, -1.0f, 0.0f),
            glm::vec3(0.0f, 1.0f, 0.0f),
        };
        const char* faceNames[6] = {"-Z", "+Z", "-X", "+X", "-Y", "+Y"};

        const glm::mat4 worldMatrix = scene.GetWorldMatrix(objectIndex);
        const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(worldMatrix)));
        out << "  Face sun alignment (world-space normal dot light direction):\n";
        for (int faceIndex = 0; faceIndex < 6; ++faceIndex)
        {
            const glm::vec3 worldNormal = glm::normalize(normalMatrix * faceNormals[faceIndex]);
            const float sunAlignment = glm::dot(worldNormal, glm::normalize(lightDirection));
            out << "    face " << faceNames[faceIndex] << ": " << sunAlignment;
            if (sunAlignment < 0.0f)
            {
                out << "  [shadowed side]";
            }
            out << "\n";
        }
    }
}

namespace RenderDiagnostics
{
    bool WriteReport(
        const Scene& scene,
        const Camera& camera,
        const int viewportWidth,
        const int viewportHeight,
        const std::string& outputPath,
        std::string& statusMessage)
    {
        namespace fs = std::filesystem;

        try
        {
            const fs::path outputFilePath(outputPath);
            if (outputFilePath.has_parent_path())
            {
                fs::create_directories(outputFilePath.parent_path());
            }

            std::ofstream out(outputPath, std::ios::trunc);
            if (!out.is_open())
            {
                statusMessage = "Failed to open " + outputPath;
                return false;
            }

            const auto now = std::chrono::system_clock::now();
            const std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
            out << "=== Render Diagnostics ===\n";
            out << "Timestamp: " << std::put_time(std::localtime(&nowTime), "%F %T") << "\n\n";

            out << "[Viewport]\n";
            out << "Size: " << viewportWidth << " x " << viewportHeight << "\n";
            out << "Camera position: " << FormatVec3(camera.GetPosition()) << "\n\n";

            scene.GetRenderer().PrepareGpuResources();
            if (!scene.GetRenderer().IsGpuResourcesReady())
            {
                statusMessage = scene.GetRenderer().GetGpuResourcesInitError();
                if (statusMessage.empty())
                {
                    statusMessage = "GPU resources are not initialized.";
                }
                return false;
            }

            glm::vec3 shadowBoundsMin;
            glm::vec3 shadowBoundsMax;
            ComputeShadowSceneBounds(scene, shadowBoundsMin, shadowBoundsMax);

            const glm::vec3 lightDirection = GetPrimaryLightDirection(scene);
            const DirectionalShadowSettings& shadowSettings = scene.GetRenderer().GetDirectionalShadowSettings();
            const CascadedShadowMap& shadowMap = scene.GetRenderer().GetShadowMap();
            const std::vector<float> cascadeSplits = ComputeCascadeSplitDistances(
                shadowSettings.GetCascadeCount(),
                camera.GetNearPlane(),
                ComputeShadowDrawDistance(camera.GetPosition(), shadowBoundsMin, shadowBoundsMax),
                shadowSettings.GetCascadeSplitLambda());

            out << "[Directional shadow cascades]\n";
            out << "Cascade count: " << shadowSettings.GetCascadeCount() << "\n";
            out << "Resolution per cascade: " << shadowMap.GetResolution() << "\n";
            out << "Split lambda: " << shadowSettings.GetCascadeSplitLambda() << "\n";
            out << "Filter mode: "
                << (shadowSettings.GetFilterMode() == DirectionalShadowFilterMode::PCSS ? "PCSS" : "PCF")
                << "\n";
            out << "Light direction: " << FormatVec3(lightDirection) << "\n";
            out << "Caster bounds min: " << FormatVec3(shadowBoundsMin) << "\n";
            out << "Caster bounds max: " << FormatVec3(shadowBoundsMax) << "\n";
            out << "Shadow draw distance: "
                << ComputeShadowDrawDistance(camera.GetPosition(), shadowBoundsMin, shadowBoundsMax)
                << "\n";
            out << "Cascade split distances: ";
            for (std::size_t splitIndex = 0; splitIndex < cascadeSplits.size(); ++splitIndex)
            {
                if (splitIndex > 0)
                {
                    out << ", ";
                }
                out << cascadeSplits[splitIndex];
            }
            out << "\n";

            const ShadowLightSpaceSetup legacySetup = BuildShadowLightSpace(
                lightDirection,
                shadowBoundsMin,
                shadowBoundsMax,
                CascadedShadowMap::DefaultResolution);
            out << "Legacy single-map texel size (full scene fit): "
                << legacySetup.texelWorldSizeX << " / " << legacySetup.texelWorldSizeY << "\n\n";

            out << "[Runtime cascade matrices (GPU path)]\n";
            const std::array<glm::mat4, CascadedShadowMap::MaxCascades>& runtimeMatrices =
                shadowMap.GetLightSpaceMatrices();
            const std::array<ShadowLightSpaceSetup, CascadedShadowMap::MaxCascades>& runtimeSetups =
                shadowMap.GetCascadeSetups();
            const std::array<float, CascadedShadowMap::MaxCascades>& runtimeSplits =
                shadowMap.GetCascadeEndSplits();
            const int activeCascadeCount = shadowMap.GetActiveCascadeCount();
            for (int cascadeIndex = 0; cascadeIndex < activeCascadeCount; ++cascadeIndex)
            {
                const ShadowLightSpaceSetup& setup = runtimeSetups[static_cast<std::size_t>(cascadeIndex)];
                out << "C" << cascadeIndex << " ortho " << setup.orthoWidth << " x " << setup.orthoHeight
                    << " m | frustum stable clipZ [" << setup.clipDepthContentMin << ", "
                    << setup.clipDepthContentMax << "]\n";
            }

            if (activeCascadeCount > 1)
            {
                const ShadowLightSpaceSetup& firstSetup = runtimeSetups[0];
                const ShadowLightSpaceSetup& lastSetup =
                    runtimeSetups[static_cast<std::size_t>(activeCascadeCount - 1)];
                const bool identicalDepthRange =
                    std::abs(firstSetup.clipDepthContentMin - lastSetup.clipDepthContentMin) < 1e-5f &&
                    std::abs(firstSetup.clipDepthContentMax - lastSetup.clipDepthContentMax) < 1e-5f;
                const bool oversizedNearOrtho = firstSetup.orthoWidth > 15.0f;
                if (identicalDepthRange && oversizedNearOrtho)
                {
                    out << "WARNING: Near cascades share full-scene ortho/depth range. "
                        << "Enable Frustum-only XY fit and move the camera >1.25 m to reset stable fit.\n";
                }
            }
            out << "Light-space depth debug shows raw stable clip Z in [0,1] (no normalization).\n\n";

            const glm::vec3 focusPoint = camera.GetPosition() + camera.GetFront() * 3.0f;
            const glm::vec4 viewFocus = camera.GetViewMatrix() * glm::vec4(focusPoint, 1.0f);
            const std::array<glm::vec3, 3> receiverProbePoints = {
                focusPoint,
                glm::vec3(0.0f, 0.0f, 0.0f),
                glm::vec3(camera.GetPosition().x, 0.0f, camera.GetPosition().z),
            };
            const char* receiverProbeNames[3] = {
                "focus-3m-ahead",
                "world-origin-floor",
                "floor-under-camera",
            };
            out << "[Light-space receiver probe (matches debug shader)]\n";
            for (std::size_t probeIndex = 0; probeIndex < receiverProbePoints.size(); ++probeIndex)
            {
                const glm::vec3& worldPoint = receiverProbePoints[probeIndex];
                const glm::vec4 viewPoint = camera.GetViewMatrix() * glm::vec4(worldPoint, 1.0f);
                const ShadowReceiverProbeResult probe = EvaluateShadowReceiverProbe(
                    worldPoint,
                    viewPoint.z,
                    runtimeMatrices.data(),
                    runtimeSetups.data(),
                    runtimeSplits.data(),
                    activeCascadeCount);
                out << receiverProbeNames[probeIndex] << " @ " << FormatVec3(worldPoint) << "\n";
                out << "  viewDepth=" << viewPoint.z << " cascade=C" << probe.cascadeIndex
                    << " inBounds=" << (probe.inBounds ? "yes" : "no") << "\n";
                out << "  clipZ=" << probe.receiverClipZ << " normalized=" << probe.normalizedClipZ
                    << " uv=" << FormatVec3(glm::vec3(probe.shadowUv, 0.0f)) << "\n";
            }
            out << "\n";

            const glm::mat4 inverseViewMatrix = glm::inverse(camera.GetViewMatrix());
            for (int cascadeIndex = 0; cascadeIndex < shadowSettings.GetCascadeCount(); ++cascadeIndex)
            {
                const float cascadeNear = cascadeSplits[static_cast<std::size_t>(cascadeIndex)];
                const float cascadeFar = cascadeSplits[static_cast<std::size_t>(cascadeIndex + 1)];
                const std::array<glm::vec3, 8> frustumCorners = ComputeCascadeFrustumCorners(
                    inverseViewMatrix,
                    camera.GetAspect(),
                    camera.GetFov(),
                    cascadeNear,
                    cascadeFar);
                const ShadowLightSpaceSetup cascadeSetup = BuildShadowLightSpace(
                    lightDirection,
                    ComputeBoundsMin(frustumCorners),
                    ComputeBoundsMax(frustumCorners),
                    CascadedShadowMap::DefaultResolution);

                out << "[Cascade " << cascadeIndex << "]\n";
                out << "Near / far (view depth): " << cascadeNear << " / " << cascadeFar << "\n";
                out << "Ortho width / height (world units): "
                    << cascadeSetup.orthoWidth << " / " << cascadeSetup.orthoHeight << "\n";
                out << "Texel size (world units / texel): "
                    << cascadeSetup.texelWorldSizeX << " / " << cascadeSetup.texelWorldSizeY << "\n";
                out << "Texel span used for bias: "
                    << std::max(cascadeSetup.texelWorldSizeX, cascadeSetup.texelWorldSizeY) << "\n";
                out << "Snap offset (NDC): "
                    << FormatVec3(glm::vec3(cascadeSetup.snapOffsetNdc, 0.0f)) << "\n\n";
            }

            const ShadowLightSpaceSetup shadowSetup = BuildShadowLightSpace(
                lightDirection,
                shadowBoundsMin,
                shadowBoundsMax,
                CascadedShadowMap::DefaultResolution);

            const ScreenSpaceEffects& effects = scene.GetRenderer().GetScreenSpaceEffects();
            out << "[Screen-space effects]\n";
            out << "Post-processing enabled: " << (effects.IsEnabled() ? "yes" : "no") << "\n";
            out << "SSAO enabled: " << (effects.IsSsaoEnabled() ? "yes" : "no") << "\n";
            out << "SSAO radius: " << effects.GetSsaoRadius() << "\n";
            out << "SSAO bias: " << effects.GetSsaoBias() << "\n";
            out << "AO strength: " << effects.GetAoStrength() << "\n";
            out << "Debug view: " << RenderDebugModeLabel(effects.GetDebugMode()) << "\n\n";

            std::uint32_t srvUsed = 0;
            std::uint32_t srvCapacity = 0;
            GfxContext::Get().GetSrvDescriptorUsage(srvUsed, srvCapacity);
            out << "[GPU descriptors]\n";
            out << "SRV heap used: " << srvUsed << " / " << srvCapacity << "\n";
            if (!GfxContext::GetLastGpuAllocationError().empty())
            {
                out << "Last GPU allocation error: " << GfxContext::GetLastGpuAllocationError() << "\n";
            }
            out << "\n";

            out << "[Scene object transforms]\n";
            const std::vector<SceneObject>& objects = scene.GetObjects();
            for (std::size_t objectIndex = 0; objectIndex < objects.size(); ++objectIndex)
            {
                const SceneObject& object = objects[objectIndex];
                const glm::mat4 worldMatrix = scene.GetWorldMatrix(static_cast<int>(objectIndex));
                const glm::vec3 worldPosition = glm::vec3(worldMatrix[3]);
                out << "  [" << objectIndex << "] \"" << object.GetName() << "\""
                    << " pos=" << FormatVec3(worldPosition)
                    << " mesh=" << (object.HasMesh() ? "yes" : "no")
                    << " import=" << (object.GetImportAssetPath().empty() ? "no" : "yes") << "\n";
            }
            out << "\n";

            const int selectedIndex = scene.GetPrimarySelection();
            out << "[Selected object shadow analysis]\n";
            if (selectedIndex < 0)
            {
                out << "No object selected. Select the cube and re-run diagnostics.\n\n";
            }
            else
            {
                WriteObjectShadowAnalysis(
                    out,
                    scene,
                    selectedIndex,
                    shadowSetup.lightSpaceMatrix,
                    lightDirection);
                out << "\n";
            }

            const std::array<glm::vec3, 3> probePoints = {
                glm::vec3(0.0f, 2.0f, 0.0f),
                glm::vec3(0.0f, 0.5f, 0.0f),
                glm::vec3(0.0f, 0.0f, 0.0f),
            };
            const char* probeNames[3] = {"cube-top", "sphere-center", "floor"};
            const std::array<glm::vec3, 3> probeNormals = {
                glm::vec3(0.0f, 1.0f, 0.0f),
                glm::vec3(0.0f, 1.0f, 0.0f),
                glm::vec3(0.0f, 1.0f, 0.0f),
            };

            out << "[Lighting probe sweep (8 orbit cameras)]\n";
            out << "Columns: viewDepth, cascade, blend, sunDotGeom\n";
            for (std::size_t probeIndex = 0; probeIndex < probePoints.size(); ++probeIndex)
            {
                out << "Probe " << probeNames[probeIndex] << " @ " << FormatVec3(probePoints[probeIndex]) << "\n";
                for (int step = 0; step < 8; ++step)
                {
                    const float angle = glm::two_pi<float>() * (static_cast<float>(step) / 8.0f);
                    const glm::vec3 eye(
                        std::cos(angle) * 6.0f,
                        3.5f,
                        std::sin(angle) * 6.0f);
                    const glm::mat4 viewMatrix = glm::lookAtLH(
                        eye,
                        probePoints[probeIndex],
                        glm::vec3(0.0f, 1.0f, 0.0f));

                    const LightingProbeResult probe = EvaluateLightingProbe(
                        viewMatrix,
                        probePoints[probeIndex],
                        probeNormals[probeIndex],
                        lightDirection,
                        cascadeSplits,
                        camera.GetNearPlane(),
                        shadowSettings.GetCascadeBlendRatio(),
                        shadowSettings.GetCascadeCount());

                    out << "  cam" << step
                        << " eye=" << FormatVec3(eye)
                        << " viewDepth=" << probe.viewDepth
                        << " cascade=" << probe.cascadeIndex
                        << " blend=" << probe.cascadeBlendFactor
                        << " sunDotGeom=" << probe.sunDotGeomNormal
                        << "\n";
                }
                out << "\n";
            }

            out << "[How to isolate the artifact]\n";
            out << "Lighting model: color = direct * shadowFactor + indirect * SSAO\n";
            out << "1. Shadow factor / Cascade blend factor: shadow map and cascade seams\n";
            out << "2. Direct lighting: full Cook-Torrance (includes normal maps)\n";
            out << "3. Direct diffuse (geom N·L): sun on geometric normals only\n";
            out << "4. Diffuse IBL / Specular IBL: split indirect lighting\n";
            out << "5. Shaded normal vs Geometric normal: normal-map / TBN issues\n";
            out << "6. View depth / Cascade index: split selection diagnostics\n";
            out << "7. SSAO buffer / Composite occlusion: screen-space AO on indirect\n";

            statusMessage = "Wrote " + fs::absolute(outputFilePath).string();
            return true;
        }
        catch (const std::exception& exception)
        {
            statusMessage = exception.what();
            return false;
        }
    }
}
