// Shared declarations of the viewport shaders (task T-014). Included by every stage; qsb resolves the #include at
// build time. The two blocks are std140 and must match FrameBlock and ItemBlock in src/render/rhi/scene_renderer.cpp.

// Per pass: camera, viewport and theme background.
layout(std140, binding = 0) uniform FrameBlock {
    mat4 viewProj;    // clip-space correction * projection * view
    mat4 view;
    vec4 lightDir;    // view space, unit length
    vec4 viewport;    // width, height, 1 / width, 1 / height in pixels
    vec4 bgTop;       // linear RGB of ViewportBgTop
    vec4 bgBottom;    // linear RGB of ViewportBgBottom
    vec4 misc;        // x: +1 when NDC y points up, -1 otherwise; y: 1 for orthographic; z: section on (T-023);
                      // w: dash period in pixels
    vec4 section;     // section plane (nx, ny, nz, d) in world space (T-023): dot(n, p) + d > 0 is cut away
    vec4 analysis;    // T-023. x: draft analysis on; y: sin(draft threshold); z: width of the cut outline in
                      // pixels (0: none); w: period of the section caps' hatch in mm (0: none)
    vec4 draftPull;   // T-023. xyz: unit pull direction in view space
    vec4 draftPositive; // T-023: linear RGB of the three draft classes
    vec4 draftNegative;
    vec4 draftVertical;
} frame;

// Per item: transform, colours from the theme tokens of its style, line parameters, and its id for T-015.
layout(std140, binding = 1) uniform ItemBlock {
    mat4 model;
    mat4 normalMatrix;
    vec4 faceColor;   // linear RGB, a = opacity
    vec4 edgeColor;   // linear RGB, a = 1
    vec4 line;        // x: width in pixels, y: 1 if dashed, z: 1 for anti-aliased (smooth) edges
    vec4 ids;         // x: item id (T-015); y: 1 when draft analysis may paint these faces (T-023); zw reserved
} item;

// Signed distance of a world point to the section plane (T-023), positive on the side that is cut away; -1 when
// there is no section. Linear in the position, so it interpolates exactly across a primitive.
float sectionDistance(vec3 world)
{
    return frame.misc.z > 0.5 ? dot(frame.section.xyz, world) + frame.section.w : -1.0;
}

vec3 encodeSrgb(vec3 linearRgb)
{
    vec3 c = clamp(linearRgb, 0.0, 1.0);
    vec3 lo = c * 12.92;
    vec3 hi = 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055;
    return mix(lo, hi, step(vec3(0.0031308), c));
}
