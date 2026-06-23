#include "app/core/PhysicsWorld.h"

#include "app/scene/Scene.h"
#include "engine/components/ColliderComponent.h"
#include "engine/components/RigidBodyComponent.h"
#include "engine/scene/SceneHierarchy.h"
#include "engine/scene/SceneObject.h"
#include "engine/scene/Transform.h"

#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include "engine/physics/JoltConversion.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

JPH_SUPPRESS_WARNINGS

using namespace JPH;

namespace
{
    constexpr float kMinShapeHalfExtent = 0.05f;
    constexpr float kMinSphereRadius = 0.05f;

    namespace Layers
    {
        static constexpr ObjectLayer NonMoving = 0;
        static constexpr ObjectLayer Moving = 1;
        static constexpr ObjectLayer Count = 2;
    }

    namespace BroadPhaseLayers
    {
        static constexpr BroadPhaseLayer NonMoving(0);
        static constexpr BroadPhaseLayer Moving(1);
        static constexpr uint Count = 2;
    }

    class ObjectLayerPairFilterImpl final : public ObjectLayerPairFilter
    {
    public:
        bool ShouldCollide(ObjectLayer object1, ObjectLayer object2) const override
        {
            switch (object1)
            {
            case Layers::NonMoving:
                return object2 == Layers::Moving;
            case Layers::Moving:
                return true;
            default:
                return false;
            }
        }
    };

    class BroadPhaseLayerInterfaceImpl final : public BroadPhaseLayerInterface
    {
    public:
        BroadPhaseLayerInterfaceImpl()
        {
            m_objectToBroadPhase[Layers::NonMoving] = BroadPhaseLayers::NonMoving;
            m_objectToBroadPhase[Layers::Moving] = BroadPhaseLayers::Moving;
        }

        uint GetNumBroadPhaseLayers() const override
        {
            return BroadPhaseLayers::Count;
        }

        BroadPhaseLayer GetBroadPhaseLayer(ObjectLayer layer) const override
        {
            return m_objectToBroadPhase[layer];
        }

        const char* GetBroadPhaseLayerName(BroadPhaseLayer layer) const override
        {
            switch (static_cast<BroadPhaseLayer::Type>(layer))
            {
            case static_cast<BroadPhaseLayer::Type>(BroadPhaseLayers::NonMoving):
                return "NON_MOVING";
            case static_cast<BroadPhaseLayer::Type>(BroadPhaseLayers::Moving):
                return "MOVING";
            default:
                return "INVALID";
            }
        }

    private:
        BroadPhaseLayer m_objectToBroadPhase[Layers::Count];
    };

    class ObjectVsBroadPhaseLayerFilterImpl final : public ObjectVsBroadPhaseLayerFilter
    {
    public:
        bool ShouldCollide(ObjectLayer layer1, BroadPhaseLayer layer2) const override
        {
            switch (layer1)
            {
            case Layers::NonMoving:
                return layer2 == BroadPhaseLayers::Moving;
            case Layers::Moving:
                return true;
            default:
                return false;
            }
        }
    };

    void EnsureJoltInitialized()
    {
        static bool initialized = false;
        if (initialized)
        {
            return;
        }

        RegisterDefaultAllocator();
        Factory::sInstance = new Factory();
        RegisterTypes();
        initialized = true;
    }

    void ApplyWorldTransformToObject(Scene& scene, int objectIndex, const glm::vec3& worldPosition, const glm::quat& worldRotation)
    {
        const glm::vec3 preservedScale =
            scene.GetObject(static_cast<std::size_t>(objectIndex)).GetTransform().scale;
        Transform worldTransform;
        worldTransform.position = worldPosition;
        worldTransform.rotation = worldRotation;
        worldTransform.scale = preservedScale;
        SetObjectWorldMatrix(scene.GetObjects(), objectIndex, worldTransform.ToMatrix());
    }

    bool IsFiniteVec3(const glm::vec3& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }

    bool IsFiniteQuat(const glm::quat& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z)
            && std::isfinite(value.w);
    }
}

