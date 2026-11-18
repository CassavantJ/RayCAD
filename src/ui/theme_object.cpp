#include "theme_object.h"

#include <QtCore/QDir>
#include <QtCore/QVariant>
#include <QtGui/QFontDatabase>
#include <QtGui/QGuiApplication>
#include <QtGui/QStyleHints>

#include <algorithm>
#include <cmath>

namespace ray::ui {
namespace {

constexpr auto kTokenCount = static_cast<std::size_t>(render::Token::Count);
constexpr const char* kEngineKey = "_ray_ui_theme";

// Encoded sRGB with straight alpha, 0..1 per channel. Qt Quick blends in this space, so mixes are done here too.
struct Srgb {
    double r, g, b, a;
};

double encode(double linear)
{
    linear = std::clamp(linear, 0.0, 1.0);
    return linear <= 0.0031308 ? linear * 12.92 : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
}

double decode(double encoded)
{
    return encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4);
}

Srgb toSrgb(const render::Color& c)
{
    return {encode(c.r), encode(c.g), encode(c.b), std::clamp(static_cast<double>(c.a), 0.0, 1.0)};
}

render::Color toLinear(const Srgb& c)
{
    return {static_cast<float>(decode(c.r)), static_cast<float>(decode(c.g)), static_cast<float>(decode(c.b)),
            static_cast<float>(c.a)};
}

// Rounded to 8 bits per channel, so a theme file's "#RRGGBB" comes back exactly.
QColor toQColor(const Srgb& c)
{
    auto byte = [](double v) {
        return static_cast<int>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0));
    };
    return QColor(byte(c.r), byte(c.g), byte(c.b), byte(c.a));
}

Srgb mix(const Srgb& a, const Srgb& b, double t)
{
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

// fg over an opaque bg.
Srgb over(const Srgb& fg, const Srgb& bg)
{
    return {fg.r * fg.a + bg.r * (1 - fg.a), fg.g * fg.a + bg.g * (1 - fg.a), fg.b * fg.a + bg.b * (1 - fg.a), 1.0};
}

Srgb withAlpha(Srgb c, double a)
{
    c.a = a;
    return c;
}

// Mix weights for the derived control colours. These are the only colour decisions in src/ui; each is a blend of
// two theme tokens, so a theme change moves them with it.
struct MixWeights {
    double controlBg = 0.07;         // PanelBg -> TextPrimary
    double controlBgHover = 0.11;    // PanelBg -> TextPrimary
    double controlBgPressed = 0.16;  // PanelBg -> TextPrimary
    double controlRaisedDark = 0.17; // PanelBg -> TextPrimary (dark themes; light themes use PanelBg itself)
    double controlBorder = 0.7;      // PanelBg -> TextSecondary (keeps 3:1 for outlines of check boxes and the like)
    double fieldInsetDark = 0.6;     // PanelBg -> WindowBg (dark themes; light themes take PanelHighlightTop)
    double accentHover = 0.12;       // Accent -> TextPrimary
    double accentPressed = 0.24;     // Accent -> TextPrimary
    double accentFillDark = 0.18;    // PanelBg -> Accent
    double accentFillLight = 0.24;   // PanelBg -> Accent
    double scrollThumb = 0.45;       // TextSecondary opacity
    double scrollThumbHover = 0.7;   // TextSecondary opacity
};

QString lowerFirst(std::string_view name)
{
    QString s = QString::fromLatin1(name.data(), static_cast<qsizetype>(name.size()));
    if (!s.isEmpty()) {
        s[0] = s[0].toLower();
    }
    return s;
}

} // namespace

