// Shell, Theme binding and base controls (task T-018), on real Qt Quick windows.
//
//   - every render::Token is a Theme property holding the theme file's colour, in both shipped themes
//   - the theme follows the OS scheme, a manual choice overrides it, and token-bound colours change in a grabbed frame
//   - glass and flat panels have identical geometry (every item's scene rectangle), only the background swaps
//   - only the first screen is created at start (menus, tooltips and other workspaces load on first use)
//   - the controls gallery at the current scale (QT_SCALE_FACTOR; CTest runs it at 1, 1.5 and 2): no control is
//     smaller than its implicit size and no text is elided or clipped
//   - no QML warnings anywhere
#include "app_context.h"
#include "app_host.h"
#include "app_options.h"
#include "theme_object.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTemporaryDir>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>
#include <QtQml/QQmlApplicationEngine>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>
#include <QtTest/QTest>

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <string>

using ray::ui::AppContext;
using ray::ui::AppHost;
using ray::ui::AppOptions;
using ray::ui::ThemeObject;

namespace {

// One application window per test, with its own cache folder.
struct Window {
    QTemporaryDir cache;
    std::unique_ptr<AppHost> host;

    explicit Window(AppOptions options)
    {
        if (!options.size.isValid()) {
            options.size = QSize(1440, 900);
        }
        host = std::make_unique<AppHost>(options, cache.path());
        REQUIRE(host->optionError().isEmpty());
        REQUIRE(host->load());
        host->show();
        REQUIRE(QTest::qWaitForWindowExposed(host->window()));
        grab(); // polishes positioners and renders once
    }

    QQuickWindow* window() const { return host->window(); }
    ThemeObject* theme() const { return host->theme(); }
    AppContext* context() const { return host->context(); }
    QImage grab() const { return host->window()->grabWindow(); }

    // Searches the visual item tree (items a Loader creates are not QObject children of the window's content).
    QQuickItem* find(const char* objectName) const { return findItem(window()->contentItem(), objectName); }

    static QQuickItem* findItem(QQuickItem* item, const char* objectName)
    {
        if (item->objectName() == QLatin1String(objectName)) {
            return item;
        }
        for (QQuickItem* child : item->childItems()) {
            if (QQuickItem* found = findItem(child, objectName)) {
                return found;
            }
        }
        return nullptr;
    }

    // Any object (popups are QObjects, not items) that is a child of an item in the visual tree.
    QObject* findObject(const char* objectName) const { return findObjectIn(window()->contentItem(), objectName); }

    static QObject* findObjectIn(QQuickItem* item, const char* objectName)
    {
        if (QObject* found = item->findChild<QObject*>(QLatin1String(objectName), Qt::FindDirectChildrenOnly)) {
            return found;
        }
        for (QQuickItem* child : item->childItems()) {
            if (QObject* found = findObjectIn(child, objectName)) {
                return found;
            }
        }
        return nullptr;
    }
};

AppOptions options(const char* theme = "dark", bool glass = false, bool gallery = false)
{
    AppOptions o;
    o.theme = QString::fromLatin1(theme);
    o.glass = glass;
    o.gallery = gallery;
    return o;
}

// Device-pixel colour of the grabbed frame at a point in scene (logical) coordinates.
QColor pixelAt(const QImage& image, QPointF scenePoint)
{
    const qreal dpr = image.devicePixelRatio();
    return image.pixelColor(static_cast<int>(std::floor(scenePoint.x() * dpr)),
                            static_cast<int>(std::floor(scenePoint.y() * dpr)));
}

bool near(const QColor& a, const QColor& b, int tolerance = 2)
{
    return std::abs(a.red() - b.red()) <= tolerance && std::abs(a.green() - b.green()) <= tolerance &&
           std::abs(a.blue() - b.blue()) <= tolerance;
}

// A point inside a panel's background, clear of its header, text and border: just inside the lower-left corner.
QPointF panelBackgroundPoint(QQuickItem* panel, const ThemeObject& theme)
{
    const QRectF r = panel->mapRectToScene(QRectF(0, 0, panel->width(), panel->height()));
    const qreal inset = theme.radiusPanel() + theme.spacingS();
    return {r.left() + inset, r.bottom() - inset};
}

// Every item below `root`, keyed by its path of child indices (and object names, for readable failures).
void collect(QQuickItem* item, const std::string& path, std::map<std::string, QRectF>& out)
{
    const QString name = item->objectName();
    const std::string key = path + (name.isEmpty() ? std::string() : "(" + name.toStdString() + ")");
    out[key] = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
    const QList<QQuickItem*> children = item->childItems();
    for (qsizetype i = 0; i < children.size(); ++i) {
        collect(children[i], key + "/" + std::to_string(i), out);
    }
}

bool effectivelyVisible(const QQuickItem* item)
{
    return item->isVisible() && item->opacity() > 0;
}

void walk(QQuickItem* item, const std::function<void(QQuickItem*)>& fn)
{
    fn(item);
    for (QQuickItem* child : item->childItems()) {
        walk(child, fn);
    }
}

std::string describe(const QQuickItem* item)
{
    std::string s = item->metaObject()->className();
    if (!item->objectName().isEmpty()) {
        s += " '" + item->objectName().toStdString() + "'";
    }
    const QVariant text = item->property("text");
    if (text.isValid()) {
        s += " text='" + text.toString().toStdString() + "'";
    }
    for (const QQuickItem* p = item->parentItem(); p != nullptr; p = p->parentItem()) {
        if (!p->objectName().isEmpty()) {
            s += " in '" + p->objectName().toStdString() + "'";
            break;
        }
    }
    return s;
}

} // namespace

