#include "engine/rendering/post/ScreenCompositePass.h"

#include "engine/camera/Camera.h"
#include "engine/lighting/DirectionalShadowSettings.h"
#include "engine/lighting/EnvironmentMap.h"
#include "engine/lighting/IBL.h"
#include "engine/platform/SceneRenderTrace.h"
#include "engine/rendering/Framebuffer.h"
#include "engine/rendering/RenderDebug.h"
#include "engine/rendering/Shader.h"

namespace
{
    glm::vec3 SolidBackgroundLinear(const EnvironmentMap& environmentMap)
    {
        const glm::vec3 colorSrgb = environmentMap.GetSolidBackgroundColorSrgb();
        auto srgbToLinear = [](const float channel) {
            return channel <= 0.04045f
                ? channel / 12.92f
                : std::pow((channel + 0.055f) / 1.055f, 2.4f);
        };
        return glm::vec3(
            srgbToLinear(colorSrgb.x),
            srgbToLinear(colorSrgb.y),
            srgbToLinear(colorSrgb.z));
    }

    void SetCompositeBackgroundUniforms(
        Shader& shader,
        const Camera& camera,
        const EnvironmentMap& environmentMap)
    {
        glm::mat4 view = camera.GetViewMatrix();
        view[3] = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        const glm::mat4 invView = glm::inverse(view);
        const glm::mat4 invProjection = glm::inverse(camera.GetProjectionMatrix());

        shader.SetMat4("uInvView", invView);
        shader.SetMat4("uInvProjection", invProjection);
        shader.SetInt(
            "uBackgroundMode",
            environmentMap.UsesSkyboxBackground() ? 0 : 1);
        shader.SetFloat("uSkyboxExposure", environmentMap.GetExposure());
        shader.SetFloat("uEnvironmentRotationY", environmentMap.GetIBL().GetRotationYRadians());
        shader.SetVec3("uSolidBackgroundColor", SolidBackgroundLinear(environmentMap));

        if (environmentMap.UsesSkyboxBackground())
        {
            shader.BindTextureSlot(5, environmentMap.GetIBL().GetHdrEquirectSrvCpuHandle());
            shader.SetInt("uEquirectMap", 5);
        }
    }
}

