#pragma once

class Scene;

class SceneHierarchyPanel
{
public:
    void Draw(Scene& scene) const;

private:
    mutable bool m_showPanel = true;
};
