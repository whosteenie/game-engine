#include "engine/rendering/SkyboxRenderer.h"

#include "engine/camera/Camera.h"
#include "engine/lighting/IBL.h"
#include "engine/rendering/Constants.h"
#include "engine/rendering/Shader.h"
#include "engine/rhi/GfxContext.h"

#include <d3d12.h>

namespace
{
    constexpr float kFullscreenQuadVertices[] = {
        -1.0f, -1.0f, 0.0f, 1.0f,
         1.0f, -1.0f, 1.0f, 1.0f,
         1.0f,  1.0f, 1.0f, 0.0f,
        -1.0f, -1.0f, 0.0f, 1.0f,
         1.0f,  1.0f, 1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f, 0.0f,
    };
}

SkyboxRenderer::SkyboxRenderer()
    : m_shader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::SkyBackgroundFragmentShader))
{
    m_quadVertexBuffer.Create(
        GpuBuffer::Type::Vertex,
        kFullscreenQuadVertices,
        static_cast<std::uint32_t>(sizeof(kFullscreenQuadVertices)));
}

SkyboxRenderer::~SkyboxRenderer() = default;

SkyboxRenderer::SkyboxRenderer(SkyboxRenderer&& other) noexcept = default;
SkyboxRenderer& SkyboxRenderer::operator=(SkyboxRenderer&& other) noexcept = default;

void SkyboxRenderer::Draw(
    const Camera& camera,
    const IBL& environment,
    const float exposure,
    const bool splitLightingMrt) const
{
    if (!environment.IsReady() || m_shader == nullptr || !m_quadVertexBuffer.IsValid())
    {
        return;
    }

    glm::mat4 view = camera.GetViewMatrix();
    view[3] = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    const glm::mat4 invView = glm::inverse(view);
    const glm::mat4 invProjection = glm::inverse(camera.GetProjectionMatrix());

    m_shader->Use(splitLightingMrt, false);
    m_shader->SetMat4("uInvProjection", invProjection);
    m_shader->SetMat4("uInvView", invView);
    m_shader->SetFloat("uExposure", exposure);
    m_shader->SetFloat("uRotationY", environment.GetRotationYRadians());
    m_shader->SetInt("uSplitLightingOutput", splitLightingMrt ? 1 : 0);
    m_shader->BindTextureSlot(0, environment.GetHdrEquirectSrvCpuHandle());
    m_shader->SetInt("uEquirectMap", 0);
    m_shader->FlushUniforms();

    auto* commandList = static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_quadVertexBuffer.BindVertex(0, 4 * static_cast<std::uint32_t>(sizeof(float)));
    commandList->DrawInstanced(6, 1, 0, 0);
}
