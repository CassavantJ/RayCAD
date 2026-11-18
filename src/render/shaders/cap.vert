#version 440
#extension GL_GOOGLE_include_directive : require
// Section cap (task T-023): the cap quad, in world space, lying in the section plane (src/render/section.h). The
// stencil limits it to the inside of one body, whose ItemBlock gives the colours; its transform is not applied.
layout(location = 0) in vec3 position;
layout(location = 0) out vec3 v_pos;   // view space
layout(location = 1) out vec3 v_world; // for the hatch

#include "frame.glsl"

void main()
{
    v_pos = (frame.view * vec4(position, 1.0)).xyz;
    v_world = position;
    gl_Position = frame.viewProj * vec4(position, 1.0);
}
