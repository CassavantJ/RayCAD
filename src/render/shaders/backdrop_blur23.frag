#version 440
#extension GL_GOOGLE_include_directive : require
// Glass backdrop (task T-016): one Gaussian pass with up to 23 taps.
#define TAPS 23
#include "backdrop.glsl"
