#pragma once

class Camera;
class DemoScene;

class DebugPanel
{
public:
    void Draw(DemoScene& scene, const Camera& camera) const;

private:
    mutable bool m_showPanel = true;
};
