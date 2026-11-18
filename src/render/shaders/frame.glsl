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
    vec4 section;     // section plane (nx, ny, nz, d) in world space, for T-023
} frame;

// Per item: transform, colours from the theme tokens of its style, line parameters, and its id for T-015.
layout(std140, binding = 1) uniform ItemBlock {
    mat4 model;
    mat4 normalMatrix;
    vec4 faceColor;   // linear RGB, a = opacity
    vec4 edgeColor;   // linear RGB, a = 1
    vec4 line;        // x: width in pixels, y: 1 if dashed, z: 1 for anti-aliased (smooth) edges
    vec4 ids;         // x: item id (T-015), yzw reserved
} item;

vec3 encodeSrgb(vec3 linearRgb)
{
    vec3 c = clamp(linearRgb, 0.0, 1.0);
    vec3 lo = c * 12.92;
    vec3 hi = 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055;
    return mix(lo, hi, step(vec3(0.0031308), c));
}
