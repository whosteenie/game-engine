#include "engine/lighting/CascadedShadowMap.h"

#include "engine/camera/Camera.h"
#include "engine/lighting/ShadowMapMath.h"
#include "engine/rendering/Shader.h"
#include "engine/rhi/GfxContext.h"

#include <D3D12MemAlloc.h>
#include <d3d12.h>
#include <dxgiformat.h>

#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace
{
    void TransitionResource(
        ID3D12GraphicsCommandList* commandList,
        ID3D12Resource* resource,
        D3D12_RESOURCE_STATES before,
        D3D12_RESOURCE_STATES after)
    {
        if (before == after)
        {
            return;
        }

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        commandList->ResourceBarrier(1, &barrier);
    }
}

CascadedShadowMap::CascadedShadowMap(const int resolutionPerCascade)
    : m_resolution(resolutionPerCascade)
{
    m_dsvIndices.fill(UINT32_MAX);
    CreateResources();
}

CascadedShadowMap::~CascadedShadowMap()
{
    DestroyResources();
}

void CascadedShadowMap::DestroyResources()
{
    if (!GfxContext::Get().IsInitialized())
    {
        return;
    }

    if (m_depthSrvIndex != UINT32_MAX)
    {
        GfxContext::Get().FreeOffscreenSrv(m_depthSrvIndex);
        m_depthSrvIndex = UINT32_MAX;
    }

    for (std::uint32_t& dsvIndex : m_dsvIndices)
    {
        if (dsvIndex != UINT32_MAX)
        {
            GfxContext::Get().FreeOffscreenDsv(dsvIndex);
            dsvIndex = UINT32_MAX;
        }
    }

    if (m_depthAllocation != nullptr)
    {
        static_cast<D3D12MA::Allocation*>(m_depthAllocation)->Release();
        m_depthAllocation = nullptr;
    }

    m_depthResource = nullptr;
    m_depthSrvCpuHandle = 0;
    m_depthInShaderReadState = false;
}

void CascadedShadowMap::CreateResources()
{
    DestroyResources();

    if (!GfxContext::Get().IsInitialized())
    {
        return;
    }

    auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());
    D3D12MA::Allocator* allocator = GfxContext::Get().GetMemoryAllocator();

    D3D12_RESOURCE_DESC depthDesc{};
    depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDesc.Width = static_cast<UINT64>(m_resolution);
    depthDesc.Height = static_cast<UINT>(m_resolution);
    depthDesc.DepthOrArraySize = static_cast<UINT16>(MaxCascades);
    depthDesc.MipLevels = 1;
    depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clearValue{};
    clearValue.Format = DXGI_FORMAT_D32_FLOAT;
    clearValue.DepthStencil.Depth = 1.0f;
    clearValue.DepthStencil.Stencil = 0;

    D3D12MA::ALLOCATION_DESC allocationDesc{};
    allocationDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    ID3D12Resource* depthResource = nullptr;
    D3D12MA::Allocation* depthAllocation = nullptr;
    if (FAILED(allocator->CreateResource(
            &allocationDesc,
            &depthDesc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            &clearValue,
            &depthAllocation,
            IID_PPV_ARGS(&depthResource))))
    {
        throw std::runtime_error("Failed to create cascaded shadow depth texture array");
    }

    m_depthResource = depthResource;
    m_depthAllocation = depthAllocation;

    for (int cascadeIndex = 0; cascadeIndex < MaxCascades; ++cascadeIndex)
    {
        m_dsvIndices[static_cast<std::size_t>(cascadeIndex)] = GfxContext::Get().AllocateOffscreenDsv();

        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dsvDesc.Texture2DArray.FirstArraySlice = static_cast<UINT>(cascadeIndex);
        dsvDesc.Texture2DArray.ArraySize = 1;
        dsvDesc.Texture2DArray.MipSlice = 0;

        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle{};
        dsvHandle.ptr = GfxContext::Get().GetOffscreenDsvCpuHandle(
            m_dsvIndices[static_cast<std::size_t>(cascadeIndex)]);
        device->CreateDepthStencilView(depthResource, &dsvDesc, dsvHandle);
    }

    m_depthSrvIndex = GfxContext::Get().AllocateOffscreenSrv();
    m_depthSrvCpuHandle = GfxContext::Get().GetSrvCpuHandle(m_depthSrvIndex);

    auto* srvHeap = static_cast<ID3D12DescriptorHeap*>(GfxContext::Get().GetSrvHeap());
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpuHandle{};
    srvCpuHandle.ptr = m_depthSrvCpuHandle;

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2DArray.MipLevels = 1;
    srvDesc.Texture2DArray.FirstArraySlice = 0;
    srvDesc.Texture2DArray.ArraySize = static_cast<UINT>(MaxCascades);
    device->CreateShaderResourceView(depthResource, &srvDesc, srvCpuHandle);
    (void)srvHeap;
}