TEST_CASE("every theme token is a Theme property with the theme file's colour", "[theme]")
{
    ThemeObject theme{QStringList()};
    REQUIRE(theme.loadErrors().isEmpty());
    REQUIRE(theme.themeIds().contains(QStringLiteral("graphite-dark")));
    REQUIRE(theme.themeIds().contains(QStringLiteral("graphite-light")));

    for (const char* id : {"graphite-dark", "graphite-light"}) {
        INFO("theme " << id);
        REQUIRE(theme.setMode(QString::fromLatin1(id)));
        CHECK(theme.themeId() == QString::fromLatin1(id));

        QFile file(QStringLiteral(RAY_THEMES_DIR "/%1.json").arg(QString::fromLatin1(id)));
        REQUIRE(file.open(QIODevice::ReadOnly));
        const QJsonObject colours = QJsonDocument::fromJson(file.readAll()).object().value("colors").toObject();
        const QStringList properties = theme.tokenProperties();
        REQUIRE(properties.size() == static_cast<qsizetype>(ray::render::Token::Count));
        for (std::size_t i = 0; i < static_cast<std::size_t>(ray::render::Token::Count); ++i) {
            const auto token = static_cast<ray::render::Token>(i);
            const std::string_view key = ray::render::tokenName(token);
            const QString hex = colours.value(QLatin1String(key.data(), static_cast<qsizetype>(key.size()))).toString();
            INFO("token " << std::string(key) << " = " << hex.toStdString());
            // "#RRGGBBAA" in the file; QColor reads "#AARRGGBB".
            const QColor expected =
                hex.size() == 9 ? QColor::fromString(QStringLiteral("#") + hex.mid(7, 2) + hex.mid(1, 6)) : QColor(hex);
            REQUIRE(expected.isValid());
            const QVariant value = theme.property(properties[static_cast<qsizetype>(i)].toLatin1().constData());
            REQUIRE(value.isValid());
            CHECK(value.value<QColor>().rgba() == expected.rgba());
            CHECK(theme.token(QString::fromLatin1(key.data(), static_cast<qsizetype>(key.size()))).rgba() ==
                  expected.rgba());
        }
        CHECK(theme.uiFont().pixelSize() == 13);
        CHECK(theme.monoFont().pixelSize() == 13);
        CHECK(theme.metric(QStringLiteral("radius.panel")) == 12);
        CHECK(theme.controlHeight() > theme.fontUiPx());
        CHECK(theme.current() != nullptr);
    }
    CHECK_FALSE(theme.setMode(QStringLiteral("no-such-theme")));
    CHECK(theme.themeId() == QStringLiteral("graphite-light"));
}

