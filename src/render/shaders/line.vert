#version 440
#extension GL_GOOGLE_include_directive : require
// Edges as 1 px lines (presets without smooth edges). edgeId carries the kernel edge index + 1 for T-015.
layout(location = 0) in vec3 position;
layout(location = 1) in vec4 edgeId;
layout(location = 0) out vec4 v_edgeId;

#include "frame.glsl"

void main()
{
    v_edgeId = edgeId;
    gl_Position = frame.viewProj * (item.model * vec4(position, 1.0));
}
