#version 330 core
out vec3 FragColor;

in vec2 vTexCoord;

uniform sampler2D uInput;
uniform float uDirectionX;
uniform float uDirectionY;
uniform float uBlurRadius;

void main()
{
    vec2 direction = vec2(uDirectionX, uDirectionY) * uBlurRadius;
    vec3 result = texture(uInput, vTexCoord).rgb * 0.227027;
    result += texture(uInput, vTexCoord + direction * 1.0).rgb * 0.1945946;
    result += texture(uInput, vTexCoord - direction * 1.0).rgb * 0.1945946;
    result += texture(uInput, vTexCoord + direction * 2.0).rgb * 0.1216216;
    result += texture(uInput, vTexCoord - direction * 2.0).rgb * 0.1216216;
    result += texture(uInput, vTexCoord + direction * 3.0).rgb * 0.054054;
    result += texture(uInput, vTexCoord - direction * 3.0).rgb * 0.054054;
    result += texture(uInput, vTexCoord + direction * 4.0).rgb * 0.016216;
    result += texture(uInput, vTexCoord - direction * 4.0).rgb * 0.016216;
    FragColor = result;
}
