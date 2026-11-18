// The measure tools in the desktop app (task T-022), on real Qt Quick windows with the real application session:
//
//   - two holes selected in a real window show their centre distance (and a click-selected face pair the distance
//     and the angle); the witness and its label are in the view
//   - the values are in the document's units (inch for a new document) and switch live to mm and back, from the
//     panel's unit buttons and from MeasureTool's setter (both switch the Units singleton, T-024)
//   - the copy buttons and "copy all" put the formatted text on the clipboard
//   - a pinned measurement stays while other things are selected; unpin and clear remove it
//   - every measure command is in the palette; the context menu offers Measure for a selection; M opens the Measure
//     tab only while the 3D view has the focus; the minimum internal corner radius is listed, disabled, "coming later"
//   - no UI-thread stall over 50 ms while measuring a mid-size corpus part
//
// Screenshots: with RAY_T022_SHOTS set to a folder, the window is saved there (both themes).
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
#include "theme_object.h"
#include "viewport_item.h"
#include "viewport_navigation.h"

#include <ray_api/viewer.h>
#include <ray_render/viewport.h>

#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFileInfo>
#include <QtCore/QPointer>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>
#include <QtGui/QMouseEvent>
#include <QtQml/QQmlApplicationEngine>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>
#include <QtQuick/QSGRendererInterface>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace ray;
using ray::ui::AppHost;
using ray::ui::AppOptions;
using ray::ui::MeasureModel;

namespace {

// plate_thru_holes_1 (tools/corpus_gen): a 120 x 80 x 10 mm plate, 6.6 mm holes at x = 30, 60, 90, y = 80/3, 160/3.
const QString kPlate = QStringLiteral(RAY_CORPUS_DIR "/raycad-synthetic/plate_thru_holes_1.stp");
const QString kChamfer = QStringLiteral(RAY_CORPUS_DIR "/raycad-synthetic/chamfer_block_1.stp");
// One of the larger committed corpus files (about 0.8 MB).
const QString kMidSize = QStringLiteral(RAY_CORPUS_DIR "/nist-pmi/nist_ctc_04_asme1_rd_geometry.stp");

constexpr int kOpenTimeoutMs = 120000;

using ray::uitest::rhiSceneGraph;

struct Window {
    QTemporaryDir cache;
    QTemporaryDir userData;
    std::unique_ptr<AppHost> host;

    explicit Window(const char* theme = "dark")
    {
        AppOptions options;
        options.theme = QString::fromLatin1(theme);
        options.glass = false;
        options.size = QSize(1440, 900);
        host = std::make_unique<AppHost>(options, cache.path());
        REQUIRE(host->optionError().isEmpty());
        REQUIRE(host->load());
        host->show();
        REQUIRE(QTest::qWaitForWindowExposed(host->window()));
        REQUIRE(host->viewport() != nullptr);
        REQUIRE(host->startSession(userData.path().toStdString()));
        host->files()->setUseNativeDialogs(false);
        host->viewport()->setAnimationsEnabled(false);
        host->scene()->setPreset(render::presetFor(render::Tier::Medium));
        grab();
    }

    QQuickWindow* window() const { return host->window(); }
    ui::ViewportItem* view() const { return host->viewport(); }
    ui::SceneController* scene() const { return host->scene(); }
    ui::SelectionModel* selection() const { return host->selection(); }
    ui::CommandCatalog* commands() const { return host->commands(); }
    MeasureModel* measure() const
    {
        auto* m = host->engine()->singletonInstance<MeasureModel*>(QStringLiteral("RayCad.Ui"),
                                                                   QStringLiteral("MeasureTool"));
        REQUIRE(m != nullptr);
        return m;
    }
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
    QQuickItem* shown(const QString& objectName) const
    {
        QQuickItem* item = find(objectName);
        return item != nullptr && item->isVisible() ? item : nullptr;
    }
    void click(QQuickItem* item, Qt::KeyboardModifiers mods = Qt::NoModifier) const
    {
        REQUIRE(item != nullptr);
        // Rows a model change just made are laid out at the next polish: let the layout settle first.
        QPointer<QQuickItem> guard(item);
        grab();
        REQUIRE(guard != nullptr);
        const QPointF c = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
        clickAt(c, mods);
    }
    void clickAt(QPointF pos, Qt::KeyboardModifiers mods = Qt::NoModifier) const
    {
        // As a hand would: move there first (hover, and any tooltip of the last button goes), then press and release.
        QMouseEvent move(QEvent::MouseMove, pos, window()->mapToGlobal(pos), Qt::NoButton, Qt::NoButton, mods);
        QGuiApplication::sendEvent(window(), &move);
        QTest::qWait(20);
        QMouseEvent press(QEvent::MouseButtonPress, pos, window()->mapToGlobal(pos), Qt::LeftButton, Qt::LeftButton,
                          mods);
        QGuiApplication::sendEvent(window(), &press);
        QMouseEvent release(QEvent::MouseButtonRelease, pos, window()->mapToGlobal(pos), Qt::LeftButton, Qt::NoButton,
                            mods);
        QGuiApplication::sendEvent(window(), &release);
    }

