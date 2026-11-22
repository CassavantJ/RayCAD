// Toolpath backplot geometry (task T-038, src/render/lines.h), shared by path.vert, path.frag and pick_path.vert.
// Included after frame.glsl.

// The colour slot of a segment from its BackplotVertex role and kind (cam::MoveRole, cam::MoveKind); the CPU
// reference is detail::pathSlot. Rapid motion first, then the role.
float pathSlot(float role, float kind)
{
    if (kind < 0.5)
        return 1.0; // Rapid
    if (role < 0.5)
        return 0.0; // Cut
    if (role < 1.5)
        return 2.0; // Plunge
    if (role < 2.5)
        return 3.0; // Ramp
    if (role < 4.5)
        return 4.0; // LeadIn, LeadOut
    return 5.0;     // Link, Retract, Approach
}

// Anti-aliasing of a path line `widthPx` wide: coverage = clamp(R + 0.5 - distance from the centre line), with this R.
// For 2 px and wider R is half the width (the exact pixel coverage of the line). Thinner lines get R = 1: fully
// opaque within half a pixel of the centre line, so some pixel across the line is always fully covered. With plain
// coverage a 1.5 px line centred between two pixels gives each 0.75, and the line's contrast falls below the theme's
// 3:1 for thin lines (render.h, loadTheme); this way it is the token's own contrast.
float pathCoreRadius(float widthPx)
{
    return max(0.5 * widthPx - 0.5, 0.5) + 0.5;
}

vec4 pathClipNear(vec4 a, vec4 b)
{
    // Moves an end that is behind the eye along the segment to just in front of it.
    const float eps = 1e-5;
    if (a.w < eps && b.w > a.w)
        a = mix(a, b, (eps - a.w) / (b.w - a.w));
    return a;
}

// Corner `corner` (x: end 0 or 1, y: side -1 or +1) of the screen-space rectangle around segment p0-p1 (item space):
// `halfWidth` pixels to each side of it and `extend` pixels past each end. `coords`: pixels along the segment from
// its start, pixels across it, and its length in pixels. A zero-length segment (a dwell) and one wholly behind the
// eye get a degenerate rectangle outside the clip volume, so they draw nothing.
vec4 expandSegment(vec3 p0, vec3 p1, vec2 corner, float halfWidth, float extend, out vec3 coords, out float section)
{
    vec4 w0 = item.model * vec4(p0, 1.0);
    vec4 w1 = item.model * vec4(p1, 1.0);
    bool atEnd = corner.x > 0.5;
    section = sectionDistance(atEnd ? w1.xyz : w0.xyz);
    coords = vec3(0.0);
    vec4 c0 = frame.viewProj * w0;
    vec4 c1 = frame.viewProj * w1;
    if (all(equal(p0, p1)) || (c0.w < 1e-5 && c1.w < 1e-5))
        return vec4(2.0, 2.0, 2.0, 1.0);
    vec4 a = pathClipNear(c0, c1);
    vec4 b = pathClipNear(c1, c0);
    vec2 halfSize = 0.5 * frame.viewport.xy;
    vec2 s0 = a.xy / a.w * halfSize;
    vec2 s1 = b.xy / b.w * halfSize;
    vec2 d = s1 - s0;
    float len = length(d);
    vec2 dir = len > 1e-4 ? d / len : vec2(1.0, 0.0);
    vec2 nrm = vec2(-dir.y, dir.x);
    vec4 pos = atEnd ? b : a;
    vec2 offset = nrm * (corner.y * halfWidth) + dir * ((atEnd ? 1.0 : -1.0) * extend);
    pos.xy += offset / halfSize * pos.w;
    coords = vec3(atEnd ? len + extend : -extend, corner.y * halfWidth, len);
    return pos;
}
