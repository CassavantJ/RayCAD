#version 440
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform buf {
    vec2 dir;
    float ySign;
    float pad;
} ub;
layout(binding = 1) uniform sampler2D src;
void main()
{
    // 17-tap Gaussian, sigma 3.5 texels of the quarter-resolution image (14 px of the full image)
    vec4 c = vec4(0.0);
    float wsum = 0.0;
    for (int i = -8; i <= 8; ++i) {
        float w = exp(-float(i * i) / 24.5);
        c += texture(src, v_uv + ub.dir * float(i)) * w;
        wsum += w;
    }
    fragColor = c / wsum;
}
