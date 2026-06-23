#pragma once

#include <memory>

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
    std::unique_ptr<Impl> m_impl;
};
