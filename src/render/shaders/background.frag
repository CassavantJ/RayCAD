#version 440
#extension GL_GOOGLE_include_directive : require
layout(location = 0) in float v_top;
layout(location = 0) out vec4 fragColor;

#include "frame.glsl"

void main()
{
    vec3 c = mix(frame.bgBottom.rgb, frame.bgTop.rgb, clamp(v_top, 0.0, 1.0));
    fragColor = vec4(encodeSrgb(c), 1.0);
}
