#pragma once

class Camera;
class DemoScene;
class Material;

class DebugPanel
{
public:
    void Draw(DemoScene& scene, Material& cubeMaterial, const Camera& camera, bool paused) const;

private:
    mutable bool m_showPanel = true;
};
