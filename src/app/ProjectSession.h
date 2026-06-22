#pragma once

#include <string>

class Scene;

class ProjectSession
{
public:
    static constexpr const char* ProjectFileExtension = ".gameproject";

    bool HasActiveProject() const { return m_hasActiveProject; }
    bool IsUntitled() const { return m_projectFilePath.empty(); }
    bool IsDirty() const { return m_dirty; }
    const std::string& GetDisplayName() const { return m_displayName; }
    const std::string& GetProjectFilePath() const { return m_projectFilePath; }
    const std::string& GetProjectRootDirectory() const { return m_projectRootDirectory; }
    const std::string& GetStatusMessage() const { return m_statusMessage; }

    void MarkDirty();
    void MarkClean();
    void SetStatusMessage(const std::string& message);

    void CloseProject();

    bool CreateNewProject(Scene& scene, const std::string& directory, const std::string& projectName);
    bool OpenProject(Scene& scene, const std::string& projectFilePath);

    bool Save(Scene& scene);
    bool SaveAs(Scene& scene, const std::string& projectFilePath);

    static std::string SanitizeProjectName(const std::string& projectName);

private:
    void SetProjectFilePath(const std::string& projectFilePath);
    void SetProjectRootDirectory(const std::string& directory);
    static std::string ExtractDisplayName(const std::string& projectFilePath);

    std::string m_projectFilePath;
    std::string m_projectRootDirectory;
    std::string m_displayName = "Untitled";
    std::string m_statusMessage;
    bool m_dirty = false;
    bool m_hasActiveProject = false;
};
