#version 440
#extension GL_GOOGLE_include_directive : require
// Two-sided Blinn-Phong in linear light, encoded to sRGB at the end.
// T-023: fragments beyond the section plane are discarded, the cut line is outlined in the edge colour, and draft
// analysis replaces the face colour by the class of the angle between the outward normal and the pull direction.
layout(location = 0) in vec3 v_normal;
layout(location = 1) in vec3 v_pos;
layout(location = 2) in vec4 v_faceId;
layout(location = 3) in float v_section;
layout(location = 0) out vec4 fragColor;

#include "frame.glsl"

void main()
{
    // Derivatives before the discard, while every fragment of the quad still runs.
    float sectionStep = fwidth(v_section);
    if (v_section > 0.0)
        discard;
    vec3 outward = normalize(v_normal);
    vec3 n = outward;
    vec3 v = frame.misc.y > 0.5 ? vec3(0.0, 0.0, 1.0) : normalize(-v_pos);
    if (dot(n, v) < 0.0)
        n = -n;
    vec3 l = frame.lightDir.xyz;
    float diffuse = max(dot(n, l), 0.0);
    float specular = pow(max(dot(n, normalize(l + v)), 0.0), 48.0);
    vec3 c;
    if (frame.analysis.x > 0.5 && item.ids.y > 0.5) {
        // Draft analysis: flatter light and no highlight, so the class colours stay recognisable.
        float s = dot(outward, frame.draftPull.xyz);
        vec3 base = s > frame.analysis.y ? frame.draftPositive.rgb
                  : (s < -frame.analysis.y ? frame.draftNegative.rgb : frame.draftVertical.rgb);
        c = base * (0.55 + 0.45 * diffuse);
    } else {
        vec3 base = item.faceColor.rgb;
        c = base * (0.25 + 0.75 * diffuse) + vec3(0.18 * specular);
    }
    // The cut line: the last `analysis.z` pixels of the face before the plane.
    if (frame.misc.z > 0.5 && frame.analysis.z > 0.0 && -v_section < sectionStep * frame.analysis.z)
        c = item.edgeColor.rgb;
    fragColor = vec4(encodeSrgb(c), item.faceColor.a);
}
