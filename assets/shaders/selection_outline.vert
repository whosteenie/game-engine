#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;
uniform float uOutlineWidth;
uniform float uOutlineWidthWorld;
uniform float uViewportHeight;

void main()
{
    mat3 normalMatrix = mat3(transpose(inverse(uModel)));
    vec3 worldNormal = normalize(normalMatrix * aNormal);
    vec4 worldPosition = uModel * vec4(aPos, 1.0);

    if (uOutlineWidth > 0.0)
    {
        vec3 viewNormal = normalize(mat3(uView) * worldNormal);
        float silhouette = 1.0 - clamp(abs(viewNormal.z), 0.0, 1.0);
        silhouette *= silhouette;

        worldPosition.xyz += worldNormal * (uOutlineWidthWorld * silhouette);

        vec4 clipPosition = uProjection * uView * worldPosition;
        vec3 clipNormal = mat3(uProjection) * viewNormal;

        vec2 offset = clipNormal.xy;
        float offsetLength = length(offset);
        if (offsetLength > 0.0001)
        {
            offset /= offsetLength;
        }

        float pixelToClip = (2.0 * uOutlineWidth * silhouette) / max(uViewportHeight, 1.0);
        clipPosition.xy += offset * pixelToClip * clipPosition.w;
        clipPosition.z -= 0.0002 * clipPosition.w;
        gl_Position = clipPosition;
        return;
    }

    gl_Position = uProjection * uView * worldPosition;
}
