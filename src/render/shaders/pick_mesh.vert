#version 440
#extension GL_GOOGLE_include_directive : require
// Picking ID pass (task T-015), faces: same inputs as mesh.vert. faceId is the kernel face index + 1 (RGBA8).
layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec4 faceId;
layout(location = 0) out vec4 v_id;
layout(location = 1) out vec3 v_depthKind; // clip z, clip w, IdKind

#include "frame.glsl"

void main()
{
    gl_Position = frame.viewProj * (item.model * vec4(position, 1.0));
    v_id = faceId;
    v_depthKind = vec3(gl_Position.z, gl_Position.w, 1.0);
}
