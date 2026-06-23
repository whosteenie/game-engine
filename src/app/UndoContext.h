#pragma once

#include <string>

class Scene;

struct UndoContext
{
    Scene& scene;
    const std::string& projectRoot;
};
