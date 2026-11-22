#version 440
#extension GL_GOOGLE_include_directive : require
// Toolpath backplot (task T-038, src/render/lines.h): one instance per segment, drawn as a 4-vertex triangle strip.
// The segment's start comes from vertex s of the backplot buffer, its end and its attributes from vertex s + 1 (a
// second binding of the same buffer, one vertex further on), so no attribute depends on a provoking vertex.
layout(location = 0) in vec2 corner; // x: end (0 or 1), y: side (-1 or +1)
layout(location = 1) in vec3 p0;
layout(location = 2) in vec3 p1;
layout(location = 3) in vec4 attrs;  // BackplotVertex role, kind and pad as unsigned bytes
layout(location = 0) out vec4 v_color;
layout(location = 1) out vec4 v_line; // xyz: pixels along, pixels across, length in pixels, all times clip w; w: clip w
layout(location = 2) out float v_dashed;
layout(location = 3) out float v_section; // T-023: signed distance to the section plane

#include "frame.glsl"
#include "path.glsl"

void main()
{
    float role = floor(attrs.x * 255.0 + 0.5);
    float kind = floor(attrs.y * 255.0 + 0.5);
    v_color = item.line.y > 0.5 ? item.edgeColor : frame.pathColor[int(pathSlot(role, kind))];
    v_dashed = kind < 0.5 ? 1.0 : 0.0;
    float reach = pathCoreRadius(item.line.x) + 1.0;
    vec3 coords;
    vec4 pos = expandSegment(p0, p1, corner, reach, reach, coords, v_section);
    // The screen-space coordinates are linear in screen space; multiplied by w here and divided by the interpolated
    // w in path.frag, perspective-correct interpolation gives them back unchanged (GLSL 120 has no noperspective).
    v_line = vec4(coords * pos.w, pos.w);
    gl_Position = pos;
}
