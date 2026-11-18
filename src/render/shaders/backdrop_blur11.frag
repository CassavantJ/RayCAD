#version 440
#extension GL_GOOGLE_include_directive : require
// Glass backdrop (task T-016): one Gaussian pass with up to 11 taps.
#define TAPS 11
#include "backdrop.glsl"
