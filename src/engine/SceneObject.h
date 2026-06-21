#pragma once

#include "engine/Material.h"
#include "engine/Transform.h"

#include <glm/glm.hpp>
#include <memory>
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
        bool receiveShadow = true);

    const std::string& GetName() const;
    void SetName(const std::string& name);

    Transform& GetTransform();
    const Transform& GetTransform() const;

    Material& GetMaterial();
    const Material& GetMaterial() const;

    Mesh* GetMesh() const;

    bool IsMovable() const;
    void SetMovable(bool movable);

    bool CastsShadow() const;
    void SetCastShadow(bool castShadow);

    bool ReceivesShadow() const;
    void SetReceiveShadow(bool receiveShadow);

    glm::mat4 BuildModelMatrix() const;
    void ApplyTransformFromMatrix(const glm::mat4& matrix);
    void GetWorldBounds(glm::vec3& boundsMin, glm::vec3& boundsMax) const;

    const glm::vec3& GetLocalBoundsMin() const;
    const glm::vec3& GetLocalBoundsMax() const;

private:
    std::string m_name;
    Mesh* m_mesh = nullptr;
    std::unique_ptr<Material> m_material;
    Transform m_transform;
    glm::vec3 m_localBoundsMin;
    glm::vec3 m_localBoundsMax;
    bool m_movable = true;
    bool m_castShadow = true;
    bool m_receiveShadow = true;
};
