#pragma once

class Camera;
class Scene;

class LightingPanel
{
public:
    void Draw(Scene& scene, const Camera& camera) const;

    bool& ShowPanel() const { return m_showPanel; }

private:
    mutable bool m_showPanel = true;
};
