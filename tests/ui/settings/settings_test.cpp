// Settings, units and tier override (task T-024), on real Qt Quick windows with the real application session:
//
//   - theme, glass, navigation preset, panel state and window geometry are saved and restored on the next start
//   - a saved window geometry is clamped to the screens there are
//   - a corrupt settings file falls back to the defaults with a message; the session works; the bad file is kept
//   - new documents default to inch; changing the default changes the next new document only
//   - the selection read-out switches units live (the quick toggle) and follows the decimals setting
//   - the measure panel (T-022), its labels over the view and the selection read-out show one unit format and switch
//     together from the status bar, the Settings panel, the measure panel's buttons and MeasureTool.setUnits
//   - changing the tier and a per-option override applies to the viewport without a restart (sample count, smooth
//     edges, mesh tolerance)
//   - the settings panel opens from Ctrl+, the view options menu and the palette; it shows the detected hardware and
//     the reason for the chosen tier; detection never runs before the first frame and never on the UI thread
//   - explainTier agrees with chooseTier
//   - warm start to the first frame with a populated settings file, against PLAN.md section 9
//
// Screenshots: set RAY_T024_SHOTS to a folder and run with "[screenshots]".
#include "../scene_graph_probe.h"
#include "app_context.h"
#include "app_host.h"
#include "app_options.h"
#include "files.h"
#include "measure_model.h"
#include "palette_commands.h"
#include "scene_controller.h"
#include "selection_model.h"
#include "session.h"
#include "settings.h"
#include "theme_object.h"
#include "units.h"
#include "viewport_item.h"
#include "viewport_navigation.h"

#include <ray/api.h>
#include <ray_api/settings.h>
#include <ray_api/viewer.h>

#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QProcess>
#include <QtCore/QRegularExpression>
#include <QtCore/QTemporaryDir>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>
#include <QtGui/QScreen>
#include <QtQml/QQmlApplicationEngine>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>
#include <QtQuick/QSGRendererInterface>
#include <QtTest/QTest>

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace ray;
using ray::ui::AppHost;

namespace {

const QString kBoxHole = QStringLiteral(RAY_KERNEL_TEST_DATA_DIR "/box_hole.step");
// plate_thru_holes_1 (tools/corpus_gen): a 120 x 80 x 10 mm plate, 6.6 mm holes at x = 30, 60, 90, y = 80/3, 160/3.
const QString kPlate = QStringLiteral(RAY_CORPUS_DIR "/raycad-synthetic/plate_thru_holes_1.stp");
constexpr int kOpenTimeoutMs = 180000;

using ray::uitest::rhiSceneGraph;

// One application window that loads its settings from `userData` the way runApp does (loadSettings before load).
struct Window {
    QTemporaryDir cache;
    std::unique_ptr<AppHost> host;

    explicit Window(const QString& userData, ui::AppOptions options = {}, bool session = true)
    {
        host = std::make_unique<AppHost>(options, cache.path());
        REQUIRE(host->optionError().isEmpty());
        host->loadSettings(userData.toStdString());
        REQUIRE(host->load());
        host->files()->setUseNativeDialogs(false);
        host->show();
        REQUIRE(QTest::qWaitForWindowExposed(window()));
        REQUIRE(host->viewport() != nullptr);
        host->viewport()->setAnimationsEnabled(false);
        grab(); // the first frame: runApp starts the session only after it
        if (session)
            start(userData);
    }

    void start(const QString& userData) { REQUIRE(host->startSession(userData.toStdString())); }

    QQuickWindow* window() const { return host->window(); }
    ui::SettingsObject* settings() const { return host->settings(); }
    ui::UnitsObject* units() const { return host->units(); }
    ui::AppContext* context() const { return host->context(); }
    ui::Session* session() const { return host->session(); }
    api::IApp* app() const { return host->app(); }
    QImage grab() const { return host->window()->grabWindow(); }

    QQuickItem* find(const QString& objectName) const { return findItem(window()->contentItem(), objectName); }
    static QQuickItem* findItem(QQuickItem* item, const QString& objectName)
    {
        if (item->objectName() == objectName)
            return item;
        for (QQuickItem* child : item->childItems()) {
            if (QQuickItem* found = findItem(child, objectName))
                return found;
        }
        return nullptr;
    }
    // `hiddenToo`: also items in a part of the window that is not shown (the right panel's other tab).
    std::vector<QQuickItem*> findAll(const QString& objectName, bool hiddenToo = false) const
    {
        std::vector<QQuickItem*> out;
        collect(window()->contentItem(), objectName, hiddenToo, out);
        return out;
    }
    static void collect(QQuickItem* item, const QString& objectName, bool hiddenToo, std::vector<QQuickItem*>& out)
    {
        if (item->objectName() == objectName && (hiddenToo || item->isVisible()))
            out.push_back(item);
        for (QQuickItem* child : item->childItems())
            collect(child, objectName, hiddenToo, out);
    }
    QObject* findObject(const QString& objectName) const { return findObjectIn(window()->contentItem(), objectName); }
    static QObject* findObjectIn(QQuickItem* item, const QString& objectName)
    {
        if (QObject* found = item->findChild<QObject*>(objectName, Qt::FindDirectChildrenOnly))
            return found;
        for (QQuickItem* child : item->childItems()) {
            if (QObject* found = findObjectIn(child, objectName))
                return found;
        }
        return nullptr;
    }
    QString text(const QString& objectName) const
    {
        QQuickItem* item = find(objectName);
        return item ? item->property("text").toString() : QString();
    }

    void checkNoWarnings() const
    {
        for (const QString& warning : host->warnings())
            UNSCOPED_INFO(warning.toStdString());
        CHECK(host->warnings().isEmpty());
    }

