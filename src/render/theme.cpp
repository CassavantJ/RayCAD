// Theme tokens and theme files (PLAN.md 6.5 and 6.6). The two shipped themes are resources/themes/*.json; their
// format is schemas/theme.schema.json. Colours in a file are sRGB hex ("#RRGGBB" or "#RRGGBBAA", alpha last);
// Theme::colors holds them as linear RGB with straight alpha.
//
// Contrast checks (WCAG 2 relative luminance and ratio), run on every load:
//  - text tokens (kTextTokens): at least 4.5:1 against PanelBg, and on a glass panel: PanelBgGlassTint blended over
//    the worst-case backdrop. Any backdrop gives a luminance between the tint over pure black and the tint over pure
//    white, so the worst case is whichever of those two is closer to the text, or 1:1 if the text lies between.
//    The blend is done on encoded sRGB values, as Qt Quick blends. Text tokens must be opaque.
//  - G-code text tokens also against GcodeCurrentLine blended over PanelBg (the highlighted line).
//  - thin-line tokens (kThinLineTokens): at least 3:1 against ViewportBgTop and ViewportBgBottom.
// The background tokens (kOpaqueTokens) must be opaque.
#include "theme_internal.h"

#include <QtCore/QByteArray>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QJsonValue>
#include <QtCore/QString>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <optional>
#include <set>

