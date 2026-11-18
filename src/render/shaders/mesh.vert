#version 440
#extension GL_GOOGLE_include_directive : require
// Shaded faces. faceId carries the kernel face index + 1 (RGBA8, little-endian) for T-015's ID pass.
layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec4 faceId;
layout(location = 0) out vec3 v_normal;
layout(location = 1) out vec3 v_pos;
layout(location = 2) out vec4 v_faceId;
layout(location = 3) out float v_section; // T-023: signed distance to the section plane

#include "frame.glsl"

void main()
{
    vec4 world = item.model * vec4(position, 1.0);
    v_normal = mat3(frame.view) * (mat3(item.normalMatrix) * normal);
    v_pos = (frame.view * world).xyz;
    v_faceId = faceId;
    v_section = sectionDistance(world.xyz);
    gl_Position = frame.viewProj * world;
}