    bool settled() const
    {
        if (host->files()->busy() || session()->jobCount() > 0 || host->viewport()->animating())
            return false;
        api::OpenDocument* od = app() ? app()->active() : nullptr;
        if (od != nullptr && od->document->rebuildPending())
            return false;
        return host->scene()->bodyCount() == 0 || host->scene()->ready();
    }
    void open(const QString& path) const
    {
        REQUIRE(host->files()->open(path));
        const bool ok = QTest::qWaitFor(
            [&] { return (settled() && session()->hasDocument()) || context()->messageIsError(); }, kOpenTimeoutMs);
        INFO("busy " << host->files()->busy() << " jobs " << session()->jobCount() << " job "
                     << session()->jobTitle().toStdString() << " animating " << host->viewport()->animating()
                     << " bodies " << host->scene()->bodyCount() << " ready " << host->scene()->ready() << " refining "
                     << host->scene()->refining() << " message " << context()->message().toStdString());
        REQUIRE(ok);
        REQUIRE_FALSE(context()->messageIsError());
        grab();
    }

    // ---- the settings panel ----
    QObject* panelPopup() const { return findObject(QStringLiteral("settingsPanel")); }
    bool panelOpen() const
    {
        QObject* popup = panelPopup();
        return popup != nullptr && popup->property("opened").toBool();
    }
    QObject* panelRoot() const
    {
        QQuickItem* loader = find(QStringLiteral("settingsLoader"));
        return loader ? loader->property("item").value<QObject*>() : nullptr;
    }
    void showSection(const char* section, bool wait = true) const
    {
        QObject* root = panelRoot();
        REQUIRE(root != nullptr);
        REQUIRE(
            QMetaObject::invokeMethod(root, "showSection", Q_ARG(QVariant, QVariant(QString::fromLatin1(section)))));
        if (wait)
            QTest::qWait(30);
    }
    void openPanel(const char* section = "general") const
    {
        settings()->open();
        REQUIRE(QTest::qWaitFor([&] { return panelOpen(); }, 3000));
        showSection(section);
    }
    void closePanel() const
    {
        if (QObject* popup = panelPopup())
            QMetaObject::invokeMethod(popup, "close");
        REQUIRE(QTest::qWaitFor([&] { return !panelOpen(); }, 3000));
    }

    // ---- measuring (T-022) ----
    ui::MeasureModel* measure() const
    {
        auto* m = host->engine()->singletonInstance<ui::MeasureModel*>(QStringLiteral("RayCad.Ui"),
                                                                       QStringLiteral("MeasureTool"));
        REQUIRE(m != nullptr);
        return m;
    }
    int rightTab() const { return find(QStringLiteral("shell"))->property("rightTab").toInt(); }
    QString panelValue(const QString& key) const
    {
        QQuickItem* row = find(QStringLiteral("measureValue.") + key);
        QQuickItem* text = row ? findItem(row, QStringLiteral("measureValueText")) : nullptr;
        return text != nullptr ? text->property("text").toString() : QString();
    }
    QStringList labelTexts() const
    {
        QStringList out;
        for (const QVariant& l : measure()->labels())
            out << l.toMap().value(QStringLiteral("text")).toString();
        return out;
    }
    // The cylindrical face of body 0 whose axis passes through (x, y).
    std::uint32_t holeAt(double x, double y) const
    {
        const api::viewer::Body* body = host->scene()->body(0);
        REQUIRE(body != nullptr);
        JobContext ctx;
        std::optional<std::uint32_t> found;
        for (std::uint32_t i = 0; i < body->shape.count(kernel::SubKind::Face) && !found; ++i) {
            auto info = api::viewer::describeElement(body->shape, kernel::SubId{kernel::SubKind::Face, i}, ctx);
            REQUIRE(info);
            if (info->surface && info->surface->type == kernel::SurfaceType::Cylinder &&
                std::hypot(info->surface->centroid.x - x, info->surface->centroid.y - y) < 1e-6)
                found = i;
        }
        if (!found)
            FAIL("no hole at " << x << ", " << y);
        return *found;
    }
    void selectFaces(std::uint32_t a, std::uint32_t b) const
    {
        const render::ItemId item = host->scene()->itemForBody(0);
        host->selection()->applyClick(render::PickHit{item, kernel::SubKind::Face, a, Vec3{}, 0}, Qt::NoModifier);
        host->selection()->applyClick(render::PickHit{item, kernel::SubKind::Face, b, Vec3{}, 0}, Qt::ControlModifier);
        REQUIRE(host->selection()->count() == 2);
    }

