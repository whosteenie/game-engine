#pragma once

#include <nlohmann/json_fwd.hpp>

#include <string>

class Scene;

struct SceneProjectIO
{
    static constexpr int CurrentFormatVersion = 1;

    static bool Save(
        const Scene& scene,
        const std::string& projectRoot,
        const std::string& projectFilePath,
        std::string& outError);

    static bool Load(
        Scene& scene,
        const std::string& projectRoot,
        const std::string& projectFilePath,
        std::string& outError);

private:
    static nlohmann::json SerializeScene(const Scene& scene, const std::string& projectRoot);
    static bool DeserializeScene(
        Scene& scene,
        const nlohmann::json& root,
        const std::string& projectRoot,
        std::string& outError);
};