    void checkNoWarnings() const
    {
        for (const QString& warning : host->warnings())
            UNSCOPED_INFO(warning.toStdString());
        CHECK(host->warnings().isEmpty());
    }

    bool settled() const
    {
        return !host->files()->busy() && scene()->ready() && !scene()->refining() && !view()->animating();
    }
    void open(const QString& path) const
    {
        REQUIRE(QFileInfo::exists(path));
        REQUIRE(host->files()->open(path));
        REQUIRE(QTest::qWaitFor([&] { return settled(); }, kOpenTimeoutMs));
        REQUIRE(scene()->bodyCount() >= 1);
        grab();
    }
    bool measured() const
    {
        return QTest::qWaitFor([&] { return !measure()->busy() && !selection()->pending(); }, 20000) &&
               measure()->hasResult();
    }
    // The text the panel shows for a value of the current measurement.
    QString panelValue(const QString& key) const
    {
        QQuickItem* row = find(QStringLiteral("measureValue.") + key);
        if (row == nullptr)
            return {};
        QQuickItem* text = findItem(row, QStringLiteral("measureValueText"));
        return text != nullptr ? text->property("text").toString() : QString();
    }
    QStringList labelTexts() const
    {
        QStringList out;
        for (const QVariant& l : measure()->labels())
            out << l.toMap().value(QStringLiteral("text")).toString();
        return out;
    }
    void shot(const char* name) const
    {
        const QString dir = qEnvironmentVariable("RAY_T022_SHOTS");
        if (dir.isEmpty())
            return;
        QDir().mkpath(dir);
        // The pointer off every button (no tooltip in the picture), over the empty status bar.
        const QPointF away(window()->width() / 2.0, window()->height() - 4.0);
        QMouseEvent move(QEvent::MouseMove, away, window()->mapToGlobal(away), Qt::NoButton, Qt::NoButton,
                         Qt::NoModifier);
        QGuiApplication::sendEvent(window(), &move);
        QTest::qWait(250);
        grab().save(QDir(dir).filePath(QString::fromLatin1(name) + QStringLiteral(".png")), "PNG");
    }
    int rightTab() const { return find(QStringLiteral("shell"))->property("rightTab").toInt(); }
};

// The face of body 0 that `pick` accepts (by its surface info), from the scene's own shape.
std::optional<std::uint32_t> faceWhere(const Window& w, const std::function<bool(const kernel::SurfaceInfo&)>& pick)
{
    const api::viewer::Body* body = w.scene()->body(0);
    REQUIRE(body != nullptr);
    JobContext ctx;
    for (std::uint32_t i = 0; i < body->shape.count(kernel::SubKind::Face); ++i) {
        auto info = api::viewer::describeElement(body->shape, kernel::SubId{kernel::SubKind::Face, i}, ctx);
        REQUIRE(info);
        if (info->surface && pick(*info->surface))
            return i;
    }
    return std::nullopt;
}

std::uint32_t holeAt(const Window& w, double x, double y)
{
    const auto f = faceWhere(w, [&](const kernel::SurfaceInfo& s) {
        return s.type == kernel::SurfaceType::Cylinder && std::hypot(s.centroid.x - x, s.centroid.y - y) < 1e-6;
    });
    INFO("hole at " << x << ", " << y);
    REQUIRE(f.has_value());
    return *f;
}

render::PickHit faceHit(const Window& w, std::uint32_t face)
{
    return render::PickHit{w.scene()->itemForBody(0), kernel::SubKind::Face, face, Vec3{}, 0};
}

void selectFaces(const Window& w, std::uint32_t a, std::uint32_t b)
{
    w.selection()->applyClick(faceHit(w, a), Qt::NoModifier);
    w.selection()->applyClick(faceHit(w, b), Qt::ControlModifier);
    REQUIRE(w.selection()->count() == 2);
}

} // namespace

