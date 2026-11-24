#version 440
#extension GL_GOOGLE_include_directive : require
// Picking ID pass (task T-015): writes the three RGBA8 attachments described in src/render/rhi/pick_pass.h. Integer
// values are split into bytes with float arithmetic, which is exact below 2^24 and needs no integer support (the
// shaders are also baked for GLSL 120).
layout(location = 0) in vec4 v_id;
layout(location = 1) in vec3 v_depthKind;
layout(location = 2) in float v_section; // T-023: beyond the section plane nothing is pickable
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
    // A stock (T-073) loses what lies within its tolerance of the plane too, as mesh.frag draws it.
    if (v_section > (item.stock.w > 0.5 ? -item.stock.z : 0.0))
        discard;
    float kind = floor(v_depthKind.z + 0.5);
    outIds = vec4(v_id.rgb, kind / 255.0);
    // Backplot segments (IdKind 5, T-038) keep the high byte of their 32-bit move index in the item's alpha.
    outItem = vec4(bytes24(floor(item.ids.x + 0.5)), kind > 4.5 ? v_id.a : 1.0);
    float ndcZ = v_depthKind.x / v_depthKind.y;
    float depth = clamp(0.5 * ndcZ + 0.5, 0.0, 1.0);
    outDepth = vec4(bytes24(floor(depth * 16777215.0 + 0.5)), 1.0);
}
