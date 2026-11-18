#version 440
#extension GL_GOOGLE_include_directive : require
// Glass backdrop (task T-016): one Gaussian pass with up to 7 taps.
#define TAPS 7
#include "backdrop.glsl"
