#pragma once

class Scene;

class PhysicsWorld
{
public:
    PhysicsWorld();
    ~PhysicsWorld();

    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    void BuildFromScene(Scene& scene);
    void Step(Scene& scene, float deltaTime);
    void Shutdown();

private:
    struct Impl;
    struct Impl* m_impl = nullptr;
};
