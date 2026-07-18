#include "app/undo/UndoCommand.h"

#include "app/editor/EditorClipboard.h"
#include "app/editor/TuningSectionState.h"
#include "app/scene/document/Scene.h"
#include "app/project/SceneDocument.h"
#include "app/project/SceneProjectIODetail.h"
#include "app/project/SceneSubtreeArchive.h"
#include "engine/rendering/resources/Mesh.h"
#include "app/undo/UndoStack.h"
#include "engine/components/CameraComponent.h"
#include "engine/components/ColliderComponent.h"
#include "engine/scene/InspectorComponentOrder.h"
#include "engine/components/LightComponent.h"
#include "engine/rendering/resources/Material.h"
#include "engine/components/RigidBodyComponent.h"
#include "engine/scene/SceneObject.h"
#include "engine/scene/SceneObjectId.h"

#include "engine/scene/SceneHierarchy.h"


namespace
{
    const std::string kEmptyProjectRoot;
}

SetObjectNameCommand::SetObjectNameCommand(SceneObjectId objectId, std::string oldName, std::string newName)
    : m_objectId(objectId),
      m_oldName(std::move(oldName)),
      m_newName(std::move(newName))
{
    m_description = "Rename \"" + m_oldName + "\"";
}

void SetObjectNameCommand::ApplyName(UndoContext& context, const std::string& name) const
{
    const int objectIndex = context.scene.FindObjectIndex(m_objectId);
    if (objectIndex < 0)
    {
        return;
    }

    context.scene.GetSceneObject(static_cast<std::size_t>(objectIndex)).SetName(name);
    context.scene.MarkDirty();
}

void SetObjectNameCommand::Undo(UndoContext& context)
{
    ApplyName(context, m_oldName);
}

void SetObjectNameCommand::Redo(UndoContext& context)
{
    ApplyName(context, m_newName);
}

const char* SetObjectNameCommand::GetName() const
{
    return m_description.c_str();
}

void PushSetObjectName(
    UndoStack& undoStack,
    Scene& scene,
    SceneObjectId objectId,
    const std::string& oldName,
    const std::string& newName)
{
    if (objectId == kInvalidSceneObjectId || oldName == newName)
    {
        return;
    }

    auto command = std::make_unique<SetObjectNameCommand>(objectId, oldName, newName);
    UndoContext context{scene, kEmptyProjectRoot};
    command->Redo(context);
    undoStack.Push(std::move(command));
}

ApplySceneDocumentCommand::ApplySceneDocumentCommand(
    SceneDocument before,
    SceneDocument after,
    std::string name,
    std::string projectRoot)
    : m_before(std::move(before)),
      m_after(std::move(after)),
      m_name(std::move(name)),
      m_projectRoot(std::move(projectRoot))
{
}

void ApplySceneDocumentCommand::ApplySnapshot(UndoContext& context, const SceneDocument& document) const
{
    std::string error;
    if (!document.Apply(context.scene, m_projectRoot, error, true))
    {
        return;
    }

    context.scene.MarkDirty();
}

void ApplySceneDocumentCommand::Undo(UndoContext& context)
{
    ApplySnapshot(context, m_before);
}

void ApplySceneDocumentCommand::Redo(UndoContext& context)
{
    ApplySnapshot(context, m_after);
}

const char* ApplySceneDocumentCommand::GetName() const
{
    return m_name.c_str();
}

void PushSceneEdit(
    UndoStack& undoStack,
    Scene& scene,
    const std::string& projectRoot,
    const std::string& commandName,
    const std::function<void(Scene&)>& mutate)
{
    if (!mutate)
    {
        return;
    }

    SceneDocument before = SceneDocument::Capture(scene, projectRoot);
    mutate(scene);
    SceneDocument after = SceneDocument::Capture(scene, projectRoot);
    if (after.IsSameAs(before))
    {
        return;
    }

    undoStack.Push(std::make_unique<ApplySceneDocumentCommand>(
        std::move(before),
        std::move(after),
        commandName,
        projectRoot));
}

DeleteObjectsCommand::DeleteObjectsCommand(SceneSubtreeArchive archive, std::string name)
    : m_archive(std::move(archive)),
      m_name(std::move(name))
{
}

void DeleteObjectsCommand::Undo(UndoContext& context)
{
    context.scene.RestoreDeleteArchive(m_archive, m_archive.selectionBefore);
}

