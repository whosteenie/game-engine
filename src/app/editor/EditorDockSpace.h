#pragma once

class EditorDockSpace
{
public:
    void Begin(float topToolbarHeight = 0.0f);
    void End();

    void AfterEditorPanels();

    bool IsLayoutBuilt() const { return m_layoutBuilt; }
    void RequestLayoutRebuild() { m_layoutBuilt = false; m_forceDefaultLayout = true; }
    void ResetLayout();

private:
    bool m_layoutBuilt = false;
    bool m_forceDefaultLayout = false;
};
