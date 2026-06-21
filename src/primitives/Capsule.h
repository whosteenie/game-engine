#pragma once

#include <memory>

class Mesh;

std::unique_ptr<Mesh> CreateCapsuleMesh(float radius = 0.5f, float height = 2.0f, int slices = 32, int capStacks = 8);
