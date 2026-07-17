#include "app/panels/ProjectFilesPanel.h"

#include "app/editor/EditorIcons.h"
#include "app/editor/EditorPanelConstraints.h"
#include "app/editor/ModelDragDrop.h"
#include "app/project/ProjectSession.h"
#include "app/scene/Scene.h"
#include "app/scene/SceneImportService.h"
#include "app/undo/UndoCommand.h"
#include "app/undo/UndoStack.h"
#include "engine/assets/FileDialog.h"
#include "engine/platform/ImGuiFonts.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    struct DirectoryEntry
    {
        std::string path;
        std::string name;
        bool isDirectory = false;
        std::uintmax_t sizeBytes = 0;
    };

    std::string FormatFileSize(std::uintmax_t bytes)
    {
        if (bytes < 1024)
        {
            return std::to_string(bytes) + " B";
        }

        const double kiloBytes = static_cast<double>(bytes) / 1024.0;
        if (kiloBytes < 1024.0)
        {
            return std::to_string(static_cast<int>(kiloBytes)) + " KB";
        }

        const double megaBytes = kiloBytes / 1024.0;
        return std::to_string(static_cast<int>(megaBytes)) + " MB";
    }

    std::string GetEntryTypeLabel(const DirectoryEntry& entry)
    {
        if (entry.isDirectory)
        {
            return "Folder";
        }

        const std::string extension = fs::path(entry.name).extension().string();
        if (extension.empty())
        {
            return "File";
        }

        if (extension.size() > 1)
        {
            return extension.substr(1) + " File";
        }

        return "File";
    }

    const char* EntryIcon(bool isDirectory, bool isOpen = false)
    {
        if (!ImGuiFonts::IconsAvailable())
        {
            return isDirectory ? "[D]" : "[F]";
        }

        if (isDirectory)
        {
            return isOpen ? ICON_EDITOR_FOLDER_OPEN : ICON_EDITOR_FOLDER;
        }

        return ICON_EDITOR_FILE;
    }

    std::string BuildEntryLabel(bool isDirectory, const std::string& name, bool isOpen = false)
    {
        return std::string(EntryIcon(isDirectory, isOpen)) + "  " + name;
    }

    void DrawCenteredWrappedText(const std::string& text, float width)
    {
        const float left = ImGui::GetCursorPosX();
        std::vector<std::string> lines;
        std::string line;
        for (const char character : text)
        {
            const std::string candidate = line + character;
            if (!line.empty() && ImGui::CalcTextSize(candidate.c_str()).x > width)
            {
                lines.push_back(std::move(line));
                line.assign(1, character);
            }
            else
            {
                line = candidate;
            }
        }

        if (!line.empty())
        {
            lines.push_back(std::move(line));
        }

        for (const std::string& wrappedLine : lines)
        {
            const float lineWidth = ImGui::CalcTextSize(wrappedLine.c_str()).x;
            ImGui::SetCursorPosX(left + std::max(0.0f, (width - lineWidth) * 0.5f));
            ImGui::TextUnformatted(wrappedLine.c_str());
        }

        // Restore the tile's left edge before the caller submits its remaining tile area.
        ImGui::SetCursorPosX(left);
    }

    bool CollectDirectoryEntries(const std::string& directory, std::vector<DirectoryEntry>& outEntries)
    {
        outEntries.clear();

        std::error_code error;
        if (!fs::exists(directory, error) || !fs::is_directory(directory, error))
        {
            return false;
        }

        for (const fs::directory_entry& entry :
             fs::directory_iterator(directory, fs::directory_options::skip_permission_denied, error))
        {
            if (error)
            {
                error.clear();
                continue;
            }

            DirectoryEntry item;
            item.path = entry.path().string();
            item.name = entry.path().filename().string();
            item.isDirectory = entry.is_directory(error);
            if (!item.isDirectory)
            {
                item.sizeBytes = entry.file_size(error);
            }

            outEntries.push_back(std::move(item));
        }

        std::sort(
            outEntries.begin(),
            outEntries.end(),
            [](const DirectoryEntry& left, const DirectoryEntry& right) {
                if (left.isDirectory != right.isDirectory)
                {
                    return left.isDirectory;
                }

                return left.name < right.name;
            });

        return true;
    }

    bool IsFolderExpanded(
        const std::string& folderPath,
        const std::string& rootPath,
        const std::unordered_map<std::string, bool>& openStates)
    {
        if (folderPath == rootPath)
        {
            const auto it = openStates.find(folderPath);
            return it != openStates.end() ? it->second : true;
        }

        const auto it = openStates.find(folderPath);
        return it != openStates.end() && it->second;
    }

    bool HasChildFolders(const std::string& folderPath)
    {
        std::vector<DirectoryEntry> entries;
        if (!CollectDirectoryEntries(folderPath, entries))
        {
            return false;
        }

        for (const DirectoryEntry& entry : entries)
        {
            if (entry.isDirectory)
            {
                return true;
            }
        }

        return false;
    }

    void CollectChildFolders(const std::string& folderPath, std::vector<std::string>& outChildFolders)
    {
        outChildFolders.clear();
        std::vector<DirectoryEntry> entries;
        if (!CollectDirectoryEntries(folderPath, entries))
        {
            return;
        }

        for (const DirectoryEntry& entry : entries)
        {
            if (entry.isDirectory)
            {
                outChildFolders.push_back(entry.path);
            }
        }
    }

    void BuildVisibleFolderOrder(
        const std::string& folderPath,
        const std::string& rootPath,
        const std::unordered_map<std::string, bool>& openStates,
        std::vector<std::string>& outVisibleFolders)
    {
        outVisibleFolders.push_back(folderPath);

        if (!IsFolderExpanded(folderPath, rootPath, openStates))
        {
            return;
        }

        std::vector<std::string> childFolders;
        CollectChildFolders(folderPath, childFolders);
        for (const std::string& childFolder : childFolders)
        {
            BuildVisibleFolderOrder(childFolder, rootPath, openStates, outVisibleFolders);
        }
    }

    bool PathsEqual(const fs::path& left, const fs::path& right)
    {
        std::error_code error;
        return fs::equivalent(left, right, error) || left == right;
    }

    bool IsPathInsideOrEqual(const fs::path& candidate, const fs::path& root)
    {
        std::error_code error;
        const fs::path absoluteCandidate = fs::weakly_canonical(candidate, error);
        const fs::path absoluteRoot = fs::weakly_canonical(root, error);
        if (error)
        {
            return false;
        }

        auto candidateIt = absoluteCandidate.begin();
        for (const auto& rootPart : absoluteRoot)
        {
            if (candidateIt == absoluteCandidate.end() || *candidateIt != rootPart)
            {
                return false;
            }
            ++candidateIt;
        }

        return true;
    }

    bool IsValidEntryName(const std::string& name)
    {
        if (name.empty() || name == "." || name == "..")
        {
            return false;
        }

        return name.find_first_of("\\/:*?\"<>|") == std::string::npos;
    }

    bool DrawInlineRenameField(
        char* renameBuffer,
        std::size_t renameBufferSize,
        bool& focusRenameInput,
        bool& renameInputEngaged,
        bool& cancelRename,
        float inputWidth = -FLT_MIN)
    {
        if (focusRenameInput)
        {
            ImGui::SetKeyboardFocusHere();
        }

        ImGui::SetNextItemWidth(inputWidth);
        const ImGuiInputTextFlags inputFlags =
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll;
        const bool confirmed = ImGui::InputText("##ProjectFileRename", renameBuffer, renameBufferSize, inputFlags);

        if (ImGui::IsItemActive() || ImGui::IsItemFocused())
        {
            renameInputEngaged = true;
            focusRenameInput = false;
        }

        if (ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            cancelRename = true;
        }
        else if (renameInputEngaged && ImGui::IsItemDeactivated() && !confirmed)
        {
            cancelRename = true;
        }

        return confirmed;
    }

    void HandleProjectFilesKeyboardNavigation(
        const std::string& rootPath,
        const bool hasProjectRoot,
        std::unordered_map<std::string, bool>& openStates,
        std::string& browsedDirectory,
        std::string& selectedEntryPath,
        bool& scrollSelectionIntoView)
    {
        if (!hasProjectRoot)
        {
            return;
        }

        if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows))
        {
            return;
        }

        if (ImGui::GetIO().WantTextInput || ImGui::IsAnyItemActive())
        {
            return;
        }

        std::vector<std::string> visibleFolders;
        BuildVisibleFolderOrder(rootPath, rootPath, openStates, visibleFolders);
        if (visibleFolders.empty())
        {
            return;
        }

        const std::string currentSelection =
            std::find(visibleFolders.begin(), visibleFolders.end(), selectedEntryPath) != visibleFolders.end()
                ? selectedEntryPath
                : browsedDirectory;

        const auto currentIt = std::find(visibleFolders.begin(), visibleFolders.end(), currentSelection);
        if (currentIt == visibleFolders.end())
        {
            return;
        }

        const int currentIndex = static_cast<int>(currentIt - visibleFolders.begin());

        bool selectionChanged = false;

        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
        {
            const int nextIndex = currentIndex + 1;
            if (nextIndex < static_cast<int>(visibleFolders.size()))
            {
                selectedEntryPath = visibleFolders[nextIndex];
                browsedDirectory = selectedEntryPath;
                selectionChanged = true;
            }
        }
        else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
        {
            const int prevIndex = currentIndex - 1;
            if (prevIndex >= 0)
            {
                selectedEntryPath = visibleFolders[prevIndex];
                browsedDirectory = selectedEntryPath;
                selectionChanged = true;
            }
        }
        else if (currentIndex >= 0)
        {
            const std::string selectedFolder = visibleFolders[currentIndex];
            if (HasChildFolders(selectedFolder))
            {
                if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)
                    && !IsFolderExpanded(selectedFolder, rootPath, openStates))
                {
                    openStates[selectedFolder] = true;
                    selectionChanged = true;
                }
                else if (
                    ImGui::IsKeyPressed(ImGuiKey_LeftArrow)
                    && IsFolderExpanded(selectedFolder, rootPath, openStates))
                {
                    openStates[selectedFolder] = false;
                    selectionChanged = true;
                }
            }
        }

        if (selectionChanged)
        {
            scrollSelectionIntoView = true;
        }
    }

    const char* MenuLabel(const char* icon, const char* text)
    {
        static thread_local char buffer[96];
        if (ImGuiFonts::IconsAvailable())
        {
            std::snprintf(buffer, sizeof(buffer), "%s  %s", icon, text);
        }
        else
        {
            std::snprintf(buffer, sizeof(buffer), "%s", text);
        }
        return buffer;
    }

    bool IsImportableModelFile(const std::string& path)
    {
        std::string extension = fs::path(path).extension().string();
        for (char& character : extension)
        {
            character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        }

        return extension == ".gltf" || extension == ".glb";
    }
}