namespace ray::render {
namespace {

constexpr std::size_t kTokenCount = static_cast<std::size_t>(Token::Count);

constexpr std::array<std::string_view, kTokenCount> kTokenNames = {
    "WindowBg",
    "PanelBg",
    "PanelBgGlassTint",
    "PanelBorder",
    "PanelHighlightTop",
    "TextPrimary",
    "TextSecondary",
    "TextDisabled",
    "Accent",
    "AccentThinLine",
    "AccentText",
    "Selection",
    "SelectionFill",
    "Hover",
    "Error",
    "Warning",
    "Success",
    "ViewportBgTop",
    "ViewportBgBottom",
    "ModelFace",
    "ModelEdge",
    "ModelSilhouette",
    "FixtureFace",
    "StockFace",
    "StockCutFace",
    "GridMajor",
    "GridMinor",
    "AxisX",
    "AxisY",
    "AxisZ",
    "SketchGeometry",
    "SketchConstruction",
    "SketchConstrained",
    "SketchConflict",
    "PathCut",
    "PathRapid",
    "PathPlunge",
    "PathRamp",
    "PathLead",
    "PathLink",
    "PathSelected",
    "PathStale",
    "SimGouge",
    "SimLeftover",
    "SimCollision",
    "GcodeText",
    "GcodeComment",
    "GcodeGWord",
    "GcodeMWord",
    "GcodeAxis",
    "GcodeFeedSpeed",
    "GcodeLineNumber",
    "GcodeCurrentLine",
};
// The enum and the table must stay in step; tests compare every name with the enumerator it belongs to.
static_assert(kTokenNames.back() == "GcodeCurrentLine" && kTokenNames.size() == kTokenCount);

constexpr double kTextContrast = 4.5;
constexpr double kThinLineContrast = 3.0;

const char* const kRequiredMetrics[] = {"font.ui.px", "font.mono.px", "radius.panel", "radius.control"};
const char* const kRequiredFonts[] = {"ui", "mono"};

double toLinear(double c)
{
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double toEncoded(double c)
{
    c = std::clamp(c, 0.0, 1.0);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
}

double luminance(const Color& c)
{
    return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;
}

double ratioOf(double l1, double l2)
{
    const double hi = std::max(l1, l2);
    const double lo = std::min(l1, l2);
    return (hi + 0.05) / (lo + 0.05);
}

// fg over bg (opaque result), blended on encoded sRGB values as Qt Quick does.
Color blendEncoded(const Color& fg, const Color& bg)
{
    auto mix = [&](float f, float b) {
        const double e = toEncoded(f) * fg.a + toEncoded(b) * (1.0 - fg.a);
        return static_cast<float>(toLinear(e));
    };
    return Color{mix(fg.r, bg.r), mix(fg.g, bg.g), mix(fg.b, bg.b), 1.0f};
}

std::string formatRatio(double ratio)
{
    char text[32];
    const auto result = std::to_chars(text, text + sizeof text, ratio, std::chars_format::fixed, 2);
    return std::string(text, result.ptr) + ":1";
}

std::string inQuotes(std::string_view name)
{
    return "'" + std::string(name) + "'";
}

std::optional<Color> parseColor(std::string_view text)
{
    if ((text.size() != 7 && text.size() != 9) || text.front() != '#') {
        return std::nullopt;
    }
    std::array<float, 4> channels = {0, 0, 0, 1};
    for (std::size_t i = 0; i * 2 + 1 < text.size(); ++i) {
        unsigned value = 0;
        const char* begin = text.data() + 1 + i * 2;
        const auto result = std::from_chars(begin, begin + 2, value, 16);
        if (result.ec != std::errc{} || result.ptr != begin + 2) {
            return std::nullopt;
        }
        channels[i] = static_cast<float>(value) / 255.0f;
    }
    return Color{static_cast<float>(toLinear(channels[0])), static_cast<float>(toLinear(channels[1])),
                 static_cast<float>(toLinear(channels[2])), channels[3]};
}

class Loader {
public:
    explicit Loader(std::string path) : path_(std::move(path)) {}

    Result<Theme> run()
    {
        QFile file(QString::fromUtf8(path_.data(), static_cast<qsizetype>(path_.size())));
        if (!file.exists()) {
            return fail(ErrorCode::NotFound, "theme file not found");
        }
        if (!file.open(QIODevice::ReadOnly)) {
            return fail(ErrorCode::IoError, "theme file cannot be read: " + file.errorString().toStdString());
        }
        constexpr qint64 kMaxBytes = 1 << 20;
        if (file.size() > kMaxBytes) {
            return fail(ErrorCode::FormatError, "theme file is larger than 1 MiB");
        }
        const QByteArray bytes = file.readAll();
        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(bytes, &parseError);
        if (parseError.error != QJsonParseError::NoError) {
            return fail(ErrorCode::FormatError, "theme file is not valid JSON at offset " +
                                                    std::to_string(parseError.offset) + ": " +
                                                    parseError.errorString().toStdString());
        }
        if (!doc.isObject()) {
            return fail(ErrorCode::FormatError, "theme file must hold a JSON object");
        }
        return parse(doc.object());
    }

private:
    Result<Theme> parse(const QJsonObject& root)
    {
        static const std::set<QString> kKeys = {"$schema", "format", "id",      "title",
                                                "dark",    "colors", "metrics", "fonts"};
        for (auto it = root.begin(); it != root.end(); ++it) {
            if (kKeys.count(it.key()) == 0) {
                return fail(ErrorCode::FormatError, "unknown key " + inQuotes(it.key().toStdString()));
            }
        }
        const QJsonValue format = root.value("format");
        if (!format.isDouble() || format.toDouble() != 1.0) {
            return fail(ErrorCode::FormatError, "'format' must be 1");
        }
        Theme theme;
        const QJsonValue id = root.value("id");
        const QJsonValue title = root.value("title");
        if (!id.isString() || id.toString().isEmpty()) {
            return fail(ErrorCode::FormatError, "'id' must be a non-empty string");
        }
        if (!title.isString() || title.toString().isEmpty()) {
            return fail(ErrorCode::FormatError, "'title' must be a non-empty string");
        }
        if (!root.value("dark").isBool()) {
            return fail(ErrorCode::FormatError, "'dark' must be true or false");
        }
        theme.id = id.toString().toStdString();
        theme.title = title.toString().toStdString();
        theme.dark = root.value("dark").toBool();
        name_ = theme.id;

        if (auto status = parseColors(root.value("colors"), theme); !status) {
            return tl::make_unexpected(status.error());
        }
        if (auto status = parseMetrics(root.value("metrics"), theme); !status) {
            return tl::make_unexpected(status.error());
        }
        if (auto status = parseFonts(root.value("fonts"), theme); !status) {
            return tl::make_unexpected(status.error());
        }
        if (auto status = detail::validateTheme(theme); !status) {
            return fail(status.error().code, status.error().message);
        }
        return theme;
    }

    Status parseColors(const QJsonValue& value, Theme& theme)
    {
        if (!value.isObject()) {
            return fail(ErrorCode::FormatError, "'colors' must be an object mapping every token to a colour");
        }
        const QJsonObject colors = value.toObject();
        for (auto it = colors.begin(); it != colors.end(); ++it) {
            const std::string key = it.key().toStdString();
            if (std::find(kTokenNames.begin(), kTokenNames.end(), key) == kTokenNames.end()) {
                return fail(ErrorCode::FormatError, "unknown token " + inQuotes(key));
            }
        }
        for (std::size_t i = 0; i < kTokenCount; ++i) {
            const std::string_view name = kTokenNames[i];
            const QJsonValue entry =
                colors.value(QString::fromLatin1(name.data(), static_cast<qsizetype>(name.size())));
            if (entry.isUndefined()) {
                return fail(ErrorCode::FormatError, "missing token " + inQuotes(name));
            }
            const std::string text = entry.isString() ? entry.toString().toStdString() : std::string();
            const std::optional<Color> colour = parseColor(text);
            if (!colour) {
                return fail(ErrorCode::FormatError,
                            "token " + inQuotes(name) + " is not a colour; expected \"#RRGGBB\" or \"#RRGGBBAA\"");
            }
            theme.colors[i] = *colour;
        }
        return std::monostate{};
    }

    Status parseMetrics(const QJsonValue& value, Theme& theme)
    {
        if (!value.isObject()) {
            return fail(ErrorCode::FormatError, "'metrics' must be an object of numbers");
        }
        const QJsonObject metrics = value.toObject();
        for (auto it = metrics.begin(); it != metrics.end(); ++it) {
            const std::string key = it.key().toStdString();
            if (!it.value().isDouble() || !std::isfinite(it.value().toDouble()) || it.value().toDouble() < 0) {
                return fail(ErrorCode::FormatError, "metric " + inQuotes(key) + " must be a number of 0 or more");
            }
            theme.metrics[key] = it.value().toDouble();
        }
        for (const char* required : kRequiredMetrics) {
            if (theme.metrics.count(required) == 0) {
                return fail(ErrorCode::FormatError, "missing metric " + inQuotes(required));
            }
        }
        return std::monostate{};
    }

    Status parseFonts(const QJsonValue& value, Theme& theme)
    {
        if (!value.isObject()) {
            return fail(ErrorCode::FormatError, "'fonts' must be an object of font family names");
        }
        const QJsonObject fonts = value.toObject();
        for (auto it = fonts.begin(); it != fonts.end(); ++it) {
            const std::string key = it.key().toStdString();
            if (!it.value().isString() || it.value().toString().trimmed().isEmpty()) {
                return fail(ErrorCode::FormatError, "font " + inQuotes(key) + " must be a non-empty family name");
            }
            theme.fonts[key] = it.value().toString().toStdString();
        }
        for (const char* required : kRequiredFonts) {
            if (theme.fonts.count(required) == 0) {
                return fail(ErrorCode::FormatError, "missing font " + inQuotes(required));
            }
        }
        return std::monostate{};
    }

    tl::unexpected<Error> fail(ErrorCode code, const std::string& what) const
    {
        const std::string subject = name_.empty() ? std::string("theme") : "theme " + inQuotes(name_);
        return tl::make_unexpected(Error{code, subject + ": " + what, {"loadTheme " + path_}});
    }

    std::string path_;
    std::string name_;
};

} // namespace

std::string_view tokenName(Token token)
{
    const auto index = static_cast<std::size_t>(token);
    return index < kTokenCount ? kTokenNames[index] : std::string_view();
}

double contrastRatio(Color fg, Color bg)
{
    const double a = std::clamp(static_cast<double>(fg.a), 0.0, 1.0);
    const Color blended{static_cast<float>(fg.r * a + bg.r * (1.0 - a)),
                        static_cast<float>(fg.g * a + bg.g * (1.0 - a)),
                        static_cast<float>(fg.b * a + bg.b * (1.0 - a)), 1.0f};
    return ratioOf(luminance(blended), luminance(bg));
}

Result<Theme> loadTheme(const std::string& jsonPath)
{
    return Loader(jsonPath).run();
}

namespace detail {

std::span<const Token> textTokens()
{
    static constexpr Token kTokens[] = {
        Token::TextPrimary, Token::TextSecondary,  Token::AccentText,     Token::Error,      Token::Warning,
        Token::Success,     Token::GcodeText,      Token::GcodeComment,   Token::GcodeGWord, Token::GcodeMWord,
        Token::GcodeAxis,   Token::GcodeFeedSpeed, Token::GcodeLineNumber};
    return kTokens;
}

std::span<const Token> gcodeTextTokens()
{
    return textTokens().subspan(6);
}

std::span<const Token> thinLineTokens()
{
    static constexpr Token kTokens[] = {Token::AccentThinLine,
                                        Token::Selection,
                                        Token::Hover,
                                        Token::AxisX,
                                        Token::AxisY,
                                        Token::AxisZ,
                                        Token::SketchGeometry,
                                        Token::SketchConstruction,
                                        Token::SketchConstrained,
                                        Token::SketchConflict,
                                        Token::PathCut,
                                        Token::PathRapid,
                                        Token::PathPlunge,
                                        Token::PathRamp,
                                        Token::PathLead,
                                        Token::PathLink,
                                        Token::PathSelected,
                                        Token::PathStale};
    return kTokens;
}

std::span<const Token> opaqueTokens()
{
    static constexpr Token kTokens[] = {Token::WindowBg, Token::PanelBg, Token::ViewportBgTop, Token::ViewportBgBottom};
    return kTokens;
}

double worstGlassContrast(Color text, Color tint, bool* onWhite)
{
    const double lt = luminance(text);
    const double lw = luminance(blendEncoded(tint, Color{1, 1, 1, 1}));
    const double lb = luminance(blendEncoded(tint, Color{0, 0, 0, 1}));
    const double rw = ratioOf(lt, lw);
    const double rb = ratioOf(lt, lb);
    if (onWhite != nullptr) {
        *onWhite = rw <= rb;
    }
    if (lt >= std::min(lw, lb) && lt <= std::max(lw, lb)) {
        return 1.0;
    }
    return std::min(rw, rb);
}

Status validateTheme(const Theme& theme)
{
    auto colour = [&](Token t) {
        return theme.colors[static_cast<std::size_t>(t)];
    };
    auto failure = [](std::string message) -> Status {
        return tl::make_unexpected(Error{ErrorCode::FormatError, std::move(message), {}});
    };

    for (Token t : opaqueTokens()) {
        if (colour(t).a < 1.0f) {
            return failure("token " + inQuotes(tokenName(t)) + " must be opaque (no alpha)");
        }
    }
    const Color panel = colour(Token::PanelBg);
    const Color tint = colour(Token::PanelBgGlassTint);
    const Color currentLine = blendEncoded(colour(Token::GcodeCurrentLine), panel);
    const auto gcode = gcodeTextTokens();
    for (Token t : textTokens()) {
        const Color fg = colour(t);
        const std::string name = inQuotes(tokenName(t));
        if (fg.a < 1.0f) {
            return failure("text token " + name + " must be opaque (no alpha)");
        }
        const double flat = contrastRatio(fg, panel);
        if (flat < kTextContrast) {
            return failure("token " + name + " has " + formatRatio(flat) +
                           " contrast against 'PanelBg'; text needs at least 4.5:1");
        }
        bool onWhite = false;
        const double glass = worstGlassContrast(fg, tint, &onWhite);
        if (glass < kTextContrast) {
            return failure("token " + name + " has " + formatRatio(glass) +
                           " contrast on a glass panel ('PanelBgGlassTint' over a " + (onWhite ? "white" : "black") +
                           " backdrop); text needs at least 4.5:1");
        }
        if (std::find(gcode.begin(), gcode.end(), t) != gcode.end()) {
            const double highlighted = contrastRatio(fg, currentLine);
            if (highlighted < kTextContrast) {
                return failure("token " + name + " has " + formatRatio(highlighted) +
                               " contrast on the highlighted line ('GcodeCurrentLine' over 'PanelBg'); text needs at "
                               "least 4.5:1");
            }
        }
    }
    for (Token t : thinLineTokens()) {
        for (Token bg : {Token::ViewportBgTop, Token::ViewportBgBottom}) {
            const double ratio = contrastRatio(colour(t), colour(bg));
            if (ratio < kThinLineContrast) {
                return failure("token " + inQuotes(tokenName(t)) + " has " + formatRatio(ratio) + " contrast against " +
                               inQuotes(tokenName(bg)) + "; thin lines need at least 3:1");
            }
        }
    }
    return std::monostate{};
}

} // namespace detail
} // namespace ray::render
