#version 330 core
out float FragOcclusion;

in vec2 vTexCoord;

uniform sampler2D uInput;
uniform float uTexelSizeX;
uniform float uTexelSizeY;

void main()
{
    vec2 texelSize = vec2(uTexelSizeX, uTexelSizeY);
    float result = 0.0;

    for (int x = -1; x <= 1; ++x)
    {
        for (int y = -1; y <= 1; ++y)
        {
            vec2 offset = vec2(float(x), float(y)) * texelSize;
            result += texture(uInput, vTexCoord + offset).r;
        }
    }

    FragOcclusion = result / 9.0;
}
