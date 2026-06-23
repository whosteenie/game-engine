#pragma once

struct RigidBodyComponent
{
    float mass = 1.0f;
    bool useGravity = true;
    bool isKinematic = false;
};

RigidBodyComponent MakeDefaultRigidBodyComponent();