void ProjectFilesPanel::ResetBrowseState(const std::string& projectRoot)
{
    m_browsedDirectory = projectRoot;
    m_selectedEntryPath.clear();
    m_trackedProjectRoot = projectRoot;
    m_folderOpenStates.clear();
    m_scrollSelectionIntoView = false;
    CancelRename();
    m_pendingDeletePath.clear();
    m_openDeleteConfirmPopup = false;
    m_statusMessage.clear();
}

void ProjectFilesPanel::CancelRename()
{
    m_renamePath.clear();
    m_renameBuffer[0] = '\0';
    m_beginRenameNextFrame = false;
    m_focusRenameInput = false;
    m_renameInputEngaged = false;
}

void ProjectFilesPanel::BeginRename(const std::string& entryPath)
{
    if (entryPath.empty() || PathsEqual(entryPath, m_trackedProjectRoot))
    {
        m_statusMessage = "Cannot rename the project root.";
        return;
    }

    std::error_code error;
    if (!fs::exists(entryPath, error))
    {
        m_statusMessage = "Selected item no longer exists.";
        return;
    }

    m_selectedEntryPath = entryPath;
    m_renamePath = entryPath;
    const fs::path path(entryPath);
    m_browsedDirectory = path.parent_path().string();
    if (m_browsedDirectory.empty())
    {
        m_browsedDirectory = m_trackedProjectRoot;
    }

    const std::string filename = path.filename().string();
    std::snprintf(m_renameBuffer, sizeof(m_renameBuffer), "%s", filename.c_str());
    m_beginRenameNextFrame = true;
    m_focusRenameInput = false;
    m_renameInputEngaged = false;
    m_statusMessage.clear();
}

