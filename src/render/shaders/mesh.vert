#version 440
#extension GL_GOOGLE_include_directive : require
// Shaded faces. faceId carries the kernel face index + 1 (RGBA8, little-endian) for T-015's ID pass.
// T-073: a stock's surface (item.stock.w) is pushed back in depth here instead of by the pipeline's depth bias
// (src/render/stock.h, "DEPTH"), the same amount for every face through a point; the walls inside the stock (faceId's
// high byte set, stock.h PackedPiece) half a 45-degree pixel slope further.
layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec4 faceId;
layout(location = 0) out vec3 v_normal;
layout(location = 1) out vec3 v_pos;
layout(location = 2) out vec4 v_faceId;
layout(location = 3) out float v_section; // T-023: signed distance to the section plane
// T-073, stock colours: the item's own position, interpolated inside the triangle (centroid: with MSAA a pixel on a
// triangle's edge is shaded at its centre, which may lie outside the triangle and outside the stock), and its own
// normal.
layout(location = 4) centroid out vec3 v_local;
layout(location = 5) out vec3 v_localNormal;

#include "frame.glsl"

// The depth slope, in NDC z per pixel, at the clip-space point c of the plane through it with unit world normal m,
// at most `cap` (an edge-on plane has no finite slope).
float planeSlope(vec4 c, vec3 m, float cap)
{
    vec3 a = abs(m.x) < 0.9 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
    vec3 u = normalize(cross(m, a));
    vec3 v = cross(m, u);
    vec4 du = frame.viewProj * vec4(u, 0.0);
    vec4 dv = frame.viewProj * vec4(v, 0.0);
    float w2 = c.w * c.w;
    vec3 nu = (du.xyz * c.w - c.xyz * du.w) / w2; // NDC per mm along u and v
    vec3 nv = (dv.xyz * c.w - c.xyz * dv.w) / w2;
    vec2 su = nu.xy * 0.5 * frame.viewport.xy;    // pixels per mm
    vec2 sv = nv.xy * 0.5 * frame.viewport.xy;
    float det = abs(su.x * sv.y - su.y * sv.x);
    float g = max(abs(nu.z * sv.y - nv.z * su.y), abs(nv.z * su.x - nu.z * sv.x));
    return g < cap * det ? g / det : cap;
}

// How far back (NDC z) a stock's surface is drawn at world point p, clip position c; `inside`: a vertex of a wall
// inside the stock.
float stockPush(vec3 p, vec4 c, bool inside)
{
    vec3 inView = (frame.view * vec4(p, 1.0)).xyz;
    vec3 rayView = frame.misc.y > 0.5 ? vec3(0.0, 0.0, -1.0) : normalize(inView);
    vec3 ray = transpose(mat3(frame.view)) * rayView; // eye to p, world
    vec3 side = normalize(cross(ray, abs(ray.z) < 0.9 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0)));
    float ref = planeSlope(c, normalize(ray + side), 1e20); // a plane at 45 degrees to the view
    float cap = 4.0 * ref;
    mat3 axes = mat3(item.model);
    float steepest = max(planeSlope(c, normalize(axes[0]), cap),
                         max(planeSlope(c, normalize(axes[1]), cap), planeSlope(c, normalize(axes[2]), cap)));
    vec4 dc = frame.viewProj * vec4(ray, 0.0);
    float alongRay = (dc.z * c.w - c.z * dc.w) / (c.w * c.w); // NDC z per mm along the ray
    return min(1.5 * steepest + 2.0 / 16777216.0 + (inside ? 0.5 * ref : 0.0), abs(alongRay) * item.uncutMax.w);
}

void main()
{
    vec4 world = item.model * vec4(position, 1.0);
    v_normal = mat3(frame.view) * (mat3(item.normalMatrix) * normal);
    v_pos = (frame.view * world).xyz;
    v_faceId = faceId;
    v_section = sectionDistance(world.xyz);
    v_local = position;
    v_localNormal = normal;
    gl_Position = frame.viewProj * world;
    if (item.stock.w > 0.5)
        gl_Position.z += stockPush(world.xyz, gl_Position, faceId.a > 0.5 / 255.0) * gl_Position.w;
}