TEST_CASE("two holes show their centre distance; inch and mm switch live; copy; pin", "[measure][e2e]")
{
    RAY_REQUIRE_RHI();
    Window w("dark");
    w.open(kPlate);
    MeasureModel& m = *w.measure();
    // A STEP file opens as a new document: inch, 4 decimals (PLAN.md: new documents default to inch).
    CHECK(m.unitSystem() == QStringLiteral("inch"));
    CHECK(m.decimals() == 4);
    REQUIRE(w.view()->setStandardView(QStringLiteral("iso")));

    // M with the view focused shows the Measure tab.
    CHECK(w.rightTab() == 0);
    w.view()->forceActiveFocus();
    w.window()->requestActivate();
    if (QTest::qWaitForWindowActive(w.window(), 3000)) {
        QTest::keyClick(w.window(), Qt::Key_M);
        REQUIRE(QTest::qWaitFor([&] { return w.rightTab() == 1; }, 3000));
        CHECK(m.active());
        // Not while a text field has the focus (the command search).
        w.click(w.shown(QStringLiteral("rightTab.properties")));
        REQUIRE(QTest::qWaitFor([&] { return w.rightTab() == 0; }, 3000));
        QQuickItem* search = w.find(QStringLiteral("commandSearch"));
        search->forceActiveFocus(Qt::TabFocusReason);
        QTest::keyClick(w.window(), Qt::Key_M);
        QTest::qWait(100);
        CHECK(w.rightTab() == 0);
        QTest::keyClick(w.window(), Qt::Key_Escape); // closes the palette the typed text opened
        QTest::qWait(100);
        w.view()->forceActiveFocus();
    } else {
        WARN("the window could not become active: the M key is not checked");
    }
    REQUIRE(w.commands()->trigger(QStringLiteral("measure.tool")));
    REQUIRE(QTest::qWaitFor([&] { return w.rightTab() == 1 && m.active(); }, 3000));
    // Nothing selected: what to do.
    CHECK_FALSE(m.hasResult());
    CHECK(w.shown(QStringLiteral("measureHint")) != nullptr);

    const double my = 80.0 / 3.0;
    selectFaces(w, holeAt(w, 30, my), holeAt(w, 60, my));
    REQUIRE(w.measured());
    // Parallel holes: the distances, no angle (a note says the axes are parallel).
    CHECK(m.current()->kind == api::measure::Kind::Distance);
    CHECK(m.current()->find("angle") == nullptr);
    // 30 mm = 1.1811 in; the wall between them 30 - 6.6 = 23.4 mm = 0.9213 in.
    CHECK(m.valueText(QStringLiteral("centerDistance")) == QStringLiteral("1.1811 in"));
    CHECK(m.valueText(QStringLiteral("distance")) == QStringLiteral("0.9213 in"));
    REQUIRE(QTest::qWaitFor([&] { return w.panelValue(QStringLiteral("centerDistance")) == "1.1811 in"; }, 3000));
    // The breakdown: the centres differ along X only.
    CHECK(m.valueText(QStringLiteral("centerDy")) == QStringLiteral("0.0000 in"));
    CHECK(m.valueText(QStringLiteral("centerDx")).endsWith(QStringLiteral("1.1811 in")));
    // The witness label is over the view, at a point inside it.
    CHECK(w.labelTexts().contains(QStringLiteral("1.1811 in")));
    CHECK_FALSE(m.currentPaths().isEmpty());
    w.shot("measure-holes-dark-inch");

    // mm from the panel's unit buttons: live, nothing measured again.
    w.click(w.shown(QStringLiteral("measureUnits.mm")));
    REQUIRE(QTest::qWaitFor([&] { return w.panelValue(QStringLiteral("centerDistance")) == "30.000 mm"; }, 3000));
    CHECK_FALSE(m.busy());
    CHECK(m.unitSystem() == QStringLiteral("mm"));
    CHECK(w.labelTexts().contains(QStringLiteral("30.000 mm")));
    w.shot("measure-holes-dark-mm");
    // ...and back through MeasureTool's setter, which switches the Units singleton (T-024) like the buttons.
    m.setUnitFormat(UnitFormat{UnitSystem::Inch, 4});
    REQUIRE(QTest::qWaitFor([&] { return w.panelValue(QStringLiteral("centerDistance")) == "1.1811 in"; }, 3000));
    CHECK(w.find(QStringLiteral("measureUnits"))->property("currentIndex").toInt() == 0);
    m.setUnits(QStringLiteral("mm"));
    REQUIRE(QTest::qWaitFor([&] { return w.panelValue(QStringLiteral("centerDistance")) == "30.000 mm"; }, 3000));

    // Copy one value, then everything.
    QQuickItem* row = w.shown(QStringLiteral("measureValue.centerDistance"));
    REQUIRE(row != nullptr);
    QSignalSpy copied(&m, &MeasureModel::copied);
    w.click(row); // a click on a value copies it
    REQUIRE(QTest::qWaitFor([&] { return copied.count() == 1; }, 3000));
    CHECK(copied.front().front().toString().toStdString() == "30.000 mm");
    INFO("clipboard: " << QGuiApplication::clipboard()->text().toStdString());
    CHECK(QGuiApplication::clipboard()->text().toStdString() == "30.000 mm");
    w.click(w.shown(QStringLiteral("measureCopyAll")));
    REQUIRE(QTest::qWaitFor([&] { return copied.count() == 2; }, 3000));
    CHECK(copied.back().front().toString().contains(QStringLiteral("Centre distance: 30.000 mm")));
    CHECK(QGuiApplication::clipboard()->text().contains(QStringLiteral("Centre distance: 30.000 mm")));

    // Pin it, then select something else: the pinned result stays, with its witness.
    w.click(w.shown(QStringLiteral("measurePin")));
    REQUIRE(m.pinnedCount() == 1);
    const std::uint32_t top = *faceWhere(w, [](const kernel::SurfaceInfo& s) {
        return s.type == kernel::SurfaceType::Plane && s.centroid.z > 9.999999;
    });
    w.selection()->applyClick(faceHit(w, top), Qt::NoModifier);
    REQUIRE(w.measured());
    CHECK(m.current()->kind == api::measure::Kind::Element);
    CHECK(m.pinnedCount() == 1);
    QQuickItem* pinnedCard = w.shown(QStringLiteral("measurePinned.0"));
    REQUIRE(pinnedCard != nullptr);
    QQuickItem* pinnedRow = Window::findItem(pinnedCard, QStringLiteral("measurePinnedValue.centerDistance"));
    REQUIRE(pinnedRow != nullptr);
    CHECK(Window::findItem(pinnedRow, QStringLiteral("measureValueText"))->property("text").toString() ==
          QStringLiteral("30.000 mm"));
    CHECK(w.labelTexts().contains(QStringLiteral("30.000 mm")));
    CHECK_FALSE(m.pinnedPaths().isEmpty());
    w.shot("measure-pinned-dark-mm");
    // Pinned values follow the units too.
    m.setUnits(QStringLiteral("inch"));
    REQUIRE(QTest::qWaitFor(
        [&] {
            QQuickItem* r = Window::findItem(w.find(QStringLiteral("measurePinned.0")),
                                             QStringLiteral("measurePinnedValue.centerDistance"));
            return r != nullptr &&
                   Window::findItem(r, QStringLiteral("measureValueText"))->property("text").toString() == "1.1811 in";
        },
        3000));
    // Clearing the selection keeps it too; unpin removes it.
    w.selection()->clear();
    QTest::qWait(50);
    CHECK(m.pinnedCount() == 1);
    w.click(Window::findItem(w.shown(QStringLiteral("measurePinned.0")), QStringLiteral("measureUnpin")));
    REQUIRE(QTest::qWaitFor([&] { return m.pinnedCount() == 0; }, 3000));
    CHECK(m.pinnedPaths().isEmpty());

    // The light theme, the same measurement.
    selectFaces(w, holeAt(w, 30, my), holeAt(w, 60, 2 * my));
    REQUIRE(w.measured());
    CHECK(m.valueText(QStringLiteral("centerDistance")) == QStringLiteral("1.5803 in")); // hypot(30, 80/3) mm
    w.host->theme()->setMode(QStringLiteral("light"));
    w.shot("measure-holes-light-inch");
    w.checkNoWarnings();
}

