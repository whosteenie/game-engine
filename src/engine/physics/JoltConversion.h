#pragma once

#include <Jolt/Jolt.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace JoltConversion
{
    inline JPH::Quat ToQuat(const glm::quat& rotation)
    {
        // Jolt stores quaternions as (x, y, z, w); GLM's constructor is (w, x, y, z).
        return JPH::Quat(rotation.x, rotation.y, rotation.z, rotation.w);
    }

    inline JPH::Vec3 ToVec3(const glm::vec3& value)
    {
        return JPH::Vec3(value.x, value.y, value.z);
    }

    inline glm::vec3 FromVec3(const JPH::Vec3& value)
    {
        return glm::vec3(value.GetX(), value.GetY(), value.GetZ());
    }

    inline glm::quat FromQuat(const JPH::Quat& rotation)
    {
        return glm::quat(rotation.GetW(), rotation.GetX(), rotation.GetY(), rotation.GetZ());
    }
}
