#version 440
layout(location = 0) in vec3 v_normal;
layout(location = 1) in vec3 v_pos;
layout(location = 2) in vec4 v_id;
layout(location = 0) out vec4 fragColor;
void main()
{
    fragColor = v_id;
}
