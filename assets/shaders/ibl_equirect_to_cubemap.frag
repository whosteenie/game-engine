#version 330 core
out vec4 FragColor;

in vec3 vLocalPos;

uniform sampler2D uEquirectangularMap;

const vec2 InvAtan = vec2(0.1591, 0.3183);

vec2 SampleSphericalMap(vec3 direction)
{
    vec2 uv = vec2(atan(direction.z, direction.x), asin(direction.y));
    uv *= InvAtan;
    uv += 0.5;
    return uv;
}

void main()
{
    vec2 uv = SampleSphericalMap(normalize(vLocalPos));
    vec3 color = texture(uEquirectangularMap, uv).rgb;
    FragColor = vec4(color, 1.0);
}