struct PhysicsWorld::Impl
{
    static constexpr uint MaxBodies = 4096;
    static constexpr uint MaxBodyPairs = 4096;
    static constexpr uint MaxContactConstraints = 4096;

    BroadPhaseLayerInterfaceImpl broadPhaseLayerInterface;
    ObjectVsBroadPhaseLayerFilterImpl objectVsBroadPhaseLayerFilter;
    ObjectLayerPairFilterImpl objectLayerPairFilter;
    TempAllocatorImpl tempAllocator{10 * 1024 * 1024};
    JobSystemThreadPool jobSystem{
        cMaxPhysicsJobs,
        cMaxPhysicsBarriers,
        static_cast<int>(std::max(1u, static_cast<unsigned int>(std::thread::hardware_concurrency()) - 1u))};
    PhysicsSystem physicsSystem;

    struct TrackedBody
    {
        BodyID bodyId;
        int objectIndex = -1;
        bool syncFromPhysics = false;
    };

    std::vector<TrackedBody> trackedBodies;

    Impl()
    {
        physicsSystem.Init(
            MaxBodies,
            0,
            MaxBodyPairs,
            MaxContactConstraints,
            broadPhaseLayerInterface,
            objectVsBroadPhaseLayerFilter,
            objectLayerPairFilter);
    }

    ~Impl()
    {
        ShutdownBodies();
    }

    void ShutdownBodies()
    {
        BodyInterface& bodyInterface = physicsSystem.GetBodyInterface();
        for (TrackedBody& trackedBody : trackedBodies)
        {
            if (!trackedBody.bodyId.IsInvalid())
            {
                bodyInterface.RemoveBody(trackedBody.bodyId);
                bodyInterface.DestroyBody(trackedBody.bodyId);
                trackedBody.bodyId = BodyID();
            }
        }

        trackedBodies.clear();
    }
};

PhysicsWorld::PhysicsWorld()
{
    EnsureJoltInitialized();
    m_impl = new Impl();
}

PhysicsWorld::~PhysicsWorld()
{
    delete m_impl;
    m_impl = nullptr;
}

void PhysicsWorld::Shutdown()
{
    if (m_impl != nullptr)
    {
        m_impl->ShutdownBodies();
    }
}