void CascadedShadowMap::SetResolution(const int resolutionPerCascade)
{
    const int clampedResolution = std::clamp(resolutionPerCascade, 512, 8192);
    if (clampedResolution == m_resolution)
    {
        return;
    }

    m_resolution = clampedResolution;
    CreateResources();
}

void CascadedShadowMap::BeginFrame(
    const Camera& camera,
    const glm::vec3& lightDirectionTowardSource,
    const glm::vec3& casterBoundsMin,
    const glm::vec3& casterBoundsMax,
    const bool hasCasterBounds,
    const DirectionalShadowSettings& settings)
{
    m_activeCascadeCount = settings.GetCascadeCount();
    m_savedViewportWidth = static_cast<float>(GfxContext::Get().GetWidth());
    m_savedViewportHeight = static_cast<float>(GfxContext::Get().GetHeight());
    int outputWidth = static_cast<int>(m_savedViewportWidth);
    int outputHeight = static_cast<int>(m_savedViewportHeight);
    GfxContext::Get().GetOutputRenderSize(outputWidth, outputHeight);
    m_savedViewportWidth = static_cast<float>(outputWidth);
    m_savedViewportHeight = static_cast<float>(outputHeight);

    const float shadowDrawDistance = hasCasterBounds
        ? ComputeShadowDrawDistance(camera.GetPosition(), casterBoundsMin, casterBoundsMax)
        : camera.GetFarPlane();
    const float cascadeFarPlane = std::min(camera.GetFarPlane(), shadowDrawDistance);

    const std::vector<float> splitDistances = ComputeCascadeSplitDistances(
        m_activeCascadeCount,
        camera.GetNearPlane(),
        cascadeFarPlane,
        settings.GetCascadeSplitLambda());

    const glm::mat4 inverseViewMatrix = glm::inverse(camera.GetViewMatrix());
    const glm::vec3 cameraPosition = camera.GetPosition();
    const glm::vec3 cameraFront = glm::normalize(camera.GetFront());
    const float cameraMoveDistance = m_hasStableOrthoHalfExtents
        ? glm::length(cameraPosition - m_lastCameraPosition)
        : std::numeric_limits<float>::max();
    const float orientationChange = m_hasLastCameraOrientation
        ? 1.0f - glm::clamp(glm::dot(cameraFront, glm::normalize(m_lastCameraFront)), -1.0f, 1.0f)
        : 2.0f;
    constexpr float kOrientationResetThreshold = 0.025f;
    const bool resetStableFit = cameraMoveDistance > 1.25f ||
        orientationChange > kOrientationResetThreshold ||
        !m_hasStableOrthoHalfExtents ||
        (m_hasLastTightNearPlaneXyFit &&
            settings.GetTightNearPlaneXyFit() != m_lastTightNearPlaneXyFit);
    if (resetStableFit)
    {
        m_stableOrthoHalfExtents.fill(0.0f);
        m_stableOrthoCentersLight.fill(glm::vec2(0.0f));
        m_stableOrthoZNear.fill(0.0f);
        m_stableOrthoZFar.fill(0.0f);
        m_hasStableOrthoHalfExtents = false;
    }

    for (int cascadeIndex = 0; cascadeIndex < m_activeCascadeCount; ++cascadeIndex)
    {
        const float cascadeNear = splitDistances[static_cast<std::size_t>(cascadeIndex)];
        const float cascadeFar = splitDistances[static_cast<std::size_t>(cascadeIndex + 1)];
        m_cascadeEndSplits[static_cast<std::size_t>(cascadeIndex)] = cascadeFar;

        const std::array<glm::vec3, 8> frustumCorners = ComputeCascadeFrustumCorners(
            inverseViewMatrix,
            camera.GetAspect(),
            camera.GetFov(),
            cascadeNear,
            cascadeFar);

        m_cascadeSetups[static_cast<std::size_t>(cascadeIndex)] = BuildShadowLightSpaceForFrustumCorners(
            lightDirectionTowardSource,
            frustumCorners,
            m_resolution,
            settings.GetXyMarginFraction(),
            settings.GetZMarginFraction(),
            hasCasterBounds ? &casterBoundsMin : nullptr,
            hasCasterBounds ? &casterBoundsMax : nullptr,
            settings.GetTightNearPlaneXyFit(),
            nullptr,
            &m_stableOrthoHalfExtents[static_cast<std::size_t>(cascadeIndex)],
            &m_stableOrthoCentersLight[static_cast<std::size_t>(cascadeIndex)],
            &m_stableOrthoZNear[static_cast<std::size_t>(cascadeIndex)],
            &m_stableOrthoZFar[static_cast<std::size_t>(cascadeIndex)],
            resetStableFit);
        m_lightSpaceMatrices[static_cast<std::size_t>(cascadeIndex)] =
            m_cascadeSetups[static_cast<std::size_t>(cascadeIndex)].lightSpaceMatrix;
    }

    m_lastCameraPosition = cameraPosition;
    m_lastCameraFront = cameraFront;
    m_hasLastCameraOrientation = true;
    m_hasStableOrthoHalfExtents = true;
    m_lastTightNearPlaneXyFit = settings.GetTightNearPlaneXyFit();
    m_hasLastTightNearPlaneXyFit = true;
    m_hasRenderedDepth = false;
}

