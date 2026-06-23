#version 330 core
out float FragOcclusion;

in vec2 vTexCoord;

uniform sampler2D uDepthMap;
uniform sampler2D uNoiseMap;

uniform mat4 uProjection;
uniform mat4 uInvProjection;
uniform mat4 uView;
uniform vec3 uLightDirection;

uniform vec3 uSamples[32];
uniform float uRadius;
uniform float uBias;
uniform int uKernelSize;
uniform float uNoiseScaleX;
uniform float uNoiseScaleY;

vec3 ReconstructViewPos(vec2 texCoord, float depth)
{
    vec4 clipSpace = vec4(texCoord * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 viewSpace = uInvProjection * clipSpace;
    return viewSpace.xyz / viewSpace.w;
}

vec3 ReconstructViewNormal(vec2 texCoord, float depth)
{
    vec2 texelSize = 1.0 / vec2(textureSize(uDepthMap, 0));
    vec3 posCenter = ReconstructViewPos(texCoord, depth);
    vec3 posX = ReconstructViewPos(
        texCoord + vec2(texelSize.x, 0.0),
        texture(uDepthMap, texCoord + vec2(texelSize.x, 0.0)).r);
    vec3 posY = ReconstructViewPos(
        texCoord + vec2(0.0, texelSize.y),
        texture(uDepthMap, texCoord + vec2(0.0, texelSize.y)).r);
    vec3 normal = normalize(cross(posX - posCenter, posY - posCenter));
    vec3 viewDir = normalize(-posCenter);
    return faceforward(normal, viewDir, normal);
}

void main()
{
    float depth = texture(uDepthMap, vTexCoord).r;
    if (depth >= 1.0)
    {
        FragOcclusion = 1.0;
        return;
    }

    vec3 fragPos = ReconstructViewPos(vTexCoord, depth);
    vec3 normal = ReconstructViewNormal(vTexCoord, depth);
    vec3 worldNormal = normalize(mat3(transpose(uView)) * normal);

    if (dot(worldNormal, normalize(uLightDirection)) > 0.65)
    {
        FragOcclusion = 1.0;
        return;
    }

    vec3 randomVec = texture(uNoiseMap, vTexCoord * vec2(uNoiseScaleX, uNoiseScaleY)).xyz;
    vec3 tangent = normalize(randomVec - normal * dot(randomVec, normal));
    vec3 bitangent = cross(normal, tangent);
    mat3 tbn = mat3(tangent, bitangent, normal);

    float occlusion = 0.0;
    int validSamples = 0;

    for (int i = 0; i < uKernelSize; ++i)
    {
        vec3 sampleOffset = tbn * uSamples[i];
        vec3 samplePos = fragPos + sampleOffset * uRadius;

        vec4 offset = uProjection * vec4(samplePos, 1.0);
        offset.xyz /= offset.w;
        offset.xyz = offset.xyz * 0.5 + 0.5;

        if (offset.x < 0.0 || offset.x > 1.0 || offset.y < 0.0 || offset.y > 1.0)
        {
            continue;
        }

        float sampleDepth = texture(uDepthMap, offset.xy).r;
        if (sampleDepth >= 1.0)
        {
            continue;
        }

        vec3 sampleViewPos = ReconstructViewPos(offset.xy, sampleDepth);
        float dist3D = length(sampleViewPos - samplePos);

        if (dist3D > uRadius)
        {
            continue;
        }

        if (abs(fragPos.z - sampleViewPos.z) > uRadius)
        {
            continue;
        }

        validSamples++;
        if (sampleViewPos.z >= samplePos.z + uBias)
        {
            occlusion += 1.0;
        }
    }

    if (validSamples == 0)
    {
        FragOcclusion = 1.0;
        return;
    }

    FragOcclusion = 1.0 - (occlusion / float(validSamples));
}
