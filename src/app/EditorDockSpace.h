#pragma once

class EditorDockSpace
{
public:
    void Begin();
    void End();

    bool IsLayoutBuilt() const { return m_layoutBuilt; }
    void RequestLayoutRebuild() { m_layoutBuilt = false; m_forceDefaultLayout = true; }
    void ResetLayout();

private:
    bool m_layoutBuilt = false;
    bool m_forceDefaultLayout = false;
};
