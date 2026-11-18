#version 440
#extension GL_GOOGLE_include_directive : require
// Picking ID pass (task T-015), mesh edges as 1 px lines: same inputs as line.vert. edgeId is the kernel edge
// index + 1 (RGBA8).
layout(location = 0) in vec3 position;
layout(location = 1) in vec4 edgeId;
layout(location = 0) out vec4 v_id;
layout(location = 1) out vec3 v_depthKind; // clip z, clip w, IdKind
layout(location = 2) out float v_section;  // T-023: signed distance to the section plane

#include "frame.glsl"

void main()
{
    vec4 world = item.model * vec4(position, 1.0);
    gl_Position = frame.viewProj * world;
    v_id = edgeId;
    v_depthKind = vec3(gl_Position.z, gl_Position.w, 2.0);
    v_section = sectionDistance(world.xyz);
}