void ScreenCompositePass::ExecutePreReflection(
    const PostProcessContext& context,
    const ScreenCompositePrePassInputs& inputs,
    ScreenCompositePrePassOutputs& outputs)
{
    outputs.shadowFactorSrv = inputs.shadowFactorSrv;
    outputs.runRadianceAssembly = false;

    if (inputs.sceneFramebuffer == nullptr || inputs.shadowSettings == nullptr)
    {
        return;
    }

    const int width = context.renderWidth;
    const int height = context.renderHeight;
    if (width <= 0 || height <= 0)
    {
        return;
    }

    if (inputs.useShadowFactorComposite && inputs.shadowBlurShader != nullptr
        && inputs.shadowBlurTarget != nullptr && inputs.shadowBlur2Target != nullptr
        && inputs.shadowSettings->GetShadowBlurEnabled()
        && inputs.shadowSettings->GetShadowBlurRadius() > 0.0f)
    {
        const float shadowClear[] = {1.0f, 1.0f, 1.0f, 1.0f};

        inputs.shadowBlurShader->SetInt("uInput", 0);
        inputs.shadowBlurShader->SetInt("uDepthMap", 1);
        inputs.shadowBlurShader->SetMat4("uInvProjection", inputs.inverseProjectionMatrix);
        inputs.shadowBlurShader->SetFloat("uDirectionX", inputs.texelSize.x);
        inputs.shadowBlurShader->SetFloat("uDirectionY", 0.0f);
        inputs.shadowBlurShader->SetFloat("uBlurRadius", inputs.shadowSettings->GetShadowBlurRadius());
        inputs.shadowBlurShader->SetFloat(
            "uDepthThreshold", inputs.shadowSettings->GetShadowBlurDepthThreshold());
        inputs.shadowBlurShader->SetFloat(
            "uShadowThreshold", inputs.shadowSettings->GetShadowBlurShadowThreshold());
        inputs.shadowBlurShader->BindTextureSlot(0, outputs.shadowFactorSrv);
        inputs.shadowBlurShader->BindTextureSlot(1, inputs.sceneFramebuffer->GetDepthSrvCpuHandle());
        context.draw.DrawFullscreenToTarget(
            *inputs.shadowBlurShader,
            *inputs.shadowBlurTarget,
            width,
            height,
            shadowClear);

        inputs.shadowBlurShader->SetFloat("uDirectionX", 0.0f);
        inputs.shadowBlurShader->SetFloat("uDirectionY", inputs.texelSize.y);
        inputs.shadowBlurShader->BindTextureSlot(0, inputs.shadowBlurTarget->srvCpuHandle);
        context.draw.DrawFullscreenToTarget(
            *inputs.shadowBlurShader,
            *inputs.shadowBlur2Target,
            width,
            height,
            shadowClear);

        outputs.shadowFactorSrv = inputs.shadowBlur2Target->srvCpuHandle;
    }

    outputs.runRadianceAssembly =
        !inputs.pbrDebugActive
        && inputs.sceneFramebuffer->HasSplitLighting()
        && inputs.sceneFramebuffer->HasMaterialGbuffer()
        && inputs.radianceTarget != nullptr
        && inputs.radianceTarget->resource != nullptr;

    if (outputs.runRadianceAssembly && inputs.radianceAssemblyShader != nullptr)
    {
        const float radianceClear[] = {0.0f, 0.0f, 0.0f, 0.0f};
        SceneRenderTrace::Scope radianceScope("radiance assembly");
        inputs.radianceAssemblyShader->Use(false);
        inputs.radianceAssemblyShader->SetInt("uDirectLighting", 0);
        inputs.radianceAssemblyShader->SetInt("uIndirectLighting", 1);
        inputs.radianceAssemblyShader->SetInt("uDepthMap", 2);
        inputs.radianceAssemblyShader->SetInt("uMaterial0Map", 3);
        inputs.radianceAssemblyShader->SetInt("uMaterial1Map", 4);
        inputs.radianceAssemblyShader->SetInt("uIncludeFillDirect", 1);
        inputs.radianceAssemblyShader->BindTextureSlot(
            0, inputs.sceneFramebuffer->GetGBufferSrvCpuHandle(GBufferSlot::DirectLighting));
        inputs.radianceAssemblyShader->BindTextureSlot(
            1, inputs.sceneFramebuffer->GetGBufferSrvCpuHandle(GBufferSlot::IndirectLighting));
        inputs.radianceAssemblyShader->BindTextureSlot(2, inputs.sceneFramebuffer->GetDepthSrvCpuHandle());
        inputs.radianceAssemblyShader->BindTextureSlot(
            3, inputs.sceneFramebuffer->GetGBufferSrvCpuHandle(GBufferSlot::MaterialAlbedoRough));
        inputs.radianceAssemblyShader->BindTextureSlot(
            4, inputs.sceneFramebuffer->GetGBufferSrvCpuHandle(GBufferSlot::MaterialMetallic));
        context.draw.DrawFullscreenToTarget(
            *inputs.radianceAssemblyShader,
            *inputs.radianceTarget,
            width,
            height,
            radianceClear);
        radianceScope.Success();
    }
}

