#pragma once

#include <glm/glm.hpp>

#include <string>

enum class ColliderShape
{
    Box = 0,
    Sphere
};

const char* ColliderShapeToString(ColliderShape shape);
bool ColliderShapeFromString(const std::string& value, ColliderShape& outShape);

struct ColliderComponent
{
    ColliderShape shape = ColliderShape::Box;
    glm::vec3 offset = glm::vec3(0.0f);
    glm::vec3 halfExtents = glm::vec3(0.5f);
    float radius = 0.5f;
    bool isTrigger = false;
    float friction = 0.2f;
    float restitution = 0.0f;
};

bool operator==(const ColliderComponent& left, const ColliderComponent& right);

ColliderComponent MakeDefaultColliderComponent();
