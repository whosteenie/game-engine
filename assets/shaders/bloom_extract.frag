#version 330 core
out vec3 FragColor;

in vec2 vTexCoord;

uniform sampler2D uHdrColor;
uniform float uThreshold;
uniform float uSoftKnee;

vec3 ExtractBrightColor(vec3 color, float threshold, float knee)
{
    float brightness = max(color.r, max(color.g, color.b));
    float kneeRange = threshold * knee;
    float soft = brightness - threshold + kneeRange;
    soft = clamp(soft, 0.0, 2.0 * kneeRange);
    soft = (soft * soft) / (4.0 * kneeRange + 0.00001);
    float contribution = max(soft, brightness - threshold);
    contribution /= max(brightness, 0.00001);
    return color * contribution;
}

void main()
{
    vec3 hdr = texture(uHdrColor, vTexCoord).rgb;
    FragColor = ExtractBrightColor(hdr, uThreshold, uSoftKnee);
}
