#pragma once

class Camera;
class EnvironmentMap;
class IBL;
class Scene;
class SceneRenderer;
class ScreenSpaceEffects;
class EditorSettings;
struct RendererEditContext;

struct LightingPanelContext
{
    Scene& scene;
    Camera& camera;
    int viewportWidth;
    int viewportHeight;
    RendererEditContext& editContext;
    SceneRenderer& renderer;
    IBL& ibl;
    EnvironmentMap& environmentMap;
    ScreenSpaceEffects& screenSpaceEffects;
    EditorSettings* editorSettings = nullptr;
};
