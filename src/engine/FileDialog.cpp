#include "engine/FileDialog.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>

#include <cstring>
#endif

namespace FileDialog
{
#ifdef _WIN32
    bool ShowOpenFileDialog(
        std::string& outPath,
        const char* filter,
        const char* title,
        const std::string& initialDirectory = "",
        const char* initialFilePattern = "*.*")
    {
        char filePath[MAX_PATH] = {};
        if (!initialDirectory.empty())
        {
            std::string seededPath = initialDirectory;
            const char lastCharacter = seededPath.back();
            if (lastCharacter != '\\' && lastCharacter != '/')
            {
                seededPath += '\\';
            }

            seededPath += initialFilePattern;
            if (seededPath.size() < MAX_PATH)
            {
                std::strncpy(filePath, seededPath.c_str(), MAX_PATH - 1);
            }
        }

        OPENFILENAMEA openInfo = {};
        openInfo.lStructSize = sizeof(openInfo);
        openInfo.lpstrFilter = filter;
        openInfo.lpstrFile = filePath;
        openInfo.nMaxFile = MAX_PATH;
        openInfo.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
        openInfo.lpstrTitle = title;

        if (!GetOpenFileNameA(&openInfo))
        {
            return false;
        }

        outPath = filePath;
        return true;
    }

    bool ShowSaveFileDialog(
        std::string& outPath,
        const char* filter,
        const char* title,
        const char* defaultExtension,
        const std::string& suggestedPath)
    {
        char filePath[MAX_PATH] = {};
        if (!suggestedPath.empty() && suggestedPath.size() < MAX_PATH)
        {
            std::strncpy(filePath, suggestedPath.c_str(), MAX_PATH - 1);
        }

        OPENFILENAMEA saveInfo = {};
        saveInfo.lStructSize = sizeof(saveInfo);
        saveInfo.lpstrFilter = filter;
        saveInfo.lpstrFile = filePath;
        saveInfo.nMaxFile = MAX_PATH;
        saveInfo.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        saveInfo.lpstrTitle = title;
        saveInfo.lpstrDefExt = defaultExtension;

        if (!GetSaveFileNameA(&saveInfo))
        {
            return false;
        }

        outPath = filePath;
        return true;
    }
#endif

    bool OpenModelFile(std::string& outPath)
    {
#ifdef _WIN32
        return ShowOpenFileDialog(
            outPath,
            "glTF Models (*.gltf;*.glb)\0*.gltf;*.glb\0All Files (*.*)\0*.*\0",
            "Import 3D Model");
#else
        (void)outPath;
        return false;
#endif
    }

    bool OpenImageFile(std::string& outPath)
    {
#ifdef _WIN32
        return ShowOpenFileDialog(
            outPath,
            "Images (*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.hdr)\0*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.hdr\0All Files (*.*)\0*.*\0",
            "Choose Texture");
#else
        (void)outPath;
        return false;
#endif
    }

    bool OpenProjectFile(std::string& outPath, const std::string& initialDirectory)
    {
#ifdef _WIN32
        return ShowOpenFileDialog(
            outPath,
            "Game Project (*.gameproject)\0*.gameproject\0All Files (*.*)\0*.*\0",
            "Open Project",
            initialDirectory,
            "*.gameproject");
#else
        (void)outPath;
        (void)initialDirectory;
        return false;
#endif
    }

    bool SaveProjectFile(std::string& outPath, const std::string& suggestedPath)
    {
#ifdef _WIN32
        return ShowSaveFileDialog(
            outPath,
            "Game Project (*.gameproject)\0*.gameproject\0All Files (*.*)\0*.*\0",
            "Save Project As",
            "gameproject",
            suggestedPath);
#else
        (void)outPath;
        (void)suggestedPath;
        return false;
#endif
    }

    bool ChooseProjectFolder(std::string& outPath)
    {
#ifdef _WIN32
        BROWSEINFOA browseInfo = {};
        browseInfo.lpszTitle = "Choose Project Location";
        browseInfo.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_VALIDATE;

        PIDLIST_ABSOLUTE itemList = SHBrowseForFolderA(&browseInfo);
        if (itemList == nullptr)
        {
            return false;
        }

        char folderPath[MAX_PATH] = {};
        const BOOL gotPath = SHGetPathFromIDListA(itemList, folderPath);
        CoTaskMemFree(itemList);

        if (!gotPath)
        {
            return false;
        }

        outPath = folderPath;
        return true;
#else
        (void)outPath;
        return false;
#endif
    }
}