bool ProjectFilesPanel::TryCommitRename()
{
    if (m_renamePath.empty())
    {
        return false;
    }

    const std::string newName = m_renameBuffer;
    if (!IsValidEntryName(newName))
    {
        m_statusMessage = "Invalid name.";
        return false;
    }

    const fs::path oldPath(m_renamePath);
    const fs::path newPath = oldPath.parent_path() / newName;
    if (PathsEqual(oldPath, newPath))
    {
        CancelRename();
        return true;
    }

    if (!IsPathInsideOrEqual(newPath.parent_path(), m_trackedProjectRoot))
    {
        m_statusMessage = "Rename must stay inside the project.";
        return false;
    }

    std::error_code error;
    if (fs::exists(newPath, error))
    {
        m_statusMessage = "An item with that name already exists.";
        return false;
    }

    fs::rename(oldPath, newPath, error);
    if (error)
    {
        m_statusMessage = "Rename failed: " + error.message();
        return false;
    }

    const std::string newPathString = newPath.string();
    if (m_selectedEntryPath == m_renamePath)
    {
        m_selectedEntryPath = newPathString;
    }
    if (m_browsedDirectory == m_renamePath)
    {
        m_browsedDirectory = newPathString;
    }

    const auto openStateIt = m_folderOpenStates.find(m_renamePath);
    if (openStateIt != m_folderOpenStates.end())
    {
        m_folderOpenStates[newPathString] = openStateIt->second;
        m_folderOpenStates.erase(openStateIt);
    }

    CancelRename();
    m_statusMessage.clear();
    return true;
}

