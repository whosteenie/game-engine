#version 330 core
out vec4 FragColor;

uniform vec3 uColor;

vec3 LinearToSrgb(vec3 linear)
{
    return pow(max(linear, vec3(0.0)), vec3(1.0 / 2.2));
}

void main()
{
    vec3 linearColor = pow(uColor, vec3(2.2));
    FragColor = vec4(LinearToSrgb(linearColor), 1.0);
}
