#include "app/ProjectFilesPanel.h"

#include "app/ProjectSession.h"

#include <imgui.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <system_error>
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
}

void ProjectFilesPanel::ResetBrowseState(const std::string& projectRoot)
{
    m_browsedDirectory = projectRoot;
    m_selectedEntryPath.clear();
    m_trackedProjectRoot = projectRoot;
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
        if (entry.path == m_browsedDirectory)
        {
            flags |= ImGuiTreeNodeFlags_Selected;
        }

        const bool opened = ImGui::TreeNodeEx(entry.name.c_str(), flags);
        if (ImGui::IsItemClicked())
        {
            m_browsedDirectory = entry.path;
            m_selectedEntryPath = entry.path;
        }

        if (opened)
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
        ImGui::TextDisabled("No project folder yet.");
        ImGui::TextUnformatted("Use File > New Project or Open Project to browse assets here.");
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

    DrawToolbar(project);
    ImGui::Separator();

    const float availWidth = ImGui::GetContentRegionAvail().x * 0.30f;
    const float folderPaneWidth = availWidth > 180.0f ? availWidth : 180.0f;
    const float footerHeight = ImGui::GetFrameHeightWithSpacing();

    ImGui::BeginChild("ProjectFolders", ImVec2(folderPaneWidth, -footerHeight), ImGuiChildFlags_Borders);
    ImGui::TextDisabled("Folders");
    ImGui::Separator();

    ImGuiTreeNodeFlags rootFlags =
        ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (m_browsedDirectory == projectRoot)
    {
        rootFlags |= ImGuiTreeNodeFlags_Selected;
    }

    const std::string rootLabel = fs::path(projectRoot).filename().string();
    const bool rootOpen = ImGui::TreeNodeEx(rootLabel.c_str(), rootFlags);
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