ThemeObject::ThemeObject(QStringList themeDirs, QObject* parent) : QObject(parent)
{
    if (themeDirs.isEmpty()) {
        themeDirs << QStringLiteral(":/ray/themes");
    }
    for (const QString& dirPath : themeDirs) {
        const QDir dir(dirPath);
        const QStringList files = dir.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
        for (const QString& file : files) {
            const QString path = dir.filePath(file);
            auto loaded = render::loadTheme(path.toStdString());
            if (!loaded) {
                loadErrors_ << QString::fromStdString(loaded.error().message);
                continue;
            }
            themes_.push_back(std::make_shared<const render::Theme>(std::move(*loaded)));
        }
    }
    if (themes_.empty() && loadErrors_.isEmpty()) {
        loadErrors_ << QStringLiteral("no theme files found in %1").arg(themeDirs.join(QStringLiteral(", ")));
    }

    if (auto* hints = QGuiApplication::styleHints()) {
        // Unknown (no OS preference, e.g. offscreen) counts as dark: Graphite Dark is the primary look.
        systemDark_ = hints->colorScheme() != Qt::ColorScheme::Light;
        connect(hints, &QStyleHints::colorSchemeChanged, this,
                [this](Qt::ColorScheme scheme) { setSystemDark(scheme != Qt::ColorScheme::Light); });
    }
    apply(resolve(mode_));
}

ThemeObject::~ThemeObject() = default;

ThemeObject* ThemeObject::create(QQmlEngine* qml, QJSEngine*)
{
    if (qml != nullptr) {
        if (auto* theme = qobject_cast<ThemeObject*>(qml->property(kEngineKey).value<QObject*>())) {
            QJSEngine::setObjectOwnership(theme, QJSEngine::CppOwnership);
            return theme;
        }
    }
    return new ThemeObject(QStringList()); // owned by the engine
}

void ThemeObject::provideFor(QQmlEngine* engine, ThemeObject* theme)
{
    engine->setProperty(kEngineKey, QVariant::fromValue<QObject*>(theme));
}

bool ThemeObject::setMode(const QString& mode)
{
    const QString normalised = mode.trimmed().toLower();
    auto theme = resolve(normalised);
    if (theme == nullptr) {
        return false;
    }
    if (normalised != mode_) {
        mode_ = normalised;
        emit modeChanged();
    }
    apply(std::move(theme));
    return true;
}

void ThemeObject::setSystemDark(bool dark)
{
    if (dark == systemDark_) {
        return;
    }
    systemDark_ = dark;
    emit systemDarkChanged();
    if (mode_ == QLatin1String("system")) {
        apply(resolve(mode_));
    }
}

std::shared_ptr<const render::Theme> ThemeObject::resolve(const QString& mode) const
{
    auto firstOfKind = [this](bool dark) -> std::shared_ptr<const render::Theme> {
        for (const auto& theme : themes_) {
            if (theme->dark == dark) {
                return theme;
            }
        }
        return themes_.empty() ? nullptr : themes_.front();
    };
    if (mode == QLatin1String("system")) {
        return firstOfKind(systemDark_);
    }
    if (mode == QLatin1String("dark") || mode == QLatin1String("light")) {
        return firstOfKind(mode == QLatin1String("dark"));
    }
    for (const auto& theme : themes_) {
        if (QString::fromStdString(theme->id) == mode) {
            return theme;
        }
    }
    return nullptr;
}

void ThemeObject::apply(std::shared_ptr<const render::Theme> theme)
{
    if (theme == nullptr || theme == current_) {
        return;
    }
    current_ = std::move(theme);
    computeDerived();
    resolveFonts();
    emit themeChanged();
}

QString ThemeObject::themeId() const
{
    return current_ ? QString::fromStdString(current_->id) : QString();
}

QString ThemeObject::title() const
{
    return current_ ? QString::fromStdString(current_->title) : QString();
}

bool ThemeObject::dark() const
{
    return current_ ? current_->dark : true;
}

QStringList ThemeObject::themeIds() const
{
    QStringList ids;
    for (const auto& theme : themes_) {
        ids << QString::fromStdString(theme->id);
    }
    return ids;
}

QStringList ThemeObject::tokenProperties() const
{
    QStringList names;
    for (std::size_t i = 0; i < kTokenCount; ++i) {
        names << propertyName(static_cast<render::Token>(i));
    }
    return names;
}

QString ThemeObject::propertyName(render::Token token)
{
    return lowerFirst(render::tokenName(token));
}

QColor ThemeObject::color(render::Token token) const
{
    const auto index = static_cast<std::size_t>(token);
    if (current_ == nullptr || index >= kTokenCount) {
        return {};
    }
    return toQColor(toSrgb(current_->colors[index]));
}

