#version 440
#extension GL_GOOGLE_include_directive : require
// Full-screen triangle for the theme's vertical background gradient.
layout(location = 0) in vec2 position;
layout(location = 0) out float v_top;

#include "frame.glsl"

void main()
{
    v_top = 0.5 + 0.5 * position.y * frame.misc.x;
    gl_Position = vec4(position, 0.0, 1.0);
}
