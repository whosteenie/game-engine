#pragma once

#include <string>

struct ImportModelAssetResult
{
    bool success = false;
    std::string projectRelativePath;
    std::string absolutePath;
    std::string errorMessage;
};

ImportModelAssetResult ImportModelToProject(
    const std::string& sourceModelPath,
    const std::string& projectRoot);

std::string MakeProjectRelativePath(const std::string& projectRoot, const std::string& absolutePath);
