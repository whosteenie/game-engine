#include "app/PhysicsWorld.h"

#include "app/Scene.h"
#include "engine/ColliderComponent.h"
#include "engine/RigidBodyComponent.h"
#include "engine/SceneObject.h"
#include "engine/Transform.h"

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

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <algorithm>
#include <cstdio>
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

    void LogPhysics(const char* message)
    {
        std::fprintf(stderr, "[Physics] %s\n", message);
        std::fflush(stderr);
    }

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

    void DecomposeWorldMatrix(
        const glm::mat4& worldMatrix,
        glm::vec3& position,
        glm::quat& rotation,
        glm::vec3& scale)
    {
        glm::vec3 skew;
        glm::vec4 perspective;
        glm::decompose(worldMatrix, scale, rotation, position, skew, perspective);
    }

    glm::mat4 BuildWorldMatrix(const glm::vec3& position, const glm::quat& rotation, const glm::vec3& scale)
    {
        glm::mat4 matrix(1.0f);
        matrix = glm::translate(matrix, position);
        matrix *= glm::mat4_cast(rotation);
        matrix = glm::scale(matrix, scale);
        return matrix;
    }

    void ApplyWorldTransformToObject(Scene& scene, int objectIndex, const glm::vec3& worldPosition, const glm::quat& worldRotation)
    {
        SceneObject& object = scene.GetObject(static_cast<std::size_t>(objectIndex));
        const glm::vec3 preservedScale = object.GetTransform().scale;

        const glm::mat4 worldMatrix =
            BuildWorldMatrix(worldPosition, worldRotation, preservedScale);

        const int parentIndex = object.GetParentIndex();
        if (parentIndex >= 0)
        {
            const glm::mat4 parentWorld = scene.GetWorldMatrix(parentIndex);
            const glm::mat4 localMatrix = glm::inverse(parentWorld) * worldMatrix;
            object.GetTransform().SetFromMatrix(localMatrix);
        }
        else
        {
            object.GetTransform().SetFromMatrix(worldMatrix);
        }
    }

    JPH::Quat ToJoltQuat(const glm::quat& rotation)
    {
        return JPH::Quat(rotation.w, rotation.x, rotation.y, rotation.z);
    }

    JPH::Vec3 ToJoltVec3(const glm::vec3& value)
    {
        return JPH::Vec3(value.x, value.y, value.z);
    }

    glm::vec3 FromJoltVec3(const JPH::Vec3& value)
    {
        return glm::vec3(value.GetX(), value.GetY(), value.GetZ());
    }

    glm::quat FromJoltQuat(const JPH::Quat& rotation)
    {
        return glm::quat(rotation.GetW(), rotation.GetX(), rotation.GetY(), rotation.GetZ());
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

    LogPhysics("BuildFromScene: begin");
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

        {
            const ColliderComponent& collider = object.GetCollider();
            const char* shape = collider.shape == ColliderShape::Sphere ? "Sphere" : "Box";
            std::fprintf(
                stderr,
                "[Physics] BuildFromScene: object=%d name=\"%s\" shape=%s rb=%d trigger=%d\n",
                objectIndex,
                object.GetName().c_str(),
                shape,
                object.HasRigidBody() ? 1 : 0,
                collider.isTrigger ? 1 : 0);
            std::fflush(stderr);
        }

        const ColliderComponent& collider = object.GetCollider();
        const glm::mat4 worldMatrix = scene.GetWorldMatrix(objectIndex);
        const glm::vec3 colliderWorldCenter = glm::vec3(worldMatrix * glm::vec4(collider.offset, 1.0f));
        glm::vec3 worldScale;
        glm::quat worldRotation;
        glm::vec3 worldPosition;
        DecomposeWorldMatrix(worldMatrix, worldPosition, worldRotation, worldScale);
        if (!IsFiniteVec3(colliderWorldCenter) || !IsFiniteVec3(worldScale) || !IsFiniteQuat(worldRotation))
        {
            std::fprintf(
                stderr,
                "[Physics] BuildFromScene: invalid transform for object=%d name=\"%s\" (skipped)\n",
                objectIndex,
                object.GetName().c_str());
            std::fflush(stderr);
            continue;
        }

        ShapeRefC shape;
        if (collider.shape == ColliderShape::Sphere)
        {
            const glm::vec3 absScale = glm::abs(worldScale);
            const float scaledRadius =
                collider.radius * std::max(absScale.x, std::max(absScale.y, absScale.z));
            std::fprintf(
                stderr,
                "[Physics] BuildFromScene: creating sphere shape radius=%.4f\n",
                std::max(scaledRadius, kMinSphereRadius));
            std::fflush(stderr);
            shape = new SphereShape(std::max(scaledRadius, kMinSphereRadius));
        }
        else
        {
            const glm::vec3 scaledHalfExtents = collider.halfExtents * glm::abs(worldScale);
            const JPH::Vec3 halfExtents =
                ToJoltVec3(glm::max(scaledHalfExtents, glm::vec3(kMinShapeHalfExtent)));
            std::fprintf(
                stderr,
                "[Physics] BuildFromScene: creating box shape halfExtents=(%.4f, %.4f, %.4f)\n",
                halfExtents.GetX(),
                halfExtents.GetY(),
                halfExtents.GetZ());
            std::fflush(stderr);
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
            ToJoltQuat(worldRotation),
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

        std::fprintf(
            stderr,
            "[Physics] BuildFromScene: CreateBody object=%d motionType=%d\n",
            objectIndex,
            static_cast<int>(motionType));
        std::fflush(stderr);
        Body* body = bodyInterface.CreateBody(bodySettings);
        if (body == nullptr)
        {
            std::fprintf(
                stderr,
                "[Physics] BuildFromScene: CreateBody failed for object=%d name=\"%s\"\n",
                objectIndex,
                object.GetName().c_str());
            std::fflush(stderr);
            continue;
        }

        body->SetUserData(static_cast<uint64_t>(objectIndex));
        const EActivation activation =
            motionType == EMotionType::Static ? EActivation::DontActivate : EActivation::Activate;
        std::fprintf(
            stderr,
            "[Physics] BuildFromScene: AddBody object=%d activation=%d\n",
            objectIndex,
            static_cast<int>(activation));
        std::fflush(stderr);
        bodyInterface.AddBody(body->GetID(), activation);
        std::fprintf(stderr, "[Physics] BuildFromScene: AddBody complete object=%d\n", objectIndex);
        std::fflush(stderr);

        m_impl->trackedBodies.push_back(Impl::TrackedBody{
            body->GetID(),
            objectIndex,
            syncFromPhysics,
        });
    }

    m_impl->physicsSystem.OptimizeBroadPhase();
    std::fprintf(
        stderr,
        "[Physics] BuildFromScene: complete trackedBodies=%zu\n",
        m_impl->trackedBodies.size());
    std::fflush(stderr);
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
        ApplyWorldTransformToObject(
            scene,
            trackedBody.objectIndex,
            FromJoltVec3(Vec3(position.GetX(), position.GetY(), position.GetZ())),
            FromJoltQuat(rotation));
    }
}
