// The `Theme` QML singleton (PLAN.md 6.5): every render::Token as a colour property, the theme metrics and fonts,
// live switching between the shipped themes, and following the OS light/dark setting with a manual override.
//
// QML reads `Theme.panelBg`, `Theme.spacingM`, `Theme.uiFont` and so on. Property names are the token's JSON key with
// a lower-case first letter (PanelBg -> panelBg, GcodeGWord -> gcodeGWord). Every property notifies through
// themeChanged(), so a switch repaints everything with no restart. C++ code (the viewport, T-020) takes the same
// theme from current() and listens to themeChanged().
//
// The token set has no control-surface colours (button fills, hover, focus ring), so the "derived" properties below
// mix existing tokens in encoded sRGB (as Qt Quick blends). The mix weights are in one table in theme_object.cpp.
// Sizes the theme files do not name (control height, row height, icon size) are derived from the font and spacing
// metrics, and a theme may override each one with the metric named next to it.
#pragma once

#include <ray/render.h>

#include <QtCore/QObject>
#include <QtCore/QStringList>
#include <QtGui/QColor>
#include <QtGui/QFont>
#include <QtQml/QQmlEngine>

#include <memory>
#include <vector>

namespace ray::ui {

class ThemeObject : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(Theme)
    QML_SINGLETON

    // "system" follows the OS colour scheme; "dark" and "light" pick the first shipped theme of that kind;
    // anything else is a theme id ("graphite-dark"). Unknown values are refused (setMode returns false).
    Q_PROPERTY(QString mode READ mode WRITE setModeProperty NOTIFY modeChanged)
    Q_PROPERTY(QString themeId READ themeId NOTIFY themeChanged)
    Q_PROPERTY(QString title READ title NOTIFY themeChanged)
    Q_PROPERTY(bool dark READ dark NOTIFY themeChanged)
    Q_PROPERTY(bool systemDark READ systemDark NOTIFY systemDarkChanged)
    Q_PROPERTY(QStringList themeIds READ themeIds CONSTANT)
    Q_PROPERTY(QStringList tokenProperties READ tokenProperties CONSTANT)

