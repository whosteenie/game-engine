#pragma once

class Scene;

class SceneInspectorPanel
{
public:
    void Draw(Scene& scene) const;

    bool& ShowPanel() const { return m_showPanel; }

private:
    mutable bool m_showPanel = true;
};
