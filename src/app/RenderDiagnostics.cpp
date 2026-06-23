#include "app/RenderDiagnostics.h"

#include "app/Scene.h"
#include "engine/Camera.h"
#include "engine/Light.h"
#include "engine/RenderDebug.h"
#include "engine/SceneLighting.h"
#include "engine/SceneObject.h"
#include "engine/ScreenSpaceEffects.h"
#include "engine/CascadedShadowMap.h"
#include "engine/ShadowMapMath.h"

#include <glm/gtc/matrix_inverse.hpp>
#include <chrono>
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
        const SceneLighting& lighting = scene.GetLighting();
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
        boundsMin = glm::vec3(std::numeric_limits<float>::max());
        boundsMax = glm::vec3(std::numeric_limits<float>::lowest());

        for (std::size_t objectIndex = 0; objectIndex < scene.GetObjects().size(); ++objectIndex)
        {
            const SceneObject& object = scene.GetObjects()[objectIndex];
            if (!object.CastsShadow() && !object.ReceivesShadow())
            {
                continue;
            }

            glm::vec3 objectBoundsMin;
            glm::vec3 objectBoundsMax;
            scene.GetWorldBounds(static_cast<int>(objectIndex), objectBoundsMin, objectBoundsMax);
            boundsMin = glm::min(boundsMin, objectBoundsMin);
            boundsMax = glm::max(boundsMax, objectBoundsMax);
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

            glm::vec3 shadowBoundsMin;
            glm::vec3 shadowBoundsMax;
            ComputeShadowSceneBounds(scene, shadowBoundsMin, shadowBoundsMax);

            const glm::vec3 lightDirection = GetPrimaryLightDirection(scene);
            const std::vector<float> cascadeSplits = ComputeCascadeSplitDistances(
                CascadedShadowMap::CascadeCount,
                camera.GetNearPlane(),
                ComputeShadowDrawDistance(camera.GetPosition(), shadowBoundsMin, shadowBoundsMax));

            out << "[Directional shadow cascades]\n";
            out << "Cascade count: " << CascadedShadowMap::CascadeCount << "\n";
            out << "Resolution per cascade: " << CascadedShadowMap::DefaultResolution << "\n";
            out << "Light direction: " << FormatVec3(lightDirection) << "\n";
            out << "Scene shadow bounds min: " << FormatVec3(shadowBoundsMin) << "\n";
            out << "Scene shadow bounds max: " << FormatVec3(shadowBoundsMax) << "\n";
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

            const glm::mat4 inverseViewMatrix = glm::inverse(camera.GetViewMatrix());
            for (int cascadeIndex = 0; cascadeIndex < CascadedShadowMap::CascadeCount; ++cascadeIndex)
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

            const ScreenSpaceEffects& effects = scene.GetScreenSpaceEffects();
            out << "[Screen-space effects]\n";
            out << "Post-processing enabled: " << (effects.IsEnabled() ? "yes" : "no") << "\n";
            out << "SSAO enabled: " << (effects.IsSsaoEnabled() ? "yes" : "no") << "\n";
            out << "SSAO radius: " << effects.GetSsaoRadius() << "\n";
            out << "SSAO bias: " << effects.GetSsaoBias() << "\n";
            out << "AO strength: " << effects.GetAoStrength() << "\n";
            out << "Debug view: " << RenderDebugModeLabel(effects.GetDebugMode()) << "\n\n";

            const int selectedIndex = scene.GetSelectedObjectIndex();
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

            out << "[How to isolate the artifact]\n";
            out << "Lighting model: color = direct + indirect * ambientOcclusion\n";
            out << "1. Shadow factor (PBR): shadow map / PCF issues\n";
            out << "2. Direct lighting (PBR): sun/specular only\n";
            out << "3. Ambient / IBL (PBR): indirect before SSAO\n";
            out << "4. SSAO buffer: screen-space AO applied to indirect only\n";
            out << "5. Composite occlusion: SSAO multiplier on indirect\n";

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
