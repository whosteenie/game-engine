#pragma once

#include "engine/Light.h"

#include <glm/glm.hpp>
#include <vector>

class Shader;

class SceneLighting
{
public:
    static constexpr int MaxLights = 8;

    void AddLight(const Light& light);
    void ClearLights();

    const std::vector<Light>& GetLights() const;

    int GetShadowLightIndex() const;
    void SetShadowLightIndex(int shadowLightIndex);

    void Apply(Shader& shader) const;

private:
    std::vector<Light> m_lights;
    int m_shadowLightIndex = -1;
};
