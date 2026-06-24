#include "engine/gizmos/GizmoDraw.h"

#include "engine/camera/Camera.h"
#include "engine/rendering/Shader.h"
#include "engine/rhi/GfxContext.h"

#include <d3d12.h>

namespace GizmoDraw
{
    void DrawLineVertices(
        const Shader& shader,
        const Camera& camera,
        const std::vector<float>& vertices,
        const glm::vec3& color)
    {
        if (vertices.size() < 6 || vertices.size() % 3 != 0)
        {
            return;
        }

        const std::uint32_t byteSize =
            static_cast<std::uint32_t>(vertices.size() * sizeof(float));
        const GfxContext::TransientUploadAllocation upload =
            GfxContext::Get().AllocateTransientUpload(vertices.data(), byteSize);
        if (upload.gpuAddress == 0 || upload.byteSize == 0)
        {
            return;
        }

        shader.Use();
        shader.SetMat4("uView", camera.GetViewMatrix());
        shader.SetMat4("uProjection", camera.GetProjectionMatrix());
        shader.SetVec3("uColor", color);
        shader.FlushUniforms();

        auto* commandList = static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());
        const std::uint32_t stride = 3 * static_cast<std::uint32_t>(sizeof(float));
        const D3D12_VERTEX_BUFFER_VIEW view{
            upload.gpuAddress,
            upload.byteSize,
            stride};
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
        commandList->IASetVertexBuffers(0, 1, &view);
        commandList->DrawInstanced(static_cast<UINT>(vertices.size() / 3), 1, 0, 0);
    }
}
