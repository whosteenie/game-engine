#pragma once

class Camera;
class DemoScene;

class DebugPanel
{
public:
    void Draw(DemoScene& scene, const Camera& camera, bool paused) const;

private:
    mutable bool m_showPanel = true;
};