void ScreenCompositePass::ExecuteDxrIndirectChain(
    const PostProcessContext& context,
    const ScreenCompositeDxrInputs& inputs,
    ScreenCompositeDxrOutputs& outputs)
{
    outputs.indirectCompositeSrv = inputs.indirectCompositeSrv;
    outputs.runRtIndirect = false;

    if (inputs.sceneFramebuffer == nullptr || inputs.camera == nullptr
        || inputs.environmentMap == nullptr)
    {
        return;
    }

    const int width = context.renderWidth;
    const int height = context.renderHeight;
    if (width <= 0 || height <= 0)
    {
        return;
    }

    outputs.runRtIndirect =
        (inputs.rtCompositeWanted || inputs.rtCompositeDebugOnly)
        && !inputs.pbrDebugActive
        && inputs.sceneFramebuffer->HasSplitLighting()
        && inputs.sceneFramebuffer->HasMaterialGbuffer()
        && inputs.rtIndirectTarget != nullptr
        && inputs.rtIndirectTarget->resource != nullptr
        && inputs.environmentMap->GetIBL().IsReady();

    if (outputs.runRtIndirect && inputs.dxrIndirectShader != nullptr)
    {
        SceneRenderTrace::Scope rtIndirectScope("dxr indirect composite");
        const float indirectClear[] = {0.0f, 0.0f, 0.0f, 1.0f};
        const glm::mat4 viewMatrix = inputs.camera->GetViewMatrix();
        const glm::mat4 invView = glm::inverse(viewMatrix);
        const IBL& ibl = inputs.environmentMap->GetIBL();
        const std::uintptr_t rt1Srv =
            inputs.sceneFramebuffer->GetGBufferSrvCpuHandle(GBufferSlot::IndirectLighting);
        const std::uintptr_t denoisedSrv = inputs.rtHasFreshTrace
            ? (inputs.dxrReflectionDenoisedSrv != 0
                ? inputs.dxrReflectionDenoisedSrv
                : inputs.dxrReflectionSrv)
            : rt1Srv;
        const std::uintptr_t rawSrv =
            inputs.rtHasFreshTrace ? inputs.dxrReflectionSrv : rt1Srv;

        inputs.dxrIndirectShader->Use(false);
        inputs.dxrIndirectShader->SetInt("uIndirectMap", 0);
        inputs.dxrIndirectShader->SetInt("uRtDenoisedMap", 1);
        inputs.dxrIndirectShader->SetInt("uRtRawMap", 2);
        inputs.dxrIndirectShader->SetInt("uDepthMap", 3);
        inputs.dxrIndirectShader->SetInt("uNormalMap", 4);
        inputs.dxrIndirectShader->SetInt("uMaterial0Map", 5);
        inputs.dxrIndirectShader->SetInt("uMaterial1Map", 6);
        inputs.dxrIndirectShader->SetInt("uPrefilterMap", 7);
        inputs.dxrIndirectShader->SetInt("uBrdfLut", 8);
        inputs.dxrIndirectShader->SetMat4("uInvProjection", inputs.inverseProjectionMatrix);
        inputs.dxrIndirectShader->SetMat4("uInvView", invView);
        inputs.dxrIndirectShader->SetFloat("uEnvironmentIntensity", ibl.GetEnvironmentIntensity());
        inputs.dxrIndirectShader->SetFloat("uMaxReflectionLod", ibl.GetMaxReflectionLod());
        inputs.dxrIndirectShader->SetFloat("uStrength", 1.0f);
        inputs.dxrIndirectShader->SetFloat(
            "uMaxTraceDistance",
            inputs.dxrReflectionMaxTraceDistance > 0.0f ? inputs.dxrReflectionMaxTraceDistance : 100.0f);
        inputs.dxrIndirectShader->SetVec2(
            "uRtUvScale",
            glm::vec2(inputs.dxrReflectionUvScaleX, inputs.dxrReflectionUvScaleY));
        inputs.dxrIndirectShader->SetInt(
            "uDebugSpecReplacement",
            inputs.rtCompositeDebugOnly ? 1 : 0);
        inputs.dxrIndirectShader->SetInt("uHasRtTrace", inputs.rtHasFreshTrace ? 1 : 0);
        inputs.dxrIndirectShader->SetFloat("uRoughnessCutoff", inputs.dxrReflectionRoughnessCutoff);
        inputs.dxrIndirectShader->BindTextureSlot(0, rt1Srv);
        inputs.dxrIndirectShader->BindTextureSlot(1, denoisedSrv);
        inputs.dxrIndirectShader->BindTextureSlot(2, rawSrv);
        inputs.dxrIndirectShader->BindTextureSlot(3, inputs.sceneFramebuffer->GetDepthSrvCpuHandle());
        inputs.dxrIndirectShader->BindTextureSlot(
            4, inputs.sceneFramebuffer->GetGBufferSrvCpuHandle(GBufferSlot::ShadingNormal));
        inputs.dxrIndirectShader->BindTextureSlot(
            5, inputs.sceneFramebuffer->GetGBufferSrvCpuHandle(GBufferSlot::MaterialAlbedoRough));
        inputs.dxrIndirectShader->BindTextureSlot(
            6, inputs.sceneFramebuffer->GetGBufferSrvCpuHandle(GBufferSlot::MaterialMetallic));
        inputs.dxrIndirectShader->BindTextureSlot(7, ibl.GetPrefilterMapSrvCpuHandle());
        inputs.dxrIndirectShader->BindTextureSlot(8, ibl.GetBrdfLutSrvCpuHandle());
        context.draw.DrawFullscreenToTarget(
            *inputs.dxrIndirectShader,
            *inputs.rtIndirectTarget,
            width,
            height,
            indirectClear);
        if (inputs.rtCompositeWanted)
        {
            outputs.indirectCompositeSrv = inputs.rtIndirectTarget->srvCpuHandle;
        }
        rtIndirectScope.Success();
    }

    if (inputs.runRtGiInject && inputs.dxrGiInjectShader != nullptr
        && inputs.rtGiInjectTarget != nullptr && inputs.rtGiInjectTarget->resource != nullptr
        && inputs.environmentMap->GetIBL().IsReady())
    {
        SceneRenderTrace::Scope rtGiInjectScope("dxr gi inject");
        const float injectClear[] = {0.0f, 0.0f, 0.0f, 1.0f};
        const IBL& giIbl = inputs.environmentMap->GetIBL();

        inputs.dxrGiInjectShader->Use(false);
        inputs.dxrGiInjectShader->SetInt("uIndirectMap", 0);
        inputs.dxrGiInjectShader->SetInt("uGiDenoisedMap", 1);
        inputs.dxrGiInjectShader->SetInt("uDepthMap", 2);
        inputs.dxrGiInjectShader->SetInt("uMaterial0Map", 3);
        inputs.dxrGiInjectShader->SetInt("uMaterial1Map", 4);
        inputs.dxrGiInjectShader->SetInt("uNormalMap", 5);
        inputs.dxrGiInjectShader->SetMat4("uInvProjection", inputs.inverseProjectionMatrix);
        inputs.dxrGiInjectShader->SetMat4("uInvView", glm::inverse(inputs.camera->GetViewMatrix()));
        inputs.dxrGiInjectShader->SetVec2(
            "uGiUvScale", glm::vec2(inputs.dxrGiUvScaleX, inputs.dxrGiUvScaleY));
        inputs.dxrGiInjectShader->SetFloat("uStrength", inputs.dxrGiStrength);
        inputs.dxrGiInjectShader->SetInt("uDebugGiInject", 0);
        inputs.dxrGiInjectShader->SetInt("uHasGiTrace", inputs.giHasFreshTrace ? 1 : 0);
        inputs.dxrGiInjectShader->SetFloat("uEnvironmentIntensity", giIbl.GetEnvironmentIntensity());
        inputs.dxrGiInjectShader->SetVec4Array(
            "uIrradianceSh",
            giIbl.GetIrradianceSh9().coefficients.data(),
            static_cast<int>(giIbl.GetIrradianceSh9().coefficients.size()));
        inputs.dxrGiInjectShader->BindTextureSlot(0, outputs.indirectCompositeSrv);
        inputs.dxrGiInjectShader->BindTextureSlot(
            1,
            inputs.giHasFreshTrace
                ? inputs.giInjectSrv
                : inputs.sceneFramebuffer->GetGBufferSrvCpuHandle(GBufferSlot::IndirectLighting));
        inputs.dxrGiInjectShader->BindTextureSlot(2, inputs.sceneFramebuffer->GetDepthSrvCpuHandle());
        inputs.dxrGiInjectShader->BindTextureSlot(
            3, inputs.sceneFramebuffer->GetGBufferSrvCpuHandle(GBufferSlot::MaterialAlbedoRough));
        inputs.dxrGiInjectShader->BindTextureSlot(
            4, inputs.sceneFramebuffer->GetGBufferSrvCpuHandle(GBufferSlot::MaterialMetallic));
        inputs.dxrGiInjectShader->BindTextureSlot(
            5, inputs.sceneFramebuffer->GetGBufferSrvCpuHandle(GBufferSlot::ShadingNormal));
        context.draw.DrawFullscreenToTarget(
            *inputs.dxrGiInjectShader,
            *inputs.rtGiInjectTarget,
            width,
            height,
            injectClear);
        outputs.indirectCompositeSrv = inputs.rtGiInjectTarget->srvCpuHandle;
        rtGiInjectScope.Success();
    }
}

