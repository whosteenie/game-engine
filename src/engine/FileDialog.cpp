#include "engine/FileDialog.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#endif

namespace FileDialog
{
    bool OpenModelFile(std::string& outPath)
    {
#ifdef _WIN32
        char filePath[MAX_PATH] = {};
        OPENFILENAMEA openInfo = {};
        openInfo.lStructSize = sizeof(openInfo);
        openInfo.lpstrFilter = "glTF Models (*.gltf;*.glb)\0*.gltf;*.glb\0All Files (*.*)\0*.*\0";
        openInfo.lpstrFile = filePath;
        openInfo.nMaxFile = MAX_PATH;
        openInfo.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        openInfo.lpstrTitle = "Import 3D Model";

        if (!GetOpenFileNameA(&openInfo))
        {
            return false;
        }

        outPath = filePath;
        return true;
#else
        (void)outPath;
        return false;
#endif
    }
}