TEST_CASE("the theme follows the OS, a manual choice overrides it, and colours change live", "[theme][shell]")
{
    Window w(options("system"));
    ThemeObject& theme = *w.theme();
    QQuickItem* panel = w.find("leftPanel");
    QQuickItem* status = w.find("statusLook");
    REQUIRE(panel != nullptr);
    REQUIRE(status != nullptr);
    const QPointF sample = panelBackgroundPoint(panel, theme);

    theme.setSystemDark(true);
    CHECK(theme.dark());
    QImage frame = w.grab();
    const QColor darkPanel = theme.panelBg();
    CHECK(near(pixelAt(frame, sample), darkPanel));
    const QColor darkViewport = pixelAt(frame, w.find("viewportArea")->mapToScene(QPointF(4, 4)));
    CHECK(status->property("text").toString().startsWith(theme.title()));

    // The OS switches to light: the frame follows with no restart.
    theme.setSystemDark(false);
    CHECK_FALSE(theme.dark());
    frame = w.grab();
    const QColor lightPanel = theme.panelBg();
    CHECK(lightPanel != darkPanel);
    CHECK(near(pixelAt(frame, sample), lightPanel));
    CHECK_FALSE(near(pixelAt(frame, w.find("viewportArea")->mapToScene(QPointF(4, 4))), darkViewport, 8));
    CHECK(status->property("text").toString().startsWith(theme.title()));

    // A manual choice wins over the OS until "system" is chosen again.
    REQUIRE(theme.setMode(QStringLiteral("dark")));
    theme.setSystemDark(true);
    theme.setSystemDark(false);
    CHECK(theme.dark());
    CHECK(near(pixelAt(w.grab(), sample), darkPanel));
    REQUIRE(theme.setMode(QStringLiteral("system")));
    CHECK_FALSE(theme.dark());
    CHECK(near(pixelAt(w.grab(), sample), lightPanel));

    // The theme toggle in the top bar switches too.
    QQuickItem* toggle = w.find("themeToggle");
    REQUIRE(toggle != nullptr);
    REQUIRE(QMetaObject::invokeMethod(toggle, "clicked"));
    CHECK(theme.dark());
    CHECK(theme.mode() == QStringLiteral("dark"));
    CHECK(w.host->warnings().isEmpty());
}

TEST_CASE("glass and flat panels have identical layout; only the background swaps", "[glass][shell]")
{
    for (const char* themeName : {"dark", "light"}) {
        INFO("theme " << themeName);
        Window w(options(themeName, false));
        std::map<std::string, QRectF> flat;
        collect(w.window()->contentItem(), "", flat);
        QQuickItem* panel = w.find("rightPanel");
        REQUIRE(panel != nullptr);
        auto* flatBg = Window::findItem(panel, "panelFlatBackground");
        auto* glassBg = Window::findItem(panel, "panelGlassBackground");
        REQUIRE(flatBg != nullptr);
        REQUIRE(glassBg != nullptr);
        CHECK(flatBg->isVisible());
        CHECK_FALSE(glassBg->isVisible());

        w.context()->setGlass(true);
        w.grab();
        std::map<std::string, QRectF> glass;
        collect(w.window()->contentItem(), "", glass);
        CHECK_FALSE(flatBg->isVisible());
        CHECK(glassBg->isVisible());

        REQUIRE(glass.size() == flat.size());
        int compared = 0;
        for (const auto& [key, rect] : flat) {
            INFO("item " << key);
            REQUIRE(glass.count(key) == 1);
            CHECK(glass[key] == rect);
            ++compared;
        }
        CHECK(compared > 100);

        // Flat backgrounds are opaque PanelBg; the glass fallback is the translucent tint.
        CHECK(flatBg->property("color").value<QColor>() == w.theme()->panelBg());
        CHECK(w.theme()->panelBgGlassTint().alpha() < 255);
        CHECK(w.host->warnings().isEmpty());
    }
}

TEST_CASE("only the first screen is created at start", "[lazy][shell]")
{
    Window w(options("dark"));
    auto* root = w.window()->contentItem();
    // Menus and tooltips are not created until used.
    CHECK(w.findObject("viewOptionsMenu") == nullptr);
    CHECK(Window::findItem(root, "toolTipText") == nullptr);
    // Only the current workspace exists.
    auto* design = w.find("workspace.0");
    auto* cam = w.find("workspace.1");
    REQUIRE(design != nullptr);
    REQUIRE(cam != nullptr);
    CHECK(design->property("item").value<QObject*>() != nullptr);
    CHECK(cam->property("item").value<QObject*>() == nullptr);
    w.context()->setWorkspace(1);
    CHECK(cam->property("item").value<QObject*>() != nullptr);
    // The gallery is not part of the shell.
    CHECK(w.find("gallery") == nullptr);

    // Opening the view options menu creates it.
    QQuickItem* shell = w.find("shell");
    REQUIRE(shell != nullptr);
    REQUIRE(QMetaObject::invokeMethod(shell, "openViewMenu"));
    CHECK(w.findObject("viewOptionsMenu") != nullptr);
    CHECK(w.host->warnings().isEmpty());
}

