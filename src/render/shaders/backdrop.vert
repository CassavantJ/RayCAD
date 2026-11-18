#version 440
// Glass backdrop (task T-016): a full-screen triangle; the fragment shader works from gl_FragCoord, so no
// orientation flip is needed on any backend (source and target share the render-target orientation).
layout(location = 0) in vec2 position;

void main()
{
    gl_Position = vec4(position, 0.0, 1.0);
}
