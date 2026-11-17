#version 440
layout(location = 0) out vec4 fragColor;
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
    fragColor = ub.edgeColor;
}
