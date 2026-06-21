#version 330 core
layout (location = 0) in vec3 aPos;

uniform float uTime;
uniform float uAspect;

void main()
{
    float yAngle = uTime * 1.5;
    float xAngle = uTime * 0.6;

    float cy = cos(yAngle);
    float sy = sin(yAngle);
    float cx = cos(xAngle);
    float sx = sin(xAngle);

    vec3 pos;
    pos.x = aPos.x * cy + aPos.z * sy;
    pos.y = aPos.y;
    pos.z = -aPos.x * sy + aPos.z * cy;

    vec3 rotated;
    rotated.x = pos.x;
    rotated.y = pos.y * cx - pos.z * sx;
    rotated.z = pos.y * sx + pos.z * cx;

    gl_Position = vec4(rotated.x * uAspect, rotated.y, rotated.z, 1.0);
}
