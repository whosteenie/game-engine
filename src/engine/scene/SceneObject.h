#pragma once

#include "engine/components/CameraComponent.h"
#include "engine/components/ColliderComponent.h"
#include "engine/scene/InspectorComponentOrder.h"
#include "engine/components/LightComponent.h"
#include "engine/components/RigidBodyComponent.h"
#include "engine/rendering/resources/Material.h"
#include "engine/scene/SceneObjectId.h"
#include "engine/scene/Transform.h"

#include <glm/glm.hpp>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class Mesh;

class SceneObject
{
public:
    SceneObject(
        std::string name,
        Mesh* mesh,
        std::unique_ptr<Material> material,
        const glm::vec3& localBoundsMin,
        const glm::vec3& localBoundsMax,
        Transform transform = Transform{},
        bool castShadow = true,
        bool receiveShadow = true,
        int parentIndex = -1,
        int siblingOrder = 0,
        std::optional<LightComponent> light = std::nullopt,
        std::optional<CameraComponent> camera = std::nullopt,
        std::optional<RigidBodyComponent> rigidBody = std::nullopt,
        std::optional<ColliderComponent> collider = std::nullopt,
        SceneObjectId id = kInvalidSceneObjectId);

    SceneObjectId GetId() const;
    void SetId(SceneObjectId id);

    const std::string& GetName() const;
    void SetName(const std::string& name);

    Transform& GetTransform();
    const Transform& GetTransform() const;

    Material& GetMaterial();
    const Material& GetMaterial() const;
    void ReplaceMaterial(std::unique_ptr<Material> material);

    Mesh* GetMesh() const;

    bool HasMesh() const;
    bool HasMaterial() const;
    bool IsRenderable() const;

    bool HasLight() const;
    LightComponent& GetLight();
    const LightComponent& GetLight() const;
    void SetLight(LightComponent light);
    void ClearLight();

    bool HasCamera() const;
    CameraComponent& GetCamera();
    const CameraComponent& GetCamera() const;
    void SetCamera(CameraComponent camera);
    void ClearCamera();

    bool HasRigidBody() const;
    RigidBodyComponent& GetRigidBody();
    const RigidBodyComponent& GetRigidBody() const;
    void SetRigidBody(RigidBodyComponent rigidBody);
    void ClearRigidBody();

    bool HasCollider() const;
    ColliderComponent& GetCollider();
    const ColliderComponent& GetCollider() const;
    void SetCollider(ColliderComponent collider);
    void ClearCollider();

    int GetParentIndex() const;
    void SetParentIndex(int parentIndex);

    int GetSiblingOrder() const;
    void SetSiblingOrder(int siblingOrder);

    bool CastsShadow() const;
    void SetCastShadow(bool castShadow);

    bool ReceivesShadow() const;
    void SetReceiveShadow(bool receiveShadow);

    void GetWorldBounds(const glm::mat4& worldMatrix, glm::vec3& boundsMin, glm::vec3& boundsMax) const;

    const glm::vec3& GetLocalBoundsMin() const;
    const glm::vec3& GetLocalBoundsMax() const;

    const std::string& GetImportAssetPath() const;
    int GetImportNodeIndex() const;
    void SetImportSource(const std::string& assetPath, int nodeIndex);
    void ClearImportSource();

    const std::vector<InspectorComponentType>& GetInspectorComponentOrder() const;
    std::vector<InspectorComponentType> GetEffectiveInspectorComponentOrder() const;
    void SetInspectorComponentOrder(std::vector<InspectorComponentType> order);

private:
    SceneObjectId m_id = kInvalidSceneObjectId;
    std::string m_name;
    Mesh* m_mesh = nullptr;
    std::unique_ptr<Material> m_material;
    Transform m_transform;
    glm::vec3 m_localBoundsMin;
    glm::vec3 m_localBoundsMax;
    int m_parentIndex = -1;
    int m_siblingOrder = 0;
    bool m_castShadow = true;
    bool m_receiveShadow = true;
    std::optional<LightComponent> m_light;
    std::optional<CameraComponent> m_camera;
    std::optional<RigidBodyComponent> m_rigidBody;
    std::optional<ColliderComponent> m_collider;
    std::vector<InspectorComponentType> m_inspectorComponentOrder;
    std::string m_importAssetPath;
    int m_importNodeIndex = -1;
};