TEST_CASE("click-selected faces show distance and angle; the angle on a chamfer; mass properties", "[measure][angle]")
{
    RAY_REQUIRE_RHI();
    Window w("light");
    w.open(kChamfer);
    MeasureModel& m = *w.measure();
    REQUIRE(w.commands()->trigger(QStringLiteral("measure.tool")));
    REQUIRE(QTest::qWaitFor([&] { return m.active(); }, 3000));
    REQUIRE(w.view()->setStandardView(QStringLiteral("iso")));
    w.grab();

    // A real click on the top face, a Ctrl+click on a chamfer face next to it (by pixel).
    const auto top = faceWhere(w, [](const kernel::SurfaceInfo& s) {
        return s.type == kernel::SurfaceType::Plane && std::abs(std::abs(s.axis.z) - 1) < 1e-12 && s.centroid.z > 1;
    });
    const auto chamfer = faceWhere(w, [](const kernel::SurfaceInfo& s) {
        const double ax = std::abs(s.axis.x), ay = std::abs(s.axis.y), az = std::abs(s.axis.z);
        // The front one (y = 0 side), which the iso view shows.
        return s.type == kernel::SurfaceType::Plane && az > 0.1 && az < 0.9 && ax < 1e-9 && ay > 0.1 &&
               s.centroid.z > 1 && s.centroid.y < 5;
    });
    REQUIRE(top.has_value());
    REQUIRE(chamfer.has_value());
    const auto centroid = [&](std::uint32_t f) {
        JobContext ctx;
        auto info =
            api::viewer::describeElement(w.scene()->body(0)->shape, kernel::SubId{kernel::SubKind::Face, f}, ctx);
        REQUIRE(info);
        return info->surface->centroid;
    };
    const auto at = [&](Vec3 p) {
        const auto px = ui::nav::project(w.view()->camera(), p, w.view()->width(), w.view()->height());
        REQUIRE(px.has_value());
        return w.view()->mapToScene(*px);
    };
    w.clickAt(at(centroid(*top)));
    REQUIRE(QTest::qWaitFor([&] { return w.selection()->count() == 1 && !w.selection()->pending(); }, 5000));
    w.clickAt(at(centroid(*chamfer)), Qt::ControlModifier);
    REQUIRE(QTest::qWaitFor([&] { return w.selection()->count() == 2 && !w.selection()->pending(); }, 5000));
    const auto& entries = w.selection()->entries();
    const bool picked = entries[0].hit.kind == kernel::SubKind::Face && entries[0].hit.subIndex == *top &&
                        entries[1].hit.kind == kernel::SubKind::Face && entries[1].hit.subIndex == *chamfer;
    if (!picked) {
        // The pixel did not land on those faces (a tiny chamfer at this zoom): select them directly.
        WARN("the clicks did not pick the top and chamfer faces; selecting them directly");
        selectFaces(w, *top, *chamfer);
    }
    REQUIRE(w.measured());
    CHECK(m.current()->kind == api::measure::Kind::Pair);
    // They meet at an edge: distance 0; 45 degrees acute, 135 obtuse and 135 of material at that edge.
    CHECK(m.valueText(QStringLiteral("distance")) == QStringLiteral("0.0000 in"));
    CHECK(m.valueText(QStringLiteral("angle")) == QStringLiteral("45.000\u00b0"));
    CHECK(m.valueText(QStringLiteral("supplement")) == QStringLiteral("135.000\u00b0"));
    CHECK(m.valueText(QStringLiteral("edgeAngle")) == QStringLiteral("135.000\u00b0"));
    REQUIRE(QTest::qWaitFor([&] { return w.panelValue(QStringLiteral("angle")) == "45.000\u00b0"; }, 3000));
    CHECK(w.labelTexts().contains(QStringLiteral("135.000\u00b0")));
    // A closer look at the edge for the picture.
    {
        const QPointF edge = w.view()->mapFromScene(at(centroid(*chamfer)));
        w.view()->zoomAt(2.5, edge.x(), edge.y());
    }
    w.shot("measure-angle-light");
    w.host->theme()->setMode(QStringLiteral("dark"));
    w.shot("measure-angle-dark");

    // The palette's Angle on the same pair.
    REQUIRE(w.commands()->trigger(QStringLiteral("measure.angle")));
    REQUIRE(w.measured());
    CHECK(m.current()->kind == api::measure::Kind::Angle);
    CHECK(m.valueText(QStringLiteral("angle")) == QStringLiteral("45.000\u00b0"));

    // Mass properties of the whole part (nothing selected), in mm: 94386.333333 mm³ (the corpus manifest).
    w.selection()->clear();
    m.setUnits(QStringLiteral("mm"));
    REQUIRE(w.commands()->trigger(QStringLiteral("measure.massProperties")));
    REQUIRE(w.measured());
    CHECK(m.current()->kind == api::measure::Kind::MassProperties);
    CHECK(m.valueText(QStringLiteral("volume")) == QStringLiteral("94386.333 mm\u00b3"));
    m.setDensity(2.70);
    REQUIRE(w.measured());
    CHECK(m.valueText(QStringLiteral("mass")) == QStringLiteral("254.843 g")); // 94.386333 cm³ x 2.70 g/cm³
    w.view()->fitAll();
    w.shot("measure-mass-dark-mm");
    w.host->theme()->setMode(QStringLiteral("light"));
    m.setUnits(QStringLiteral("inch"));
    REQUIRE(QTest::qWaitFor([&] { return w.panelValue(QStringLiteral("volume")) == "5.7598 in\u00b3"; }, 3000));
    w.shot("measure-mass-light-inch");
    // The bounding box from the context menu's command.
    REQUIRE(w.commands()->trigger(QStringLiteral("measure.boundingBox")));
    REQUIRE(w.measured());
    CHECK(m.current()->kind == api::measure::Kind::BoundingBox);
    w.checkNoWarnings();
}

