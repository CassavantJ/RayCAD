#version 440
#extension GL_GOOGLE_include_directive : require
layout(location = 0) in vec4 v_edgeId;
layout(location = 0) out vec4 fragColor;

#include "frame.glsl"

void main()
{
    fragColor = vec4(encodeSrgb(item.edgeColor.rgb), 1.0);
}