bool ProjectFilesPanel::TryDeletePath(const std::string& entryPath)
{
    if (entryPath.empty() || PathsEqual(entryPath, m_trackedProjectRoot))
    {
        m_statusMessage = "Cannot delete the project root.";
        return false;
    }

    if (!IsPathInsideOrEqual(entryPath, m_trackedProjectRoot))
    {
        m_statusMessage = "Delete must stay inside the project.";
        return false;
    }

    std::error_code error;
    if (!fs::exists(entryPath, error))
    {
        m_statusMessage = "Selected item no longer exists.";
        return false;
    }

    const bool wasDirectory = fs::is_directory(entryPath, error);
    if (wasDirectory)
    {
        fs::remove_all(entryPath, error);
    }
    else
    {
        fs::remove(entryPath, error);
    }

    if (error)
    {
        m_statusMessage = "Delete failed: " + error.message();
        return false;
    }

    if (m_selectedEntryPath == entryPath
        || IsPathInsideOrEqual(m_selectedEntryPath, entryPath))
    {
        m_selectedEntryPath = fs::path(entryPath).parent_path().string();
    }

    if (m_browsedDirectory == entryPath
        || IsPathInsideOrEqual(m_browsedDirectory, entryPath))
    {
        m_browsedDirectory = fs::path(entryPath).parent_path().string();
    }

    m_folderOpenStates.erase(entryPath);
    if (m_renamePath == entryPath)
    {
        CancelRename();
    }

    m_statusMessage.clear();
    return true;
}

void ProjectFilesPanel::BeginEntrySelectionGesture(const std::string& entryPath)
{
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        m_selectionGesture = SelectionGesture::Entry;
        m_selectionGesturePath = entryPath;
    }
}

void ProjectFilesPanel::BeginBlankSelectionGesture()
{
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)
        && m_selectionGesture == SelectionGesture::None)
    {
        m_selectionGesture = SelectionGesture::Blank;
    }
}

void ProjectFilesPanel::CommitSelectionGesture()
{
    if (!ImGui::IsMouseReleased(ImGuiMouseButton_Left))
    {
        return;
    }

    if (m_selectionGesture == SelectionGesture::Entry)
    {
        m_selectedEntryPath = m_selectionGesturePath;
    }
    else if (m_selectionGesture == SelectionGesture::Blank)
    {
        m_selectedEntryPath.clear();
        CancelRename();
    }

    m_selectionGesture = SelectionGesture::None;
    m_selectionGesturePath.clear();
}

void ProjectFilesPanel::ImportModelIntoScene(ProjectSession& project, const std::string& modelPath)
{
    if (m_drawScene == nullptr || m_drawUndoStack == nullptr)
    {
        m_statusMessage = "Scene is not available for import.";
        return;
    }

    if (!IsImportableModelFile(modelPath))
    {
        m_statusMessage = "Unsupported model file type.";
        return;
    }

    Scene& scene = *m_drawScene;
    UndoStack& undoStack = *m_drawUndoStack;
    const std::string& projectRoot = project.GetProjectRootDirectory();

    PushInsertSubtree(undoStack, scene, "Import Model", [&](Scene& target) {
        const std::vector<int> importedIndices = target.ImportModel(modelPath, -1, projectRoot);
        if (!importedIndices.empty())
        {
            target.SelectSingle(importedIndices.front());
        }

        return importedIndices;
    });

    if (!scene.GetImportService().GetLastImportError().empty())
    {
        project.SetStatusMessage(scene.GetImportService().GetLastImportError());
        m_statusMessage = scene.GetImportService().GetLastImportError();
    }
    else if (!scene.GetImportService().GetLastImportWarning().empty())
    {
        project.SetStatusMessage(scene.GetImportService().GetLastImportWarning());
        m_statusMessage = scene.GetImportService().GetLastImportWarning();
    }
    else
    {
        m_statusMessage.clear();
    }
}