    // ---- tokens (one per render::Token, same order) ----
    Q_PROPERTY(QColor windowBg READ windowBg NOTIFY themeChanged)
    Q_PROPERTY(QColor panelBg READ panelBg NOTIFY themeChanged)
    Q_PROPERTY(QColor panelBgGlassTint READ panelBgGlassTint NOTIFY themeChanged)
    Q_PROPERTY(QColor panelBorder READ panelBorder NOTIFY themeChanged)
    Q_PROPERTY(QColor panelHighlightTop READ panelHighlightTop NOTIFY themeChanged)
    Q_PROPERTY(QColor textPrimary READ textPrimary NOTIFY themeChanged)
    Q_PROPERTY(QColor textSecondary READ textSecondary NOTIFY themeChanged)
    Q_PROPERTY(QColor textDisabled READ textDisabled NOTIFY themeChanged)
    Q_PROPERTY(QColor accent READ accent NOTIFY themeChanged)
    Q_PROPERTY(QColor accentThinLine READ accentThinLine NOTIFY themeChanged)
    Q_PROPERTY(QColor accentText READ accentText NOTIFY themeChanged)
    Q_PROPERTY(QColor selection READ selection NOTIFY themeChanged)
    Q_PROPERTY(QColor selectionFill READ selectionFill NOTIFY themeChanged)
    Q_PROPERTY(QColor hover READ hover NOTIFY themeChanged)
    Q_PROPERTY(QColor error READ error NOTIFY themeChanged)
    Q_PROPERTY(QColor warning READ warning NOTIFY themeChanged)
    Q_PROPERTY(QColor success READ success NOTIFY themeChanged)
    Q_PROPERTY(QColor viewportBgTop READ viewportBgTop NOTIFY themeChanged)
    Q_PROPERTY(QColor viewportBgBottom READ viewportBgBottom NOTIFY themeChanged)
    Q_PROPERTY(QColor modelFace READ modelFace NOTIFY themeChanged)
    Q_PROPERTY(QColor modelEdge READ modelEdge NOTIFY themeChanged)
    Q_PROPERTY(QColor modelSilhouette READ modelSilhouette NOTIFY themeChanged)
    Q_PROPERTY(QColor fixtureFace READ fixtureFace NOTIFY themeChanged)
    Q_PROPERTY(QColor stockFace READ stockFace NOTIFY themeChanged)
    Q_PROPERTY(QColor stockCutFace READ stockCutFace NOTIFY themeChanged)
    Q_PROPERTY(QColor gridMajor READ gridMajor NOTIFY themeChanged)
    Q_PROPERTY(QColor gridMinor READ gridMinor NOTIFY themeChanged)
    Q_PROPERTY(QColor axisX READ axisX NOTIFY themeChanged)
    Q_PROPERTY(QColor axisY READ axisY NOTIFY themeChanged)
    Q_PROPERTY(QColor axisZ READ axisZ NOTIFY themeChanged)
    Q_PROPERTY(QColor sketchGeometry READ sketchGeometry NOTIFY themeChanged)
    Q_PROPERTY(QColor sketchConstruction READ sketchConstruction NOTIFY themeChanged)
    Q_PROPERTY(QColor sketchConstrained READ sketchConstrained NOTIFY themeChanged)
    Q_PROPERTY(QColor sketchConflict READ sketchConflict NOTIFY themeChanged)
    Q_PROPERTY(QColor pathCut READ pathCut NOTIFY themeChanged)
    Q_PROPERTY(QColor pathRapid READ pathRapid NOTIFY themeChanged)
    Q_PROPERTY(QColor pathPlunge READ pathPlunge NOTIFY themeChanged)
    Q_PROPERTY(QColor pathRamp READ pathRamp NOTIFY themeChanged)
    Q_PROPERTY(QColor pathLead READ pathLead NOTIFY themeChanged)
    Q_PROPERTY(QColor pathLink READ pathLink NOTIFY themeChanged)
    Q_PROPERTY(QColor pathSelected READ pathSelected NOTIFY themeChanged)
    Q_PROPERTY(QColor pathStale READ pathStale NOTIFY themeChanged)
    Q_PROPERTY(QColor simGouge READ simGouge NOTIFY themeChanged)
    Q_PROPERTY(QColor simLeftover READ simLeftover NOTIFY themeChanged)
    Q_PROPERTY(QColor simCollision READ simCollision NOTIFY themeChanged)
    Q_PROPERTY(QColor draftPositive READ draftPositive NOTIFY themeChanged) // T-023: draft analysis
    Q_PROPERTY(QColor draftNegative READ draftNegative NOTIFY themeChanged)
    Q_PROPERTY(QColor draftVertical READ draftVertical NOTIFY themeChanged)
    Q_PROPERTY(QColor gcodeText READ gcodeText NOTIFY themeChanged)
    Q_PROPERTY(QColor gcodeComment READ gcodeComment NOTIFY themeChanged)
    Q_PROPERTY(QColor gcodeGWord READ gcodeGWord NOTIFY themeChanged)
    Q_PROPERTY(QColor gcodeMWord READ gcodeMWord NOTIFY themeChanged)
    Q_PROPERTY(QColor gcodeAxis READ gcodeAxis NOTIFY themeChanged)
    Q_PROPERTY(QColor gcodeFeedSpeed READ gcodeFeedSpeed NOTIFY themeChanged)
    Q_PROPERTY(QColor gcodeLineNumber READ gcodeLineNumber NOTIFY themeChanged)
    Q_PROPERTY(QColor gcodeCurrentLine READ gcodeCurrentLine NOTIFY themeChanged)

