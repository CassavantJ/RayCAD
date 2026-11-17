#version 440
layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec4 faceId;
layout(location = 0) out vec3 v_normal;
layout(location = 1) out vec3 v_pos;
layout(location = 2) out vec4 v_id;
layout(std140, binding = 0) uniform buf {
    mat4 mvp;
    mat4 mv;
    vec4 lightDir;
    vec4 faceColor;
    vec4 edgeColor;
    vec4 highlightId;
} ub;
void main()
{
    v_normal = mat3(ub.mv) * normal;
    v_pos = (ub.mv * vec4(position, 1.0)).xyz;
    v_id = faceId;
    gl_Position = ub.mvp * vec4(position, 1.0);
}