void ProjectFilesPanel::DrawEntryContextMenu(
    ProjectSession& project,
    const std::string& entryPath,
    const std::string& entryName,
    bool isDirectory)
{
    if (!ImGui::BeginPopupContextItem())
    {
        return;
    }

    ImGui::TextDisabled("%s", entryName.c_str());
    ImGui::Separator();

    if (ImGui::MenuItem(MenuLabel(ICON_EDITOR_REVEAL, "Show in Explorer")))
    {
        if (!FileDialog::RevealInExplorer(entryPath))
        {
            m_statusMessage = "Could not open Explorer for this path.";
        }
    }

    const bool canMutate = !PathsEqual(entryPath, m_trackedProjectRoot);
    if (ImGui::MenuItem(MenuLabel(ICON_EDITOR_RENAME, "Rename"), "F2", false, canMutate))
    {
        BeginRename(entryPath);
    }

    if (ImGui::MenuItem(MenuLabel(ICON_EDITOR_TRASH, "Delete"), "Del", false, canMutate))
    {
        m_pendingDeletePath = entryPath;
        m_openDeleteConfirmPopup = true;
    }

    if (!isDirectory)
    {
        ImGui::Separator();
        const bool canImport = IsImportableModelFile(entryPath);
        if (ImGui::MenuItem(
                MenuLabel(ICON_EDITOR_IMPORT, "Import Into Scene"),
                nullptr,
                false,
                canImport))
        {
            ImportModelIntoScene(project, entryPath);
        }
    }

    ImGui::EndPopup();
}

void ProjectFilesPanel::DrawDeleteConfirmPopup()
{
    if (m_openDeleteConfirmPopup)
    {
        ImGui::OpenPopup("Delete Project Item");
        m_openDeleteConfirmPopup = false;
    }

    if (!ImGui::BeginPopupModal("Delete Project Item", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        return;
    }

    const std::string name = fs::path(m_pendingDeletePath).filename().string();
    ImGui::TextWrapped("Delete \"%s\"?\nThis cannot be undone.", name.c_str());
    ImGui::Separator();

    if (ImGui::Button("Delete", ImVec2(120.0f, 0.0f)))
    {
        TryDeletePath(m_pendingDeletePath);
        m_pendingDeletePath.clear();
        ImGui::CloseCurrentPopup();
    }

    ImGui::SetItemDefaultFocus();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)))
    {
        m_pendingDeletePath.clear();
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

void ProjectFilesPanel::HandleFilesPanelHotkeys()
{
    if (ImGui::GetIO().WantTextInput || ImGui::IsAnyItemActive())
    {
        return;
    }

    // Called from the docked Project window so either folder tree or file list focus works.
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows))
    {
        return;
    }

    if (m_selectedEntryPath.empty() || m_renamePath == m_selectedEntryPath)
    {
        return;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_F2))
    {
        BeginRename(m_selectedEntryPath);
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Delete))
    {
        if (!PathsEqual(m_selectedEntryPath, m_trackedProjectRoot))
        {
            m_pendingDeletePath = m_selectedEntryPath;
            m_openDeleteConfirmPopup = true;
        }
    }
}

void ProjectFilesPanel::DrawToolbar(ProjectSession& project)
{
    const bool hasProjectRoot = !project.GetProjectRootDirectory().empty();

    ImGui::BeginDisabled(!hasProjectRoot);
    if (ImGui::SmallButton("Up"))
    {
        const fs::path parent = fs::path(m_browsedDirectory).parent_path();
        const fs::path root(project.GetProjectRootDirectory());
        if (!parent.empty() && parent.string().size() >= root.string().size())
        {
            m_browsedDirectory = parent.string();
        }
    }

    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh"))
    {
        m_statusMessage.clear();
    }

    ImGui::SameLine();
    ImGui::BeginDisabled();
    ImGui::SmallButton("New Folder");
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    char pathBuffer[512] = {};
    std::snprintf(pathBuffer, sizeof(pathBuffer), "%s", m_browsedDirectory.c_str());
    ImGui::InputText("##BrowsePath", pathBuffer, sizeof(pathBuffer), ImGuiInputTextFlags_ReadOnly);
}