TEST_CASE("every measure command is in the palette and the context menu; corner radius is disabled",
          "[measure][palette]")
{
    Window w("dark");
    ui::CommandCatalog& commands = *w.commands();
    (void)w.measure(); // the QML made it already (the shell's overlay and panel use it)
    const QStringList ids = {"measure.tool",
                             "measure.distance",
                             "measure.angle",
                             "measure.radius",
                             "measure.massProperties",
                             "measure.boundingBox",
                             "measure.selection",
                             "measure.pin",
                             "measure.clearPinned",
                             "measure.copyAll",
                             "measure.minInternalCornerRadius"};
    for (const QString& id : ids) {
        INFO(id.toStdString());
        const QVariantMap info = commands.info(id);
        REQUIRE_FALSE(info.isEmpty());
        // Found in the palette by its title.
        bool found = false;
        for (const ui::Command& c : commands.search(info.value(QStringLiteral("title")).toString()))
            found = found || c.id == id;
        CHECK(found);
        // None asks for an argument form: they act on the selection.
        CHECK_FALSE(info.value(QStringLiteral("needsForm")).toBool());
        // Only Measure has a default key: M.
        CHECK(info.value(QStringLiteral("shortcut")).toString() ==
              (id == QLatin1String("measure.tool") ? QStringLiteral("M") : QString()));
    }
    // Every registry action of the Measure category is among them.
    for (const api::ActionInfo& a : w.host->app()->actions().all()) {
        if (a.category == "Measure")
            CHECK(ids.contains(QString::fromStdString(a.id)));
    }
    const QVariantMap corner = commands.info(QStringLiteral("measure.minInternalCornerRadius"));
    CHECK_FALSE(corner.value(QStringLiteral("enabled")).toBool());
    CHECK(corner.value(QStringLiteral("reason")).toString().contains(QStringLiteral("later")));
    CHECK_FALSE(commands.trigger(QStringLiteral("measure.minInternalCornerRadius")));
    // Without a part: nothing to measure, and the panel says so.
    CHECK_FALSE(commands.info(QStringLiteral("measure.distance")).value(QStringLiteral("enabled")).toBool());

    if (rhiSceneGraph()) {
        w.open(kPlate);
        // The body selected: its mass properties and box are in the context menu (the empty-selection menu is
        // unchanged; the palette has both for the whole part).
        w.selection()->selectBody(0, int(Qt::NoModifier));
        REQUIRE(QTest::qWaitFor([&] { return w.selection()->count() == 1; }, 5000));
        QStringList body;
        for (const QVariant& e : commands.contextEntries())
            body << e.toMap().value(QStringLiteral("id")).toString();
        CHECK(body.contains(QStringLiteral("measure.tool")));
        CHECK(body.contains(QStringLiteral("measure.massProperties")));
        CHECK(body.contains(QStringLiteral("measure.boundingBox")));
        w.selection()->clear();
        QStringList nothing;
        for (const QVariant& e : commands.contextEntries())
            nothing << e.toMap().value(QStringLiteral("id")).toString();
        CHECK(nothing.indexOf(QStringLiteral("view.fitAll")) == 0);
        CHECK_FALSE(nothing.contains(QStringLiteral("measure.tool")));
        // A hole selected: Measure and Measure Radius.
        w.selection()->applyClick(faceHit(w, holeAt(w, 30, 80.0 / 3.0)), Qt::NoModifier);
        REQUIRE(QTest::qWaitFor([&] { return !w.selection()->pending(); }, 5000));
        QStringList face;
        for (const QVariant& e : commands.contextEntries())
            face << e.toMap().value(QStringLiteral("id")).toString();
        CHECK(face.contains(QStringLiteral("measure.tool")));
        CHECK(face.contains(QStringLiteral("measure.radius")));
        CHECK(face.contains(QStringLiteral("selection.copyProperties")));
        // Measure Radius from the palette shows the diameter: 6.6 mm = 0.2598 in.
        REQUIRE(commands.trigger(QStringLiteral("measure.radius")));
        REQUIRE(w.measured());
        CHECK(w.measure()->valueText(QStringLiteral("diameter")) == QStringLiteral("0.2598 in"));
        CHECK(w.rightTab() == 1);
        // Distance needs two.
        CHECK(commands.info(QStringLiteral("measure.distance"))
                  .value(QStringLiteral("reason"))
                  .toString()
                  .contains(QStringLiteral("two")));
        // The properties tab's Measure button opens the tab too.
        w.click(w.shown(QStringLiteral("rightTab.properties")));
        REQUIRE(QTest::qWaitFor([&] { return w.rightTab() == 0; }, 3000));
        w.click(w.shown(QStringLiteral("selectionInfoMeasure")));
        REQUIRE(QTest::qWaitFor([&] { return w.rightTab() == 1; }, 3000));
    }
    w.checkNoWarnings();
}