TEST_CASE("the shell runs without an application session", "[session][shell]")
{
    Window w(options("light"));
    AppContext& context = *w.context();
    CHECK_FALSE(context.hasSession());
    CHECK_FALSE(context.invoke(QStringLiteral("file.open")));
    QQuickItem* status = w.find("statusText");
    REQUIRE(status != nullptr);
    CHECK(status->property("text").toString() == QStringLiteral("No session"));
    CHECK_FALSE(w.find("openButton")->isEnabled());

    // The job slot appears only while a job runs.
    QQuickItem* jobSlot = w.find("jobSlot");
    REQUIRE(jobSlot != nullptr);
    CHECK_FALSE(jobSlot->isVisible());
    context.setJob(QStringLiteral("Importing"), 0.25);
    CHECK(jobSlot->isVisible());
    bool cancelled = false;
    QObject::connect(&context, &AppContext::jobCancelRequested, [&] { cancelled = true; });
    REQUIRE(QMetaObject::invokeMethod(w.find("jobCancel"), "clicked"));
    CHECK(cancelled);
    context.clearJob();
    CHECK_FALSE(jobSlot->isVisible());

    // Panels hide and show; the viewport area grows to fill the space.
    QQuickItem* area = w.find("viewportArea");
    const qreal before = area->width();
    w.find("shell")->setProperty("leftPanelOpen", false);
    w.grab();
    CHECK(area->width() > before);
    CHECK(w.host->warnings().isEmpty());
}

TEST_CASE("the shell is usable from the keyboard", "[keyboard][shell]")
{
    Window w(options("dark"));
    QQuickWindow* window = w.window();
    window->requestActivate();
    // Keyboard events are sent to the window directly, so the test does not depend on the OS granting focus.
    (void)QTest::qWaitForWindowActive(window, 2000);

    // Tab walks through the top bar; every stop has an accessible name.
    QStringList stops;
    for (int i = 0; i < 24; ++i) {
        QTest::keyClick(window, Qt::Key_Tab);
        QQuickItem* focused = window->activeFocusItem();
        REQUIRE(focused != nullptr);
        // Text fields take focus themselves; buttons are the focus item.
        QQuickItem* named = focused;
        while (named != nullptr && named->objectName().isEmpty()) {
            named = named->parentItem();
        }
        if (named != nullptr && !stops.contains(named->objectName())) {
            stops << named->objectName();
        }
    }
    INFO("focus stops: " << stops.join(QStringLiteral(", ")).toStdString());
    for (const char* expected : {"workspaceTab.0", "commandSearch", "toggleLeftPanel", "themeToggle", "glassToggle",
                                 "viewOptions", "leftPanelClose", "leftPanelSplitter"}) {
        CHECK(stops.contains(QLatin1String(expected)));
    }

    // A focused tool button shows its focus ring, and Space presses it.
    QQuickItem* toggle = w.find("themeToggle");
    toggle->forceActiveFocus(Qt::TabFocusReason);
    CHECK(toggle->property("visualFocus").toBool());
    const bool wasDark = w.theme()->dark();
    QTest::keyClick(window, Qt::Key_Space);
    CHECK(w.theme()->dark() != wasDark);

    // Arrow keys move between workspace tabs.
    QQuickItem* tab = w.find("workspaceTab.0");
    tab->forceActiveFocus(Qt::TabFocusReason);
    QTest::keyClick(window, Qt::Key_Right);
    CHECK(w.context()->workspace() == 1);
    QTest::keyClick(window, Qt::Key_Left);
    CHECK(w.context()->workspace() == 0);

    // The splitter resizes its panel from the keyboard.
    QQuickItem* panel = w.find("leftPanel");
    const qreal width = panel->width();
    w.find("leftPanelSplitter")->forceActiveFocus(Qt::TabFocusReason);
    QTest::keyClick(window, Qt::Key_Right);
    CHECK(panel->width() > width);
    CHECK(w.host->warnings().isEmpty());
}