void ProjectFilesPanel::DrawFolderTree(ProjectSession& project, const std::string& directory)
{
    std::vector<DirectoryEntry> entries;
    if (!CollectDirectoryEntries(directory, entries))
    {
        ImGui::TextDisabled("Unable to read folder.");
        return;
    }

    for (const DirectoryEntry& entry : entries)
    {
        if (!entry.isDirectory)
        {
            continue;
        }

        ImGui::PushID(entry.path.c_str());

        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (entry.path == m_selectedEntryPath)
        {
            flags |= ImGuiTreeNodeFlags_Selected;
        }

        const bool hasChildren = HasChildFolders(entry.path);
        if (!hasChildren)
        {
            flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        }
        else
        {
            const bool isOpen = IsFolderExpanded(entry.path, m_trackedProjectRoot, m_folderOpenStates);
            ImGui::SetNextItemOpen(isOpen, ImGuiCond_Always);
        }

        const bool folderOpenPreview =
            IsFolderExpanded(entry.path, m_trackedProjectRoot, m_folderOpenStates);
        const std::string label = BuildEntryLabel(true, entry.name, folderOpenPreview);
        bool opened = ImGui::TreeNodeEx(label.c_str(), flags);
        if (hasChildren)
        {
            m_folderOpenStates[entry.path] = opened;
        }

        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            m_browsedDirectory = entry.path;
            BeginEntrySelectionGesture(entry.path);
            m_scrollSelectionIntoView = false;
        }

        DrawEntryContextMenu(project, entry.path, entry.name, true);

        if (m_scrollSelectionIntoView && entry.path == m_selectedEntryPath)
        {
            ImGui::SetScrollHereY(0.5f);
            m_scrollSelectionIntoView = false;
        }

        if (opened && hasChildren)
        {
            DrawFolderTree(project, entry.path);
            ImGui::TreePop();
        }

        ImGui::PopID();
    }
}

void ProjectFilesPanel::DrawFileDetailsView(ProjectSession& project, const std::string& directory)
{
    std::vector<DirectoryEntry> entries;
    if (!CollectDirectoryEntries(directory, entries))
    {
        ImGui::TextDisabled("Unable to read folder.");
        return;
    }

    const ImVec2 tableMin = ImGui::GetCursorScreenPos();
    const ImVec2 tableSize = ImGui::GetContentRegionAvail();
    if (ImGui::BeginTable(
            "ProjectFilesTable",
            3,
            ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY
                | ImGuiTableFlags_BordersInnerV,
            ImVec2(0.0f, 0.0f)))
    {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.55f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 96.0f);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 72.0f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (const DirectoryEntry& entry : entries)
        {
            ImGui::PushID(entry.path.c_str());
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);

            const bool isSelected = entry.path == m_selectedEntryPath;
            const bool isRenaming = entry.path == m_renamePath;

            if (isRenaming)
            {
                if (ImGuiFonts::IconsAvailable())
                {
                    ImGui::TextUnformatted(EntryIcon(entry.isDirectory));
                    ImGui::SameLine();
                }

                bool cancelRename = false;
                if (DrawInlineRenameField(
                        m_renameBuffer,
                        sizeof(m_renameBuffer),
                        m_focusRenameInput,
                        m_renameInputEngaged,
                        cancelRename))
                {
                    TryCommitRename();
                }
                else if (cancelRename)
                {
                    CancelRename();
                }
            }
            else
            {
                const std::string label = BuildEntryLabel(entry.isDirectory, entry.name);
                ImGui::Selectable(
                    label.c_str(),
                    isSelected,
                    ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick);
                if (ImGui::IsItemHovered())
                {
                    BeginEntrySelectionGesture(entry.path);
                }

                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)
                    && entry.isDirectory)
                {
                    m_browsedDirectory = entry.path;
                }

                if (!entry.isDirectory && IsImportableModelFile(entry.path)
                    && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
                {
                    ImGui::SetDragDropPayload(
                        ModelDragDrop::kModelFilePayload,
                        entry.path.c_str(),
                        entry.path.size() + 1);
                    ImGui::TextUnformatted("Import model into scene");
                    ImGui::TextDisabled("%s", entry.name.c_str());
                    ImGui::EndDragDropSource();
                }

                DrawEntryContextMenu(project, entry.path, entry.name, entry.isDirectory);
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(GetEntryTypeLabel(entry).c_str());

            ImGui::TableSetColumnIndex(2);
            if (entry.isDirectory)
            {
                ImGui::TextUnformatted("-");
            }
            else
            {
                ImGui::TextUnformatted(FormatFileSize(entry.sizeBytes).c_str());
            }

            ImGui::PopID();
        }

        ImGui::EndTable();
    }

    if (ImGui::IsMouseHoveringRect(tableMin, tableMin + tableSize))
    {
        BeginBlankSelectionGesture();
    }
}

