#version 330 core
out vec4 FragColor;

in vec3 vFragPos;
in vec3 vNormal;

uniform vec3 uLightPos;
uniform vec3 uViewPos;
uniform vec3 uObjectColor;

void main()
{
    float ambientStrength = 0.1;
    float diffuseStrength = 0.7;
    float specularStrength = 0.5;
    float shininess = 32.0;

    vec3 norm = normalize(vNormal);
    vec3 lightDir = normalize(uLightPos - vFragPos);
    vec3 viewDir = normalize(uViewPos - vFragPos);

    vec3 ambient = ambientStrength * uObjectColor;

    float diff = max(dot(norm, lightDir), 0.0);
    vec3 diffuse = diffuseStrength * diff * uObjectColor;

    vec3 reflectDir = reflect(-lightDir, norm);
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), shininess);
    vec3 specular = specularStrength * spec * vec3(1.0);

    vec3 result = ambient + diffuse + specular;
    FragColor = vec4(result, 1.0);
}
