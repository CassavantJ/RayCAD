#version 440
layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec4 rectInView;   // panel x, y, w, h as fractions of the viewport
    vec2 sizePx;
    float radius;
    float flipY;
    vec4 tint;
} ub;
layout(binding = 1) uniform sampler2D backdrop;
void main()
{
    vec2 uv = ub.rectInView.xy + qt_TexCoord0 * ub.rectInView.zw;
    if (ub.flipY > 0.5)
        uv.y = 1.0 - uv.y;
    vec3 c = texture(backdrop, uv).rgb;
    c = mix(c, ub.tint.rgb, ub.tint.a);
    vec2 p = (qt_TexCoord0 - 0.5) * ub.sizePx;
    vec2 q = abs(p) - ub.sizePx * 0.5 + ub.radius;
    float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - ub.radius;   // rounded-rect signed distance
    float alpha = clamp(0.5 - d, 0.0, 1.0);
    float border = clamp(1.0 - abs(d + 1.0), 0.0, 1.0);
    float top = 1.0 - smoothstep(0.0, 3.0, p.y + ub.sizePx.y * 0.5);
    c = mix(c, vec3(1.0), 0.18 * border + 0.22 * top * border);
    fragColor = vec4(c, 1.0) * alpha * ub.qt_Opacity;
}
