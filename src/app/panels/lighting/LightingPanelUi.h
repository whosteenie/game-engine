#pragma once

class SceneRenderer;
class ScreenSpaceEffects;

namespace LightingPanelUi
{
    void DrawWrappedNote(const char* text);
    void DrawWrappedHelp(const char* text);

    struct FeatureState
    {
        bool postProcessingEnabled = false;
        bool pathTracingActive = false;
        bool dxrEnabled = false;
        bool rtGiEnabled = false;
        bool ssgiEnabled = false;
        bool rayReconstructionActive = false;
        bool debugViewActive = false;
    };

    FeatureState QueryFeatures(const SceneRenderer& renderer, const ScreenSpaceEffects& screenSpaceEffects);
}
