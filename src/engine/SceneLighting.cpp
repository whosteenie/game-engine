#include "engine/SceneLighting.h"

#include "engine/Shader.h"

#include <stdexcept>

void SceneLighting::AddLight(const Light& light)
{
    if (static_cast<int>(m_lights.size()) >= MaxLights)
    {
        throw std::runtime_error("SceneLighting exceeds MaxLights");
    }

    m_lights.push_back(light);
}

void SceneLighting::ClearLights()
{
    m_lights.clear();
}

const std::vector<Light>& SceneLighting::GetLights() const
{
    return m_lights;
}

int SceneLighting::GetShadowLightIndex() const
{
    return m_shadowLightIndex;
}

void SceneLighting::SetShadowLightIndex(int shadowLightIndex)
{
    m_shadowLightIndex = shadowLightIndex;
}

void SceneLighting::Apply(Shader& shader) const
{
    const int lightCount = static_cast<int>(m_lights.size());

    int lightTypes[MaxLights]{};
    glm::vec3 lightPositions[MaxLights]{};
    glm::vec3 lightDirections[MaxLights]{};
    glm::vec3 lightColors[MaxLights]{};
    float lightIntensities[MaxLights]{};
    float lightAttenConstant[MaxLights]{};
    float lightAttenLinear[MaxLights]{};
    float lightAttenQuadratic[MaxLights]{};
    float lightRange[MaxLights]{};
    float lightInnerCutoffCos[MaxLights]{};
    float lightOuterCutoffCos[MaxLights]{};

    for (int i = 0; i < lightCount; ++i)
    {
        const Light& light = m_lights[static_cast<size_t>(i)];
        lightTypes[i] = static_cast<int>(light.GetType());
        lightPositions[i] = light.GetPosition();
        lightDirections[i] = light.GetDirection();
        lightColors[i] = light.GetColor();
        lightIntensities[i] = light.GetIntensity();
        lightAttenConstant[i] = light.GetConstantAttenuation();
        lightAttenLinear[i] = light.GetLinearAttenuation();
        lightAttenQuadratic[i] = light.GetQuadraticAttenuation();
        lightRange[i] = light.GetRange();
        lightInnerCutoffCos[i] = light.GetInnerCutoffCos();
        lightOuterCutoffCos[i] = light.GetOuterCutoffCos();
    }

    shader.SetInt("uLightCount", lightCount);
    shader.SetIntArray("uLightTypes", lightTypes, lightCount);
    shader.SetVec3Array("uLightPositions", lightPositions, lightCount);
    shader.SetVec3Array("uLightDirections", lightDirections, lightCount);
    shader.SetVec3Array("uLightColors", lightColors, lightCount);
    shader.SetFloatArray("uLightIntensities", lightIntensities, lightCount);
    shader.SetFloatArray("uLightAttenConstant", lightAttenConstant, lightCount);
    shader.SetFloatArray("uLightAttenLinear", lightAttenLinear, lightCount);
    shader.SetFloatArray("uLightAttenQuadratic", lightAttenQuadratic, lightCount);
    shader.SetFloatArray("uLightRange", lightRange, lightCount);
    shader.SetFloatArray("uLightInnerCutoffCos", lightInnerCutoffCos, lightCount);
    shader.SetFloatArray("uLightOuterCutoffCos", lightOuterCutoffCos, lightCount);
    shader.SetInt("uShadowLightIndex", m_shadowLightIndex);
}
