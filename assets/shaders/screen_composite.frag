#version 330 core
out vec4 FragColor;

in vec2 vTexCoord;

uniform sampler2D uDirectLighting;
uniform sampler2D uIndirectLighting;
uniform sampler2D uDepthMap;
uniform sampler2D uSsaoMap;

uniform int uUseSplitLighting;
uniform int uUseSsao;
uniform float uSsaoPower;
uniform float uAoStrength;
uniform int uDebugOcclusionOnly;

void main()
{
    float depth = texture(uDepthMap, vTexCoord).r;

    vec3 direct = vec3(0.0);
    vec3 indirect = vec3(0.0);

    if (uUseSplitLighting != 0)
    {
        direct = texture(uDirectLighting, vTexCoord).rgb;
        indirect = texture(uIndirectLighting, vTexCoord).rgb;
    }
    else
    {
        vec3 sceneColor = texture(uDirectLighting, vTexCoord).rgb;
        direct = sceneColor;
    }

    if (depth >= 0.9999)
    {
        FragColor = vec4(direct + indirect, 1.0);
        return;
    }

    float indirectOcclusion = 1.0;

    if (uUseSsao != 0)
    {
        float ssao = pow(texture(uSsaoMap, vTexCoord).r, uSsaoPower);
        indirectOcclusion *= mix(1.0, ssao, uAoStrength);
    }

    if (uDebugOcclusionOnly != 0)
    {
        FragColor = vec4(vec3(indirectOcclusion), 1.0);
        return;
    }

    FragColor = vec4(direct + indirect * indirectOcclusion, 1.0);
}
