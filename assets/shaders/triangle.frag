#version 330 core
out vec4 FragColor;

uniform float uTime;

void main()
{
    FragColor = vec4(
        0.5 + 0.5 * sin(uTime),
        0.5 + 0.5 * sin(uTime + 2.094),
        0.5 + 0.5 * sin(uTime + 4.189),
        1.0
    );
}
