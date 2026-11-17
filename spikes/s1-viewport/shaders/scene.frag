#version 440
layout(location = 0) in vec3 v_normal;
layout(location = 1) in vec3 v_pos;
layout(location = 2) in vec4 v_id;
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
    vec3 n = normalize(v_normal);
    vec3 v = normalize(-v_pos);
    if (dot(n, v) < 0.0)
        n = -n;
    vec3 l = normalize(ub.lightDir.xyz);
    float diff = max(dot(n, l), 0.0);
    float spec = pow(max(dot(n, normalize(l + v)), 0.0), 48.0);
    vec3 base = ub.faceColor.rgb;
    vec4 d = abs(v_id - ub.highlightId);
    if (max(max(d.x, d.y), max(d.z, d.w)) < 0.002)
        base = mix(base, vec3(0.96, 0.65, 0.14), 0.7);
    fragColor = vec4(base * (0.22 + 0.78 * diff) + vec3(0.25 * spec), 1.0);
}
