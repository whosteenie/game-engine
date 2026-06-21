#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aTexCoord;
layout (location = 3) in vec3 aTangent;

out vec3 vFragPos;
out vec3 vNormal;
out vec2 vTexCoord;
out vec3 vTangent;
out vec4 vFragPosLightSpace;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;
uniform mat4 uLightSpaceMatrix;

void main()
{
    vec4 worldPos = uModel * vec4(aPos, 1.0);
    vFragPos = worldPos.xyz;

    mat3 normalMatrix = mat3(transpose(inverse(uModel)));
    vNormal = normalMatrix * aNormal;
    vTangent = normalMatrix * aTangent;
    vTexCoord = aTexCoord;

    vFragPosLightSpace = uLightSpaceMatrix * worldPos;
    gl_Position = uProjection * uView * worldPos;
}