QColor ThemeObject::token(const QString& name) const
{
    for (std::size_t i = 0; i < kTokenCount; ++i) {
        const auto token = static_cast<render::Token>(i);
        const std::string_view key = render::tokenName(token);
        if (name == QLatin1String(key.data(), static_cast<qsizetype>(key.size())) || name == propertyName(token)) {
            return color(token);
        }
    }
    return {};
}

qreal ThemeObject::metric(const QString& name, qreal fallback) const
{
    if (current_ == nullptr) {
        return fallback;
    }
    const auto it = current_->metrics.find(name.toStdString());
    return it == current_->metrics.end() ? fallback : static_cast<qreal>(it->second);
}

qreal ThemeObject::sizeMetric(const char* name, qreal derived) const
{
    return metric(QString::fromLatin1(name), derived);
}

void ThemeObject::computeDerived()
{
    const MixWeights w;
    auto tok = [this](render::Token t) {
        return toSrgb(current_->colors[static_cast<std::size_t>(t)]);
    };
    const Srgb panel = tok(render::Token::PanelBg);
    const Srgb text = tok(render::Token::TextPrimary);
    const Srgb secondary = tok(render::Token::TextSecondary);
    const Srgb window = tok(render::Token::WindowBg);
    const Srgb accent = tok(render::Token::Accent);
    const Srgb accentLine = tok(render::Token::AccentThinLine);
    const Srgb highlight = tok(render::Token::PanelHighlightTop);
    const bool isDark = current_->dark;

    // The candidate with the best contrast against `bg`, among the opaque text and surface tokens.
    auto bestOn = [&](const Srgb& bg) {
        const Srgb candidates[] = {text, window, panel};
        const render::Color bgLinear = toLinear(over(bg, panel));
        Srgb best = text;
        double bestRatio = 0;
        for (const Srgb& c : candidates) {
            const double ratio = render::contrastRatio(toLinear(c), bgLinear);
            if (ratio > bestRatio) {
                bestRatio = ratio;
                best = c;
            }
        }
        return best;
    };

    derived_.controlBg = toQColor(mix(panel, text, w.controlBg));
    derived_.controlBgHover = toQColor(mix(panel, text, w.controlBgHover));
    derived_.controlBgPressed = toQColor(mix(panel, text, w.controlBgPressed));
    derived_.controlRaised = toQColor(isDark ? mix(panel, text, w.controlRaisedDark) : panel);
    derived_.controlBorder = toQColor(mix(panel, secondary, w.controlBorder));
    derived_.fieldBg = toQColor(isDark ? mix(panel, window, w.fieldInsetDark) : over(highlight, panel));
    derived_.accentHover = toQColor(mix(accent, text, w.accentHover));
    derived_.accentPressed = toQColor(mix(accent, text, w.accentPressed));
    derived_.accentFill = toQColor(mix(panel, accent, isDark ? w.accentFillDark : w.accentFillLight));
    derived_.onAccent = toQColor(bestOn(accent));
    // Glyphs on the thin-line accent (check marks, switch thumbs) take the panel colour when it reads at 3:1 (the
    // non-text contrast minimum), which keeps a light check mark on the darker light-theme accent.
    const double panelOnLine = render::contrastRatio(toLinear(panel), toLinear(over(accentLine, panel)));
    derived_.onAccentLine = toQColor(panelOnLine >= 3.0 ? panel : bestOn(accentLine));
    derived_.focusRing = toQColor(accentLine);
    derived_.tooltipBg = derived_.controlRaised;
    derived_.scrollThumb = toQColor(withAlpha(secondary, w.scrollThumb));
    derived_.scrollThumbHover = toQColor(withAlpha(secondary, w.scrollThumbHover));
}

