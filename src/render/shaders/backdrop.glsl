// Glass backdrop (task T-016): the fragment shader of every blur pass, a weighted sum of TAPS bilinear taps. Each
// backdrop_*.frag defines TAPS and includes this: a loop of constant length runs much faster than one that stops
// at a count from the uniforms (measured on an Intel UHD 770: 0.19 ms against 0.37 ms for the whole blur at
// 1920x1080). Must match BlurBlock in src/render/rhi/backdrop_pass.cpp.
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform BlurBlock {
    vec4 info;     // xy: 1 / target size in pixels
    vec4 taps[23]; // xy: offset in source texture coordinates; z: weight (0 for padding)
} blur;
layout(binding = 1) uniform sampler2D source;

void main()
{
    // gl_FragCoord and texture coordinates share their origin on every backend for a texture that was itself
    // rendered into, so the target texel maps onto the same place of the source.
    vec2 uv = gl_FragCoord.xy * blur.info.xy;
    vec4 c = vec4(0.0);
    for (int i = 0; i < TAPS; ++i)
        c += texture(source, uv + blur.taps[i].xy) * blur.taps[i].z;
    fragColor = c;
}