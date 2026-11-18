#version 440
#extension GL_GOOGLE_include_directive : require
// Picking ID pass (task T-015), segments of addLines items as screen-space quads: same inputs and geometry as
// wideline.vert, without the anti-aliasing fringe and without dashes. edgeId is the segment index + 1 (RGBA8).
layout(location = 0) in vec2 corner;
layout(location = 1) in vec3 p0;
layout(location = 2) in vec3 p1;
layout(location = 3) in vec4 edgeId;
layout(location = 0) out vec4 v_id;
layout(location = 1) out vec3 v_depthKind; // clip z, clip w, IdKind
layout(location = 2) out float v_section;  // T-023: signed distance to the section plane

#include "frame.glsl"

vec4 clipNear(vec4 a, vec4 b)
{
    const float eps = 1e-5;
    if (a.w < eps && b.w > a.w)
        a = mix(a, b, (eps - a.w) / (b.w - a.w));
    return a;
}

void main()
{
    vec4 w0 = item.model * vec4(p0, 1.0);
    vec4 w1 = item.model * vec4(p1, 1.0);
    vec4 c0 = frame.viewProj * w0;
    vec4 c1 = frame.viewProj * w1;
    vec4 a = clipNear(c0, c1);
    vec4 b = clipNear(c1, c0);
    vec2 halfSize = 0.5 * frame.viewport.xy;
    vec2 s0 = a.xy / a.w * halfSize;
    vec2 s1 = b.xy / b.w * halfSize;
    vec2 d = s1 - s0;
    float len = length(d);
    vec2 dir = len > 1e-4 ? d / len : vec2(1.0, 0.0);
    vec2 nrm = vec2(-dir.y, dir.x);
    float halfWidth = max(0.5 * item.line.x, 0.5);

    vec4 pos = corner.x < 0.5 ? a : b;
    pos.xy += nrm * (corner.y * halfWidth) / halfSize * pos.w;
    v_id = edgeId;
    v_depthKind = vec3(pos.z, pos.w, 3.0);
    v_section = sectionDistance(corner.x < 0.5 ? w0.xyz : w1.xyz);
    gl_Position = pos;
}
