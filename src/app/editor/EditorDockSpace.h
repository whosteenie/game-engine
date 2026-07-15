#pragma once

class EditorDockSpace
{
public:
    void Begin(float topToolbarHeight = 0.0f, bool deferLayoutBuild = false);
    void CommitLayout();
    void End();

    void AfterEditorPanels(bool validateRestoredLayout = false);

    bool IsLayoutBuilt() const { return m_layoutBuilt; }
    void SetAutomationDualViewportLayout(bool enabled)
    {
        m_automationDualViewportLayout = enabled;
        if (enabled)
        {
            RequestLayoutRebuild();
        }
    }
    void RequestLayoutRebuild() { m_layoutBuilt = false; m_forceDefaultLayout = true; }
    void InvalidateBuiltLayout() { m_layoutBuilt = false; m_forceDefaultLayout = false; }
    void ResetLayout();

private:
    void BuildLayoutIfNeeded();

    bool m_layoutBuilt = false;
    bool m_forceDefaultLayout = false;
    bool m_deferLayoutBuild = false;
    bool m_automationDualViewportLayout = false;
};
