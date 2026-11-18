#version 440
#extension GL_GOOGLE_include_directive : require
// Glass backdrop (task T-016): one Gaussian pass with up to 15 taps.
#define TAPS 15
#include "backdrop.glsl"
