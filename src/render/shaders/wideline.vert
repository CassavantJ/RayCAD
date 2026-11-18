#version 440
#extension GL_GOOGLE_include_directive : require
// Lines of any width as screen-space quads, one instance per segment: smooth (anti-aliased) edges, and addLines
// items with a width or dashes. `corner` is the quad template: x selects the end (0 or 1), y the side (-1 or +1).
layout(location = 0) in vec2 corner;
layout(location = 1) in vec3 p0;
layout(location = 2) in vec3 p1;
layout(location = 3) in vec4 edgeId;
layout(location = 0) out float v_across;   // pixels from the centre line
layout(location = 1) out float v_along;    // pixels from the segment start
layout(location = 2) out vec4 v_edgeId;

#include "frame.glsl"

vec4 clipNear(vec4 a, vec4 b)
{
    // Moves an end that is behind the eye along the segment to just in front of it.
    const float eps = 1e-5;
    if (a.w < eps && b.w > a.w)
        a = mix(a, b, (eps - a.w) / (b.w - a.w));
    return a;
}

void main()
{
    vec4 c0 = frame.viewProj * (item.model * vec4(p0, 1.0));
    vec4 c1 = frame.viewProj * (item.model * vec4(p1, 1.0));
    vec4 a = clipNear(c0, c1);
    vec4 b = clipNear(c1, c0);
    vec2 halfSize = 0.5 * frame.viewport.xy;
    vec2 s0 = a.xy / a.w * halfSize;
    vec2 s1 = b.xy / b.w * halfSize;
    vec2 d = s1 - s0;
    float len = length(d);
    vec2 dir = len > 1e-4 ? d / len : vec2(1.0, 0.0);
    vec2 nrm = vec2(-dir.y, dir.x);
    bool smoothEdge = item.line.z > 0.5;
    float halfWidth = 0.5 * item.line.x + (smoothEdge ? 1.0 : 0.0);
    float extend = smoothEdge ? 0.5 * item.line.x : 0.0;

    vec4 pos = corner.x < 0.5 ? a : b;
    vec2 offset = nrm * (corner.y * halfWidth) + dir * ((corner.x < 0.5 ? -1.0 : 1.0) * extend);
    pos.xy += offset / halfSize * pos.w;

    v_across = corner.y * halfWidth;
    v_along = corner.x < 0.5 ? -extend : len + extend;
    v_edgeId = edgeId;
    gl_Position = pos;
}
