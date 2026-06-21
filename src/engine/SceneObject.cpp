#include "engine/SceneObject.h"

#include "engine/Mesh.h"

#include <imgui.h>
#include <ImGuizmo.h>

#include <array>
#include <glm/gtc/type_ptr.hpp>
#include <limits>
#include <utility>

namespace
{
    glm::mat3 NormalizeColumns(const glm::mat3& matrix)
    {
        return glm::mat3(
            glm::normalize(glm::vec3(matrix[0])),
            glm::normalize(glm::vec3(matrix[1])),
            glm::normalize(glm::vec3(matrix[2])));
    }
}

SceneObject::SceneObject(
    std::string name,
    Mesh* mesh,
    std::unique_ptr<Material> material,
    const glm::vec3& localBoundsMin,
    const glm::vec3& localBoundsMax,
    Transform transform,
    bool movable,
    bool autoSpin,
    bool castShadow,
    bool receiveShadow)
    : m_name(std::move(name)),
      m_mesh(mesh),
      m_material(std::move(material)),
      m_transform(transform),
      m_localBoundsMin(localBoundsMin),
      m_localBoundsMax(localBoundsMax),
      m_movable(movable),
      m_autoSpin(autoSpin),
      m_castShadow(castShadow),
      m_receiveShadow(receiveShadow)
{
}

const std::string& SceneObject::GetName() const
{
    return m_name;
}

void SceneObject::SetName(const std::string& name)
{
    m_name = name;
}

Transform& SceneObject::GetTransform()
{
    return m_transform;
}

const Transform& SceneObject::GetTransform() const
{
    return m_transform;
}

Material& SceneObject::GetMaterial()
{
    return *m_material;
}

const Material& SceneObject::GetMaterial() const
{
    return *m_material;
}

Mesh* SceneObject::GetMesh() const
{
    return m_mesh;
}

bool SceneObject::IsMovable() const
{
    return m_movable;
}

void SceneObject::SetMovable(bool movable)
{
    m_movable = movable;
}

bool SceneObject::HasAutoSpin() const
{
    return m_autoSpin;
}

void SceneObject::SetAutoSpin(bool autoSpin)
{
    m_autoSpin = autoSpin;
}

bool SceneObject::CastsShadow() const
{
    return m_castShadow;
}

void SceneObject::SetCastShadow(bool castShadow)
{
    m_castShadow = castShadow;
}

bool SceneObject::ReceivesShadow() const
{
    return m_receiveShadow;
}

void SceneObject::SetReceiveShadow(bool receiveShadow)
{
    m_receiveShadow = receiveShadow;
}

glm::mat4 SceneObject::BuildModelMatrix(double animationTime) const
{
    return m_transform.ToMatrix(animationTime, m_autoSpin);
}

glm::mat4 SceneObject::BuildEditMatrix() const
{
    return m_transform.ToMatrix(0.0, false);
}

void SceneObject::ApplyTransformFromMatrix(const glm::mat4& matrix)
{
    float translation[3];
    float rotationDegrees[3];
    float scale[3];
    ImGuizmo::DecomposeMatrixToComponents(glm::value_ptr(matrix), translation, rotationDegrees, scale);

    m_transform.position = glm::make_vec3(translation);
    m_transform.rotationDegrees = glm::make_vec3(rotationDegrees);
    m_transform.scale = glm::make_vec3(scale);
}

glm::vec3 SceneObject::GetWorldPivot(double animationTime) const
{
    const glm::mat4 modelMatrix = BuildModelMatrix(animationTime);
    return glm::vec3(modelMatrix[3]);
}

void SceneObject::GetWorldBounds(double animationTime, glm::vec3& boundsMin, glm::vec3& boundsMax) const
{
    const glm::mat4 modelMatrix = BuildModelMatrix(animationTime);
    const std::array<glm::vec3, 8> corners = {
        glm::vec3(m_localBoundsMin.x, m_localBoundsMin.y, m_localBoundsMin.z),
        glm::vec3(m_localBoundsMax.x, m_localBoundsMin.y, m_localBoundsMin.z),
        glm::vec3(m_localBoundsMin.x, m_localBoundsMax.y, m_localBoundsMin.z),
        glm::vec3(m_localBoundsMax.x, m_localBoundsMax.y, m_localBoundsMin.z),
        glm::vec3(m_localBoundsMin.x, m_localBoundsMin.y, m_localBoundsMax.z),
        glm::vec3(m_localBoundsMax.x, m_localBoundsMin.y, m_localBoundsMax.z),
        glm::vec3(m_localBoundsMin.x, m_localBoundsMax.y, m_localBoundsMax.z),
        glm::vec3(m_localBoundsMax.x, m_localBoundsMax.y, m_localBoundsMax.z),
    };

    boundsMin = glm::vec3(std::numeric_limits<float>::max());
    boundsMax = glm::vec3(std::numeric_limits<float>::lowest());

    for (const glm::vec3& corner : corners)
    {
        const glm::vec4 worldCorner = modelMatrix * glm::vec4(corner, 1.0f);
        const glm::vec3 worldPosition = glm::vec3(worldCorner);
        boundsMin = glm::min(boundsMin, worldPosition);
        boundsMax = glm::max(boundsMax, worldPosition);
    }
}

glm::mat3 SceneObject::GetLocalAxisMatrix(double animationTime) const
{
    if (m_autoSpin)
    {
        return glm::mat3(1.0f);
    }

    return NormalizeColumns(glm::mat3(BuildModelMatrix(animationTime)));
}

const glm::vec3& SceneObject::GetLocalBoundsMin() const
{
    return m_localBoundsMin;
}

const glm::vec3& SceneObject::GetLocalBoundsMax() const
{
    return m_localBoundsMax;
}
