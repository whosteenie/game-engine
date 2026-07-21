#pragma once

struct ProjectViewportRevealState
{
    bool layoutStable = false;
    bool sceneImageRequired = false;
    bool sceneImageReady = false;
    bool gameImageRequired = false;
    bool gameImageReady = false;
};

inline bool CanRevealProjectEditor(const ProjectViewportRevealState& state)
{
    return state.layoutStable
        && (!state.sceneImageRequired || state.sceneImageReady)
        && (!state.gameImageRequired || state.gameImageReady);
}
