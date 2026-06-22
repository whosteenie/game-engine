#pragma once

#include "engine/LightComponent.h"
#include "engine/Material.h"
#include "engine/Transform.h"

#include <glm/glm.hpp>
#include <memory>
#include <optional>
#include <string>

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
        bool movable = true,
        bool castShadow = true,
        bool receiveShadow = true,
        int parentIndex = -1,
        int siblingOrder = 0,
        std::optional<LightComponent> light = std::nullopt);

    const std::string& GetName() const;
    void SetName(const std::string& name);

    Transform& GetTransform();
    const Transform& GetTransform() const;

    Material& GetMaterial();
    const Material& GetMaterial() const;

    Mesh* GetMesh() const;

    bool HasMesh() const;
    bool HasMaterial() const;
    bool IsRenderable() const;

    bool HasLight() const;
    LightComponent& GetLight();
    const LightComponent& GetLight() const;
    void SetLight(LightComponent light);
    void ClearLight();

    int GetParentIndex() const;
    void SetParentIndex(int parentIndex);

    int GetSiblingOrder() const;
    void SetSiblingOrder(int siblingOrder);

    bool IsMovable() const;
    void SetMovable(bool movable);

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

private:
    std::string m_name;
    Mesh* m_mesh = nullptr;
    std::unique_ptr<Material> m_material;
    Transform m_transform;
    glm::vec3 m_localBoundsMin;
    glm::vec3 m_localBoundsMax;
    int m_parentIndex = -1;
    int m_siblingOrder = 0;
    bool m_movable = true;
    bool m_castShadow = true;
    bool m_receiveShadow = true;
    std::optional<LightComponent> m_light;
    std::string m_importAssetPath;
    int m_importNodeIndex = -1;
};
