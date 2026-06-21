#pragma once

class Camera;
class SceneObject;
class Shader;

class SelectionRenderer
{
public:
    SelectionRenderer();
    ~SelectionRenderer();

    void Draw(const Camera& camera, const SceneObject& object) const;

private:
    Shader* m_shader = nullptr;
    mutable unsigned int m_vao = 0;
    mutable unsigned int m_vbo = 0;
};
