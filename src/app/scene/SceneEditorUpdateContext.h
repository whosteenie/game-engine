#pragma once

#include "app/editor/EditorViewportRect.h"

#include <string>

class Camera;
class Input;
class UndoStack;

struct SceneEditorUpdateContext
{
    Input& input;
    Camera& camera;
    int framebufferWidth = 0;
    int framebufferHeight = 0;
    int windowWidth = 0;
    int windowHeight = 0;
    bool allowMouseInput = true;
    bool allowKeyboardInput = true;
    UndoStack* undoStack = nullptr;
    std::string projectRoot;
    const EditorViewportRect* viewport = nullptr;
};
