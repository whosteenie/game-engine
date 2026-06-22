#pragma once

namespace EditorPanelLayout
{
    enum class Panel
    {
        RendererTuning,
        Toolbar,
        Hierarchy,
        ProjectFiles,
        Inspector,
    };

    void ApplyFirstUseLayout(Panel panel);
}