void CascadedShadowMap::BeginCascade(const int cascadeIndex)
{
    if (m_depthResource == nullptr ||
        cascadeIndex < 0 ||
        cascadeIndex >= MaxCascades ||
        m_dsvIndices[static_cast<std::size_t>(cascadeIndex)] == UINT32_MAX)
    {
        return;
    }

    auto* commandList = static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());
    auto* depthResource = static_cast<ID3D12Resource*>(m_depthResource);

    if (cascadeIndex == 0 && m_depthInShaderReadState)
    {
        TransitionResource(
            commandList,
            depthResource,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_DEPTH_WRITE);
        m_depthInShaderReadState = false;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle{};
    dsvHandle.ptr = GfxContext::Get().GetOffscreenDsvCpuHandle(
        m_dsvIndices[static_cast<std::size_t>(cascadeIndex)]);

    commandList->OMSetRenderTargets(0, nullptr, FALSE, &dsvHandle);

    D3D12_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(m_resolution);
    viewport.Height = static_cast<float>(m_resolution);
    viewport.MaxDepth = 1.0f;
    D3D12_RECT scissor{0, 0, m_resolution, m_resolution};
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);

    commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    m_inShadowPass = true;
    m_hasRenderedDepth = true;
}

void CascadedShadowMap::RestoreRasterState() const
{
    if (!m_inShadowPass)
    {
        return;
    }

    auto* commandList = static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());

    D3D12_VIEWPORT viewport{};
    viewport.Width = m_savedViewportWidth;
    viewport.Height = m_savedViewportHeight;
    viewport.MaxDepth = 1.0f;
    D3D12_RECT scissor{
        0,
        0,
        static_cast<LONG>(m_savedViewportWidth),
        static_cast<LONG>(m_savedViewportHeight)};
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);
}

void CascadedShadowMap::EndFrame()
{
    if (m_depthResource != nullptr && m_inShadowPass)
    {
        auto* commandList = static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());
        TransitionResource(
            commandList,
            static_cast<ID3D12Resource*>(m_depthResource),
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_depthInShaderReadState = true;
    }

    RestoreRasterState();
    m_inShadowPass = false;
}

const glm::mat4& CascadedShadowMap::GetLightSpaceMatrix(const int cascadeIndex) const
{
    return m_lightSpaceMatrices[static_cast<std::size_t>(cascadeIndex)];
}

const std::array<glm::mat4, CascadedShadowMap::MaxCascades>& CascadedShadowMap::GetLightSpaceMatrices() const
{
    return m_lightSpaceMatrices;
}

const std::array<ShadowLightSpaceSetup, CascadedShadowMap::MaxCascades>& CascadedShadowMap::GetCascadeSetups() const
{
    return m_cascadeSetups;
}

const std::array<float, CascadedShadowMap::MaxCascades>& CascadedShadowMap::GetCascadeEndSplits() const
{
    return m_cascadeEndSplits;
}

int CascadedShadowMap::GetResolution() const
{
    return m_resolution;
}

int CascadedShadowMap::GetActiveCascadeCount() const
{
    return m_activeCascadeCount;
}

void CascadedShadowMap::BindDepthTexture(const unsigned int textureUnit) const
{
    if (m_depthSrvCpuHandle == 0 || !m_hasRenderedDepth)
    {
        return;
    }

    if (const Shader* shader = Shader::GetActiveShader())
    {
        shader->BindTextureSlot(static_cast<int>(textureUnit), m_depthSrvCpuHandle);
    }
}

bool CascadedShadowMap::HasRenderedDepth() const
{
    return m_hasRenderedDepth;
}
