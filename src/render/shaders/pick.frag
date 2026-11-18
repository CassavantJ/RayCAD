#version 440
#extension GL_GOOGLE_include_directive : require
// Picking ID pass (task T-015): writes the three RGBA8 attachments described in src/render/rhi/pick_pass.h. Integer
// values are split into bytes with float arithmetic, which is exact below 2^24 and needs no integer support (the
// shaders are also baked for GLSL 120).
layout(location = 0) in vec4 v_id;
layout(location = 1) in vec3 v_depthKind;
layout(location = 0) out vec4 outIds;
layout(location = 1) out vec4 outItem;
layout(location = 2) out vec4 outDepth;

#include "frame.glsl"

vec3 bytes24(float v)
{
    float b0 = mod(v, 256.0);
    float rest = floor(v / 256.0);
    float b1 = mod(rest, 256.0);
    float b2 = mod(floor(rest / 256.0), 256.0);
    return vec3(b0, b1, b2) / 255.0;
}

void main()
{
    outIds = vec4(v_id.rgb, floor(v_depthKind.z + 0.5) / 255.0);
    outItem = vec4(bytes24(floor(item.ids.x + 0.5)), 1.0);
    float ndcZ = v_depthKind.x / v_depthKind.y;
    float depth = clamp(0.5 * ndcZ + 0.5, 0.0, 1.0);
    outDepth = vec4(bytes24(floor(depth * 16777215.0 + 0.5)), 1.0);
}
