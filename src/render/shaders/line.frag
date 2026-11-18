#version 440
#extension GL_GOOGLE_include_directive : require
layout(location = 0) in vec4 v_edgeId;
layout(location = 1) in float v_section;
layout(location = 0) out vec4 fragColor;

#include "frame.glsl"

void main()
{
    if (v_section > 0.0)
        discard; // beyond the section plane (T-023)
    fragColor = vec4(encodeSrgb(item.edgeColor.rgb), 1.0);
}