    // ---- derived control colours (mixes of the tokens above) ----
    Q_PROPERTY(QColor controlBg READ controlBg NOTIFY themeChanged)               // resting fill of buttons, tracks
    Q_PROPERTY(QColor controlBgHover READ controlBgHover NOTIFY themeChanged)     // hovered fill, hovered rows
    Q_PROPERTY(QColor controlBgPressed READ controlBgPressed NOTIFY themeChanged) // pressed fill, empty grooves
    Q_PROPERTY(QColor controlRaised READ controlRaised NOTIFY themeChanged)       // selected segment of a tab bar
    Q_PROPERTY(QColor controlBorder READ controlBorder NOTIFY themeChanged)       // outlines that must read (3:1)
    Q_PROPERTY(QColor fieldBg READ fieldBg NOTIFY themeChanged)                   // text and number fields
    Q_PROPERTY(QColor accentHover READ accentHover NOTIFY themeChanged)
    Q_PROPERTY(QColor accentPressed READ accentPressed NOTIFY themeChanged)
    Q_PROPERTY(QColor accentFill READ accentFill NOTIFY themeChanged)     // checked tool buttons: a faint accent wash
    Q_PROPERTY(QColor onAccent READ onAccent NOTIFY themeChanged)         // text and glyphs on an Accent fill
    Q_PROPERTY(QColor onAccentLine READ onAccentLine NOTIFY themeChanged) // glyphs on an AccentThinLine fill
    Q_PROPERTY(QColor focusRing READ focusRing NOTIFY themeChanged)
    Q_PROPERTY(QColor tooltipBg READ tooltipBg NOTIFY themeChanged)
    Q_PROPERTY(QColor scrollThumb READ scrollThumb NOTIFY themeChanged)
    Q_PROPERTY(QColor scrollThumbHover READ scrollThumbHover NOTIFY themeChanged)

    // ---- metrics from the theme file ----
    Q_PROPERTY(qreal fontUiPx READ fontUiPx NOTIFY themeChanged)
    Q_PROPERTY(qreal fontMonoPx READ fontMonoPx NOTIFY themeChanged)
    Q_PROPERTY(qreal fontSmallPx READ fontSmallPx NOTIFY themeChanged)
    Q_PROPERTY(qreal fontTitlePx READ fontTitlePx NOTIFY themeChanged)
    Q_PROPERTY(qreal radiusPanel READ radiusPanel NOTIFY themeChanged)
    Q_PROPERTY(qreal radiusControl READ radiusControl NOTIFY themeChanged)
    Q_PROPERTY(qreal radiusSmall READ radiusSmall NOTIFY themeChanged) // check boxes, rows, menus, tooltips
    Q_PROPERTY(qreal spacingXs READ spacingXs NOTIFY themeChanged)
    Q_PROPERTY(qreal spacingS READ spacingS NOTIFY themeChanged)
    Q_PROPERTY(qreal spacingM READ spacingM NOTIFY themeChanged)
    Q_PROPERTY(qreal spacingL READ spacingL NOTIFY themeChanged)
    Q_PROPERTY(qreal borderWidth READ borderWidth NOTIFY themeChanged)
    Q_PROPERTY(qreal lineThinPx READ lineThinPx NOTIFY themeChanged)
    Q_PROPERTY(qreal blurRadius READ blurRadius NOTIFY themeChanged)

    // ---- derived sizes (override with the metric in brackets) ----
    Q_PROPERTY(qreal controlHeight READ controlHeight NOTIFY themeChanged)           // [size.control]
    Q_PROPERTY(qreal controlHeightSmall READ controlHeightSmall NOTIFY themeChanged) // [size.control.small]
    Q_PROPERTY(qreal rowHeight READ rowHeight NOTIFY themeChanged)                   // [size.row]
    Q_PROPERTY(qreal iconSize READ iconSize NOTIFY themeChanged)                     // [size.icon]
    Q_PROPERTY(qreal indicatorSize READ indicatorSize NOTIFY themeChanged)           // [size.indicator]
    Q_PROPERTY(qreal panelHeaderHeight READ panelHeaderHeight NOTIFY themeChanged)   // [size.panel.header]
    Q_PROPERTY(qreal topBarHeight READ topBarHeight NOTIFY themeChanged)             // [size.topbar]
    Q_PROPERTY(qreal statusBarHeight READ statusBarHeight NOTIFY themeChanged)       // [size.statusbar]
    Q_PROPERTY(qreal sidePanelWidth READ sidePanelWidth NOTIFY themeChanged)         // [size.sidepanel]
    Q_PROPERTY(qreal sidePanelMinWidth READ sidePanelMinWidth NOTIFY themeChanged)   // [size.sidepanel.min]
    Q_PROPERTY(qreal sidePanelMaxWidth READ sidePanelMaxWidth NOTIFY themeChanged)   // [size.sidepanel.max]
    Q_PROPERTY(qreal panelGap READ panelGap NOTIFY themeChanged)                     // [size.panel.gap]
    Q_PROPERTY(qreal focusRingWidth READ focusRingWidth NOTIFY themeChanged)         // [size.focus]
    Q_PROPERTY(int durationFast READ durationFast NOTIFY themeChanged)               // [motion.fast.ms]
    Q_PROPERTY(int toolTipDelay READ toolTipDelay NOTIFY themeChanged)               // [motion.tooltip.ms]

