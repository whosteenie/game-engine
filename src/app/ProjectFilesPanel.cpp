#include "app/ProjectFilesPanel.h"

#include "app/ProjectSession.h"

#include <imgui.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
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

    bool CollectDirectoryEntries(const std::string& directory, std::vector<DirectoryEntry>& outEntries)
    {
        outEntries.clear();

        std::error_code error;
        if (!fs::exists(directory, error) || !fs::is_directory(directory, error))
        {
            return false;
        }

        for (const fs::directory_entry& entry : fs::directory_iterator(directory, fs::directory_options::skip_permission_denied, error))
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
            // Root defaults to expanded so the tree isn't empty.
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
                if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) && !IsFolderExpanded(selectedFolder, rootPath, openStates))
                {
                    openStates[selectedFolder] = true;
                    selectionChanged = true; // keeps it visible/scrollable
                }
                else if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) && IsFolderExpanded(selectedFolder, rootPath, openStates))
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
}

void ProjectFilesPanel::ResetBrowseState(const std::string& projectRoot)
{
    m_browsedDirectory = projectRoot;
    m_selectedEntryPath.clear();
    m_trackedProjectRoot = projectRoot;
    m_folderOpenStates.clear();
    m_scrollSelectionIntoView = false;
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

void ProjectFilesPanel::DrawFolderTree(const std::string& directory)
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
            // Keep open state in our control so keyboard left/right works reliably.
            const bool isOpen = IsFolderExpanded(entry.path, m_trackedProjectRoot, m_folderOpenStates);
            ImGui::SetNextItemOpen(isOpen, ImGuiCond_Always);
        }

        bool opened = ImGui::TreeNodeEx(entry.name.c_str(), flags);
        if (hasChildren)
        {
            m_folderOpenStates[entry.path] = opened;
        }

        if (ImGui::IsItemClicked())
        {
            m_browsedDirectory = entry.path;
            m_selectedEntryPath = entry.path;
            m_scrollSelectionIntoView = false;
        }

        if (m_scrollSelectionIntoView && entry.path == m_selectedEntryPath)
        {
            ImGui::SetScrollHereY(0.5f);
            // Reset once we actually drew the selected row.
            m_scrollSelectionIntoView = false;
        }

        if (opened && hasChildren)
        {
            DrawFolderTree(entry.path);
            ImGui::TreePop();
        }
    }
}

void ProjectFilesPanel::DrawFileList(const std::string& directory)
{
    std::vector<DirectoryEntry> entries;
    if (!CollectDirectoryEntries(directory, entries))
    {
        ImGui::TextDisabled("Unable to read folder.");
        return;
    }

    if (ImGui::BeginTable(
            "ProjectFilesTable",
            3,
            ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV,
            ImVec2(0.0f, 0.0f)))
    {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.55f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 96.0f);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 72.0f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (const DirectoryEntry& entry : entries)
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);

            const bool isSelected = entry.path == m_selectedEntryPath;
            if (ImGui::Selectable(entry.name.c_str(), isSelected, ImGuiSelectableFlags_SpanAllColumns))
            {
                m_selectedEntryPath = entry.path;
            }

            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && entry.isDirectory)
            {
                m_browsedDirectory = entry.path;
            }

            if (ImGui::BeginPopupContextItem())
            {
                ImGui::TextDisabled("%s", entry.name.c_str());
                ImGui::Separator();

                ImGui::BeginDisabled();
                ImGui::MenuItem("Show in Explorer");
                ImGui::MenuItem("Rename");
                ImGui::MenuItem("Delete");
                ImGui::EndDisabled();

                if (!entry.isDirectory)
                {
                    ImGui::Separator();
                    ImGui::BeginDisabled();
                    ImGui::MenuItem("Import Into Scene");
                    ImGui::EndDisabled();
                }

                ImGui::EndPopup();
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
        }

        ImGui::EndTable();
    }
}

void ProjectFilesPanel::Draw(ProjectSession& project)
{
    ImGui::SetNextWindowPos(ImVec2(8.0f, 520.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(720.0f, 220.0f), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Project", &m_showPanel))
    {
        ImGui::End();
        return;
    }

    const std::string& projectRoot = project.GetProjectRootDirectory();
    if (projectRoot.empty())
    {
        ImGui::End();
        return;
    }

    if (projectRoot != m_trackedProjectRoot)
    {
        ResetBrowseState(projectRoot);
    }

    if (m_browsedDirectory.empty())
    {
        m_browsedDirectory = projectRoot;
    }

    if (m_selectedEntryPath.empty())
    {
        // Default selection drives the file list; needed for keyboard navigation to work immediately.
        m_selectedEntryPath = m_browsedDirectory;
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

    const std::string rootLabel = fs::path(projectRoot).filename().string();
    const bool rootHasChildren = HasChildFolders(projectRoot);
    const bool rootIsOpen = IsFolderExpanded(projectRoot, projectRoot, m_folderOpenStates);
    ImGui::SetNextItemOpen(rootIsOpen, ImGuiCond_Always);

    if (!rootHasChildren)
    {
        rootFlags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }

    const bool rootOpen = ImGui::TreeNodeEx(rootLabel.c_str(), rootFlags);
    if (rootHasChildren)
    {
        m_folderOpenStates[projectRoot] = rootOpen;
    }
    if (ImGui::IsItemClicked())
    {
        m_browsedDirectory = projectRoot;
        m_selectedEntryPath = projectRoot;
    }

    if (rootOpen)
    {
        DrawFolderTree(projectRoot);
        ImGui::TreePop();
    }

    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("ProjectFiles", ImVec2(0.0f, -footerHeight), ImGuiChildFlags_Borders);
    ImGui::TextDisabled("Files");
    ImGui::Separator();
    DrawFileList(m_browsedDirectory);
    ImGui::EndChild();

    if (m_selectedEntryPath.empty())
    {
        ImGui::TextDisabled("No selection");
    }
    else
    {
        ImGui::TextDisabled("%s", m_selectedEntryPath.c_str());
    }

    ImGui::End();
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
}
