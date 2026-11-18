#version 440
#extension GL_GOOGLE_include_directive : require
// Section cap (task T-023): a darker shade of the body's face colour (a quarter of the way to its edge colour), lit
// like a face whose normal is the plane's, with diagonal hatch lines in the edge colour as in a drawing's section, so
// the cut reads as solid material. The hatch lies in the plane (straight in world space); its period
// (FrameBlock.analysis.w, in mm) is about ten pixels, snapped to a power of two so it only steps while zooming.
layout(location = 0) in vec3 v_pos;
layout(location = 1) in vec3 v_world;
layout(location = 0) out vec4 fragColor;

#include "frame.glsl"

void main()
{
    vec3 plane = frame.section.xyz;
    vec3 n = normalize(mat3(frame.view) * plane);
    vec3 v = frame.misc.y > 0.5 ? vec3(0.0, 0.0, 1.0) : normalize(-v_pos);
    if (dot(n, v) < 0.0)
        n = -n;
    float diffuse = max(dot(n, frame.lightDir.xyz), 0.0);
    vec3 c = mix(item.faceColor.rgb, item.edgeColor.rgb, 0.25) * (0.35 + 0.65 * diffuse);

    // In-plane diagonal: from the world axis least aligned with the normal, so the pattern is stable.
    vec3 axis = abs(plane.x) < 0.57 ? vec3(1.0, 0.0, 0.0) : (abs(plane.y) < 0.57 ? vec3(0.0, 1.0, 0.0)
                                                                                   : vec3(0.0, 0.0, 1.0));
    vec3 u = normalize(cross(plane, axis));
    vec3 w = cross(plane, u);
    float t = dot(v_world, normalize(u + w));
    float period = frame.analysis.w;
    float px = fwidth(t); // mm per pixel across the lines
    if (period > 0.0 && px > 0.0) {
        float dist = abs(fract(t / period + 0.5) - 0.5) * period; // mm to the nearest line
        float cover = 1.0 - smoothstep(0.25 * px, 1.0 * px, dist);
        c = mix(c, item.edgeColor.rgb, 0.55 * cover);
    }
    fragColor = vec4(encodeSrgb(c), 1.0);
}
