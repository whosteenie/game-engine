#include "engine/ProjectAssets.h"

#include <filesystem>
#include <stdexcept>

namespace fs = std::filesystem;

namespace
{
    std::string NormalizeSlashes(std::string path)
    {
        for (char& character : path)
        {
            if (character == '\\')
            {
                character = '/';
            }
        }

        return path;
    }

    std::string GetExtensionLower(const fs::path& path)
    {
        std::string extension = path.extension().string();
        for (char& character : extension)
        {
            character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        }

        return extension;
    }

    fs::path MakeUniqueAssetDirectory(const fs::path& modelsRoot, const std::string& baseName)
    {
        fs::path directory = modelsRoot / baseName;
        if (!fs::exists(directory))
        {
            return directory;
        }

        for (int suffix = 2; suffix < 1000; ++suffix)
        {
            directory = modelsRoot / (baseName + "_" + std::to_string(suffix));
            if (!fs::exists(directory))
            {
                return directory;
            }
        }

        return modelsRoot / (baseName + "_copy");
    }

    void CopyFileCreateParents(const fs::path& source, const fs::path& destination)
    {
        std::error_code error;
        fs::create_directories(destination.parent_path(), error);
        fs::copy_file(source, destination, fs::copy_options::overwrite_existing, error);
        if (error)
        {
            throw std::runtime_error("Failed to copy file: " + source.string());
        }
    }

    void CopyDirectoryRecursive(const fs::path& sourceDirectory, const fs::path& destinationDirectory)
    {
        std::error_code error;
        fs::create_directories(destinationDirectory, error);

        for (const fs::directory_entry& entry : fs::recursive_directory_iterator(sourceDirectory, error))
        {
            if (error)
            {
                throw std::runtime_error("Failed to enumerate model directory: " + sourceDirectory.string());
            }

            const fs::path relativePath = fs::relative(entry.path(), sourceDirectory, error);
            const fs::path destinationPath = destinationDirectory / relativePath;

            if (entry.is_directory())
            {
                fs::create_directories(destinationPath, error);
                continue;
            }

            if (entry.is_regular_file())
            {
                CopyFileCreateParents(entry.path(), destinationPath);
            }
        }
    }
}

std::string MakeProjectRelativePath(const std::string& projectRoot, const std::string& absolutePath)
{
    if (absolutePath.empty())
    {
        return {};
    }

    const std::string normalized = NormalizeSlashes(absolutePath);
    const fs::path stored(absolutePath);

    if (!stored.is_absolute() && normalized.rfind("..", 0) != 0 && !projectRoot.empty())
    {
        const fs::path rooted = fs::path(projectRoot) / stored;
        std::error_code existsError;
        if (fs::exists(rooted, existsError))
        {
            return NormalizeSlashes(stored.generic_string());
        }
    }

    std::error_code error;
    const fs::path absolute = fs::weakly_canonical(fs::path(absolutePath), error);
    if (error)
    {
        return NormalizeSlashes(absolutePath);
    }

    if (!projectRoot.empty())
    {
        const fs::path root = fs::weakly_canonical(fs::path(projectRoot), error);
        if (!error)
        {
            std::error_code relativeError;
            const fs::path relative = fs::relative(absolute, root, relativeError);
            if (!relativeError && !relative.empty() && relative.generic_string().rfind("..", 0) != 0)
            {
                return NormalizeSlashes(relative.generic_string());
            }
        }
    }

    return NormalizeSlashes(absolute.generic_string());
}

ImportModelAssetResult ImportModelToProject(
    const std::string& sourceModelPath,
    const std::string& projectRoot)
{
    ImportModelAssetResult result;

    if (projectRoot.empty())
    {
        result.errorMessage = "Project root is not set.";
        return result;
    }

    std::error_code error;
    const fs::path sourcePath = fs::weakly_canonical(fs::path(sourceModelPath), error);
    if (error || !fs::exists(sourcePath) || !fs::is_regular_file(sourcePath))
    {
        result.errorMessage = "Model file does not exist.";
        return result;
    }

    const std::string extension = GetExtensionLower(sourcePath);
    if (extension != ".gltf" && extension != ".glb")
    {
        result.errorMessage = "Unsupported model file type.";
        return result;
    }

    const fs::path modelsRoot = fs::path(projectRoot) / "Assets" / "Models";
    const fs::path destinationDirectory = MakeUniqueAssetDirectory(modelsRoot, sourcePath.stem().string());
    fs::create_directories(destinationDirectory, error);
    if (error)
    {
        result.errorMessage = "Failed to create project asset directory.";
        return result;
    }

    try
    {
        if (extension == ".glb")
        {
            CopyFileCreateParents(sourcePath, destinationDirectory / sourcePath.filename());
        }
        else
        {
            CopyDirectoryRecursive(sourcePath.parent_path(), destinationDirectory);
        }
    }
    catch (const std::exception& exception)
    {
        result.errorMessage = exception.what();
        return result;
    }

    const fs::path destinationModelPath = destinationDirectory / sourcePath.filename();
    result.success = true;
    result.absolutePath = destinationModelPath.string();
    result.projectRelativePath = MakeProjectRelativePath(projectRoot, result.absolutePath);
    return result;
}