void ThemeObject::resolveFonts()
{
    // The theme's family if it is installed (the bundled fonts arrive with a later task, resources/fonts/README.md),
    // else the first installed fallback, else the system font of that kind. The mono fallbacks are the platforms'
    // own programming fonts: the generic fixed font is Courier New on Windows, which reads poorly at 13 px.
    auto pick = [](const std::map<std::string, std::string>& fonts, const char* role, QStringList fallbacks,
                   QFontDatabase::SystemFont systemFont) {
        const auto it = fonts.find(role);
        if (it != fonts.end()) {
            fallbacks.prepend(QString::fromStdString(it->second));
        }
        for (const QString& family : fallbacks) {
            if (QFontDatabase::hasFamily(family)) {
                return family;
            }
        }
        return QFontDatabase::systemFont(systemFont).family();
    };
    uiFamily_ = pick(current_->fonts, "ui", {}, QFontDatabase::GeneralFont);
    monoFamily_ = pick(current_->fonts, "mono",
                       {QStringLiteral("Cascadia Mono"), QStringLiteral("Consolas"), QStringLiteral("SF Mono"),
                        QStringLiteral("Menlo"), QStringLiteral("DejaVu Sans Mono"), QStringLiteral("Liberation Mono")},
                       QFontDatabase::FixedFont);
}

qreal ThemeObject::radiusSmall() const
{
    return std::min(radiusPanel() / 2, radiusControl());
}

qreal ThemeObject::controlHeight() const
{
    return sizeMetric("size.control", std::round(fontUiPx() * 2.15));
}

qreal ThemeObject::controlHeightSmall() const
{
    return sizeMetric("size.control.small", std::round(fontUiPx() * 1.7));
}

qreal ThemeObject::rowHeight() const
{
    return sizeMetric("size.row", std::round(fontUiPx() * 1.85));
}

qreal ThemeObject::iconSize() const
{
    return sizeMetric("size.icon", spacingL());
}

qreal ThemeObject::indicatorSize() const
{
    return sizeMetric("size.indicator", std::round(fontUiPx() * 1.25));
}

qreal ThemeObject::panelHeaderHeight() const
{
    return sizeMetric("size.panel.header", controlHeight() + spacingXs());
}

qreal ThemeObject::topBarHeight() const
{
    return sizeMetric("size.topbar", controlHeight() + 2 * spacingS());
}

qreal ThemeObject::statusBarHeight() const
{
    return sizeMetric("size.statusbar", controlHeightSmall() + spacingS());
}

qreal ThemeObject::sidePanelWidth() const
{
    return sizeMetric("size.sidepanel", std::round(fontUiPx() * 22));
}

qreal ThemeObject::sidePanelMinWidth() const
{
    return sizeMetric("size.sidepanel.min", std::round(fontUiPx() * 16));
}

qreal ThemeObject::sidePanelMaxWidth() const
{
    return sizeMetric("size.sidepanel.max", std::round(fontUiPx() * 40));
}

qreal ThemeObject::panelGap() const
{
    return sizeMetric("size.panel.gap", spacingS());
}

qreal ThemeObject::focusRingWidth() const
{
    return sizeMetric("size.focus", 2 * borderWidth());
}

int ThemeObject::durationFast() const
{
    return static_cast<int>(sizeMetric("motion.fast.ms", 120));
}

int ThemeObject::toolTipDelay() const
{
    return static_cast<int>(sizeMetric("motion.tooltip.ms", 600));
}

QFont ThemeObject::uiFont() const
{
    QFont font(uiFamily_);
    font.setPixelSize(static_cast<int>(std::lround(fontUiPx())));
    return font;
}

QFont ThemeObject::strongFont() const
{
    QFont font = uiFont();
    font.setWeight(QFont::DemiBold);
    return font;
}

QFont ThemeObject::smallFont() const
{
    QFont font(uiFamily_);
    font.setPixelSize(static_cast<int>(std::lround(fontSmallPx())));
    return font;
}

QFont ThemeObject::titleFont() const
{
    QFont font(uiFamily_);
    font.setPixelSize(static_cast<int>(std::lround(fontTitlePx())));
    font.setWeight(QFont::DemiBold);
    return font;
}

QFont ThemeObject::monoFont() const
{
    QFont font(monoFamily_);
    font.setPixelSize(static_cast<int>(std::lround(fontMonoPx())));
    font.setStyleHint(QFont::Monospace);
    font.setFixedPitch(true);
    return font;
}

} // namespace ray::ui
