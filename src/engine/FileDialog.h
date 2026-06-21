#pragma once

#include <string>

namespace FileDialog
{
    // Returns true and sets outPath when the user picks a file. No-op on unsupported platforms.
    bool OpenModelFile(std::string& outPath);
}
