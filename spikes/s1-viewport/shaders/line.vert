#version 440
layout(location = 0) in vec3 position;
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
    gl_Position = ub.mvp * vec4(position, 1.0);
}
