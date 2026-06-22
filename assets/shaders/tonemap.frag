#version 330 core
out vec4 FragColor;

in vec2 vTexCoord;

uniform sampler2D uHdrColor;
uniform float uExposure;
uniform int uTonemapMode;

vec3 LinearToSrgb(vec3 linear)
{
    return pow(max(linear, vec3(0.0)), vec3(1.0 / 2.2));
}

vec3 ACESFilm(vec3 color)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((color * (a * color + b)) / (color * (c * color + d) + e), 0.0, 1.0);
}

vec3 Reinhard(vec3 color)
{
    const float whitePoint = 4.0;
    vec3 numerator = color * (1.0 + color / (whitePoint * whitePoint));
    return numerator / (1.0 + color);
}

void main()
{
    vec3 hdr = texture(uHdrColor, vTexCoord).rgb * exp2(uExposure);
    vec3 mapped;

    if (uTonemapMode == 0)
    {
        mapped = LinearToSrgb(hdr);
    }
    else if (uTonemapMode == 1)
    {
        mapped = Reinhard(hdr);
    }
    else
    {
        mapped = ACESFilm(hdr);
    }

    FragColor = vec4(clamp(mapped, 0.0, 1.0), 1.0);
}
