// Theme tokens and theme files (task T-013, PLAN.md 6.5 and 6.6). The broken themes below are made by editing the
// text of a shipped theme, so this file holds no colour of its own.
#include "theme_internal.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <string>

using namespace ray::render;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;

namespace {

const std::string kDark = std::string(RAY_THEMES_DIR) + "/graphite-dark.json";
const std::string kLight = std::string(RAY_THEMES_DIR) + "/graphite-light.json";

std::string readText(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// A theme file written to the temporary folder and removed again at the end of the test.
class TempTheme {
public:
    explicit TempTheme(const std::string& text)
    {
        std::random_device random;
        path_ = std::filesystem::temp_directory_path() /
                ("ray_render_theme_test_" + std::to_string(random()) + "_" + std::to_string(random()) + ".json");
        std::ofstream out(path_, std::ios::binary);
        out << text;
    }
    TempTheme(const TempTheme&) = delete;
    TempTheme& operator=(const TempTheme&) = delete;
    ~TempTheme()
    {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    std::string path() const { return path_.string(); }

private:
    std::filesystem::path path_;
};

std::regex tokenEntry(const std::string& token)
{
    return std::regex("\"" + token + "\"\\s*:\\s*\"([^\"]*)\"");
}

// The colour text a theme file gives a token.
std::string valueOf(const std::string& theme, const std::string& token)
{
    std::smatch match;
    REQUIRE(std::regex_search(theme, match, tokenEntry(token)));
    return match[1].str();
}

// The theme text with one token's colour replaced.
std::string withValue(const std::string& theme, const std::string& token, const std::string& value)
{
    REQUIRE(std::regex_search(theme, tokenEntry(token)));
    return std::regex_replace(theme, tokenEntry(token), "\"" + token + "\": \"" + value + "\"",
                              std::regex_constants::format_first_only);
}

// The theme text without the line of one token (every token sits on its own line, none of them last).
std::string withoutToken(const std::string& theme, const std::string& token)
{
    const std::regex line("\\n[ \\t]*\"" + token + "\"\\s*:\\s*\"[^\"]*\",");
    REQUIRE(std::regex_search(theme, line));
    return std::regex_replace(theme, line, "", std::regex_constants::format_first_only);
}

// The error message loadTheme gives for a theme text; fails the test if the theme loads.
std::string rejection(const std::string& text)
{
    TempTheme file(text);
    const auto theme = loadTheme(file.path());
    REQUIRE_FALSE(theme.has_value());
    CHECK(theme.error().code == ray::ErrorCode::FormatError);
    return theme.error().message;
}

const Color& colourOf(const Theme& theme, Token token)
{
    return theme.colors[static_cast<std::size_t>(token)];
}

// A linear colour back to the "#RRGGBB" it was written as.
std::string hexOf(const Color& c)
{
    auto encode = [](float v) {
        const double e = v <= 0.0031308f ? v * 12.92 : 1.055 * std::pow(static_cast<double>(v), 1.0 / 2.4) - 0.055;
        return static_cast<unsigned>(std::lround(std::clamp(e, 0.0, 1.0) * 255.0));
    };
    char text[16];
    std::snprintf(text, sizeof text, "#%02X%02X%02X", encode(c.r), encode(c.g), encode(c.b));
    return text;
}

Color opaque(float r, float g, float b)
{
    return Color{r, g, b, 1.0f};
}

} // namespace

TEST_CASE("token names", "[theme]")
{
    std::set<std::string> names;
    for (std::size_t i = 0; i < static_cast<std::size_t>(Token::Count); ++i) {
        const std::string_view name = tokenName(static_cast<Token>(i));
        CHECK_FALSE(name.empty());
        names.insert(std::string(name));
    }
    CHECK(names.size() == static_cast<std::size_t>(Token::Count));
    CHECK(tokenName(Token::Count).empty());
    // Spot checks that the table follows the enum order.
    CHECK(tokenName(Token::WindowBg) == "WindowBg");
    CHECK(tokenName(Token::TextPrimary) == "TextPrimary");
    CHECK(tokenName(Token::ViewportBgTop) == "ViewportBgTop");
    CHECK(tokenName(Token::PathCut) == "PathCut");
    CHECK(tokenName(Token::SimCollision) == "SimCollision");
    CHECK(tokenName(Token::GcodeText) == "GcodeText");
    CHECK(tokenName(Token::GcodeCurrentLine) == "GcodeCurrentLine");
}

TEST_CASE("the theme schema names every token", "[theme]")
{
    const std::string schema = readText(RAY_THEME_SCHEMA);
    REQUIRE_FALSE(schema.empty());
    for (std::size_t i = 0; i < static_cast<std::size_t>(Token::Count); ++i) {
        const std::string name(tokenName(static_cast<Token>(i)));
        INFO(name);
        // Once under "properties" and once in "required".
        const std::string quotedName = "\"" + name + "\"";
        const auto first = schema.find(quotedName);
        REQUIRE(first != std::string::npos);
        CHECK(schema.find(quotedName, first + 1) != std::string::npos);
    }
}

TEST_CASE("both shipped themes load", "[theme]")
{
    const auto dark = loadTheme(kDark);
    REQUIRE(dark.has_value());
    CHECK(dark->id == "graphite-dark");
    CHECK(dark->dark);
    const auto light = loadTheme(kLight);
    REQUIRE(light.has_value());
    CHECK(light->id == "graphite-light");
    CHECK_FALSE(light->dark);
    for (const Theme* theme : {&*dark, &*light}) {
        INFO(theme->id);
        CHECK_FALSE(theme->title.empty());
        CHECK(theme->fonts.at("ui") == "Inter");
        CHECK(theme->fonts.at("mono") == "JetBrains Mono");
        CHECK(theme->metrics.at("font.ui.px") == 13.0);
        CHECK(theme->metrics.count("radius.panel") == 1);
        CHECK(theme->metrics.count("blur.radius") == 1);
        CHECK(ray::render::detail::validateTheme(*theme).has_value());
    }
}

TEST_CASE("the brief's starting colours are in the themes", "[theme]")
{
    const auto dark = loadTheme(kDark);
    const auto light = loadTheme(kLight);
    REQUIRE(dark.has_value());
    REQUIRE(light.has_value());
    // One amber accent for primary actions and cutting moves, darkened for thin lines in light mode.
    CHECK(hexOf(colourOf(*dark, Token::Accent)) == "#F5A524");
    CHECK(hexOf(colourOf(*light, Token::Accent)) == "#F5A524");
    CHECK(hexOf(colourOf(*light, Token::AccentThinLine)) == "#B36A00");
    CHECK(hexOf(colourOf(*dark, Token::PathCut)) == "#F5A524");
    CHECK(hexOf(colourOf(*light, Token::PathCut)) == "#B36A00");
    // Blue reserved for selection.
    CHECK(hexOf(colourOf(*dark, Token::Selection)) == "#4C9AFF");
    CHECK(hexOf(colourOf(*light, Token::Selection)) == "#1F6FE0");
    // Rapids are red (drawn dashed by the viewport).
    for (const Theme* theme : {&*dark, &*light}) {
        const Color rapid = colourOf(*theme, Token::PathRapid);
        CHECK(rapid.r > 3 * rapid.g);
        CHECK(rapid.r > 3 * rapid.b);
    }
    // Why the light theme needs the darker amber: the accent itself is too light for a thin line there.
    CHECK(contrastRatio(colourOf(*light, Token::Accent), colourOf(*light, Token::ViewportBgTop)) < 3.0);
    CHECK(contrastRatio(colourOf(*light, Token::AccentThinLine), colourOf(*light, Token::ViewportBgBottom)) >= 3.0);
}

TEST_CASE("colours are converted from sRGB to linear RGB", "[theme]")
{
    const auto dark = loadTheme(kDark);
    REQUIRE(dark.has_value());
    // Accent #F5A524: 0xA5 = 165 encodes 0.376 linear; alpha stays straight.
    const Color accent = colourOf(*dark, Token::Accent);
    CHECK_THAT(accent.g, WithinAbs(0.3763, 0.001));
    CHECK(accent.a == 1.0f);
    // SelectionFill is translucent.
    CHECK(colourOf(*dark, Token::SelectionFill).a < 1.0f);
    CHECK(colourOf(*dark, Token::PanelBgGlassTint).a < 1.0f);
}

TEST_CASE("contrastRatio", "[theme]")
{
    const Color black = opaque(0, 0, 0);
    const Color white = opaque(1, 1, 1);
    CHECK_THAT(contrastRatio(black, white), WithinAbs(21.0, 1e-9));
    CHECK_THAT(contrastRatio(white, black), WithinAbs(21.0, 1e-9));
    CHECK_THAT(contrastRatio(white, white), WithinAbs(1.0, 1e-9));
    // #767676 on white is the classic 4.54:1 (0x76 = 118 is 0.1812 linear).
    const float grey = 0.18116f;
    CHECK_THAT(contrastRatio(opaque(grey, grey, grey), white), WithinAbs(4.54, 0.01));
    // A transparent foreground disappears into the background.
    CHECK_THAT(contrastRatio(Color{0, 0, 0, 0}, white), WithinAbs(1.0, 1e-9));
    CHECK_THAT(contrastRatio(Color{0, 0, 0, 0.5f}, white), WithinAbs(1.05 / 0.55, 1e-6));
}

TEST_CASE("worstGlassContrast", "[theme]")
{
    using ray::render::detail::worstGlassContrast;
    const Color white = opaque(1, 1, 1);
    // An opaque tint is a flat panel.
    const Color panel = opaque(0.01f, 0.01f, 0.012f);
    CHECK_THAT(worstGlassContrast(white, panel), WithinAbs(contrastRatio(white, panel), 1e-3));
    // A clear tint: white text over a white backdrop.
    bool onWhite = false;
    CHECK_THAT(worstGlassContrast(white, Color{0, 0, 0, 0}, &onWhite), WithinAbs(1.0, 1e-9));
    CHECK(onWhite);
    // Mid-grey text with a half-clear tint lies inside the range of backdrops: no contrast guaranteed.
    CHECK(worstGlassContrast(opaque(0.2f, 0.2f, 0.2f), Color{0.2f, 0.2f, 0.2f, 0.5f}) == 1.0);
}

TEST_CASE("a theme missing a token is rejected, naming the token", "[theme]")
{
    const std::string path = GENERATE(kDark, kLight);
    const std::string text = readText(path);
    for (const char* token : {"TextSecondary", "Accent", "PanelBg", "PathRapid", "GcodeComment", "SimGouge"}) {
        INFO(path << " without " << token);
        const std::string message = rejection(withoutToken(text, token));
        CHECK_THAT(message, ContainsSubstring("missing token '" + std::string(token) + "'"));
    }
}

TEST_CASE("a text token under 4.5:1 is rejected, naming the token", "[theme]")
{
    const std::string path = GENERATE(kDark, kLight);
    const std::string text = readText(path);
    const std::string panel = valueOf(text, "PanelBg");
    for (const char* token :
         {"TextPrimary", "TextSecondary", "AccentText", "Error", "GcodeComment", "GcodeLineNumber"}) {
        INFO(path << ": " << token << " set to PanelBg");
        const std::string message = rejection(withValue(text, token, panel));
        CHECK_THAT(message, ContainsSubstring("'" + std::string(token) + "'"));
        CHECK_THAT(message, ContainsSubstring("against 'PanelBg'"));
        CHECK_THAT(message, ContainsSubstring("4.5:1"));
    }
    // TextDisabled is deliberately dim (it is not a checked text token); as secondary text it fails.
    const std::string disabled = valueOf(text, "TextDisabled");
    const std::string message = rejection(withValue(text, "TextSecondary", disabled));
    CHECK_THAT(message, ContainsSubstring("'TextSecondary'"));
}

TEST_CASE("glass that is too clear for its text is rejected, naming the tint", "[theme]")
{
    const std::string path = GENERATE(kDark, kLight);
    const std::string text = readText(path);
    // The same tint colour at 25 % opacity.
    const std::string tint = valueOf(text, "PanelBgGlassTint").substr(0, 7) + "40";
    const std::string message = rejection(withValue(text, "PanelBgGlassTint", tint));
    CHECK_THAT(message, ContainsSubstring("'PanelBgGlassTint'"));
    CHECK_THAT(message, ContainsSubstring("glass panel"));
    CHECK_THAT(message, ContainsSubstring("'TextPrimary'"));
}

TEST_CASE("a thin-line token under 3:1 is rejected, naming the token", "[theme]")
{
    const std::string path = GENERATE(kDark, kLight);
    const std::string text = readText(path);
    for (const char* bg : {"ViewportBgTop", "ViewportBgBottom"}) {
        const std::string background = valueOf(text, bg);
        for (const char* token : {"PathRapid", "PathCut", "Selection", "AccentThinLine", "SketchGeometry", "AxisX"}) {
            INFO(path << ": " << token << " set to " << bg);
            const std::string message = rejection(withValue(text, token, background));
            CHECK_THAT(message, ContainsSubstring("'" + std::string(token) + "'"));
            CHECK_THAT(message, ContainsSubstring("3:1"));
        }
    }
}

TEST_CASE("the light theme rejects the bright accent as its thin-line colour", "[theme]")
{
    const std::string text = readText(kLight);
    const std::string accent = valueOf(text, "Accent");
    const std::string message = rejection(withValue(text, "AccentThinLine", accent));
    CHECK_THAT(message, ContainsSubstring("'AccentThinLine'"));
    CHECK_THAT(message, ContainsSubstring("thin lines need at least 3:1"));
    const std::string cut = rejection(withValue(text, "PathCut", accent));
    CHECK_THAT(cut, ContainsSubstring("'PathCut'"));
}

TEST_CASE("malformed theme files are rejected with a clear error", "[theme]")
{
    const std::string text = readText(kDark);
    const std::string panel = valueOf(text, "PanelBg");

    CHECK_THAT(rejection(withValue(text, "Accent", "amber")), ContainsSubstring("token 'Accent' is not a colour"));
    CHECK_THAT(rejection(withValue(text, "Accent", "#F5A5")), ContainsSubstring("token 'Accent' is not a colour"));
    CHECK_THAT(rejection(withValue(text, "Accent", "#GGA524")), ContainsSubstring("token 'Accent' is not a colour"));
    CHECK_THAT(rejection(withValue(text, "PanelBg", panel + "80")), ContainsSubstring("'PanelBg' must be opaque"));
    CHECK_THAT(rejection(withValue(text, "TextPrimary", valueOf(text, "TextPrimary") + "C0")),
               ContainsSubstring("'TextPrimary' must be opaque"));

    // A misspelt token is reported, not ignored.
    std::string misspelt = text;
    misspelt.replace(misspelt.find("\"PathLead\""), 10, "\"PathLeed\"");
    CHECK_THAT(rejection(misspelt), ContainsSubstring("unknown token 'PathLeed'"));

    std::string unknownKey = text;
    unknownKey.replace(unknownKey.find("\"dark\""), 6, "\"night\"");
    CHECK_THAT(rejection(unknownKey), ContainsSubstring("unknown key 'night'"));

    std::string format = text;
    format.replace(format.find("\"format\": 1"), 11, "\"format\": 2");
    CHECK_THAT(rejection(format), ContainsSubstring("'format' must be 1"));

    std::string noMetric = text;
    noMetric.replace(noMetric.find("\"font.ui.px\""), 12, "\"font.uix.px\"");
    CHECK_THAT(rejection(noMetric), ContainsSubstring("missing metric 'font.ui.px'"));

    std::string noFont = text;
    noFont.replace(noFont.find("\"mono\""), 6, "\"code\"");
    CHECK_THAT(rejection(noFont), ContainsSubstring("missing font 'mono'"));

    CHECK_THAT(rejection(text.substr(0, text.size() / 2)), ContainsSubstring("not valid JSON"));
    CHECK_THAT(rejection("[1, 2, 3]"), ContainsSubstring("JSON object"));

    const auto missing = loadTheme(std::string(RAY_THEMES_DIR) + "/no-such-theme.json");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == ray::ErrorCode::NotFound);
}

TEST_CASE("the rejection names the theme", "[theme]")
{
    const std::string text = readText(kLight);
    const std::string message = rejection(withoutToken(text, "Hover"));
    CHECK_THAT(message, ContainsSubstring("theme 'graphite-light'"));
}