void DeleteObjectsCommand::Redo(UndoContext& context)
{
    const ArchivedSelectionState selectionBefore = m_archive.selectionBefore;

    std::vector<int> rootIndices;
    rootIndices.reserve(m_archive.removedRootIds.size());
    for (SceneObjectId rootId : m_archive.removedRootIds)
    {
        const int rootIndex = context.scene.FindObjectIndex(rootId);
        if (rootIndex >= 0)
        {
            rootIndices.push_back(rootIndex);
        }
    }

    if (rootIndices.empty() || !context.scene.CreateDeleteArchive(rootIndices, m_archive))
    {
        return;
    }

    m_archive.selectionBefore = selectionBefore;

    if (!context.scene.DeleteUsingArchive(m_archive))
    {
        return;
    }

    m_archive.selectionAfter = CaptureArchivedSelection(context.scene);
    ApplyArchivedSelection(context.scene, m_archive.selectionAfter);
    context.scene.MarkDirty();
}

const char* DeleteObjectsCommand::GetName() const
{
    return m_name.c_str();
}

void PushDeleteObjects(
    UndoStack& undoStack,
    Scene& scene,
    const std::string& commandName,
    const std::vector<int>& rootIndices)
{
    if (rootIndices.empty())
    {
        return;
    }

    SceneSubtreeArchive archive;
    archive.selectionBefore = CaptureArchivedSelection(scene);
    if (!scene.CreateDeleteArchive(rootIndices, archive))
    {
        return;
    }

    if (!scene.DeleteUsingArchive(archive))
    {
        return;
    }

    archive.selectionAfter = CaptureArchivedSelection(scene);

    undoStack.Push(std::make_unique<DeleteObjectsCommand>(std::move(archive), commandName));
}

void PushDeleteSelection(UndoStack& undoStack, Scene& scene, const std::string& commandName)
{
    const std::vector<int> roots =
        FilterToTopmostSelectedIndices(scene.GetObjects(), scene.GetSelection().indices);
    PushDeleteObjects(undoStack, scene, commandName, roots);
}

InsertSubtreeCommand::InsertSubtreeCommand(SceneSubtreeArchive archive, std::string name)
    : m_archive(std::move(archive)),
      m_name(std::move(name))
{
}

void InsertSubtreeCommand::Undo(UndoContext& context)
{
    std::vector<int> rootIndices;
    rootIndices.reserve(m_archive.removedRootIds.size());
    for (SceneObjectId rootId : m_archive.removedRootIds)
    {
        const int rootIndex = context.scene.FindObjectIndex(rootId);
        if (rootIndex >= 0)
        {
            rootIndices.push_back(rootIndex);
        }
    }

    // RestoreDeleteArchive moves component ownership into the live objects. Capture
    // them again before removing the inserted subtree so repeated undo/redo cycles
    // retain material and optional object components.
    if (rootIndices.empty() || !context.scene.CreateDeleteArchive(rootIndices, m_archive))
    {
        return;
    }

    if (!context.scene.DeleteUsingArchive(m_archive))
    {
        return;
    }

    ApplyArchivedSelection(context.scene, m_archive.selectionBefore);
    context.scene.MarkDirty();
}

void InsertSubtreeCommand::Redo(UndoContext& context)
{
    context.scene.RestoreDeleteArchive(m_archive, m_archive.selectionAfter);
}

const char* InsertSubtreeCommand::GetName() const
{
    return m_name.c_str();
}

ReparentObjectsCommand::ReparentObjectsCommand(ReparentArchive archive, std::string name)
    : m_archive(std::move(archive)),
      m_name(std::move(name))
{
}

void ReparentObjectsCommand::Undo(UndoContext& context)
{
    ApplyHierarchyArchive(context.scene, m_archive.before);
    ApplyArchivedSelection(context.scene, m_archive.selectionBefore);
}

void ReparentObjectsCommand::Redo(UndoContext& context)
{
    ApplyHierarchyArchive(context.scene, m_archive.after);
    ApplyArchivedSelection(context.scene, m_archive.selectionAfter);
}

const char* ReparentObjectsCommand::GetName() const
{
    return m_name.c_str();
}

