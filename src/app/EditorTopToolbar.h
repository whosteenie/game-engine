#pragma once

class PlayModeController;
class ProjectSession;
class Scene;

class EditorTopToolbar
{
public:
    void Draw(PlayModeController& playMode, Scene& editScene, ProjectSession& project);

    float GetHeight() const { return m_height; }

private:
    float m_height = 0.0f;
};
