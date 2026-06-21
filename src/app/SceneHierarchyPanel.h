#pragma once

class DemoScene;

class SceneHierarchyPanel
{
public:
    void Draw(DemoScene& scene) const;

private:
    mutable bool m_showPanel = true;
};
