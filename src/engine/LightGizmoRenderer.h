#pragma once

class Camera;
class SceneLighting;
class Shader;

class LightGizmoRenderer
{
public:
    LightGizmoRenderer();
    ~LightGizmoRenderer();

    void Draw(const Camera& camera, const SceneLighting& lighting, int selectedLightIndex) const;

private:
    Shader* m_shader = nullptr;
    mutable unsigned int m_vao = 0;
    mutable unsigned int m_vbo = 0;
};
