#pragma once

#include <memory>

class Mesh;

std::unique_ptr<Mesh> CreateSphereMesh(float radius = 0.5f, int slices = 48, int stacks = 24);
