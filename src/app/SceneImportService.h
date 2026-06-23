#pragma once

#include <string>
#include <vector>

class Scene;

class SceneImportService
{
public:
    std::vector<int> ImportModel(
        Scene& scene,
        const std::string& path,
        int parentIndex,
        const std::string& projectRoot);

    const std::string& GetLastImportError() const;
    const std::string& GetLastImportWarning() const;
    void ClearMessages();

private:
    std::string m_lastImportError;
    std::string m_lastImportWarning;
};
