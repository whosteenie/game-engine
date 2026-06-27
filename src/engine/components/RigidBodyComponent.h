#pragma once

enum class RigidBodyCollisionDetection
{
    Discrete = 0,
    Continuous
};

const char* RigidBodyCollisionDetectionToString(RigidBodyCollisionDetection mode);
bool RigidBodyCollisionDetectionFromString(const char* value, RigidBodyCollisionDetection& outMode);

struct RigidBodyComponent
{
    float mass = 1.0f;
    bool useGravity = true;
    bool isKinematic = false;
    RigidBodyCollisionDetection collisionDetection = RigidBodyCollisionDetection::Continuous;
    float linearDamping = 0.05f;
    float angularDamping = 0.05f;
    bool allowSleeping = true;
};

bool operator==(const RigidBodyComponent& left, const RigidBodyComponent& right);

RigidBodyComponent MakeDefaultRigidBodyComponent();
