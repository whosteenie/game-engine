#version 330 core

out vec4 FragColor;

in vec2 vTexCoord;

uniform sampler2D uMask;
uniform vec2 uTexelSize;
uniform float uOutlineWidth;
uniform vec3 uColor;

void main()
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

            vec2 uv = vTexCoord + vec2(x, y) * uTexelSize;
            float maskSample = texture(uMask, uv).r;
            dilated = max(dilated, maskSample);
            eroded = min(eroded, maskSample);
        }
    }

    float edge = dilated - eroded;
    float alpha = smoothstep(0.12, 0.88, edge);
    if (alpha < 0.004)
    {
        discard;
    }

    FragColor = vec4(uColor, alpha);
}