    void shot(const QString& name) const
    {
        const QString dir = qEnvironmentVariable("RAY_T024_SHOTS");
        if (dir.isEmpty())
            return;
        QDir().mkpath(dir);
        QTest::qWait(150);
        grab().save(QDir(dir).filePath(name + QStringLiteral(".png")), "PNG");
    }
};

QString readFile(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

bool near(const QRect& a, const QRect& b, int tolerance = 2)
{
    return std::abs(a.x() - b.x()) <= tolerance && std::abs(a.y() - b.y()) <= tolerance &&
           std::abs(a.width() - b.width()) <= tolerance && std::abs(a.height() - b.height()) <= tolerance;
}

QStringList readOutValues(const Window& w, bool hiddenToo = false)
{
    QStringList out;
    for (QQuickItem* item : w.findAll(QStringLiteral("selectionInfoValue"), hiddenToo))
        out << item->property("text").toString();
    return out;
}

} // namespace

TEST_CASE("theme, glass, navigation, panels and window geometry are restored on the next start", "[ui][settings]")
{
    QTemporaryDir user;
    const QString navigationId = QString::fromLatin1(ui::nav::presets().at(2).id);
    // Fits on any screen this runs on, away from the edges.
    const QScreen* screen = QGuiApplication::primaryScreen();
    REQUIRE(screen != nullptr);
    const QRect area = screen->availableGeometry();
    const QRect wanted(area.x() + 40, area.y() + 60, std::min(1100, area.width() - 80),
                       std::min(720, area.height() - 120));
    // A screen too small for the window's minimum size away from the edges (the offscreen platform's fake 800x600
    // screen on Linux) cannot show `wanted`: the window keeps its minimum size, so the geometry is not checked there.
    bool checkGeometry = true;
    {
        Window w(user.path());
        const QSize minimum = w.window()->minimumSize();
        checkGeometry = wanted.width() >= minimum.width() && wanted.height() >= minimum.height();
        if (!checkGeometry)
            WARN("the screen (" << area.width() << "x" << area.height()
                                << ") is too small for the window's minimum size (" << minimum.width() << "x"
                                << minimum.height() << "): geometry restore is not checked");
        CHECK_FALSE(w.settings()->loadProblem().size() > 0);
        REQUIRE(w.settings()->set(QStringLiteral("appearance.theme"), QStringLiteral("light")));
        CHECK(w.host->theme()->mode() == QStringLiteral("light"));
        // As the top bar's glass button and the view menu do it.
        w.context()->setGlass(true);
        CHECK(w.settings()->isSet(QStringLiteral("appearance.glass")));
        w.host->viewport()->setNavigationPreset(2);
        CHECK(w.settings()->value(QStringLiteral("navigation.preset")).toString() == navigationId);
        QQuickItem* shell = w.find(QStringLiteral("shell"));
        REQUIRE(shell != nullptr);
        shell->setProperty("leftPanelOpen", false);
        CHECK_FALSE(w.settings()->panelOpen(QStringLiteral("model")));
        if (checkGeometry) {
            w.window()->setGeometry(wanted);
            REQUIRE(QTest::qWaitFor([&] { return near(w.window()->geometry(), wanted); }, 3000));
        }
        w.checkNoWarnings();
        w.window()->close(); // saves the geometry
    }
    const QString file = QDir(user.path()).filePath(QStringLiteral("settings.json"));
    REQUIRE(QFileInfo::exists(file));
    const QString text = readFile(file);
    INFO(text.toStdString());
    CHECK(text.contains(QStringLiteral("\"theme\":\"light\"")));
    CHECK(text.contains(QStringLiteral("\"glass\":true")));
    CHECK(text.contains(QStringLiteral("\"preset\":\"%1\"").arg(navigationId)));
    CHECK(text.contains(QStringLiteral("\"window\":")));
    {
        Window w(user.path());
        CHECK(w.host->theme()->mode() == QStringLiteral("light"));
        CHECK_FALSE(w.host->theme()->dark());
        CHECK(w.context()->glass());
        CHECK(w.host->viewport()->navigationPreset() == 2);
        CHECK_FALSE(w.find(QStringLiteral("shell"))->property("leftPanelOpen").toBool());
        CHECK_FALSE(w.find(QStringLiteral("leftPanel"))->isVisible());
        INFO("restored " << w.window()->geometry().x() << "," << w.window()->geometry().y() << " "
                         << w.window()->geometry().width() << "x" << w.window()->geometry().height());
        if (checkGeometry)
            CHECK(near(w.window()->geometry(), wanted));
        w.checkNoWarnings();
    }
    // The command line wins over the saved theme and glass, and does not overwrite them.
    {
        ui::AppOptions options;
        options.theme = QStringLiteral("dark");
        options.glass = false;
        Window w(user.path(), options);
        CHECK(w.host->theme()->dark());
        CHECK_FALSE(w.context()->glass());
        CHECK(w.settings()->value(QStringLiteral("appearance.theme")).toString() == QStringLiteral("light"));
        CHECK(w.settings()->value(QStringLiteral("appearance.glass")).toBool());
    }
}

TEST_CASE("a saved window geometry is clamped to the screens there are", "[ui][settings]")
{
    const QScreen* primary = QGuiApplication::primaryScreen();
    REQUIRE(primary != nullptr);
    const QRect area = primary->availableGeometry();
    const QSize minimum(400, 300);
    // On a monitor that is gone: centred on the primary screen.
    QRect r = AppHost::clampToScreens(QRect(-50000, -50000, 900, 600), minimum);
    CHECK(area.contains(r));
    CHECK(r.size() == QSize(std::min(900, area.width()), std::min(600, area.height())));
    CHECK(std::abs(r.center().x() - area.center().x()) <= 1);
    // Larger than the screen: no larger than its available area, and wholly on it.
    r = AppHost::clampToScreens(QRect(area.x(), area.y(), area.width() * 3, area.height() * 3), minimum);
    CHECK(r == area);
    // Hanging off the right and bottom edges: moved back on.
    r = AppHost::clampToScreens(QRect(area.right() - 100, area.bottom() - 100, 800, 500), minimum);
    CHECK(area.contains(r));
    CHECK(r.size() == QSize(std::min(800, area.width()), std::min(500, area.height())));
}

TEST_CASE("a corrupt settings file falls back to the defaults without losing the session", "[ui][settings]")
{
    QTemporaryDir user;
    const QByteArray garbage = "{\"appearance\": {\"theme\": \"li";
    {
        QFile f(QDir(user.path()).filePath(QStringLiteral("settings.json")));
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(garbage);
    }
    Window w(user.path());
    // The user is told, once, in the banner; the defaults are in effect.
    CHECK(w.context()->message().contains(QStringLiteral("default settings")));
    CHECK(w.host->theme()->mode() == QStringLiteral("system"));
    CHECK(w.units()->system() == UnitSystem::Inch);
    const QStringList aside = QDir(user.path()).entryList({QStringLiteral("settings.corrupt-*.json")}, QDir::Files);
    REQUIRE(aside.size() == 1);
    CHECK(readFile(QDir(user.path()).filePath(aside.front())).toUtf8() == garbage);
    // The session works: a new document opens and settings save again.
    REQUIRE(w.app() != nullptr);
    REQUIRE(w.context()->invoke(QStringLiteral("file.new")));
    REQUIRE(QTest::qWaitFor([&] { return w.session()->hasDocument(); }, 10000));
    REQUIRE(w.settings()->set(QStringLiteral("units.decimalsInch"), 3));
    CHECK(QFileInfo::exists(QDir(user.path()).filePath(QStringLiteral("settings.json"))));
    w.checkNoWarnings();
}

TEST_CASE("new documents default to inch; changing the default changes the next new document only", "[ui][settings]")
{
    QTemporaryDir user;
    Window w(user.path());
    REQUIRE(w.context()->invoke(QStringLiteral("file.new")));
    REQUIRE(QTest::qWaitFor([&] { return w.session()->hasDocument(); }, 10000));
    api::OpenDocument* first = w.app()->active();
    REQUIRE(first != nullptr);
    CHECK(first->document->state()->settings.units == UnitSystem::Inch);
    CHECK(w.units()->system() == UnitSystem::Inch);
    CHECK(w.units()->formatLength(25.4) == QStringLiteral("1.0000 in"));

    REQUIRE(w.settings()->set(QStringLiteral("units.newDocument"), QStringLiteral("metric")));
    // The open document keeps its units, and so does what is shown for it.
    CHECK(first->document->state()->settings.units == UnitSystem::Inch);
    CHECK(w.units()->system() == UnitSystem::Inch);

    const std::string firstId = first->id;
    // A second document beside the first (the File menu would replace it).
    auto made = w.app()->newDocument("");
    REQUIRE(made);
    REQUIRE(QTest::qWaitFor([&] { return w.session()->documentId().toStdString() != firstId; }, 10000));
    api::OpenDocument* second = w.app()->active();
    REQUIRE(second == *made);
    CHECK(second->document->state()->settings.units == UnitSystem::Metric);
    CHECK(second->document->state()->settings.decimals == 3);
    REQUIRE(QTest::qWaitFor([&] { return w.units()->system() == UnitSystem::Metric; }, 3000));
    CHECK(w.units()->formatLength(25.4) == QStringLiteral("25.400 mm"));
    // Back to the first document: its own units again.
    w.app()->setActive(first);
    REQUIRE(QTest::qWaitFor([&] { return w.units()->system() == UnitSystem::Inch; }, 3000));
    w.checkNoWarnings();
}

TEST_CASE("the selection read-out switches units live and follows the decimals setting", "[ui][settings]")
{
    QTemporaryDir user;
    Window w(user.path());
    w.open(kBoxHole); // a new document: inch, the default
    CHECK(w.units()->system() == UnitSystem::Inch);
    w.host->selection()->selectBody(0, 0);
    REQUIRE(QTest::qWaitFor([&] { return !w.host->selection()->infoBusy() && !w.host->selection()->info().isEmpty(); },
                            30000));
    QTest::qWait(50);
    QStringList values = readOutValues(w);
    INFO(values.join(QStringLiteral(" | ")).toStdString());
    REQUIRE(
        std::any_of(values.begin(), values.end(), [](const QString& v) { return v.endsWith(QStringLiteral(" in³")); }));
    REQUIRE(
        std::any_of(values.begin(), values.end(), [](const QString& v) { return v.endsWith(QStringLiteral(" in")); }));
    w.shot(QStringLiteral("readout-inch"));

    // The quick toggle in the status bar: the same values in mm, without selecting again.
    QQuickItem* toggle = w.find(QStringLiteral("unitsToggle"));
    REQUIRE(toggle != nullptr);
    REQUIRE(toggle->isVisible());
    CHECK(toggle->property("text").toString() == QStringLiteral("in"));
    REQUIRE(QMetaObject::invokeMethod(toggle, "clicked"));
    QTest::qWait(30);
    CHECK(w.units()->system() == UnitSystem::Metric);
    CHECK(w.units()->overridden());
    // The document itself is unchanged: switching is display only.
    CHECK(w.app()->active()->document->state()->settings.units == UnitSystem::Inch);
    values = readOutValues(w);
    INFO(values.join(QStringLiteral(" | ")).toStdString());
    CHECK(
        std::any_of(values.begin(), values.end(), [](const QString& v) { return v.endsWith(QStringLiteral(" mm³")); }));
    CHECK(
        std::none_of(values.begin(), values.end(), [](const QString& v) { return v.endsWith(QStringLiteral(" in")); }));
    const auto decimalsOf = [](const QString& v) {
        const QString number = v.section(QLatin1Char(' '), 0, 0).section(QLatin1Char(','), 0, 0);
        const int dot = number.indexOf(QLatin1Char('.'));
        return dot < 0 ? 0 : int(number.size() - dot - 1);
    };
    const QString volumeMm = *std::find_if(values.begin(), values.end(),
                                           [](const QString& v) { return v.endsWith(QStringLiteral(" mm³")); });
    CHECK(decimalsOf(volumeMm) == 3);
    // The decimals setting applies live too.
    REQUIRE(w.settings()->set(QStringLiteral("units.decimalsMetric"), 1));
    QTest::qWait(30);
    values = readOutValues(w);
    const QString volumeMm1 = *std::find_if(values.begin(), values.end(),
                                            [](const QString& v) { return v.endsWith(QStringLiteral(" mm³")); });
    CHECK(decimalsOf(volumeMm1) == 1);
    w.shot(QStringLiteral("readout-mm"));
    // And back.
    REQUIRE(QMetaObject::invokeMethod(toggle, "clicked"));
    QTest::qWait(30);
    CHECK(w.units()->system() == UnitSystem::Inch);
    CHECK_FALSE(w.units()->overridden());
    w.checkNoWarnings();
}

TEST_CASE("changing the tier and a per-option override applies to the viewport without a restart", "[ui][settings]")
{
    RAY_REQUIRE_RHI();
    QTemporaryDir user;
    Window w(user.path());
    w.open(kBoxHole);
    render::IRenderViewport* view = w.host->viewport()->viewport();
    const auto frameUntil = [&](const std::function<bool()>& done) {
        return QTest::qWaitFor(
            [&] {
                w.host->viewport()->update();
                w.grab();
                return done();
            },
            10000);
    };

    REQUIRE(w.settings()->set(QStringLiteral("graphics.tier"), QStringLiteral("low")));
    CHECK(w.host->scene()->preset().tier == render::Tier::Low);
    CHECK(w.host->scene()->preset().meshChordTolMm == 0.2);
    REQUIRE(frameUntil([&] { return view->info().sampleCount == 1; }));
    CHECK_FALSE(view->info().smoothEdges);

    // One option over the tier: 4x MSAA on Low, then smooth edges.
    REQUIRE(w.settings()->set(QStringLiteral("graphics.msaaSamples"), 4));
    REQUIRE(frameUntil([&] { return view->info().sampleCount == 4; }));
    CHECK(w.host->scene()->preset().tier == render::Tier::Low);
    REQUIRE(w.settings()->set(QStringLiteral("graphics.smoothEdges"), true));
    REQUIRE(frameUntil([&] { return view->info().smoothEdges; }));
    w.shot(QStringLiteral("tier-low-msaa4-smooth"));

    // Back to the tier default.
    REQUIRE(w.settings()->reset(QStringLiteral("graphics.msaaSamples")));
    REQUIRE(frameUntil([&] { return view->info().sampleCount == 1; }));
    CHECK(view->info().smoothEdges); // still overridden

    // Another tier: its own values, the remaining override on top; the display mesh tolerance follows.
    REQUIRE(w.settings()->set(QStringLiteral("graphics.tier"), QStringLiteral("high")));
    REQUIRE(frameUntil([&] { return view->info().sampleCount == 4; }));
    CHECK(w.host->scene()->preset().meshChordTolMm == 0.01);
    REQUIRE(w.settings()->set(QStringLiteral("graphics.smoothEdges"), false));
    REQUIRE(frameUntil([&] { return !view->info().smoothEdges; }));
    REQUIRE(w.settings()->set(QStringLiteral("graphics.meshChordTolMm"), 0.03));
    CHECK(w.host->scene()->preset().meshChordTolMm == 0.03);
    REQUIRE(QTest::qWaitFor([&] { return w.settled(); }, kOpenTimeoutMs)); // refined again at the new tolerance
    // Glass follows the tier until it is set, and with it the real blurred backdrop (T-016) is on or off.
    CHECK(w.context()->glass());
    REQUIRE(QTest::qWaitFor([&] { return w.host->viewport()->backdropUsers() > 0; }, 3000));
    REQUIRE(w.settings()->set(QStringLiteral("graphics.tier"), QStringLiteral("medium")));
    CHECK_FALSE(w.context()->glass());
    REQUIRE(QTest::qWaitFor([&] { return w.host->viewport()->backdropUsers() == 0; }, 3000));
    REQUIRE(w.settings()->set(QStringLiteral("appearance.glass"), true)); // explicit: over the tier's default
    CHECK(w.context()->glass());
    REQUIRE(QTest::qWaitFor([&] { return w.host->viewport()->backdropUsers() > 0; }, 3000));
    REQUIRE(w.settings()->reset(QStringLiteral("appearance.glass")));
    CHECK_FALSE(w.context()->glass());
    // Section caps (used by the section view, T-023): an override reaches the preset and its reset the tier value.
    CHECK(w.host->scene()->preset().sectionCaps); // Medium has them
    REQUIRE(w.settings()->set(QStringLiteral("graphics.sectionCaps"), false));
    CHECK_FALSE(w.host->scene()->preset().sectionCaps);
    CHECK_FALSE(w.settings()->effectivePreset().value(QStringLiteral("graphics.sectionCaps")).toBool());
    REQUIRE(w.settings()->reset(QStringLiteral("graphics.sectionCaps")));
    CHECK(w.host->scene()->preset().sectionCaps);
    w.checkNoWarnings();
}

TEST_CASE("the settings panel shows the detected hardware and why the tier was chosen", "[ui][settings]")
{
    QTemporaryDir user;
    Window w(user.path(), {}, false);
    // Nothing is detected before the first frame (it was drawn) or before something asks.
    CHECK(w.settings()->hardwareState() == QStringLiteral("idle"));
    w.openPanel("general");
    w.showSection("graphics", false);
    // Detection runs on a worker: the UI thread returned at once and shows that it is under way.
    CHECK(w.settings()->hardwareState() == QStringLiteral("detecting"));
    QQuickItem* detecting = w.find(QStringLiteral("hardwareDetecting"));
    REQUIRE(detecting != nullptr);
    CHECK(detecting->isVisible());
    CHECK(detecting->property("text").toString().startsWith(QStringLiteral("Detecting")));
    REQUIRE(QTest::qWaitFor([&] { return w.settings()->hardwareState() == QStringLiteral("ready"); }, 20000));
    QTest::qWait(30);
    const auto hw = w.settings()->detectedHardware();
    REQUIRE(hw.has_value());
    CHECK_FALSE(detecting->isVisible());
    CHECK(w.text(QStringLiteral("hardware.gpuName")) ==
          (hw->gpuName.empty() ? QStringLiteral("unknown") : QString::fromStdString(hw->gpuName)));
    CHECK_FALSE(w.text(QStringLiteral("hardware.gpuType")).isEmpty());
    CHECK_FALSE(w.text(QStringLiteral("hardware.vram")).isEmpty());
    CHECK_FALSE(w.text(QStringLiteral("hardware.ram")).isEmpty());
    CHECK_FALSE(w.text(QStringLiteral("hardware.cores")).isEmpty());
    CHECK_FALSE(w.text(QStringLiteral("hardware.graphicsApi")).isEmpty());
    const ui::TierExplanation why = ui::explainTier(*hw);
    CHECK(why.tier == render::chooseTier(*hw));
    const QString reason = w.text(QStringLiteral("tierReason"));
    INFO(reason.toStdString());
    CHECK(reason.contains(why.reason));
    CHECK(reason.startsWith(QStringLiteral("In use: ")));
    // The tier is remembered for the next start.
    CHECK(w.settings()->current().lastDetectedTier.has_value());

    // Forcing a tier says so, and still says what detection would choose.
    REQUIRE(w.settings()->set(QStringLiteral("graphics.tier"), QStringLiteral("high")));
    QTest::qWait(30);
    CHECK(w.text(QStringLiteral("tierReason")).contains(QStringLiteral("which you chose")));
    CHECK(w.text(QStringLiteral("tierReason")).contains(why.reason));
    // An override shows its tier default and a reset button.
    REQUIRE(w.settings()->set(QStringLiteral("graphics.msaaSamples"), 2));
    QTest::qWait(30);
    QQuickItem* reset = w.find(QStringLiteral("settingReset.graphics.msaaSamples"));
    REQUIRE(reset != nullptr);
    CHECK(reset->isVisible());
    CHECK(w.text(QStringLiteral("settingDefault.graphics.msaaSamples")).contains(QStringLiteral("4x")));
    REQUIRE(QMetaObject::invokeMethod(reset, "clicked"));
    QTest::qWait(30);
    CHECK_FALSE(w.settings()->isSet(QStringLiteral("graphics.msaaSamples")));
    CHECK_FALSE(reset->isVisible());
    w.closePanel();
    w.checkNoWarnings();
}

TEST_CASE("hardware detection waits for the first frame and runs off the UI thread", "[ui][settings]")
{
    QTemporaryDir user;
    Window w(user.path(), {}, false);
    CHECK(w.settings()->hardwareState() == QStringLiteral("idle"));
    QElapsedTimer clock;
    clock.start();
    w.start(user.path()); // runApp does this after the first frame; it asks for the detection
    const qint64 ms = clock.elapsed();
    CHECK(w.settings()->hardwareState() == QStringLiteral("detecting"));
    WARN("startSession returned after " << ms << " ms with the hardware detection still running on a worker");
    REQUIRE(QTest::qWaitFor([&] { return w.settings()->hardwareState() == QStringLiteral("ready"); }, 20000));
    // Auto tier: the scene now has the detected tier.
    CHECK(w.host->scene()->preset().tier == render::chooseTier(*w.settings()->detectedHardware()));
}

TEST_CASE("Ctrl+comma, the view options menu and the palette open the settings", "[ui][settings]")
{
    QTemporaryDir user;
    Window w(user.path());
    w.host->viewport()->forceActiveFocus();
    QTest::keyClick(w.window(), Qt::Key_Comma, Qt::ControlModifier);
    REQUIRE(QTest::qWaitFor([&] { return w.panelOpen(); }, 3000));
    w.closePanel();

    QQuickItem* shell = w.find(QStringLiteral("shell"));
    REQUIRE(QMetaObject::invokeMethod(shell, "openViewMenu"));
    QObject* item = w.findObject(QStringLiteral("openSettings"));
    if (item == nullptr)
        item = w.find(QStringLiteral("openSettings"));
    REQUIRE(item != nullptr);
    REQUIRE(QMetaObject::invokeMethod(item, "triggered"));
    REQUIRE(QTest::qWaitFor([&] { return w.panelOpen(); }, 3000));
    w.closePanel();

    REQUIRE(w.host->commands()->run(QStringLiteral("ui.settings")));
    REQUIRE(QTest::qWaitFor([&] { return w.panelOpen(); }, 3000));
    // Keyboard: opens the shortcut settings.
    w.showSection("keyboard");
    QQuickItem* openShortcuts = w.find(QStringLiteral("settingsOpenShortcuts"));
    REQUIRE(openShortcuts != nullptr);
    REQUIRE(QMetaObject::invokeMethod(openShortcuts, "clicked"));
    REQUIRE(QTest::qWaitFor(
        [&] {
            QObject* s = w.findObject(QStringLiteral("shortcutSettings"));
            return s != nullptr && s->property("opened").toBool();
        },
        3000));
    CHECK_FALSE(w.panelOpen());
    w.checkNoWarnings();
}

TEST_CASE("explainTier agrees with chooseTier", "[ui][settings]")
{
    constexpr std::uint64_t GiB = std::uint64_t{1} << 30;
    const auto hw = [](render::GpuType type, std::uint64_t vram, std::uint64_t ram, bool compute, const char* name) {
        render::HardwareInfo h;
        h.gpuType = type;
        h.vramBytes = vram;
        h.ramBytes = ram;
        h.computeShaders = compute;
        h.gpuName = name;
        return h;
    };
    const std::vector<render::HardwareInfo> cases = {
        hw(render::GpuType::Software, 0, 32 * GiB, false, "Microsoft Basic Render Driver"),
        hw(render::GpuType::Unknown, 0, 32 * GiB, true, ""),
        hw(render::GpuType::Integrated, 128ull << 20, 16 * GiB, true, "Integrated GPU"),
        hw(render::GpuType::Discrete, 2 * GiB, 32 * GiB, true, "Small card"),
        hw(render::GpuType::Discrete, 12 * GiB, 8 * GiB, true, "Big card"),
        hw(render::GpuType::Discrete, 6 * GiB, 16 * GiB, true, "Mid card"),
        hw(render::GpuType::Discrete, 12 * GiB, 32 * GiB, false, "No compute"),
        hw(render::GpuType::Discrete, 0, 32 * GiB, true, "Unknown memory"),
        hw(render::GpuType::Discrete, 12 * GiB, 0, true, "Unknown RAM"),
        hw(render::GpuType::Discrete, 12 * GiB, 32 * GiB, true, "Fast card"),
        hw(render::GpuType::Discrete, 8 * GiB - (256ull << 20), 24 * GiB - (512ull << 20), true, "Just High"),
        hw(render::GpuType::Discrete, 12 * GiB, 32 * GiB, true, "llvmpipe (LLVM 15)"),
    };
    for (const render::HardwareInfo& h : cases) {
        const ui::TierExplanation why = ui::explainTier(h);
        INFO(h.gpuName << ": " << why.reason.toStdString());
        CHECK(why.tier == render::chooseTier(h));
        CHECK_FALSE(why.reason.contains(QStringLiteral("chosen from the detected")));
        const QString start = why.tier == render::Tier::Low      ? QStringLiteral("Low")
                              : why.tier == render::Tier::Medium ? QStringLiteral("Medium")
                                                                 : QStringLiteral("High");
        CHECK(why.reason.startsWith(start));
    }
}

TEST_CASE("the measure panel, its labels over the view and the selection read-out switch units together",
          "[ui][settings][measure]")
{
    RAY_REQUIRE_RHI();
    QTemporaryDir user;
    Window w(user.path());
    w.open(kPlate);
    ui::MeasureModel* m = w.measure();
    REQUIRE(w.host->commands()->trigger(QStringLiteral("measure.tool")));
    REQUIRE(QTest::qWaitFor([&] { return w.rightTab() == 1 && m->active(); }, 3000));
    // Two holes 30 mm apart (radius 3.3 mm): the read-out shows the last one's radius.
    w.selectFaces(w.holeAt(30, 80.0 / 3.0), w.holeAt(60, 80.0 / 3.0));
    REQUIRE(QTest::qWaitFor([&] { return !m->busy() && !w.host->selection()->pending() && m->hasResult(); }, 20000));
    REQUIRE(QTest::qWaitFor([&] { return !w.host->selection()->infoBusy() && !w.host->selection()->info().isEmpty(); },
                            20000));
    QQuickItem* toggle = w.find(QStringLiteral("unitsToggle"));
    REQUIRE(toggle != nullptr);

    // Everything that shows a length: the panel, the label over the view, the selection read-out, the status bar.
    const auto showing = [&](const QString& distance, const QString& radius, const QString& label) {
        return QTest::qWaitFor(
            [&] {
                w.grab();
                const QStringList readOut = readOutValues(w, true);
                return w.panelValue(QStringLiteral("centerDistance")) == distance &&
                       w.labelTexts().contains(distance) && readOut.contains(radius) &&
                       std::none_of(readOut.begin(), readOut.end(),
                                    [&](const QString& v) {
                                        return v.endsWith(label == QStringLiteral("in") ? QStringLiteral(" mm")
                                                                                        : QStringLiteral(" in"));
                                    }) &&
                       toggle->property("text").toString() == label &&
                       w.units()->format().system ==
                           (label == QStringLiteral("in") ? UnitSystem::Inch : UnitSystem::Metric);
            },
            3000);
    };
    const auto describe = [&] {
        UNSCOPED_INFO("panel " << w.panelValue(QStringLiteral("centerDistance")).toStdString() << " | labels "
                               << w.labelTexts().join(QStringLiteral(", ")).toStdString() << " | read-out "
                               << readOutValues(w, true).join(QStringLiteral(", ")).toStdString() << " | status "
                               << toggle->property("text").toString().toStdString());
    };
    // A new document: inch, 4 decimals.
    describe();
    REQUIRE(showing(QStringLiteral("1.1811 in"), QStringLiteral("0.1299 in"), QStringLiteral("in")));

    // 1. The status bar's toggle.
    REQUIRE(QMetaObject::invokeMethod(toggle, "clicked"));
    describe();
    REQUIRE(showing(QStringLiteral("30.000 mm"), QStringLiteral("3.300 mm"), QStringLiteral("mm")));
    CHECK_FALSE(m->busy()); // re-formatted, not measured again
    REQUIRE(QMetaObject::invokeMethod(toggle, "clicked"));
    REQUIRE(showing(QStringLiteral("1.1811 in"), QStringLiteral("0.1299 in"), QStringLiteral("in")));

    // 2. The Settings panel: the display units of this document, and the decimals.
    w.openPanel("general");
    QQuickItem* mmButton = w.find(QStringLiteral("displayUnits.metric"));
    REQUIRE(mmButton != nullptr);
    REQUIRE(QMetaObject::invokeMethod(mmButton, "clicked"));
    describe();
    REQUIRE(showing(QStringLiteral("30.000 mm"), QStringLiteral("3.300 mm"), QStringLiteral("mm")));
    REQUIRE(w.settings()->set(QStringLiteral("units.decimalsMetric"), 1));
    REQUIRE(showing(QStringLiteral("30.0 mm"), QStringLiteral("3.3 mm"), QStringLiteral("mm")));
    QQuickItem* inchButton = w.find(QStringLiteral("displayUnits.inch"));
    REQUIRE(QMetaObject::invokeMethod(inchButton, "clicked"));
    REQUIRE(w.settings()->set(QStringLiteral("units.decimalsInch"), 2));
    describe();
    REQUIRE(showing(QStringLiteral("1.18 in"), QStringLiteral("0.13 in"), QStringLiteral("in")));
    w.closePanel();

    // 3. The Measure panel's own in/mm buttons drive the same switch.
    QQuickItem* measureMm = w.find(QStringLiteral("measureUnits.mm"));
    REQUIRE(measureMm != nullptr);
    REQUIRE(QMetaObject::invokeMethod(measureMm, "clicked"));
    REQUIRE(showing(QStringLiteral("30.0 mm"), QStringLiteral("3.3 mm"), QStringLiteral("mm")));
    CHECK(w.find(QStringLiteral("measureUnits"))->property("currentIndex").toInt() == 1);
    // ...and MeasureTool's setter (scripts, tests) as well.
    m->setUnits(QStringLiteral("inch"));
    REQUIRE(showing(QStringLiteral("1.18 in"), QStringLiteral("0.13 in"), QStringLiteral("in")));
    CHECK(w.find(QStringLiteral("measureUnits"))->property("currentIndex").toInt() == 0);
    // The document itself never changed: switching is display only.
    CHECK(w.app()->active()->document->state()->settings.units == UnitSystem::Inch);
    // The right panel's tab is saved with the panels.
    CHECK(w.settings()->value(QStringLiteral("panels.properties.tab")).toString() == QStringLiteral("measure"));
    w.checkNoWarnings();
}

TEST_CASE("screenshots of every settings section in both themes", "[.screenshots][screenshots]")
{
    RAY_REQUIRE_RHI();
    if (qEnvironmentVariable("RAY_T024_SHOTS").isEmpty())
        SKIP("set RAY_T024_SHOTS to a folder");
    for (const char* theme : {"dark", "light"}) {
        QTemporaryDir user;
        ui::AppOptions options;
        options.theme = QString::fromLatin1(theme);
        options.size = QSize(1440, 900);
        Window w(user.path(), options);
        w.open(kBoxHole);
        REQUIRE(QTest::qWaitFor([&] { return w.settings()->hardwareState() == QStringLiteral("ready"); }, 20000));
        for (const char* section : {"general", "appearance", "graphics", "navigation", "keyboard"}) {
            w.openPanel(section);
            w.shot(QStringLiteral("%1-%2").arg(QString::fromLatin1(theme), QString::fromLatin1(section)));
            if (QString::fromLatin1(section) == QLatin1String("graphics")) {
                // The lower half: the per-option overrides, one of them changed.
                REQUIRE(w.settings()->set(QStringLiteral("graphics.shadows"), false));
                QQuickItem* content = w.find(QStringLiteral("settingsContent"));
                REQUIRE(content != nullptr);
                content->setProperty("contentY", content->property("contentHeight").toReal() - content->height());
                w.shot(QStringLiteral("%1-graphics-options").arg(QString::fromLatin1(theme)));
                REQUIRE(w.settings()->reset(QStringLiteral("graphics.shadows")));
            }
            w.closePanel();
        }
        w.checkNoWarnings();
    }
}

TEST_CASE("warm start to the first frame with saved settings, against PLAN.md section 9", "[bench]")
{
    // A populated settings file (theme, glass, tier, overrides, window and panels): reading and applying it is
    // part of the start; hardware detection is not (it runs after the first frame).
    QTemporaryDir data;
    {
        QFile f(QDir(data.path()).filePath(QStringLiteral("settings.json")));
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(R"({"format":"raycad.settings","version":1,"appearance":{"theme":"light","glass":true},)"
                R"("graphics":{"tier":"medium","msaaSamples":4,"lastDetectedTier":"high"},)"
                R"("navigation":{"preset":"middle-orbit"},"panels":{"model":{"open":false}},)"
                R"("units":{"newDocument":"metric"},"window":{"x":80,"y":80,"width":1280,"height":800}})");
    }
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("RAYCAD_USER_DATA_DIR"), data.path());
    const QRegularExpression frameRe(QStringLiteral("first_frame_ms=([0-9.]+)"));
    std::vector<double> frames;
    for (int run = 0; run < 9; ++run) {
        QProcess p;
        p.setProcessEnvironment(env);
        p.start(QStringLiteral(RAY_RAYCAD_EXE), {QStringLiteral("--bench-start")});
        REQUIRE(p.waitForFinished(60000));
        const QString out = QString::fromUtf8(p.readAllStandardOutput());
        INFO(out.toStdString() << QString::fromUtf8(p.readAllStandardError()).toStdString());
        REQUIRE(p.exitCode() == 0);
        const auto frame = frameRe.match(out);
        REQUIRE(frame.hasMatch());
        if (run >= 2)
            frames.push_back(frame.captured(1).toDouble());
    }
    std::sort(frames.begin(), frames.end());
    const double frame = frames[frames.size() / 2];
    WARN("warm start to the first frame with saved settings: median " << frame << " ms over " << frames.size()
                                                                      << " runs (PLAN.md 9: Low 2500, Medium 1500, "
                                                                         "High 1000 ms)");
    CHECK(frame < 2500);
    // --bench-start does not save the window (it would replace the user's own).
    CHECK(readFile(QDir(data.path()).filePath(QStringLiteral("settings.json"))).contains(QStringLiteral("\"x\":80")));
}

int main(int argc, char** argv)
{
    // The real user profile is never touched: anything that falls back to the default per-user folder uses this.
    QTemporaryDir profile;
    qputenv("RAYCAD_USER_DATA_DIR", QDir::toNativeSeparators(profile.path()).toUtf8());
    QGuiApplication app(argc, argv);
    QGuiApplication::setQuitOnLastWindowClosed(false);
    return Catch::Session().run(argc, argv);
}
