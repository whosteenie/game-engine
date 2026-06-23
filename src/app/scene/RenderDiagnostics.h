#pragma once

#include <string>

class Camera;
class Scene;

namespace RenderDiagnostics
{
    bool WriteReport(
        const Scene& scene,
        const Camera& camera,
        int viewportWidth,
        int viewportHeight,
        const std::string& outputPath,
        std::string& statusMessage);
}
