#pragma once

#include <memory>

class Mesh;

std::unique_ptr<Mesh> CreateSphereMesh(float radius = 0.5f, int slices = 32, int stacks = 16);
