#pragma once

#include <memory>

class Mesh;

std::unique_ptr<Mesh> CreateCylinderMesh(float radius = 0.5f, float height = 1.0f, int slices = 32);
