#version 330 core

in vec3 vWorldPos;

out vec4 FragColor;

uniform vec3 uColor;
uniform vec3 uCameraPosition;
uniform float uCellSize;
uniform float uMajorInterval;

vec3 LinearToSrgb(vec3 linear)
{
    return pow(max(linear, vec3(0.0)), vec3(1.0 / 2.2));
}

float LineAlpha(vec2 uv)
{
    vec2 grid = abs(fract(uv - 0.5) - 0.5);
    vec2 fw = max(fwidth(uv), vec2(0.0001));
    vec2 lines = grid / fw;
    return 1.0 - clamp(min(lines.x, lines.y), 0.0, 1.0);
}

void main()
{
    vec2 xz = vWorldPos.xz;
    vec2 minorUv = xz / uCellSize;
    vec2 majorUv = xz / (uCellSize * uMajorInterval);

    float minor = LineAlpha(minorUv);
    float major = LineAlpha(majorUv);

    float axisMask = max(
        1.0 - clamp(abs(xz.x) / max(fwidth(xz.x) * 1.5, 0.0001), 0.0, 1.0),
        1.0 - clamp(abs(xz.y) / max(fwidth(xz.y) * 1.5, 0.0001), 0.0, 1.0));

    float lineStrength = max(max(minor * 0.45, major * 0.75), axisMask * minor * 0.9);

    vec3 viewDir = normalize(uCameraPosition - vWorldPos);
    float grazeFade = smoothstep(0.04, 0.22, abs(viewDir.y));

    float alpha = lineStrength * grazeFade;
    if (alpha < 0.02)
    {
        discard;
    }

    vec3 linearColor = pow(uColor, vec3(2.2));
    FragColor = vec4(LinearToSrgb(linearColor), alpha);
}
