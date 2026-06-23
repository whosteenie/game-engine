#include "engine/SceneObject.h"

#include "engine/Mesh.h"

#include <array>
#include <limits>
#include <stdexcept>
#include <utility>

SceneObject::SceneObject(
    std::string name,
    Mesh* mesh,
    std::unique_ptr<Material> material,
    const glm::vec3& localBoundsMin,
    const glm::vec3& localBoundsMax,
    Transform transform,
    bool castShadow,
    bool receiveShadow,
    int parentIndex,
    int siblingOrder,
    std::optional<LightComponent> light,
    SceneObjectId id)
    : m_id(id),
      m_name(std::move(name)),
      m_mesh(mesh),
      m_material(std::move(material)),
      m_transform(transform),
      m_localBoundsMin(localBoundsMin),
      m_localBoundsMax(localBoundsMax),
      m_parentIndex(parentIndex),
      m_siblingOrder(siblingOrder),
      m_castShadow(castShadow),
      m_receiveShadow(receiveShadow),
      m_light(std::move(light))
{
}

SceneObjectId SceneObject::GetId() const
{
    return m_id;
}

void SceneObject::SetId(SceneObjectId id)
{
    m_id = id;
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
    if (m_material == nullptr)
    {
        throw std::logic_error("SceneObject has no material.");
    }

    return *m_material;
}

const Material& SceneObject::GetMaterial() const
{
    if (m_material == nullptr)
    {
        throw std::logic_error("SceneObject has no material.");
    }

    return *m_material;
}

Mesh* SceneObject::GetMesh() const
{
    return m_mesh;
}

bool SceneObject::HasMesh() const
{
    return m_mesh != nullptr;
}

bool SceneObject::HasMaterial() const
{
    return m_material != nullptr;
}

bool SceneObject::IsRenderable() const
{
    return m_mesh != nullptr && m_material != nullptr;
}

bool SceneObject::HasLight() const
{
    return m_light.has_value();
}

LightComponent& SceneObject::GetLight()
{
    if (!m_light.has_value())
    {
        throw std::logic_error("SceneObject has no light component.");
    }

    return *m_light;
}

const LightComponent& SceneObject::GetLight() const
{
    if (!m_light.has_value())
    {
        throw std::logic_error("SceneObject has no light component.");
    }

    return *m_light;
}

void SceneObject::SetLight(LightComponent light)
{
    m_light = std::move(light);
}

void SceneObject::ClearLight()
{
    m_light.reset();
}

int SceneObject::GetParentIndex() const
{
    return m_parentIndex;
}

void SceneObject::SetParentIndex(int parentIndex)
{
    m_parentIndex = parentIndex;
}

int SceneObject::GetSiblingOrder() const
{
    return m_siblingOrder;
}

void SceneObject::SetSiblingOrder(int siblingOrder)
{
    m_siblingOrder = siblingOrder;
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

void SceneObject::GetWorldBounds(
    const glm::mat4& worldMatrix,
    glm::vec3& boundsMin,
    glm::vec3& boundsMax) const
{
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
        const glm::vec4 worldCorner = worldMatrix * glm::vec4(corner, 1.0f);
        const glm::vec3 worldPosition = glm::vec3(worldCorner);
        boundsMin = glm::min(boundsMin, worldPosition);
        boundsMax = glm::max(boundsMax, worldPosition);
    }
}

const glm::vec3& SceneObject::GetLocalBoundsMin() const
{
    return m_localBoundsMin;
}

const glm::vec3& SceneObject::GetLocalBoundsMax() const
{
    return m_localBoundsMax;
}

const std::string& SceneObject::GetImportAssetPath() const
{
    return m_importAssetPath;
}

int SceneObject::GetImportNodeIndex() const
{
    return m_importNodeIndex;
}

void SceneObject::SetImportSource(const std::string& assetPath, int nodeIndex)
{
    m_importAssetPath = assetPath;
    m_importNodeIndex = nodeIndex;
}

void SceneObject::ClearImportSource()
{
    m_importAssetPath.clear();
    m_importNodeIndex = -1;
}
