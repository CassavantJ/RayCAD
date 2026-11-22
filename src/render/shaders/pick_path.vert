#version 440
#extension GL_GOOGLE_include_directive : require
// Picking ID pass (task T-038), backplot segments: same instances and geometry as path.vert, item.line.w pixels wide,
// square ends, no anti-aliasing and no dashes. moveId is the segment's BackplotVertex moveIndex as four bytes: the
// low three go to the ids attachment, the high one to the item attachment's alpha (pick.frag, IdKind 5), so a move
// index of any size survives.
layout(location = 0) in vec2 corner;
layout(location = 1) in vec3 p0;
layout(location = 2) in vec3 p1;
layout(location = 4) in vec4 moveId;
layout(location = 0) out vec4 v_id;
layout(location = 1) out vec3 v_depthKind; // clip z, clip w, IdKind
layout(location = 2) out float v_section;  // T-023: signed distance to the section plane

#include "frame.glsl"
#include "path.glsl"

void main()
{
    float halfWidth = max(0.5 * item.line.w, 0.5);
    vec3 coords;
    vec4 pos = expandSegment(p0, p1, corner, halfWidth, halfWidth, coords, v_section);
    v_id = moveId;
    v_depthKind = vec3(pos.z, pos.w, 5.0);
    gl_Position = pos;
}
