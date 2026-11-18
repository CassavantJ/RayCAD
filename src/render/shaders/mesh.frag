#version 440
#extension GL_GOOGLE_include_directive : require
// Two-sided Blinn-Phong in linear light, encoded to sRGB at the end.
layout(location = 0) in vec3 v_normal;
layout(location = 1) in vec3 v_pos;
layout(location = 2) in vec4 v_faceId;
layout(location = 0) out vec4 fragColor;

#include "frame.glsl"

void main()
{
    vec3 n = normalize(v_normal);
    vec3 v = frame.misc.y > 0.5 ? vec3(0.0, 0.0, 1.0) : normalize(-v_pos);
    if (dot(n, v) < 0.0)
        n = -n;
    vec3 l = frame.lightDir.xyz;
    float diffuse = max(dot(n, l), 0.0);
    float specular = pow(max(dot(n, normalize(l + v)), 0.0), 48.0);
    vec3 base = item.faceColor.rgb;
    vec3 c = base * (0.25 + 0.75 * diffuse) + vec3(0.18 * specular);
    fragColor = vec4(encodeSrgb(c), item.faceColor.a);
}