void ProjectFilesPanel::DrawFileIconView(ProjectSession& project, const std::string& directory)
{
    std::vector<DirectoryEntry> entries;
    if (!CollectDirectoryEntries(directory, entries))
    {
        ImGui::TextDisabled("Unable to read folder.");
        return;
    }

    constexpr float tileWidth = 96.0f;
    constexpr float tileHeight = 104.0f;
    const float iconFontSize = ImGui::GetStyle().FontSizeBase * 2.5f;
    const float iconHeight = iconFontSize + ImGui::GetStyle().FramePadding.y * 2.0f;
    const float itemSpacing = ImGui::GetStyle().ItemSpacing.x;
    const int columns = std::max(
        1,
        static_cast<int>((ImGui::GetContentRegionAvail().x + itemSpacing) / (tileWidth + itemSpacing)));

    for (std::size_t index = 0; index < entries.size(); ++index)
    {
        const DirectoryEntry& entry = entries[index];
        ImGui::PushID(entry.path.c_str());
        ImGui::BeginGroup();
        const float tileTop = ImGui::GetCursorPosY();

        const bool isSelected = entry.path == m_selectedEntryPath;
        const bool isRenaming = entry.path == m_renamePath;
        if (isRenaming)
        {
            bool cancelRename = false;
            if (DrawInlineRenameField(
                    m_renameBuffer,
                    sizeof(m_renameBuffer),
                    m_focusRenameInput,
                    m_renameInputEngaged,
                    cancelRename,
                    tileWidth))
            {
                TryCommitRename();
            }
            else if (cancelRename)
            {
                CancelRename();
            }
        }
        else
        {
            ImGui::PushFont(nullptr, iconFontSize);
            ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.5f, 0.5f));
            ImGui::Selectable(
                EntryIcon(entry.isDirectory),
                isSelected,
                ImGuiSelectableFlags_AllowDoubleClick,
                ImVec2(tileWidth, iconHeight));
            ImGui::PopStyleVar();
            ImGui::PopFont();
            const bool isTileHovered = ImGui::IsItemHovered();
            if (isTileHovered)
            {
                BeginEntrySelectionGesture(entry.path);
            }

            if (isTileHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)
                && entry.isDirectory)
            {
                m_browsedDirectory = entry.path;
            }

            if (!entry.isDirectory && IsImportableModelFile(entry.path)
                && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
            {
                ImGui::SetDragDropPayload(
                    ModelDragDrop::kModelFilePayload,
                    entry.path.c_str(),
                    entry.path.size() + 1);
                ImGui::TextUnformatted("Import model into scene");
                ImGui::TextDisabled("%s", entry.name.c_str());
                ImGui::EndDragDropSource();
            }

            DrawEntryContextMenu(project, entry.path, entry.name, entry.isDirectory);

            DrawCenteredWrappedText(entry.name, tileWidth);
        }

        const float remainingTileHeight = tileHeight - (ImGui::GetCursorPosY() - tileTop);
        if (remainingTileHeight > 0.0f)
        {
            ImGui::Dummy(ImVec2(tileWidth, remainingTileHeight));
        }

        ImGui::EndGroup();
        ImGui::PopID();

        if (index + 1 < entries.size()
            && (index + 1) % static_cast<std::size_t>(columns) != 0)
        {
            ImGui::SameLine();
        }
    }

    if (ImGui::IsWindowHovered() && m_renamePath.empty())
    {
        BeginBlankSelectionGesture();
    }
}

