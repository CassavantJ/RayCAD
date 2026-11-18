#version 440
#extension GL_GOOGLE_include_directive : require
layout(location = 0) in float v_across;
layout(location = 1) in float v_along;
layout(location = 2) in vec4 v_edgeId;
layout(location = 0) out vec4 fragColor;

#include "frame.glsl"

void main()
{
    float alpha = 1.0;
    if (item.line.z > 0.5)
        alpha = clamp(0.5 * item.line.x + 0.5 - abs(v_across), 0.0, 1.0);
    if (item.line.y > 0.5 && fract(max(v_along, 0.0) / frame.misc.w) > 0.6)
        discard;
    if (alpha <= 0.0)
        discard;
    fragColor = vec4(encodeSrgb(item.edgeColor.rgb), alpha);
}
