#pragma once

#include "engine/Material.h"
#include "engine/Transform.h"

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
        Transform transform = Transform{},
        bool movable = true,
        bool autoSpin = false,
        bool castShadow = true,
        bool receiveShadow = false);

    const std::string& GetName() const;
    void SetName(const std::string& name);

    Transform& GetTransform();
    const Transform& GetTransform() const;

    Material& GetMaterial();
    const Material& GetMaterial() const;

    Mesh* GetMesh() const;

    bool IsMovable() const;
    void SetMovable(bool movable);

    bool HasAutoSpin() const;
    void SetAutoSpin(bool autoSpin);

    bool CastsShadow() const;
    void SetCastShadow(bool castShadow);

    bool ReceivesShadow() const;
    void SetReceiveShadow(bool receiveShadow);

    glm::mat4 BuildModelMatrix(double animationTime) const;

private:
    std::string m_name;
    Mesh* m_mesh = nullptr;
    std::unique_ptr<Material> m_material;
    Transform m_transform;
    bool m_movable = true;
    bool m_autoSpin = false;
    bool m_castShadow = true;
    bool m_receiveShadow = false;
};
