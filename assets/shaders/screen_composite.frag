#version 330 core
out vec4 FragColor;

in vec2 vTexCoord;

uniform sampler2D uSceneColor;
uniform sampler2D uSsaoMap;
uniform sampler2D uContactShadowMap;

uniform int uUseSsao;
uniform int uUseContactShadows;
uniform float uSsaoPower;
uniform float uAoStrength;
uniform float uContactStrength;

void main()
{
    vec3 sceneColor = texture(uSceneColor, vTexCoord).rgb;
    float occlusion = 1.0;

    if (uUseSsao != 0)
    {
        float ssao = pow(texture(uSsaoMap, vTexCoord).r, uSsaoPower);
        float luma = dot(sceneColor, vec3(0.2126, 0.7152, 0.0722));
        float indirectWeight = 1.0 - smoothstep(0.06, 0.35, luma);
        float ssaoBlend = mix(1.0, ssao, uAoStrength * mix(0.35, 1.0, indirectWeight));
        occlusion *= ssaoBlend;
    }

    if (uUseContactShadows != 0)
    {
        float contact = texture(uContactShadowMap, vTexCoord).r;
        occlusion *= mix(1.0, contact, uContactStrength);
    }

    FragColor = vec4(sceneColor * occlusion, 1.0);
}
