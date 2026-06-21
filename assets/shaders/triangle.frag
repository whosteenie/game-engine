#version 330 core
out vec4 FragColor;

in vec3 vFragPos;
in vec3 vNormal;

uniform vec3 uLightPos;
uniform vec3 uObjectColor;

void main()
{
    float ambientStrength = 0.15;
    float diffuseStrength = 0.85;

    vec3 norm = normalize(vNormal);
    vec3 lightDir = normalize(uLightPos - vFragPos);
    float diff = max(dot(norm, lightDir), 0.0);

    vec3 ambient = ambientStrength * uObjectColor;
    vec3 diffuse = diffuseStrength * diff * uObjectColor;
    vec3 result = ambient + diffuse;

    FragColor = vec4(result, 1.0);
}