#pragma once

#include <memory>

struct SceneSubtreeArchive;

class EditorClipboard
{
public:
    EditorClipboard();
    ~EditorClipboard();

    EditorClipboard(const EditorClipboard&) = delete;
    EditorClipboard& operator=(const EditorClipboard&) = delete;

    bool HasContent() const;
    void Clear();

    void SetSubtreeArchive(SceneSubtreeArchive archive);
    const SceneSubtreeArchive* GetSubtreeArchive() const;

private:
    std::unique_ptr<SceneSubtreeArchive> m_archive;
};
