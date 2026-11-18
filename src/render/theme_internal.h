// Private to src/render (and its unit tests): the contrast rules loadTheme applies (see theme.cpp).
// No Qt header here.
#pragma once

#include <ray/render.h>

#include <span>

namespace ray::render::detail {

// Text drawn on panels: at least 4.5:1 against PanelBg and on a glass panel over any backdrop.
std::span<const Token> textTokens();
// The G-code editor's text tokens (a subset of textTokens), also checked on the GcodeCurrentLine highlight.
std::span<const Token> gcodeTextTokens();
// Thin lines in the viewport: at least 3:1 against ViewportBgTop and ViewportBgBottom.
std::span<const Token> thinLineTokens();
// Backgrounds that must be opaque.
std::span<const Token> opaqueTokens();

// The lowest contrast of opaque text on a glass panel whose tint is blended (on encoded sRGB values) over any
// backdrop. onWhite, if given, says whether the worst backdrop is white (true) or black (false).
double worstGlassContrast(Color text, Color tint, bool* onWhite = nullptr);

// Every contrast and opacity rule; FormatError naming the first offending token.
Status validateTheme(const Theme& theme);

} // namespace ray::render::detail
