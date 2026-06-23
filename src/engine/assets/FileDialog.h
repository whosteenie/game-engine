#pragma once

#include <string>

namespace FileDialog
{
    // Returns true and sets outPath when the user picks a file or folder.
    // No-op on unsupported platforms.

    bool OpenModelFile(std::string& outPath);
    bool OpenImageFile(std::string& outPath);
    bool OpenProjectFile(std::string& outPath, const std::string& initialDirectory = "");
    bool SaveProjectFile(std::string& outPath, const std::string& suggestedPath = "");
    bool ChooseProjectFolder(std::string& outPath);
}