void PhysicsWorld::BuildFromScene(Scene& scene)
{
    if (m_impl == nullptr)
    {
        return;
    }

    m_impl->ShutdownBodies();

    BodyInterface& bodyInterface = m_impl->physicsSystem.GetBodyInterface();
    const std::vector<SceneObject>& objects = scene.GetObjects();

    for (int objectIndex = 0; objectIndex < static_cast<int>(objects.size()); ++objectIndex)
    {
        const SceneObject& object = objects[static_cast<std::size_t>(objectIndex)];
        if (!object.HasCollider())
        {
            continue;
        }

        const ColliderComponent& collider = object.GetCollider();
        const glm::mat4 worldMatrix = scene.GetWorldMatrix(objectIndex);
        const glm::vec3 colliderWorldCenter = glm::vec3(worldMatrix * glm::vec4(collider.offset, 1.0f));
        Transform worldTransform;
        worldTransform.SetFromMatrix(worldMatrix);
        const glm::vec3& worldScale = worldTransform.scale;
        const glm::quat& worldRotation = worldTransform.rotation;
        if (!IsFiniteVec3(colliderWorldCenter) || !IsFiniteVec3(worldScale) || !IsFiniteQuat(worldRotation))
        {
            continue;
        }

        ShapeRefC shape;
        if (collider.shape == ColliderShape::Sphere)
        {
            const glm::vec3 absScale = glm::abs(worldScale);
            const float scaledRadius =
                collider.radius * std::max(absScale.x, std::max(absScale.y, absScale.z));
            shape = new SphereShape(std::max(scaledRadius, kMinSphereRadius));
        }
        else
        {
            const glm::vec3 scaledHalfExtents = collider.halfExtents * glm::abs(worldScale);
            const JPH::Vec3 halfExtents =
                JoltConversion::ToVec3(glm::max(scaledHalfExtents, glm::vec3(kMinShapeHalfExtent)));
            shape = new BoxShape(halfExtents);
        }

        EMotionType motionType = EMotionType::Static;
        ObjectLayer objectLayer = Layers::NonMoving;
        bool syncFromPhysics = false;

        if (object.HasRigidBody())
        {
            const RigidBodyComponent& rigidBody = object.GetRigidBody();
            if (rigidBody.isKinematic)
            {
                motionType = EMotionType::Kinematic;
                objectLayer = Layers::Moving;
            }
            else
            {
                motionType = EMotionType::Dynamic;
                objectLayer = Layers::Moving;
                syncFromPhysics = true;
            }
        }

        BodyCreationSettings bodySettings(
            shape,
            RVec3(colliderWorldCenter.x, colliderWorldCenter.y, colliderWorldCenter.z),
            JoltConversion::ToQuat(worldRotation),
            motionType,
            objectLayer);

        if (object.HasRigidBody())
        {
            const RigidBodyComponent& rigidBody = object.GetRigidBody();
            bodySettings.mOverrideMassProperties = EOverrideMassProperties::CalculateInertia;
            bodySettings.mMassPropertiesOverride.mMass = std::max(rigidBody.mass, 0.001f);
            bodySettings.mGravityFactor = rigidBody.useGravity ? 1.0f : 0.0f;
            bodySettings.mIsSensor = collider.isTrigger;
        }
        else
        {
            bodySettings.mIsSensor = collider.isTrigger;
        }

        Body* body = bodyInterface.CreateBody(bodySettings);
        if (body == nullptr)
        {
            continue;
        }

        body->SetUserData(static_cast<uint64_t>(objectIndex));
        const EActivation activation =
            motionType == EMotionType::Static ? EActivation::DontActivate : EActivation::Activate;
        bodyInterface.AddBody(body->GetID(), activation);

        m_impl->trackedBodies.push_back(Impl::TrackedBody{
            body->GetID(),
            objectIndex,
            syncFromPhysics,
        });
    }

    m_impl->physicsSystem.OptimizeBroadPhase();
}

void PhysicsWorld::Step(Scene& scene, float deltaTime)
{
    if (m_impl == nullptr || deltaTime <= 0.0f)
    {
        return;
    }

    constexpr float kPhysicsTick = 1.0f / 60.0f;
    const int collisionSteps = std::max(1, static_cast<int>(std::ceil(deltaTime / kPhysicsTick)));

    m_impl->physicsSystem.Update(
        deltaTime,
        collisionSteps,
        &m_impl->tempAllocator,
        &m_impl->jobSystem);

    BodyInterface& bodyInterface = m_impl->physicsSystem.GetBodyInterface();
    for (const Impl::TrackedBody& trackedBody : m_impl->trackedBodies)
    {
        if (!trackedBody.syncFromPhysics || trackedBody.bodyId.IsInvalid())
        {
            continue;
        }

        if (!bodyInterface.IsAdded(trackedBody.bodyId))
        {
            continue;
        }

        const RVec3 position = bodyInterface.GetCenterOfMassPosition(trackedBody.bodyId);
        const Quat rotation = bodyInterface.GetRotation(trackedBody.bodyId);
        const glm::quat worldRotation = JoltConversion::FromQuat(rotation);
        const glm::vec3 colliderWorldCenter = JoltConversion::FromVec3(
            Vec3(position.GetX(), position.GetY(), position.GetZ()));

        const SceneObject& object =
            scene.GetObject(static_cast<std::size_t>(trackedBody.objectIndex));
        const ColliderComponent& collider = object.GetCollider();

        Transform worldTransform;
        worldTransform.SetFromMatrix(scene.GetWorldMatrix(trackedBody.objectIndex));
        const glm::vec3 objectWorldPosition =
            colliderWorldCenter - worldRotation * (collider.offset * worldTransform.scale);

        ApplyWorldTransformToObject(
            scene,
            trackedBody.objectIndex,
            objectWorldPosition,
            worldRotation);
    }
}