    // ---- fonts: family from the theme, or the system UI / fixed font when that family is not installed ----
    Q_PROPERTY(QString uiFontFamily READ uiFontFamily NOTIFY themeChanged)
    Q_PROPERTY(QString monoFontFamily READ monoFontFamily NOTIFY themeChanged)
    Q_PROPERTY(QFont uiFont READ uiFont NOTIFY themeChanged)         // font.ui.px
    Q_PROPERTY(QFont strongFont READ strongFont NOTIFY themeChanged) // font.ui.px, semi-bold (headers, tabs)
    Q_PROPERTY(QFont smallFont READ smallFont NOTIFY themeChanged)   // font.small.px
    Q_PROPERTY(QFont titleFont READ titleFont NOTIFY themeChanged)   // font.title.px, semi-bold
    Q_PROPERTY(QFont monoFont READ monoFont NOTIFY themeChanged)     // font.mono.px

public:
    // Loads every theme in `themeDirs` (empty: the themes compiled into the binary, ":/ray/themes").
    // Deliberately not default-constructible: QML must get the instance through create() (see provideFor).
    explicit ThemeObject(QStringList themeDirs, QObject* parent = nullptr);
    ~ThemeObject() override;

    // QML singleton factory: the instance registered for this engine (provideFor), or a new one owned by the engine.
    static ThemeObject* create(QQmlEngine* qml, QJSEngine* js);
    static void provideFor(QQmlEngine* engine, ThemeObject* theme);

    QString mode() const { return mode_; }
    bool setMode(const QString& mode); // false (and nothing changes) if the mode is unknown
    QString themeId() const;
    QString title() const;
    bool dark() const;
    bool systemDark() const { return systemDark_; }
    QStringList themeIds() const;
    QStringList tokenProperties() const;
    // Load problems, one per theme file that failed (empty when every file loaded).
    QStringList loadErrors() const { return loadErrors_; }
    bool valid() const { return current_ != nullptr; }

    // The theme in use, for C++ consumers (IViewport::setTheme). Never null once valid().
    std::shared_ptr<const render::Theme> current() const { return current_; }

    // The property name QML uses for a token ("panelBg" for Token::PanelBg).
    static QString propertyName(render::Token token);
    // sRGB colour of a token in the current theme.
    QColor color(render::Token token) const;