TEST_CASE("command-line options", "[options]")
{
    auto parse = [](QStringList args) {
        args.prepend(QStringLiteral("raycad"));
        return ray::ui::parseOptions(args);
    };
    const AppOptions a = parse({"--theme", "light", "--glass", "on", "--gallery", "--size", "800x600"});
    CHECK(a.error.isEmpty());
    CHECK(a.theme == QStringLiteral("light"));
    CHECK(a.glass == true);
    CHECK(a.gallery);
    CHECK(a.size == QSize(800, 600));
    CHECK(parse({"--glass", "off"}).glass == false);
    CHECK(parse({"--bench-start"}).benchStart);
    CHECK(parse({"--bench-startup"}).benchStart);
    CHECK(parse({"--smoke", "--screenshot", "x.png"}).screenshot == QStringLiteral("x.png"));
    CHECK_FALSE(parse({"--glass", "maybe"}).error.isEmpty());
    CHECK_FALSE(parse({"--theme"}).error.isEmpty());
    CHECK_FALSE(parse({"--size", "10x10"}).error.isEmpty());
    CHECK_FALSE(parse({"--frobnicate"}).error.isEmpty());

    QTemporaryDir cache;
    AppOptions bad;
    bad.theme = QStringLiteral("no-such-theme");
    AppHost host(bad, cache.path());
    CHECK_FALSE(host.optionError().isEmpty());
}

TEST_CASE("the controls gallery: nothing clipped or elided at this scale", "[scale]")
{
    const qreal expectedScale = qEnvironmentVariableIsSet("RAY_TEST_SCALE")
                                    ? QString::fromLocal8Bit(qgetenv("RAY_TEST_SCALE")).toDouble()
                                    : 1.0;
    for (const char* themeName : {"dark", "light"}) {
        for (const bool glass : {false, true}) {
            INFO("theme " << themeName << (glass ? ", glass" : ", flat"));
            Window w(options(themeName, glass, true));
            QQuickItem* gallery = w.find("gallery");
            REQUIRE(gallery != nullptr);
            REQUIRE(QMetaObject::invokeMethod(gallery, "openPopups"));
            QTest::qWait(50);
            const QImage frame = w.grab();
            const qreal dpr = w.window()->effectiveDevicePixelRatio();
            INFO("device pixel ratio " << dpr);
            if (qEnvironmentVariable("QT_ENABLE_HIGHDPI_SCALING") == QLatin1String("0")) {
                CHECK(std::abs(dpr - expectedScale) < 0.01);
            } else {
                CHECK(dpr >= expectedScale - 0.01);
            }
            CHECK(frame.width() == static_cast<int>(std::lround(w.window()->width() * dpr)));
            CHECK(frame.height() == static_cast<int>(std::lround(w.window()->height() * dpr)));

            int controls = 0;
            int texts = 0;
            walk(w.window()->contentItem(), [&](QQuickItem* item) {
                if (!effectivelyVisible(item)) {
                    return;
                }
                if (item->objectName().startsWith(QStringLiteral("gallery."))) {
                    ++controls;
                    INFO(describe(item) << " size " << item->width() << "x" << item->height() << ", implicit "
                                        << item->implicitWidth() << "x" << item->implicitHeight());
                    CHECK(item->width() + 0.5 >= item->implicitWidth());
                    CHECK(item->height() + 0.5 >= item->implicitHeight());
                }
                if (item->inherits("QQuickText") && !item->property("text").toString().isEmpty()) {
                    ++texts;
                    INFO(describe(item));
                    CHECK_FALSE(item->property("truncated").toBool());
                    // A single line must fit its height (no vertical clipping).
                    CHECK(item->property("contentHeight").toReal() <= item->height() + 1.0);
                }
            });
            CHECK(controls >= 40);
            CHECK(texts >= 60);
            // The popups were measured too.
            CHECK(w.findObject("gallery.menu") != nullptr);
            CHECK(w.host->warnings().isEmpty());
            for (const QString& warning : w.host->warnings()) {
                UNSCOPED_INFO(warning.toStdString());
            }
        }
    }
}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    // Theme follows no OS setting here unless a test asks for "system".
    return Catch::Session().run(argc, argv);
}
