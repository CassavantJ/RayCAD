#version 440
#extension GL_GOOGLE_include_directive : require
// Toolpath backplot (task T-038): a capsule around each segment, so consecutive segments overlap in round ends and
// leave no gap at any angle; rapids dashed in screen space from each segment's start (frame.misc.w is the period).
layout(location = 0) in vec4 v_color;
layout(location = 1) in vec4 v_line;
layout(location = 2) in float v_dashed;
layout(location = 3) in float v_section;
layout(location = 0) out vec4 fragColor;

#include "frame.glsl"
#include "path.glsl"

void main()
{
    if (v_section > 0.0)
        discard; // beyond the section plane (T-023)
    vec3 l = v_line.xyz / v_line.w;
    float along = l.x;
    if (v_dashed > 0.5 && fract(max(along, 0.0) / frame.misc.w) > 0.6)
        discard;
    float beyond = along < 0.0 ? -along : max(along - l.z, 0.0);
    float alpha = clamp(pathCoreRadius(item.line.x) + 0.5 - length(vec2(beyond, l.y)), 0.0, 1.0);
    if (alpha <= 0.0)
        discard;
    fragColor = vec4(encodeSrgb(v_color.rgb), alpha * item.faceColor.a);
}
