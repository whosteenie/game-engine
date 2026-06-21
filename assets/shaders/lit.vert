#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aTexCoord0;
layout (location = 3) in vec2 aTexCoord1;
layout (location = 4) in vec4 aTangent;

out vec3 vFragPos;
out vec3 vNormal;
out vec2 vTexCoord0;
out vec2 vTexCoord1;
out vec4 vTangent;
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
    vTangent = vec4(normalize(normalMatrix * aTangent.xyz), aTangent.w);
    vTexCoord0 = aTexCoord0;
    vTexCoord1 = aTexCoord1;

    vFragPosLightSpace = uLightSpaceMatrix * worldPos;
    gl_Position = uProjection * uView * worldPos;
}
