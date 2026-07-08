#include "engine/rendering/post/AmbientOcclusionPass.h"

#include "engine/rendering/Shader.h"

#include <algorithm>

void AmbientOcclusionPass::Execute(
    const PostProcessContext& context,
    const AmbientOcclusionPassInputs& inputs,
    AmbientOcclusionPassOutputs& outputs)
{
    if (!inputs.runSsao && !inputs.runGtao)
    {
        return;
    }

    const int width = context.renderWidth;
    const int height = context.renderHeight;
    if (width <= 0 || height <= 0)
    {
        return;
    }

    const float ssaoClear[] = {1.0f, 1.0f, 1.0f, 1.0f};

    if (inputs.runSsao && inputs.ssaoShader != nullptr && inputs.ssaoTarget != nullptr
        && inputs.kernelSamples != nullptr && inputs.kernelSampleCount > 0)
    {
        glm::vec4 packedKernelSamples[32];
        const int sampleCount = std::min(inputs.kernelSampleCount, 32);
        for (int sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex)
        {
            packedKernelSamples[sampleIndex] =
                glm::vec4(inputs.kernelSamples[sampleIndex], 0.0f);
        }

        inputs.ssaoShader->Use(false);
        inputs.ssaoShader->SetInt("uDepthMap", 0);
        inputs.ssaoShader->SetInt("uNormalMap", 1);
        inputs.ssaoShader->SetInt("uNoiseMap", 2);
        inputs.ssaoShader->SetInt("uUseGeometryNormals", inputs.hasGeometryNormals ? 1 : 0);
        inputs.ssaoShader->SetMat4("uProjection", inputs.projectionMatrix);
        inputs.ssaoShader->SetMat4("uInvProjection", inputs.inverseProjectionMatrix);
        inputs.ssaoShader->SetMat4("uView", inputs.viewMatrix);
        inputs.ssaoShader->SetFloat("uRadius", inputs.ssaoRadius);
        inputs.ssaoShader->SetFloat("uBias", inputs.ssaoBias);
        inputs.ssaoShader->SetFloat("uNearPlane", inputs.nearPlane);
        inputs.ssaoShader->SetFloat("uFarPlane", inputs.farPlane);
        inputs.ssaoShader->SetInt("uKernelSize", sampleCount);
        inputs.ssaoShader->SetInt("uDebugMode", inputs.ssaoShaderDebugMode);
        inputs.ssaoShader->SetVec4Array("uSamples", packedKernelSamples, sampleCount);
        inputs.ssaoShader->BindTextureSlot(0, inputs.depthSrv);
        inputs.ssaoShader->BindTextureSlot(1, inputs.normalSrv);
        inputs.ssaoShader->BindTextureSlot(2, inputs.noiseSrv);
        context.draw.DrawFullscreenToTarget(
            *inputs.ssaoShader, *inputs.ssaoTarget, width, height, ssaoClear);
    }
    else if (inputs.runGtao && inputs.gtaoShader != nullptr && inputs.gtaoRawTarget != nullptr)
    {
        inputs.gtaoShader->Use(false);
        inputs.gtaoShader->SetInt("uDepthMap", 0);
        inputs.gtaoShader->SetInt("uNormalMap", 1);
        inputs.gtaoShader->SetMat4("uProjection", inputs.projectionMatrix);
        inputs.gtaoShader->SetMat4("uInvProjection", inputs.inverseProjectionMatrix);
        inputs.gtaoShader->SetMat4("uView", inputs.viewMatrix);
        inputs.gtaoShader->SetVec2(
            "uProjectionScale",
            glm::vec2(inputs.projectionMatrix[0][0], inputs.projectionMatrix[1][1]));
        inputs.gtaoShader->SetFloat("uRadius", inputs.gtaoRadius);
        inputs.gtaoShader->SetFloat("uThickness", inputs.gtaoThickness);
        inputs.gtaoShader->SetFloat("uFalloff", inputs.gtaoFalloff);
        inputs.gtaoShader->SetFloat("uNearPlane", inputs.nearPlane);
        inputs.gtaoShader->SetFloat("uFarPlane", inputs.farPlane);
        inputs.gtaoShader->SetInt("uDirections", inputs.gtaoDirections);
        inputs.gtaoShader->SetInt("uSteps", inputs.gtaoSteps);
        inputs.gtaoShader->SetInt("uUseGeometryNormals", inputs.hasGeometryNormals ? 1 : 0);
        inputs.gtaoShader->BindTextureSlot(0, inputs.depthSrv);
        inputs.gtaoShader->BindTextureSlot(1, inputs.normalSrv);
        context.draw.DrawFullscreenToTarget(
            *inputs.gtaoShader, *inputs.gtaoRawTarget, width, height, ssaoClear);
        outputs.aoCompositeSrv = inputs.gtaoRawTarget->srvCpuHandle;
    }

    if (inputs.blurShader == nullptr || inputs.ssaoTarget == nullptr
        || inputs.ssaoBlurTarget == nullptr)
    {
        if (outputs.aoCompositeSrv == 0 && inputs.ssaoTarget != nullptr)
        {
            outputs.aoCompositeSrv = inputs.ssaoTarget->srvCpuHandle;
        }
        return;
    }

    if ((inputs.runSsao && inputs.ssaoShaderDebugMode == 0)
        || (inputs.runGtao && inputs.gtaoDenoiseEnabled))
    {
        inputs.blurShader->Use(false);
        inputs.blurShader->SetInt("uInput", 0);
        inputs.blurShader->SetInt("uDepthMap", 1);
        inputs.blurShader->SetInt("uNormalMap", 2);
        inputs.blurShader->SetMat4("uInvProjection", inputs.inverseProjectionMatrix);
        inputs.blurShader->SetVec2("uTexelSize", inputs.texelSize);
        inputs.blurShader->SetFloat("uDepthThreshold", inputs.ssaoBlurDepthThreshold);
        inputs.blurShader->SetFloat("uBlurSpread", inputs.runGtao ? 0.8f : 1.0f);
        inputs.blurShader->SetFloat("uNormalPower", inputs.runGtao ? 8.0f : 4.0f);
        inputs.blurShader->SetInt("uUseNormalWeight", inputs.hasGeometryNormals ? 1 : 0);

        const std::uintptr_t rawAoSrv = inputs.runGtao && inputs.gtaoRawTarget != nullptr
            ? inputs.gtaoRawTarget->srvCpuHandle
            : inputs.ssaoTarget->srvCpuHandle;

        inputs.blurShader->SetVec2("uBlurDirection", glm::vec2(1.0f, 0.0f));
        inputs.blurShader->BindTextureSlot(0, rawAoSrv);
        inputs.blurShader->BindTextureSlot(1, inputs.depthSrv);
        inputs.blurShader->BindTextureSlot(2, inputs.normalSrv);
        context.draw.DrawFullscreenToTarget(
            *inputs.blurShader, *inputs.ssaoBlurTarget, width, height, ssaoClear);

        inputs.blurShader->SetVec2("uBlurDirection", glm::vec2(0.0f, 1.0f));
        inputs.blurShader->BindTextureSlot(0, inputs.ssaoBlurTarget->srvCpuHandle);
        inputs.blurShader->BindTextureSlot(1, inputs.depthSrv);
        inputs.blurShader->BindTextureSlot(2, inputs.normalSrv);
        context.draw.DrawFullscreenToTarget(
            *inputs.blurShader, *inputs.ssaoTarget, width, height, ssaoClear);

        if (inputs.runSsao)
        {
            inputs.blurShader->SetFloat("uBlurSpread", 2.5f);
            inputs.blurShader->SetVec2("uBlurDirection", glm::vec2(1.0f, 0.0f));
            inputs.blurShader->BindTextureSlot(0, inputs.ssaoTarget->srvCpuHandle);
            inputs.blurShader->BindTextureSlot(1, inputs.depthSrv);
            inputs.blurShader->BindTextureSlot(2, inputs.normalSrv);
            context.draw.DrawFullscreenToTarget(
                *inputs.blurShader, *inputs.ssaoBlurTarget, width, height, ssaoClear);

            inputs.blurShader->SetVec2("uBlurDirection", glm::vec2(0.0f, 1.0f));
            inputs.blurShader->BindTextureSlot(0, inputs.ssaoBlurTarget->srvCpuHandle);
            inputs.blurShader->BindTextureSlot(1, inputs.depthSrv);
            inputs.blurShader->BindTextureSlot(2, inputs.normalSrv);
            context.draw.DrawFullscreenToTarget(
                *inputs.blurShader, *inputs.ssaoTarget, width, height, ssaoClear);
        }

        outputs.aoCompositeSrv = inputs.ssaoTarget->srvCpuHandle;
    }
    else if (outputs.aoCompositeSrv == 0)
    {
        outputs.aoCompositeSrv = inputs.ssaoTarget->srvCpuHandle;
    }
}
