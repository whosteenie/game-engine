#include "engine/SceneObject.h"

#include "engine/Mesh.h"

#include <utility>

SceneObject::SceneObject(
    std::string name,
    Mesh* mesh,
    std::unique_ptr<Material> material,
    Transform transform,
    bool movable,
    bool autoSpin,
    bool castShadow,
    bool receiveShadow)
    : m_name(std::move(name)),
      m_mesh(mesh),
      m_material(std::move(material)),
      m_transform(transform),
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