void ScreenCompositePass::ExecuteHdrComposite(
    const PostProcessContext& context,
    const ScreenCompositeHdrInputs& inputs,
    ScreenCompositeHdrOutputs& outputs)
{
    outputs.hdrColorSrv = inputs.sceneFramebuffer != nullptr
        ? inputs.sceneFramebuffer->GetGBufferSrvCpuHandle(GBufferSlot::DirectLighting)
        : 0;
    outputs.hdrColorSource = "scene_direct";
    outputs.compositeRan = false;

    if (inputs.compositeShader == nullptr || inputs.hdrCompositeTarget == nullptr
        || inputs.sceneFramebuffer == nullptr || inputs.camera == nullptr
        || inputs.environmentMap == nullptr)
    {
        return;
    }

    const int width = context.renderWidth;
    const int height = context.renderHeight;
    if (width <= 0 || height <= 0)
    {
        return;
    }

    const auto debugMode = static_cast<RenderDebugMode>(inputs.debugMode);

    if (inputs.sceneFramebuffer->HasSplitLighting() && !inputs.pbrDebugActive)
    {
        const float compositeClear[] = {0.0f, 0.0f, 0.0f, 1.0f};

        inputs.compositeShader->SetInt("uDirectLighting", 0);
        inputs.compositeShader->SetInt("uIndirectLighting", 1);
        inputs.compositeShader->SetInt("uDepthMap", 2);
        inputs.compositeShader->SetInt("uSsaoMap", 3);
        inputs.compositeShader->SetInt("uUseSplitLighting", 1);
        inputs.compositeShader->SetInt("uUseSsao", inputs.runAo ? 1 : 0);
        inputs.compositeShader->SetInt("uUseShadowFactor", inputs.useShadowFactorComposite ? 1 : 0);
        inputs.compositeShader->SetInt("uShadowFactorMap", 4);
        inputs.compositeShader->SetFloat("uSsaoPower", inputs.runGtao ? inputs.gtaoPower : inputs.ssaoPower);
        inputs.compositeShader->SetFloat("uAoStrength", inputs.aoStrength);
        inputs.compositeShader->SetInt(
            "uDebugOcclusionOnly",
            debugMode == RenderDebugMode::CompositeOcclusion ? 1 : 0);
        const bool useSsgiInject =
            inputs.runSsgiTrace && inputs.lastSsgiInjectSrv != 0 && !inputs.runRtGiInject;
        inputs.compositeShader->SetInt("uUseSsgi", useSsgiInject ? 1 : 0);
        inputs.compositeShader->SetFloat("uSsgiStrength", inputs.ssgiStrength);
        inputs.compositeShader->SetInt("uSsgiMap", 6);
        const bool useRtShadow =
            inputs.dxrShadowCompositeEnabled && inputs.dxrShadowDenoisedSrv != 0;
        inputs.compositeShader->SetInt("uUseRtShadow", useRtShadow ? 1 : 0);
        inputs.compositeShader->SetInt("uRtShadowMap", 7);
        inputs.compositeShader->SetVec2(
            "uRtShadowUvScale",
            glm::vec2(inputs.dxrShadowUvScaleX, inputs.dxrShadowUvScaleY));
        SetCompositeBackgroundUniforms(*inputs.compositeShader, *inputs.camera, *inputs.environmentMap);
        inputs.compositeShader->BindTextureSlot(
            0, inputs.sceneFramebuffer->GetGBufferSrvCpuHandle(GBufferSlot::DirectLighting));
        inputs.compositeShader->BindTextureSlot(1, inputs.indirectCompositeSrv);
        inputs.compositeShader->BindTextureSlot(2, inputs.sceneFramebuffer->GetDepthSrvCpuHandle());
        inputs.compositeShader->BindTextureSlot(3, inputs.aoCompositeSrv);
        inputs.compositeShader->BindTextureSlot(4, inputs.shadowFactorSrv);
        inputs.compositeShader->BindTextureSlot(
            6,
            useSsgiInject ? inputs.lastSsgiInjectSrv
                : (inputs.radianceTarget != nullptr ? inputs.radianceTarget->srvCpuHandle : 0));
        inputs.compositeShader->BindTextureSlot(
            7, useRtShadow ? inputs.dxrShadowDenoisedSrv : inputs.shadowFactorSrv);
        context.draw.DrawFullscreenToTarget(
            *inputs.compositeShader,
            *inputs.hdrCompositeTarget,
            width,
            height,
            compositeClear);

        outputs.hdrColorSrv = inputs.hdrCompositeTarget->srvCpuHandle;
        outputs.hdrColorSource = "hdr_composite_split";
        outputs.compositeRan = true;
    }
    else if (inputs.runAo)
    {
        const float compositeClear[] = {0.0f, 0.0f, 0.0f, 1.0f};

        inputs.compositeShader->SetInt("uDirectLighting", 0);
        inputs.compositeShader->SetInt("uIndirectLighting", 0);
        inputs.compositeShader->SetInt("uDepthMap", 2);
        inputs.compositeShader->SetInt("uSsaoMap", 3);
        inputs.compositeShader->SetInt("uUseSplitLighting", 0);
        inputs.compositeShader->SetInt("uUseSsao", 1);
        inputs.compositeShader->SetFloat("uSsaoPower", inputs.runGtao ? inputs.gtaoPower : inputs.ssaoPower);
        inputs.compositeShader->SetFloat("uAoStrength", inputs.aoStrength);
        inputs.compositeShader->SetInt(
            "uDebugOcclusionOnly",
            debugMode == RenderDebugMode::CompositeOcclusion ? 1 : 0);
        SetCompositeBackgroundUniforms(*inputs.compositeShader, *inputs.camera, *inputs.environmentMap);
        inputs.compositeShader->BindTextureSlot(
            0, inputs.sceneFramebuffer->GetGBufferSrvCpuHandle(GBufferSlot::DirectLighting));
        inputs.compositeShader->BindTextureSlot(3, inputs.aoCompositeSrv);
        inputs.compositeShader->BindTextureSlot(2, inputs.sceneFramebuffer->GetDepthSrvCpuHandle());
        context.draw.DrawFullscreenToTarget(
            *inputs.compositeShader,
            *inputs.hdrCompositeTarget,
            width,
            height,
            compositeClear);

        outputs.hdrColorSrv = inputs.hdrCompositeTarget->srvCpuHandle;
        outputs.hdrColorSource = "hdr_composite_ssao_only";
        outputs.compositeRan = true;
    }
}
