#version 330 core

out vec4 FragColor;

in vec2 vTexCoord;

uniform sampler2D uEdge;
uniform vec3 uColor;

void main()
{
    float sharp = texture(uEdge, vTexCoord).r;
    if (sharp < 0.004)
    {
        discard;
    }

    FragColor = vec4(uColor, sharp);
}
