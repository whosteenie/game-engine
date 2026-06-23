#pragma once

#include <cstddef>
#include <string>
#include <vector>

class Scene;

class SceneObjectOperations
{
public:
    static bool RemoveObject(Scene& scene, std::size_t index);
    static bool RemoveSelectedObjects(Scene& scene);
    static std::string MakeDuplicateObjectName(const Scene& scene, const std::string& sourceName);
    static int DuplicateObject(Scene& scene, int objectIndex);
    static std::vector<int> DuplicateSelectedObjects(Scene& scene);
};
