#version 440
#extension GL_GOOGLE_include_directive : require
// Glass backdrop (task T-016): the 4:1 box filter (four bilinear taps, each the mean of 2 x 2 pixels).
#define TAPS 4
#include "backdrop.glsl"
