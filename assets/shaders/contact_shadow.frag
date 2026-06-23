#version 330 core
out float FragShadow;

in vec2 vTexCoord;

uniform sampler2D uDepthMap;
uniform mat4 uProjection;
uniform mat4 uInvProjection;
uniform mat4 uView;
uniform vec3 uLightDirection;
uniform float uMaxDistance;
uniform int uStepCount;

const float kThickness = 0.02;

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
        FragShadow = 1.0;
        return;
    }

    vec3 viewPos = ReconstructViewPos(vTexCoord, depth);
    vec3 normal = ReconstructViewNormal(vTexCoord, depth);
    vec3 worldNormal = normalize(mat3(transpose(uView)) * normal);
    vec3 lightDir = normalize(uLightDirection);

    if (worldNormal.y > 0.85)
    {
        FragShadow = 1.0;
        return;
    }

    float worldLightFacing = dot(worldNormal, lightDir);
    if (worldLightFacing > 0.55)
    {
        FragShadow = 1.0;
        return;
    }

    // Surfaces already in shadow from the directional light are handled by the shadow map.
    // Ray-marching here hits the object's own depth and produces screen-space artifacts.
    if (worldLightFacing < 0.15)
    {
        FragShadow = 1.0;
        return;
    }

    vec3 viewLightDir = normalize((uView * vec4(lightDir, 0.0)).xyz);
    float facingLight = dot(normal, viewLightDir);

    if (facingLight < 0.15)
    {
        FragShadow = 1.0;
        return;
    }

    float stepSize = uMaxDistance / float(uStepCount);
    float shadow = 1.0;

    for (int step = 1; step <= uStepCount; ++step)
    {
        float rayTravel = stepSize * float(step);
        vec3 rayViewPos = viewPos + viewLightDir * rayTravel;

        vec4 projected = uProjection * vec4(rayViewPos, 1.0);
        projected.xyz /= projected.w;

        if (projected.z < -1.0 || projected.z > 1.0)
        {
            break;
        }

        vec2 sampleUv = projected.xy * 0.5 + 0.5;
        if (sampleUv.x < 0.0 || sampleUv.x > 1.0 || sampleUv.y < 0.0 || sampleUv.y > 1.0)
        {
            continue;
        }

        sampleUv = clamp(sampleUv, vec2(0.0005), vec2(0.9995));

        float sampleDepth = texture(uDepthMap, sampleUv).r;
        vec3 sceneViewPos = ReconstructViewPos(sampleUv, sampleDepth);
        vec3 toOccluder = sceneViewPos - viewPos;
        float distAlongLight = dot(toOccluder, viewLightDir);

        if (distAlongLight <= 0.0 || distAlongLight > rayTravel + kThickness)
        {
            continue;
        }

        vec3 closestOnRay = viewPos + viewLightDir * distAlongLight;
        if (length(sceneViewPos - closestOnRay) > kThickness * 3.0)
        {
            continue;
        }

        if (sceneViewPos.z > rayViewPos.z - kThickness)
        {
            float fade = 1.0 - (float(step) / float(uStepCount));
            shadow = mix(1.0, 0.0, fade * facingLight);
            break;
        }
    }

    FragShadow = shadow;
}