void ProjectFilesPanel::Draw(Scene& scene, ProjectSession& project, UndoStack& undoStack)
{
    m_drawScene = &scene;
    m_drawUndoStack = &undoStack;

    EditorPanelConstraints::ApplyProjectPanel();
    if (!EditorPanelConstraints::BeginDockedPanel("Project", m_showPanel))
    {
        m_drawScene = nullptr;
        m_drawUndoStack = nullptr;
        return;
    }

    const std::string& projectRoot = project.GetProjectRootDirectory();
    if (projectRoot.empty())
    {
        ImGui::End();
        m_drawScene = nullptr;
        m_drawUndoStack = nullptr;
        return;
    }

    if (projectRoot != m_trackedProjectRoot)
    {
        if (m_trackedProjectRoot.empty())
        {
            m_trackedProjectRoot = projectRoot;
        }
        else
        {
            ResetBrowseState(projectRoot);
        }
    }

    if (m_browsedDirectory.empty())
    {
        m_browsedDirectory = projectRoot;
    }

    if (m_beginRenameNextFrame)
    {
        m_focusRenameInput = true;
        m_beginRenameNextFrame = false;
        m_renameInputEngaged = false;
    }

    DrawToolbar(project);
    ImGui::Separator();

    const float availWidth = ImGui::GetContentRegionAvail().x * 0.30f;
    const float folderPaneWidth = availWidth > 180.0f ? availWidth : 180.0f;
    const float footerHeight = ImGui::GetFrameHeightWithSpacing();

    ImGui::BeginChild("ProjectFolders", ImVec2(folderPaneWidth, -footerHeight), ImGuiChildFlags_Borders);
    ImGui::TextDisabled("Folders");
    ImGui::Separator();

    HandleProjectFilesKeyboardNavigation(
        projectRoot,
        true,
        m_folderOpenStates,
        m_browsedDirectory,
        m_selectedEntryPath,
        m_scrollSelectionIntoView);

    ImGuiTreeNodeFlags rootFlags =
        ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (m_selectedEntryPath == projectRoot)
    {
        rootFlags |= ImGuiTreeNodeFlags_Selected;
    }

    const std::string rootName = fs::path(projectRoot).filename().string();
    const bool rootHasChildren = HasChildFolders(projectRoot);
    const bool rootIsOpen = IsFolderExpanded(projectRoot, projectRoot, m_folderOpenStates);
    ImGui::SetNextItemOpen(rootIsOpen, ImGuiCond_Always);

    if (!rootHasChildren)
    {
        rootFlags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }

    const std::string rootLabel = BuildEntryLabel(true, rootName, rootIsOpen);
    const bool rootOpen = ImGui::TreeNodeEx(rootLabel.c_str(), rootFlags);
    if (rootHasChildren)
    {
        m_folderOpenStates[projectRoot] = rootOpen;
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        m_browsedDirectory = projectRoot;
        BeginEntrySelectionGesture(projectRoot);
    }

    DrawEntryContextMenu(project, projectRoot, rootName, true);

    if (rootOpen)
    {
        DrawFolderTree(project, projectRoot);
        ImGui::TreePop();
    }

    {
        const ImVec2 backgroundSpace = ImGui::GetContentRegionAvail();
        if (backgroundSpace.y > 0.0f)
        {
            ImGui::InvisibleButton("##ProjectFoldersBackground", backgroundSpace);
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
            {
                BeginBlankSelectionGesture();
            }
        }
    }

    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("ProjectFiles", ImVec2(0.0f, -footerHeight), ImGuiChildFlags_Borders);
    ImGui::TextDisabled("Files");
    ImGui::SameLine();
    const float viewButtonsWidth = ImGui::CalcTextSize("Details").x
        + ImGui::CalcTextSize("Icons").x + ImGui::GetStyle().ItemSpacing.x
        + ImGui::GetStyle().FramePadding.x * 4.0f;
    const float viewButtonsStart = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - viewButtonsWidth;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), viewButtonsStart));
    if (ImGui::SmallButton("Details"))
    {
        m_fileViewMode = FileViewMode::Details;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Icons"))
    {
        m_fileViewMode = FileViewMode::Icons;
    }
    ImGui::Separator();
    if (m_fileViewMode == FileViewMode::Details)
    {
        DrawFileDetailsView(project, m_browsedDirectory);
    }
    else
    {
        DrawFileIconView(project, m_browsedDirectory);
    }

    if (m_fileViewMode == FileViewMode::Details)
    {
        const ImVec2 backgroundSpace = ImGui::GetContentRegionAvail();
        if (backgroundSpace.y > 0.0f)
        {
            ImGui::InvisibleButton("##ProjectFilesBackground", backgroundSpace);
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
            {
                BeginBlankSelectionGesture();
            }
        }
    }

    ImGui::EndChild();

    HandleFilesPanelHotkeys();
    DrawDeleteConfirmPopup();

    // Clear selection when clicking outside this panel (other panels, viewport, etc.).
    // Skip while any popup is open so context-menu actions like Rename still apply.
    if (!m_selectedEntryPath.empty()
        && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
        && !ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows)
        && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup))
    {
        BeginBlankSelectionGesture();
    }

    CommitSelectionGesture();

    if (!m_statusMessage.empty())
    {
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "%s", m_statusMessage.c_str());
    }
    else if (m_selectedEntryPath.empty())
    {
        ImGui::TextDisabled("No selection");
    }
    else
    {
        ImGui::TextDisabled("%s", m_selectedEntryPath.c_str());
    }

    ImGui::End();
    m_drawScene = nullptr;
    m_drawUndoStack = nullptr;
}

void ProjectFilesPanel::GetBrowseState(
    std::string& outBrowsedDirectory,
    std::string& outSelectedPath,
    std::unordered_map<std::string, bool>& outFolderOpenStates) const
{
    outBrowsedDirectory = m_browsedDirectory;
    outSelectedPath = m_selectedEntryPath;
    outFolderOpenStates = m_folderOpenStates;
}

void ProjectFilesPanel::SetBrowseState(
    const std::string& browsedDirectory,
    const std::string& selectedPath,
    const std::unordered_map<std::string, bool>& folderOpenStates)
{
    m_browsedDirectory = browsedDirectory;
    m_selectedEntryPath = selectedPath;
    m_folderOpenStates = folderOpenStates;
    m_trackedProjectRoot.clear();
    m_scrollSelectionIntoView = !selectedPath.empty();
    CancelRename();
    m_pendingDeletePath.clear();
    m_openDeleteConfirmPopup = false;
    m_statusMessage.clear();
}
