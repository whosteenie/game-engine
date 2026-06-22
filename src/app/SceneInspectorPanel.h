#pragma once

class Scene;

class SceneInspectorPanel
{
public:
    void Draw(Scene& scene) const;

private:
    mutable bool m_showPanel = true;
};
