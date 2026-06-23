#version 330 core

out vec4 FragColor;

in vec2 vTexCoord;

uniform sampler2D uMask;
uniform vec2 uTexelSize;
uniform float uOutlineWidth;

float ComputeEdgeAlpha(vec2 uv)
{
    const int radius = 4;
    float dilated = 0.0;
    float eroded = 1.0;

    for (int y = -radius; y <= radius; ++y)
    {
        for (int x = -radius; x <= radius; ++x)
        {
            float dist = length(vec2(x, y));
            if (dist > uOutlineWidth + 0.5)
            {
                continue;
            }

            vec2 sampleUv = uv + vec2(x, y) * uTexelSize;
            float maskSample = texture(uMask, sampleUv).r;
            dilated = max(dilated, maskSample);
            eroded = min(eroded, maskSample);
        }
    }

    float edge = dilated - eroded;
    return smoothstep(0.12, 0.88, edge);
}

void main()
{
    float alpha = ComputeEdgeAlpha(vTexCoord);
    FragColor = vec4(vec3(alpha), alpha);
}
