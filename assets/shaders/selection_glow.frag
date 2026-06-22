#version 330 core

out vec4 FragColor;

in vec2 vTexCoord;

uniform sampler2D uGlow;
uniform sampler2D uEdge;
uniform vec3 uColor;
uniform float uGlowIntensity;

void main()
{
    float blurred = texture(uGlow, vTexCoord).r;
    float sharp = texture(uEdge, vTexCoord).r;

    // Keep glow outside the crisp rim and drop the long blur tail quickly.
    float halo = max(blurred - sharp * 0.92, 0.0);
    float strength = pow(halo, 2.2) * uGlowIntensity;
    if (strength < 0.004)
    {
        discard;
    }

    FragColor = vec4(uColor * strength, 1.0);
}
