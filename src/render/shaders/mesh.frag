#version 440
#extension GL_GOOGLE_include_directive : require
// Two-sided Blinn-Phong in linear light, encoded to sRGB at the end.
// T-023: fragments beyond the section plane are discarded, the cut line is outlined in the edge colour, and draft
// analysis replaces the face colour by the class of the angle between the outward normal and the pull direction.
// T-073: a simulation stock (item.stock.x) takes the face colour where its surface lies on the uncut box, the edge
// colour (StockCutFace) everywhere else, and a simulation colour inside a mark; it has no cut outline.
layout(location = 0) in vec3 v_normal;
layout(location = 1) in vec3 v_pos;
layout(location = 2) in vec4 v_faceId;
layout(location = 3) in float v_section;
// T-073: the item's own position inside the triangle (centroid; see mesh.vert) and its own normal.
layout(location = 4) centroid in vec3 v_local;
layout(location = 5) in vec3 v_localNormal;
layout(location = 0) out vec4 fragColor;

#include "frame.glsl"

// Whether p lies in the box [lo, hi] grown by e.
bool inBox(vec3 p, vec3 lo, vec3 hi, float e)
{
    return all(greaterThanEqual(p, lo - vec3(e))) && all(lessThanEqual(p, hi + vec3(e)));
}

// The stock's colour at p, where its own normal is m.
vec3 stockColour(vec3 p, vec3 m)
{
    float e = item.stock.z;
    vec3 c = item.faceColor.rgb; // no uncut box: every face in the face colour
    if (item.uncutMin.w > 0.5) {
        c = item.edgeColor.rgb; // cut, unless it lies on the uncut box
        vec3 lo = item.uncutMin.xyz;
        vec3 hi = item.uncutMax.xyz;
        vec3 a = abs(m);
        // On a face of the uncut box: within the tolerance of its plane AND facing along that plane's axis. Every face
        // of the column mesh lies in an axis plane: a wall's normal is its axis exactly (no z part), a top's or a
        // bottom's has a z part (a top's is tilted for shading by up to about 63 degrees). So a wall that meets the
        // box's top keeps the cut colour up to its edge.
        bool wall = a.z < 0.01;
        bool onX = wall && a.x >= a.y && min(abs(p.x - lo.x), abs(p.x - hi.x)) < e;
        bool onY = wall && a.y > a.x && min(abs(p.y - lo.y), abs(p.y - hi.y)) < e;
        bool onZ = !wall && min(abs(p.z - lo.z), abs(p.z - hi.z)) < e;
        if ((onX || onY || onZ) && inBox(p, lo, hi, e))
            c = item.faceColor.rgb;
    }
    int marks = int(item.stock.y + 0.5);
    for (int i = 0; i < 8; ++i) {
        if (i < marks && inBox(p, item.markMin[i].xyz, item.markMax[i].xyz, e))
            c = frame.simColor[int(item.markMin[i].w + 0.5)].rgb;
    }
    return c;
}

void main()
{
    // Derivatives before the discard, while every fragment of the quad still runs.
    float sectionStep = fwidth(v_section);
    bool stock = item.stock.x > 0.5;
    // A stock also loses what lies within its tolerance of the plane: the walls that close its tiles inside the
    // material, should the plane fall exactly on one, would otherwise be kept or cut by the rounding of each pixel.
    if (v_section > (item.stock.w > 0.5 ? -item.stock.z : 0.0))
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
        vec3 base = stock ? stockColour(v_local, v_localNormal) : item.faceColor.rgb;
        c = base * (0.25 + 0.75 * diffuse) + vec3(0.18 * specular);
    }
    // The cut line: the last `analysis.z` pixels of the face before the plane (not on a stock, whose tiles are closed
    // by walls inside the material that the section's cap covers).
    if (item.stock.w < 0.5 && frame.misc.z > 0.5 && frame.analysis.z > 0.0 && -v_section < sectionStep * frame.analysis.z)
        c = item.edgeColor.rgb;
    // Test hook (simColor[3].w, scene_renderer.cpp): the walls inside the stock in magenta, to prove they never show.
    if (frame.simColor[3].w > 0.5 && stock && v_faceId.a > 0.5 / 255.0)
        c = vec3(1.0, 0.0, 1.0);
    fragColor = vec4(encodeSrgb(c), item.faceColor.a);
}