    Q_INVOKABLE QColor token(const QString& name) const; // by JSON key ("PanelBg") or property name ("panelBg")
    Q_INVOKABLE qreal metric(const QString& name, qreal fallback = 0) const;
    // Applies the OS scheme. Called by the QStyleHints connection; public so tests can simulate an OS switch.
    void setSystemDark(bool dark);

#define RAY_UI_TOKEN_GETTER(prop, tok)                                                                                 \
    QColor prop() const { return color(render::Token::tok); }
    RAY_UI_TOKEN_GETTER(windowBg, WindowBg)
    RAY_UI_TOKEN_GETTER(panelBg, PanelBg)
    RAY_UI_TOKEN_GETTER(panelBgGlassTint, PanelBgGlassTint)
    RAY_UI_TOKEN_GETTER(panelBorder, PanelBorder)
    RAY_UI_TOKEN_GETTER(panelHighlightTop, PanelHighlightTop)
    RAY_UI_TOKEN_GETTER(textPrimary, TextPrimary)
    RAY_UI_TOKEN_GETTER(textSecondary, TextSecondary)
    RAY_UI_TOKEN_GETTER(textDisabled, TextDisabled)
    RAY_UI_TOKEN_GETTER(accent, Accent)
    RAY_UI_TOKEN_GETTER(accentThinLine, AccentThinLine)
    RAY_UI_TOKEN_GETTER(accentText, AccentText)
    RAY_UI_TOKEN_GETTER(selection, Selection)
    RAY_UI_TOKEN_GETTER(selectionFill, SelectionFill)
    RAY_UI_TOKEN_GETTER(hover, Hover)
    RAY_UI_TOKEN_GETTER(error, Error)
    RAY_UI_TOKEN_GETTER(warning, Warning)
    RAY_UI_TOKEN_GETTER(success, Success)
    RAY_UI_TOKEN_GETTER(viewportBgTop, ViewportBgTop)
    RAY_UI_TOKEN_GETTER(viewportBgBottom, ViewportBgBottom)
    RAY_UI_TOKEN_GETTER(modelFace, ModelFace)
    RAY_UI_TOKEN_GETTER(modelEdge, ModelEdge)
    RAY_UI_TOKEN_GETTER(modelSilhouette, ModelSilhouette)
    RAY_UI_TOKEN_GETTER(fixtureFace, FixtureFace)
    RAY_UI_TOKEN_GETTER(stockFace, StockFace)
    RAY_UI_TOKEN_GETTER(stockCutFace, StockCutFace)
    RAY_UI_TOKEN_GETTER(gridMajor, GridMajor)
    RAY_UI_TOKEN_GETTER(gridMinor, GridMinor)
    RAY_UI_TOKEN_GETTER(axisX, AxisX)
    RAY_UI_TOKEN_GETTER(axisY, AxisY)
    RAY_UI_TOKEN_GETTER(axisZ, AxisZ)
    RAY_UI_TOKEN_GETTER(sketchGeometry, SketchGeometry)
    RAY_UI_TOKEN_GETTER(sketchConstruction, SketchConstruction)
    RAY_UI_TOKEN_GETTER(sketchConstrained, SketchConstrained)
    RAY_UI_TOKEN_GETTER(sketchConflict, SketchConflict)
    RAY_UI_TOKEN_GETTER(pathCut, PathCut)
    RAY_UI_TOKEN_GETTER(pathRapid, PathRapid)
    RAY_UI_TOKEN_GETTER(pathPlunge, PathPlunge)
    RAY_UI_TOKEN_GETTER(pathRamp, PathRamp)
    RAY_UI_TOKEN_GETTER(pathLead, PathLead)
    RAY_UI_TOKEN_GETTER(pathLink, PathLink)
    RAY_UI_TOKEN_GETTER(pathSelected, PathSelected)
    RAY_UI_TOKEN_GETTER(pathStale, PathStale)
    RAY_UI_TOKEN_GETTER(simGouge, SimGouge)
    RAY_UI_TOKEN_GETTER(simLeftover, SimLeftover)
    RAY_UI_TOKEN_GETTER(simCollision, SimCollision)
    RAY_UI_TOKEN_GETTER(draftPositive, DraftPositive)
    RAY_UI_TOKEN_GETTER(draftNegative, DraftNegative)
    RAY_UI_TOKEN_GETTER(draftVertical, DraftVertical)
    RAY_UI_TOKEN_GETTER(gcodeText, GcodeText)
    RAY_UI_TOKEN_GETTER(gcodeComment, GcodeComment)
    RAY_UI_TOKEN_GETTER(gcodeGWord, GcodeGWord)
    RAY_UI_TOKEN_GETTER(gcodeMWord, GcodeMWord)
    RAY_UI_TOKEN_GETTER(gcodeAxis, GcodeAxis)
    RAY_UI_TOKEN_GETTER(gcodeFeedSpeed, GcodeFeedSpeed)
    RAY_UI_TOKEN_GETTER(gcodeLineNumber, GcodeLineNumber)
    RAY_UI_TOKEN_GETTER(gcodeCurrentLine, GcodeCurrentLine)
#undef RAY_UI_TOKEN_GETTER

