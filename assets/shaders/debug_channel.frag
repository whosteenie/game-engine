#version 330 core
out vec4 FragColor;

in vec2 vTexCoord;

uniform sampler2D uInput;

void main()
{
    float value = texture(uInput, vTexCoord).r;
    FragColor = vec4(vec3(value), 1.0);
}
