#version 440
layout(location = 0) in vec2 position;
layout(location = 0) out vec2 v_uv;
layout(std140, binding = 0) uniform buf {
    vec2 dir;      // down: source texel size; blur: one texel step along the blur axis
    float ySign;   // +1 when the framebuffer is Y-up (OpenGL), -1 otherwise
    float pad;
} ub;
void main()
{
    v_uv = vec2(0.5 + 0.5 * position.x, 0.5 + 0.5 * position.y * ub.ySign);
    gl_Position = vec4(position, 0.0, 1.0);
}
