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
    // 4 bilinear taps = 4x4 box filter for a 4:1 downsample
    vec4 c = texture(src, v_uv + ub.dir * vec2(-1.0, -1.0));
    c += texture(src, v_uv + ub.dir * vec2(1.0, -1.0));
    c += texture(src, v_uv + ub.dir * vec2(-1.0, 1.0));
    c += texture(src, v_uv + ub.dir * vec2(1.0, 1.0));
    fragColor = c * 0.25;
}
