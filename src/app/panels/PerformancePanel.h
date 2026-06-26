#pragma once

#include <cstdint>

class Scene;

class PerformancePanel
{
public:
    void OnFrame(double deltaTimeSeconds);

    void Draw(
        const Scene& scene,
        int sceneViewWidth,
        int sceneViewHeight,
        int windowWidth,
        int windowHeight,
        bool playModeActive) const;

    bool& ShowPanel() const { return m_showPanel; }

private:
    static constexpr int kHistorySize = 120;

    mutable bool m_showPanel = true;
    mutable float m_frameTimeHistory[kHistorySize] = {};
    mutable int m_historyWriteIndex = 0;
    mutable int m_historyCount = 0;
    mutable float m_minFrameMs = 0.0f;
    mutable float m_maxFrameMs = 0.0f;
    mutable float m_sumFrameMs = 0.0f;
    mutable std::uint64_t m_frameCounter = 0;
};