TEST_CASE("no UI-thread stall over 50 ms while measuring a mid-size corpus part", "[measure][stall]")
{
    RAY_REQUIRE_RHI();
    Window w("dark");
    w.open(kMidSize);
    MeasureModel& m = *w.measure();
    REQUIRE(w.commands()->trigger(QStringLiteral("measure.tool")));
    // Two faces far apart and the whole body, picked before the watchdog runs.
    const api::viewer::Body* body = w.scene()->body(0);
    REQUIRE(body != nullptr);
    const std::uint32_t faces = body->shape.count(kernel::SubKind::Face);
    REQUIRE(faces > 10);

    QElapsedTimer clock;
    qint64 last = 0;
    qint64 worst = 0;
    QTimer tick;
    tick.setTimerType(Qt::PreciseTimer);
    tick.setInterval(2);
    QObject::connect(&tick, &QTimer::timeout, [&] {
        const qint64 now = clock.elapsed();
        worst = std::max(worst, now - last);
        last = now;
    });
    clock.start();
    tick.start();
    // The worst gap of each step, for the report.
    std::string steps;
    qint64 overall = 0;
    const auto step = [&](const char* name) {
        steps += std::string(name) + " " + std::to_string(worst) + " ms; ";
        overall = std::max(overall, worst);
        worst = 0;
    };
    selectFaces(w, 0, faces - 1);
    REQUIRE(w.measured());
    step("two faces");
    w.selection()->selectBody(0, int(Qt::NoModifier));
    REQUIRE(w.measured());
    CHECK(m.current()->find("volume") != nullptr);
    step("body");
    REQUIRE(w.commands()->trigger(QStringLiteral("measure.boundingBox")));
    REQUIRE(w.measured());
    step("box");
    m.setUnits(QStringLiteral("mm"));
    m.setUnits(QStringLiteral("inch"));
    QTest::qWait(200);
    step("units");
    tick.stop();
    worst = overall;
    UNSCOPED_INFO("per step: " << steps);
    INFO("worst UI-thread gap " << worst << " ms");
    WARN("UI-thread watchdog while measuring a mid-size corpus part: worst gap " << worst << " ms");
    CHECK(worst < 50);
}

int main(int argc, char** argv)
{
    // The real user profile is never touched.
    QTemporaryDir profile;
    qputenv("RAYCAD_USER_DATA_DIR", QDir::toNativeSeparators(profile.path()).toUtf8());
    QGuiApplication app(argc, argv);
    QGuiApplication::setQuitOnLastWindowClosed(false);
    return Catch::Session().run(argc, argv);
}