void PushInsertSubtree(
    UndoStack& undoStack,
    Scene& scene,
    const std::string& commandName,
    const std::function<std::vector<int>(Scene&)>& mutate)
{
    if (!mutate)
    {
        return;
    }

    const ArchivedSelectionState selectionBefore = CaptureArchivedSelection(scene);
    const std::vector<int> insertedRoots = mutate(scene);
    if (insertedRoots.empty())
    {
        return;
    }

    SceneSubtreeArchive archive;
    if (!scene.CreateDeleteArchive(insertedRoots, archive))
    {
        return;
    }

    archive.selectionBefore = selectionBefore;
    archive.selectionAfter = CaptureArchivedSelection(scene);
    undoStack.Push(std::make_unique<InsertSubtreeCommand>(std::move(archive), commandName));
}

bool CopySelection(EditorClipboard& clipboard, Scene& scene)
{
    if (!scene.HasSelection())
    {
        return false;
    }

    const std::vector<int> roots =
        FilterToTopmostSelectedIndices(scene.GetObjects(), scene.GetSelection().indices);
    if (roots.empty())
    {
        return false;
    }

    SceneSubtreeArchive archive;
    if (!scene.CreateDeleteArchive(roots, archive, false))
    {
        return false;
    }

    clipboard.SetSubtreeArchive(std::move(archive));
    return true;
}

void CutSelection(UndoStack& undoStack, EditorClipboard& clipboard, Scene& scene)
{
    if (!CopySelection(clipboard, scene))
    {
        return;
    }

    PushDeleteSelection(undoStack, scene, "Cut");
}

void PushPasteFromClipboard(
    UndoStack& undoStack,
    Scene& scene,
    const EditorClipboard& clipboard,
    int referenceIndex,
    HierarchyInsertMode rootPlacement)
{
    const SceneSubtreeArchive* sourceArchive = clipboard.GetSubtreeArchive();
    if (sourceArchive == nullptr)
    {
        return;
    }

    PushInsertSubtree(undoStack, scene, "Paste", [&](Scene& target) {
        SceneSubtreeArchive workingCopy = CloneSubtreeArchive(*sourceArchive);
        RemapSubtreeArchiveIds(target, workingCopy);
        std::vector<int> insertedRoots =
            target.InsertSubtreeArchive(workingCopy, referenceIndex, rootPlacement);
        if (!insertedRoots.empty())
        {
            target.SetSelection(insertedRoots, insertedRoots.back());
        }

        return insertedRoots;
    });
}

void PushReparentObjects(
    UndoStack& undoStack,
    Scene& scene,
    const std::string& commandName,
    const std::vector<SceneObjectId>& objectIds,
    SceneObjectId referenceId,
    HierarchyInsertMode mode)
{
    const int referenceIndex = scene.FindObjectIndex(referenceId);
    if (referenceIndex < 0 || objectIds.empty())
    {
        return;
    }

    ReparentArchive archive;
    archive.selectionBefore = CaptureArchivedSelection(scene);
    archive.before = CaptureHierarchyArchive(scene);

    bool anyPlaced = false;
    for (SceneObjectId objectId : objectIds)
    {
        if (objectId == referenceId)
        {
            continue;
        }

        const int objectIndex = scene.FindObjectIndex(objectId);
        const int currentReferenceIndex = scene.FindObjectIndex(referenceId);
        if (objectIndex < 0 || currentReferenceIndex < 0)
        {
            continue;
        }

        if (!scene.CanPlaceObjectInHierarchy(objectIndex, currentReferenceIndex, mode)
            || !scene.WouldPlaceObjectInHierarchyChange(objectIndex, currentReferenceIndex, mode))
        {
            continue;
        }

        if (scene.PlaceObjectInHierarchy(objectIndex, currentReferenceIndex, mode))
        {
            anyPlaced = true;
        }
    }

    if (!anyPlaced)
    {
        return;
    }

    archive.after = CaptureHierarchyArchive(scene);
    if (AreHierarchyArchivesEqual(archive.before, archive.after))
    {
        return;
    }

    // Keep multi-select after a package reparent; single-object matches prior SelectSingle.
    if (objectIds.size() > 1)
    {
        ApplyArchivedSelection(scene, archive.selectionBefore);
    }
    else
    {
        const int selectedIndex = scene.FindObjectIndex(objectIds.front());
        if (selectedIndex >= 0)
        {
            scene.SelectSingle(selectedIndex);
        }
    }
    archive.selectionAfter = CaptureArchivedSelection(scene);
    undoStack.Push(std::make_unique<ReparentObjectsCommand>(std::move(archive), commandName));
}

