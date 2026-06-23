#include "app/EditorClipboard.h"

#include "app/SceneSubtreeArchive.h"

#include "engine/Mesh.h"

EditorClipboard::EditorClipboard() = default;

EditorClipboard::~EditorClipboard() = default;

bool EditorClipboard::HasContent() const
{
    return m_archive != nullptr && !m_archive->removedObjects.empty();
}

void EditorClipboard::Clear()
{
    m_archive.reset();
}

void EditorClipboard::SetSubtreeArchive(SceneSubtreeArchive archive)
{
    m_archive = std::make_unique<SceneSubtreeArchive>(std::move(archive));
}

const SceneSubtreeArchive* EditorClipboard::GetSubtreeArchive() const
{
    return m_archive.get();
}
