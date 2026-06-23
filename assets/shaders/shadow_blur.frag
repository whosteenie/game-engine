#version 330 core
out float FragShadow;

in vec2 vTexCoord;

uniform sampler2D uInput;
uniform sampler2D uDepthMap;
uniform mat4 uInvProjection;
uniform float uDirectionX;
uniform float uDirectionY;
uniform float uBlurRadius;
uniform float uDepthThreshold;
uniform float uShadowThreshold;

const float kKernelWeights[5] = float[](
    0.227027,
    0.1945946,
    0.1216216,
    0.054054,
    0.016216);

float ViewDepth(vec2 texCoord)
{
    float depth = texture(uDepthMap, texCoord).r;
    vec4 clipSpace = vec4(texCoord * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 viewSpace = uInvProjection * clipSpace;
    return viewSpace.z / viewSpace.w;
}

float SampleWeight(float centerShadow, float centerViewDepth, vec2 sampleUv, float kernelWeight)
{
    float sampleShadow = texture(uInput, sampleUv).r;
    float sampleViewDepth = ViewDepth(sampleUv);
    float depthWeight = 1.0 - smoothstep(
        uDepthThreshold * 0.5,
        uDepthThreshold,
        abs(sampleViewDepth - centerViewDepth));
    float shadowWeight = 1.0 - smoothstep(
        uShadowThreshold * 0.5,
        uShadowThreshold,
        abs(sampleShadow - centerShadow));
    return kernelWeight * depthWeight * shadowWeight;
}

void main()
{
    vec2 direction = vec2(uDirectionX, uDirectionY) * uBlurRadius;
    float centerShadow = texture(uInput, vTexCoord).r;
    float centerViewDepth = ViewDepth(vTexCoord);

    float result = 0.0;
    float weightSum = 0.0;

    for (int tap = 0; tap < 5; ++tap)
    {
        float kernelWeight = kKernelWeights[tap];
        if (tap == 0)
        {
            result += centerShadow * kernelWeight;
            weightSum += kernelWeight;
            continue;
        }

        vec2 offset = direction * float(tap);
        for (int sign = -1; sign <= 1; sign += 2)
        {
            vec2 sampleUv = vTexCoord + offset * float(sign);
            float weight = SampleWeight(centerShadow, centerViewDepth, sampleUv, kernelWeight);
            result += texture(uInput, sampleUv).r * weight;
            weightSum += weight;
        }
    }

    FragShadow = result / max(weightSum, 1e-5);
}
