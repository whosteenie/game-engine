#pragma once

class Scene;

class SceneToolbarPanel
{
public:
    void Draw(Scene& scene) const;

private:
    mutable bool m_showPanel = true;
};
