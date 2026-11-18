#version 440
#extension GL_GOOGLE_include_directive : require
// Picking ID pass, section caps (task T-023): the cap quad in world space (the body's transform is not applied),
// drawn where the stencil marks the inside of the body. It answers as its body only (IdKind 4), so a cap hides the
// faces behind it and is picked with PickFilter::Bodies.
layout(location = 0) in vec3 position;
layout(location = 0) out vec4 v_id;
layout(location = 1) out vec3 v_depthKind; // clip z, clip w, IdKind
layout(location = 2) out float v_section;

#include "frame.glsl"

void main()
{
    gl_Position = frame.viewProj * vec4(position, 1.0);
    v_id = vec4(1.0 / 255.0, 0.0, 0.0, 0.0); // index 0 (+ 1)
    v_depthKind = vec3(gl_Position.z, gl_Position.w, 4.0);
    v_section = -1.0; // the cap lies in the plane: never cut away
}
