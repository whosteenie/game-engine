#version 330 core
out vec4 FragColor;

in vec3 vFragPos;
in vec3 vNormal;

uniform vec3 uLightPos;
uniform vec3 uLightColor;
uniform vec3 uViewPos;
uniform vec3 uObjectColor;

uniform float uAmbientStrength;
uniform float uDiffuseStrength;
uniform float uSpecularStrength;
uniform float uShininess;
uniform float uAttenConstant;
uniform float uAttenLinear;
uniform float uAttenQuadratic;
uniform float uDiffuseWrap;
uniform float uIndirectStrength;

uniform vec3 uFillLightDirection;
uniform vec3 uFillLightColor;
uniform float uFillLightStrength;

float WrapDiffuse(float nDotL, float wrap)
{
    return clamp((nDotL + wrap) / (1.0 + wrap), 0.0, 1.0);
}

void main()
{
    vec3 norm = normalize(vNormal);
    vec3 lightDir = normalize(uLightPos - vFragPos);
    vec3 viewDir = normalize(uViewPos - vFragPos);

    float distance = length(uLightPos - vFragPos);
    float attenuation = 1.0 / (
        uAttenConstant +
        uAttenLinear * distance +
        uAttenQuadratic * distance * distance);

    vec3 skyAmbient = vec3(0.06, 0.06, 0.09);
    vec3 groundAmbient = vec3(0.02, 0.02, 0.025);
    float hemisphere = norm.y * 0.5 + 0.5;
    vec3 ambient = mix(groundAmbient, skyAmbient, hemisphere) * uObjectColor * uAmbientStrength;

    // Fake bounced light from the key — surfaces facing the key pick up a soft red tint in shadow.
    vec3 bounceDir = normalize(uLightPos);
    float bounce = max(dot(norm, bounceDir), 0.0);
    vec3 indirect = bounce * uIndirectStrength * uObjectColor * uLightColor;

    float keyDiff = WrapDiffuse(dot(norm, lightDir), uDiffuseWrap);
    vec3 keyDiffuse = uDiffuseStrength * keyDiff * uObjectColor * uLightColor;

    vec3 halfDir = normalize(lightDir + viewDir);
    float spec = pow(max(dot(norm, halfDir), 0.0), uShininess);
    vec3 specular = uSpecularStrength * spec * uLightColor;

    vec3 fillDir = normalize(uFillLightDirection);
    float fillDiff = max(dot(norm, fillDir), 0.0);
    vec3 fillDiffuse = uFillLightStrength * fillDiff * uFillLightColor * uObjectColor;

    vec3 result = ambient + indirect + (keyDiffuse + specular) * attenuation + fillDiffuse;
    FragColor = vec4(result, 1.0);
}
