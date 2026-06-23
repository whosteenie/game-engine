#pragma once

#include <glm/glm.hpp>

#include <functional>
#include <memory>
#include <vector>

class Camera;
class SceneObject;
class Shader;

class CameraGizmoRenderer
{
public:
    CameraGizmoRenderer();
    ~CameraGizmoRenderer();

    void Draw(
        const Camera& camera,
        const std::vector<SceneObject>& objects,
        const std::function<glm::mat4(int objectIndex)>& getWorldMatrix,
        const std::vector<int>& selectedObjectIndices) const;

private:
    std::unique_ptr<Shader> m_shader;
    mutable unsigned int m_vao = 0;
    mutable unsigned int m_vbo = 0;
};
