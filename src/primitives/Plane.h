#pragma once

#include <memory>

class Mesh;

std::unique_ptr<Mesh> CreatePlaneMesh(float halfExtent = 5.0f);
