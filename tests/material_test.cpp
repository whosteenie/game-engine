#include "engine/components/ComponentSerialization.h"
#include "engine/rendering/Constants.h"
#include "engine/rendering/Material.h"

#include "test_expect.h"

#include <cmath>
#include <memory>
#include <string>

namespace
{
    MaterialStoredPathFn IdentityStoredPath()
    {
        return [](const std::string& path) { return path; };
    }

    MaterialResolvePathFn IdentityResolvePath()
    {
        return [](const std::string& path) { return path; };
    }

    Material MakeDefaultMaterial()
    {
        return Material(
            EngineConstants::LitVertexShader,
            EngineConstants::PbrFragmentShader,
            glm::vec3(0.8f, 0.2f, 0.1f),
            0.5f,
            0.0f);
    }
}

void RunMaterialTests()
{
    {
        Material material = MakeDefaultMaterial();
        test::ExpectNear(material.GetTransmission(), 0.0f, 1e-6f, "Default transmission should be 0");
        test::ExpectNear(material.GetIndexOfRefraction(), 1.5f, 1e-6f, "Default IOR should be 1.5");
        test::ExpectTrue(!material.IsThinWalled(), "Default thinWalled should be false");
    }

    {
        Material material = MakeDefaultMaterial();
        material.SetTransmission(1.5f);
        material.SetIndexOfRefraction(0.25f);
        test::ExpectNear(material.GetTransmission(), 1.0f, 1e-6f, "Transmission should clamp to 1");
        test::ExpectNear(material.GetIndexOfRefraction(), 1.0f, 1e-6f, "IOR should clamp to 1");
        material.SetIndexOfRefraction(5.0f);
        test::ExpectNear(material.GetIndexOfRefraction(), 3.0f, 1e-6f, "IOR should clamp to 3");
    }

    {
        Material material = MakeDefaultMaterial();
        material.SetTransmission(0.85f);
        material.SetThinWalled(true);
        material.SetIndexOfRefraction(1.45f);
        test::ExpectTrue(material.IsThinWalled(), "thinWalled setter should persist");
        test::ExpectNear(material.GetTransmission(), 0.85f, 1e-6f, "Transmission setter should persist");
        test::ExpectNear(material.GetIndexOfRefraction(), 1.45f, 1e-6f, "IOR setter should persist");
    }

    {
        Material material = MakeDefaultMaterial();
        material.SetTransmission(0.9f);
        material.SetThinWalled(true);
        material.SetIndexOfRefraction(1.52f);
        material.SetMetallic(0.1f);
        material.SetDoubleSided(true);

        const nlohmann::json json =
            MaterialToJson(material, IdentityStoredPath());

        test::ExpectNear(json.at("transmission").get<float>(), 0.9f, 1e-6f, "JSON transmission");
        test::ExpectTrue(json.at("thinWalled").get<bool>(), "JSON thinWalled");
        test::ExpectNear(json.at("indexOfRefraction").get<float>(), 1.52f, 1e-6f, "JSON IOR");
        test::ExpectTrue(json.at("doubleSided").get<bool>(), "JSON doubleSided");

        std::unique_ptr<Material> restored = MaterialFromJson(
            json,
            IdentityResolvePath(),
            IdentityStoredPath());
        test::ExpectTrue(restored != nullptr, "MaterialFromJson should return material");
        test::ExpectTrue(
            material.ContentEquals(*restored),
            "Material JSON round-trip should preserve content");
    }
}
