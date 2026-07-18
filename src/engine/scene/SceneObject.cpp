#include "engine/scene/SceneObject.h"

#include "engine/scene/InspectorComponentOrder.h"
#include "engine/rendering/resources/Mesh.h"

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
    std::optional<CameraComponent> camera,
    std::optional<RigidBodyComponent> rigidBody,
    std::optional<ColliderComponent> collider,
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
      m_light(std::move(light)),
      m_camera(std::move(camera)),
      m_rigidBody(std::move(rigidBody)),
      m_collider(std::move(collider))
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

void SceneObject::ReplaceMaterial(std::unique_ptr<Material> material)
{
    m_material = std::move(material);
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

bool SceneObject::HasCamera() const
{
    return m_camera.has_value();
}

CameraComponent& SceneObject::GetCamera()
{
    if (!m_camera.has_value())
    {
        throw std::logic_error("SceneObject has no camera component.");
    }

    return *m_camera;
}

const CameraComponent& SceneObject::GetCamera() const
{
    if (!m_camera.has_value())
    {
        throw std::logic_error("SceneObject has no camera component.");
    }

    return *m_camera;
}

void SceneObject::SetCamera(CameraComponent camera)
{
    m_camera = std::move(camera);
}

void SceneObject::ClearCamera()
{
    m_camera.reset();
}

bool SceneObject::HasRigidBody() const
{
    return m_rigidBody.has_value();
}

RigidBodyComponent& SceneObject::GetRigidBody()
{
    if (!m_rigidBody.has_value())
    {
        throw std::logic_error("SceneObject has no rigid body component.");
    }

    return *m_rigidBody;
}

const RigidBodyComponent& SceneObject::GetRigidBody() const
{
    if (!m_rigidBody.has_value())
    {
        throw std::logic_error("SceneObject has no rigid body component.");
    }

    return *m_rigidBody;
}

void SceneObject::SetRigidBody(RigidBodyComponent rigidBody)
{
    m_rigidBody = std::move(rigidBody);
}

void SceneObject::ClearRigidBody()
{
    m_rigidBody.reset();
}

bool SceneObject::HasCollider() const
{
    return m_collider.has_value();
}

ColliderComponent& SceneObject::GetCollider()
{
    if (!m_collider.has_value())
    {
        throw std::logic_error("SceneObject has no collider component.");
    }

    return *m_collider;
}

const ColliderComponent& SceneObject::GetCollider() const
{
    if (!m_collider.has_value())
    {
        throw std::logic_error("SceneObject has no collider component.");
    }

    return *m_collider;
}

void SceneObject::SetCollider(ColliderComponent collider)
{
    m_collider = std::move(collider);
}

void SceneObject::ClearCollider()
{
    m_collider.reset();
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

const std::vector<InspectorComponentType>& SceneObject::GetInspectorComponentOrder() const
{
    return m_inspectorComponentOrder;
}

std::vector<InspectorComponentType> SceneObject::GetEffectiveInspectorComponentOrder() const
{
    std::vector<InspectorComponentType> order = m_inspectorComponentOrder;
    if (order.empty())
    {
        order = BuildDefaultInspectorComponentOrder(*this);
    }

    NormalizeInspectorComponentOrder(order, *this);
    return order;
}

void SceneObject::SetInspectorComponentOrder(std::vector<InspectorComponentType> order)
{
    NormalizeInspectorComponentOrder(order, *this);
    m_inspectorComponentOrder = std::move(order);
}