    QColor controlBg() const { return derived_.controlBg; }
    QColor controlBgHover() const { return derived_.controlBgHover; }
    QColor controlBgPressed() const { return derived_.controlBgPressed; }
    QColor controlRaised() const { return derived_.controlRaised; }
    QColor controlBorder() const { return derived_.controlBorder; }
    QColor fieldBg() const { return derived_.fieldBg; }
    QColor accentHover() const { return derived_.accentHover; }
    QColor accentPressed() const { return derived_.accentPressed; }
    QColor accentFill() const { return derived_.accentFill; }
    QColor onAccent() const { return derived_.onAccent; }
    QColor onAccentLine() const { return derived_.onAccentLine; }
    QColor focusRing() const { return derived_.focusRing; }
    QColor tooltipBg() const { return derived_.tooltipBg; }
    QColor scrollThumb() const { return derived_.scrollThumb; }
    QColor scrollThumbHover() const { return derived_.scrollThumbHover; }

    qreal fontUiPx() const { return metric(QStringLiteral("font.ui.px"), 13); }
    qreal fontMonoPx() const { return metric(QStringLiteral("font.mono.px"), fontUiPx()); }
    qreal fontSmallPx() const { return metric(QStringLiteral("font.small.px"), fontUiPx()); }
    qreal fontTitlePx() const { return metric(QStringLiteral("font.title.px"), fontUiPx()); }
    qreal radiusPanel() const { return metric(QStringLiteral("radius.panel"), 0); }
    qreal radiusControl() const { return metric(QStringLiteral("radius.control"), 0); }
    qreal radiusSmall() const;
    qreal spacingXs() const { return metric(QStringLiteral("spacing.xs"), 4); }
    qreal spacingS() const { return metric(QStringLiteral("spacing.s"), 8); }
    qreal spacingM() const { return metric(QStringLiteral("spacing.m"), 12); }
    qreal spacingL() const { return metric(QStringLiteral("spacing.l"), 16); }
    qreal borderWidth() const { return metric(QStringLiteral("border.width"), 1); }
    qreal lineThinPx() const { return metric(QStringLiteral("line.thin.px"), 1); }
    qreal blurRadius() const { return metric(QStringLiteral("blur.radius"), 0); }

    qreal controlHeight() const;
    qreal controlHeightSmall() const;
    qreal rowHeight() const;
    qreal iconSize() const;
    qreal indicatorSize() const;
    qreal panelHeaderHeight() const;
    qreal topBarHeight() const;
    qreal statusBarHeight() const;
    qreal sidePanelWidth() const;
    qreal sidePanelMinWidth() const;
    qreal sidePanelMaxWidth() const;
    qreal panelGap() const;
    qreal focusRingWidth() const;
    int durationFast() const;
    int toolTipDelay() const;

    QString uiFontFamily() const { return uiFamily_; }
    QString monoFontFamily() const { return monoFamily_; }
    QFont uiFont() const;
    QFont strongFont() const;
    QFont smallFont() const;
    QFont titleFont() const;
    QFont monoFont() const;

signals:
    void modeChanged();
    void themeChanged();
    void systemDarkChanged();

private:
    struct Derived {
        QColor controlBg, controlBgHover, controlBgPressed, controlRaised, controlBorder, fieldBg;
        QColor accentHover, accentPressed, accentFill, onAccent, onAccentLine, focusRing, tooltipBg;
        QColor scrollThumb, scrollThumbHover;
    };

    void setModeProperty(const QString& mode) { setMode(mode); }
    std::shared_ptr<const render::Theme> resolve(const QString& mode) const;
    void apply(std::shared_ptr<const render::Theme> theme);
    void computeDerived();
    void resolveFonts();
    qreal sizeMetric(const char* name, qreal derived) const;

    std::vector<std::shared_ptr<const render::Theme>> themes_;
    std::shared_ptr<const render::Theme> current_;
    QStringList loadErrors_;
    QString mode_ = QStringLiteral("system");
    bool systemDark_ = false;
    Derived derived_;
    QString uiFamily_, monoFamily_;
};

} // namespace ray::ui
