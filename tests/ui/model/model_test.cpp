// The feature tools, the feature tree and the parameter table in the desktop app (task T-098), on the real Qt Quick
// window with the real application session:
//
//   - a part by hand: a rectangle sketched on XY, extruded 10 mm into a new body through the Extrude panel; a circle
//     sketched on the top face (the T-092 sketch mode) and cut through, its region picked by a click in the view (the
//     Extrude offered after the sketch); an M6 tapped counterbored hole at a sketch point, the thread from the table;
//     two edges picked in the view and filleted. The feature list and the body's volume match the closed form; the
//     first extrude edited by a double-click in the tree changes the volume and every later feature still builds;
//     undo back to the empty document and redo to the end give equal state hashes
//   - the preview never commits and never blocks the UI thread; Cancel leaves the document hash as it was
//   - a fillet whose radius is too large shows the rebuild's message in the panel and, once added, in the tree
//   - a reorder that would put an extrude before its sketch is refused with a message
//   - a parameter used by a feature: renaming it rewrites the feature's expression; deleting it is refused, naming
//     the feature
//   - the view renders nothing while idle (with and without a tool open)
//
// Screenshots: with RAY_T098_SHOTS set to a folder, the window is saved there (every tool's panel, the finished part).
#include "../scene_graph_probe.h"
#include "../ui_budget.h"
#include "../ui_watchdog.h"
#include "../window_focus.h"
#include "app_context.h"
#include "app_host.h"
#include "app_options.h"
#include "files.h"
#include "model/feature_tools.h"
#include "model/feature_tree.h"
#include "model/params.h"
#include "palette_commands.h"
#include "scene_controller.h"
#include "selection_model.h"
#include "session.h"
#include "shortcuts.h"
#include "sketch/sketch_mode.h"
#include "sketch/sketch_overlay.h"
#include "ui_commands.h"
#include "viewport_item.h"
#include "viewport_navigation.h"

#include <ray_api/sketch_edit.h>
#include <ray_api/viewer.h>
#include <ray_render/viewport.h>

#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>
#include <QtGui/QMouseEvent>
#include <QtQml/QQmlApplicationEngine>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>
#include <QtTest/QTest>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace ray;
using Catch::Approx;
using ray::ui::AppHost;
using ray::ui::AppOptions;
using ray::ui::FeatureToolModel;
using ray::ui::FeatureTreeModel;
using ray::ui::ParamTableModel;
using ray::ui::SketchModel;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kTimeoutMs = 60000;

struct Window {
    QTemporaryDir cache;
    QTemporaryDir userData;
    std::unique_ptr<AppHost> host;

    Window()
    {
        AppOptions options;
        options.theme = QStringLiteral("dark");
        options.glass = false;
        options.size = QSize(1440, 900);
        host = std::make_unique<AppHost>(options, cache.path());
        REQUIRE(host->optionError().isEmpty());
        REQUIRE(host->load());
        host->show();
        REQUIRE(QTest::qWaitForWindowExposed(host->window()));
        // Keys and window shortcuts go to Qt's focus window: taken whether or not the desktop grants it
        // (window_focus.h).
        REQUIRE(ray::test::focusWindow(host->window()));
        REQUIRE(host->viewport() != nullptr);
        REQUIRE(host->startSession(userData.path().toStdString()));
        host->files()->setUseNativeDialogs(false);
        host->viewport()->setAnimationsEnabled(false);
        host->scene()->setPreset(render::presetFor(render::Tier::Medium));
        grab();
    }

    QQuickWindow* window() const { return host->window(); }
    ui::ViewportItem* view() const { return host->viewport(); }
    template <class T>
    T* singleton(const char* name) const
    {
        auto* s = host->engine()->singletonInstance<T*>(QStringLiteral("RayCad.Ui"), QString::fromLatin1(name));
        REQUIRE(s != nullptr);
        return s;
    }
    FeatureToolModel* tool() const { return singleton<FeatureToolModel>("FeatureTool"); }
    FeatureTreeModel* tree() const { return singleton<FeatureTreeModel>("FeatureTree"); }
    ParamTableModel* params() const { return singleton<ParamTableModel>("Parameters"); }
    SketchModel* sketch() const { return singleton<SketchModel>("SketchTool"); }
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
    void sendMouse(QEvent::Type type, QPointF scenePos, Qt::MouseButton button, Qt::MouseButtons buttons,
                   Qt::KeyboardModifiers mods = Qt::NoModifier) const
    {
        QMouseEvent e(type, scenePos, window()->mapToGlobal(scenePos), button, buttons, mods);
        QGuiApplication::sendEvent(window(), &e);
    }
    void clickScene(QPointF p, Qt::KeyboardModifiers mods = Qt::NoModifier) const
    {
        sendMouse(QEvent::MouseMove, p, Qt::NoButton, Qt::NoButton, mods);
        QCoreApplication::processEvents();
        sendMouse(QEvent::MouseButtonPress, p, Qt::LeftButton, Qt::LeftButton, mods);
        sendMouse(QEvent::MouseButtonRelease, p, Qt::LeftButton, Qt::NoButton, mods);
        QCoreApplication::processEvents();
    }
    void click(QQuickItem* item) const
    {
        REQUIRE(item != nullptr);
        grab();
        clickScene(item->mapToScene(QPointF(item->width() / 2, item->height() / 2)));
    }
    void click(const char* objectName) const { click(find(QString::fromLatin1(objectName))); }
    void doubleClick(QQuickItem* item) const
    {
        REQUIRE(item != nullptr);
        grab();
        const QPointF c = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
        sendMouse(QEvent::MouseMove, c, Qt::NoButton, Qt::NoButton);
        QTest::mouseDClick(window(), Qt::LeftButton, Qt::NoModifier, c.toPoint());
        QCoreApplication::processEvents();
    }
    // Types into a field of the open tool's panel, as a person would (select all, type).
    void typeInto(const QString& key, const QString& text) const
    {
        QQuickItem* field = nullptr;
        REQUIRE(QTest::qWaitFor(
            [&] { return (field = find(QStringLiteral("featureInput.") + key)) && field->isVisible(); }, 5000));
        field->forceActiveFocus();
        QMetaObject::invokeMethod(field, "selectAll");
        for (const QChar c : text)
            QTest::keyClick(window(), c.toLatin1());
        QCoreApplication::processEvents();
    }
    // A world point in window coordinates.
    QPointF sceneOf(Vec3 world) const
    {
        const auto v = ui::nav::project(view()->camera(), world, view()->width(), view()->height());
        REQUIRE(v.has_value());
        return view()->mapToScene(*v);
    }
    bool run(const char* id) const { return host->commands()->trigger(QString::fromLatin1(id)); }
    doc::Document* document() const
    {
        api::OpenDocument* od = host->app()->active();
        REQUIRE(od != nullptr);
        return od->document.get();
    }
    std::string stateHash() const { return document()->state()->stateHash().hex(); }
    void newDocument() const
    {
        host->files()->newPart();
        REQUIRE(QTest::qWaitFor([&] { return host->session()->hasDocument(); }, 20000));
        grab();
    }
    // The document's rebuild, the scene's meshes and the tree's jobs are done.
    // The rebuild's notification is posted when it ends: it can give the scene a new body set (meshing again) during
    // the pause after the first wait, so the state is checked once more after it.
    void settle() const
    {
        const auto idle = [&] {
            doc::Document* d = document();
            return d != nullptr && !d->rebuildPending() && !host->scene()->meshing() && !tree()->busy() &&
                   sketch()->inFlight() == 0 && !sketch()->solving();
        };
        for (int pass = 0; pass < 2; ++pass) {
            REQUIRE(QTest::qWaitFor(idle, kTimeoutMs));
            QTest::qWait(50);
        }
        REQUIRE(QTest::qWaitFor(idle, kTimeoutMs));
    }
    // The open tool's preview is built and shown.
    void previewed(const char* status = "ok") const
    {
        const bool done = QTest::qWaitFor(
            [&] {
                FeatureToolModel* t = tool();
                return !t->previewBusy() && t->previewStatus() == QLatin1String(status);
            },
            kTimeoutMs);
        INFO("the " << tool()->type().toStdString() << " preview: builds " << tool()->previewBuilds() << ", status '"
                    << tool()->previewStatus().toStdString() << "', busy " << tool()->previewBusy() << ", meshing "
                    << tool()->overlay().meshing() << ", message '" << tool()->message().toStdString() << "'");
        REQUIRE(done);
        CHECK(tool()->previewStatus() == QLatin1String(status));
    }
    void commit() const
    {
        const std::size_t before = document()->undoCount();
        REQUIRE(tool()->canCommit());
        click("featureOk");
        const bool done =
            QTest::qWaitFor([&] { return !tool()->active() && document()->undoCount() == before + 1; }, kTimeoutMs);
        INFO("commit: tool active " << tool()->active() << ", committing " << tool()->committing() << ", jobs "
                                    << host->app()->activeJobs().size() << ", status '"
                                    << tool()->previewStatus().toStdString() << "', message '"
                                    << tool()->message().toStdString() << "', undo steps " << document()->undoCount()
                                    << " (before " << before << ")");
        REQUIRE(done);
        settle();
    }
    bool invoke(const char* action, const std::string& args) const
    {
        const std::size_t before = document()->undoCount();
        JobHandle<api::ActionResult> job = host->app()->actions().invoke(action, args, api::Caller::User);
        REQUIRE(QTest::qWaitFor([&] { return job.done(); }, kTimeoutMs));
        auto r = job.wait();
        if (!r)
            UNSCOPED_INFO(action << ": " << r.error().message);
        if (r)
            REQUIRE(QTest::qWaitFor([&] { return document()->undoCount() == before + 1; }, kTimeoutMs));
        return r.has_value();
    }
    void undo(bool redo = false) const
    {
        const std::size_t before = document()->undoCount();
        REQUIRE(host->context()->invoke(redo ? QStringLiteral("edit.redo") : QStringLiteral("edit.undo")));
        REQUIRE(QTest::qWaitFor([&] { return document()->undoCount() != before; }, kTimeoutMs));
    }
    double volume(int body = 0) const
    {
        const api::viewer::Body* b = host->scene()->body(body);
        REQUIRE(b != nullptr);
        JobContext ctx;
        auto info = api::viewer::describeElement(b->shape, kernel::SubId{kernel::SubKind::Solid, 0}, ctx);
        REQUIRE(info);
        REQUIRE(info->mass.has_value());
        return info->mass->volume;
    }
    std::vector<std::string> featureTypes() const
    {
        std::vector<std::string> out;
        for (const doc::Feature& f : document()->state()->features)
            out.push_back(f.type);
        return out;
    }
    QString status(int row) const { return tree()->get(row).value(QStringLiteral("status")).toString(); }
    void shot(const char* name) const
    {
        const QString dir = qEnvironmentVariable("RAY_T098_SHOTS");
        if (dir.isEmpty())
            return;
        QDir().mkpath(dir);
        QTest::qWait(300);
        grab().save(QDir(dir).filePath(QString::fromLatin1(name) + QStringLiteral(".png")), "PNG");
    }
    void checkNoWarnings() const
    {
        for (const QString& warning : host->warnings())
            UNSCOPED_INFO(warning.toStdString());
        CHECK(host->warnings().isEmpty());
    }
    // Frames swapped over `ms` of idle time.
    int framesOver(int ms) const
    {
        int swapped = 0;
        auto c =
            QObject::connect(window(), &QQuickWindow::frameSwapped, window(), [&] { ++swapped; }, Qt::DirectConnection);
        QTest::qWait(ms);
        QObject::disconnect(c);
        return swapped;
    }
};

// A rectangle sketch (w x h mm, corner at the origin) on XY through the document's actions; its feature id.
FeatureId rectangleSketch(const Window& w, double width, double height)
{
    sketch::Sketch m;
    std::vector<SketchEntId> p;
    const Vec2 corners[] = {{0, 0}, {width, 0}, {width, height}, {0, height}};
    for (const Vec2 c : corners) {
        p.push_back(SketchEntId{Uuid::generate()});
        m.entities.push_back(sketch::Entity{p.back(), sketch::Point{c}, false});
    }
    for (int i = 0; i < 4; ++i)
        m.entities.push_back(sketch::Entity{SketchEntId{Uuid::generate()}, sketch::Line{p[i], p[(i + 1) % 4]}, false});
    std::sort(m.entities.begin(), m.entities.end(),
              [](const sketch::Entity& a, const sketch::Entity& b) { return a.id < b.id; });
    const FeatureId id{Uuid::generate()};
    REQUIRE(
        w.invoke("feature.sketch.add", api::sketch_edit::sketchAddArgs(id, "Sketch 1", "{\"principal\":\"xy\"}", m)));
    return id;
}

// The index of a planar face of body 0 whose centroid is at height z.
std::uint32_t faceAtZ(const Window& w, double z)
{
    const api::viewer::Body* body = w.host->scene()->body(0);
    REQUIRE(body != nullptr);
    JobContext ctx;
    std::optional<std::uint32_t> found;
    for (std::uint32_t i = 0; i < body->shape.count(kernel::SubKind::Face) && !found; ++i) {
        auto info = api::viewer::describeElement(body->shape, kernel::SubId{kernel::SubKind::Face, i}, ctx);
        REQUIRE(info);
        if (info->surface && info->surface->type == kernel::SurfaceType::Plane &&
            std::abs(info->surface->centroid.z - z) < 1e-6 && info->surface->axis.z > 0.5)
            found = i;
    }
    if (!found)
        FAIL("no planar face at that height");
    return *found;
}

// The straight edge of body 0 along X at (y, z); its index and midpoint.
std::pair<std::uint32_t, Vec3> edgeAlongX(const Window& w, double y, double z)
{
    const api::viewer::Body* body = w.host->scene()->body(0);
    REQUIRE(body != nullptr);
    JobContext ctx;
    std::optional<std::pair<std::uint32_t, Vec3>> found;
    for (std::uint32_t i = 0; i < body->shape.count(kernel::SubKind::Edge) && !found; ++i) {
        auto info = api::viewer::describeElement(body->shape, kernel::SubId{kernel::SubKind::Edge, i}, ctx);
        REQUIRE(info);
        if (!info->curve || info->curve->type != kernel::CurveType::Line)
            continue;
        const kernel::CurveInfo& c = *info->curve;
        if (std::abs(c.start.y - y) < 1e-6 && std::abs(c.end.y - y) < 1e-6 && std::abs(c.start.z - z) < 1e-6 &&
            std::abs(c.end.z - z) < 1e-6 && std::abs(c.start.x - c.end.x) > 1)
            found = std::pair<std::uint32_t, Vec3>{i, Vec3{(c.start.x + c.end.x) / 2, y, z}};
    }
    if (!found)
        FAIL("no edge there");
    return *found;
}

// Opens the tool and waits for its panel.
void openTool(const Window& w, const char* command, const char* type)
{
    REQUIRE(w.run(command));
    REQUIRE(QTest::qWaitFor([&] { return w.tool()->active() && w.tool()->type() == QLatin1String(type); }, 5000));
    QQuickItem* panel = nullptr;
    REQUIRE(
        QTest::qWaitFor([&] { return (panel = w.find(QStringLiteral("featurePanel"))) && panel->isVisible(); }, 5000));
}

// The first extrude of a new document: a 40 x 25 mm rectangle, `distance` typed into the panel.
void firstExtrude(Window& w, const char* distance)
{
    rectangleSketch(w, 40, 25);
    w.settle();
    openTool(w, "model.tool.extrude", "extrude");
    CHECK(w.tool()->values().value(QStringLiteral("operation")).toString() == QStringLiteral("new"));
    w.typeInto(QStringLiteral("distance"), QString::fromLatin1(distance));
    w.previewed();
    w.view()->fitAll();
}

} // namespace

TEST_CASE("a part by hand: extrude, cut, hole, fillet; edit; undo and redo", "[model][e2e][wallclock]")
{
    RAY_REQUIRE_RHI();
    Window w;
    w.newDocument();
    const std::string emptyHash = w.stateHash();

    // ---- Extrude 10 mm, new body ----
    firstExtrude(w, "10 mm");
    CHECK(w.featureTypes() == std::vector<std::string>{"sketch"}); // the preview commits nothing
    REQUIRE(w.tool()->overlay().itemCount() == 1);
    CHECK(!w.tool()->handles().isEmpty());
    w.shot("t098_extrude_panel");
    w.commit();
    REQUIRE(w.host->scene()->bodyCount() == 1);
    CHECK(w.volume() == Approx(40.0 * 25 * 10).epsilon(1e-6));
    // The commit is the previewed command: the document's rebuild takes the extrude from the preview's (a cache hit).
    REQUIRE(w.document()->build());
    CHECK(w.document()->build()->reports.back().cacheHit);

    // ---- A circle on the top face (the sketch mode), then the Extrude it offers: a cut through ----
    const std::uint32_t top = faceAtZ(w, 10);
    w.host->selection()->applyClick(
        render::PickHit{w.host->scene()->itemForBody(0), kernel::SubKind::Face, top, Vec3{20, 12.5, 10}, 0},
        Qt::NoModifier);
    REQUIRE(w.run("sketch.mode.new"));
    QQuickItem* face = nullptr;
    REQUIRE(QTest::qWaitFor([&] { return (face = w.find(QStringLiteral("sketchPlane.face"))) != nullptr; }, 5000));
    w.click(face);
    REQUIRE(QTest::qWaitFor([&] { return w.sketch()->active(); }, 20000));
    REQUIRE(w.run("sketch.tool.circle"));
    {
        const auto at = w.sketch()->toView(Vec2{12, 12.5});
        REQUIRE(at.has_value());
        // The view looks down at the face; frame the rectangle so the circle is a good size on screen.
        w.view()->setCamera(w.view()->framed(w.view()->camera(), Box3{{-5, -5, 10}, {45, 30, 10}}));
        w.grab();
        const auto centre = w.sketch()->toView(Vec2{12, 12.5});
        const auto rim = w.sketch()->toView(Vec2{15, 12.5});
        REQUIRE(centre.has_value());
        REQUIRE(rim.has_value());
        w.clickScene(w.view()->mapToScene(*centre));
        w.sendMouse(QEvent::MouseMove, w.view()->mapToScene(*rim), Qt::NoButton, Qt::NoButton);
        QCoreApplication::processEvents();
        for (const char c : {'1', '0', 'm', 'm'}) // diameter 10 mm (the document is in inches)
            QTest::keyClick(w.window(), c);
        QTest::keyClick(w.window(), Qt::Key_Return);
    }
    REQUIRE(QTest::qWaitFor([&] { return w.sketch()->inFlight() == 0 && !w.sketch()->solving(); }, 20000));
    const std::optional<FeatureId> circleSketch = w.sketch()->featureId();
    REQUIRE(circleSketch.has_value());
    REQUIRE(w.run("sketch.mode.finish"));
    w.settle();
    // The sketch is shown in the view outside the sketch mode.
    CHECK(w.tree()->sketchLines().has(QStringLiteral("sketch:") +
                                      QString::fromStdString(circleSketch->value.toString()) + QStringLiteral(":g")));
    REQUIRE(QTest::qWaitFor([&] { return !w.tool()->offerSketch().isEmpty(); }, 5000));
    w.shot("t098_extrude_offer");
    w.click("extrudeOfferAccept");
    REQUIRE(QTest::qWaitFor([&] { return w.tool()->active(); }, 5000));
    CHECK(w.tool()->values().value(QStringLiteral("sketch")).toString() ==
          QString::fromStdString(circleSketch->value.toString()));
    w.tool()->setValue(QStringLiteral("operation"), QStringLiteral("cut"));
    CHECK(w.tool()->values().value(QStringLiteral("flip")).toBool()); // a cut from a face goes into the body
    w.typeInto(QStringLiteral("distance"), QStringLiteral("15 mm"));
    REQUIRE(QTest::qWaitFor([&] { return w.tool()->sketchPlane().has_value() && !w.tool()->regions().empty(); },
                            kTimeoutMs));
    // Pick the circle's region by a click in the view.
    w.view()->setStandardView(QStringLiteral("top"));
    w.grab();
    const kernel::Plane plane = *w.tool()->sketchPlane();
    // Inside the circle, to the right of its centre: clear of the panel (taller since it has the End row).
    w.clickScene(w.sceneOf(Vec3{plane.origin.x + 16, plane.origin.y + 12.5, plane.origin.z}));
    REQUIRE(QTest::qWaitFor([&] { return w.tool()->pickCount() == 1; }, 5000));
    w.previewed();
    w.view()->setStandardView(QStringLiteral("iso"));
    w.shot("t098_cut_panel");
    w.commit();
    const double cut = kPi * 5 * 5 * 10;
    CHECK(w.volume() == Approx(10000 - cut).epsilon(1e-5));

    // ---- A hole at a sketch point: M6 tapped, counterbored ----
    {
        auto planeJson = api::sketch_edit::planeJsonOf(*w.document()->state(), *circleSketch);
        REQUIRE(planeJson);
        sketch::Sketch m;
        m.entities.push_back(sketch::Entity{SketchEntId{Uuid::generate()}, sketch::Point{Vec2{30, 12.5}}, false});
        REQUIRE(w.invoke("feature.sketch.add",
                         api::sketch_edit::sketchAddArgs(FeatureId{Uuid::generate()}, "Hole points", *planeJson, m)));
        w.settle();
    }
    openTool(w, "model.tool.hole", "hole");
    REQUIRE(QTest::qWaitFor([&] { return !w.tool()->threadSeries().isEmpty(); }, kTimeoutMs));
    w.tool()->setValue(QStringLiteral("kind"), QStringLiteral("counterbore"));
    w.tool()->setValue(QStringLiteral("tapped"), true);
    w.tool()->setValue(QStringLiteral("series"), QStringLiteral("iso_metric_coarse"));
    bool m6 = false;
    for (const QVariant& s : w.tool()->threadSizes())
        m6 = m6 || s.toMap().value(QStringLiteral("designation")).toString() == QStringLiteral("M6x1");
    REQUIRE(m6);
    w.tool()->setValue(QStringLiteral("size"), QStringLiteral("M6x1"));
    w.tool()->setValue(QStringLiteral("end"), QStringLiteral("through"));
    w.typeInto(QStringLiteral("cboreDiameter"), QStringLiteral("11 mm"));
    w.typeInto(QStringLiteral("cboreDepth"), QStringLiteral("6 mm"));
    w.previewed();
    w.shot("t098_hole_panel");
    w.commit();
    {
        // The thread is stored for CAM.
        const doc::Feature& hole = w.document()->state()->features.back();
        const QJsonObject p = QJsonDocument::fromJson(QByteArray::fromStdString(hole.propsJson)).object();
        CHECK(p.value(QStringLiteral("kind")).toString() == QStringLiteral("counterbore_tapped"));
        CHECK(p.value(QStringLiteral("thread")).toObject().value(QStringLiteral("designation")).toString() ==
              QStringLiteral("M6x1"));
    }
    const double tapDrill = 5.0; // M6x1 (resources/standards/threads.json)
    const double hole = kPi * (5.5 * 5.5 * 6 + (tapDrill / 2) * (tapDrill / 2) * 4);
    CHECK(w.volume() == Approx(10000 - cut - hole).epsilon(1e-5));

    // ---- Fillet two top edges picked in the view ----
    openTool(w, "model.tool.fillet", "fillet");
    CHECK(w.tool()->picking());
    const auto e1 = edgeAlongX(w, 0, 10);
    const auto e2 = edgeAlongX(w, 25, 10);
    const render::ItemId item = w.host->scene()->itemForBody(0);
    w.host->selection()->applyClick(render::PickHit{item, kernel::SubKind::Edge, e1.first, e1.second, 0},
                                    Qt::NoModifier);
    w.host->selection()->applyClick(render::PickHit{item, kernel::SubKind::Edge, e2.first, e2.second, 0},
                                    Qt::ShiftModifier);
    REQUIRE(QTest::qWaitFor([&] { return w.tool()->pickCount() == 2; }, kTimeoutMs));
    w.typeInto(QStringLiteral("radius"), QStringLiteral("2 mm"));
    w.previewed();
    CHECK(!w.tool()->handles().isEmpty());
    w.shot("t098_fillet_panel");
    w.commit();
    const double fillets = 2 * (1 - kPi / 4) * 2 * 2 * 40;
    const double finalVolume = 10000 - cut - hole - fillets;
    CHECK(w.volume() == Approx(finalVolume).epsilon(1e-4));
    CHECK(w.featureTypes() ==
          std::vector<std::string>{"sketch", "extrude", "sketch", "extrude", "sketch", "hole", "fillet"});
    for (int i = 0; i < w.tree()->count(); ++i)
        CHECK(w.status(i) == QStringLiteral("ok"));
    w.view()->setStandardView(QStringLiteral("iso"));
    w.view()->fitAll();
    w.shot("t098_finished_part");
    const std::string finished = w.stateHash();

    // ---- Idle: nothing renders ----
    w.settle();
    QTest::qWait(500);
    CHECK(w.framesOver(1000) == 0);

    // ---- Rolled back to after Extrude 1 (document state, one undo step): the block alone is built and shown, later
    // features are greyed; a feature added now goes in at the bar, which moves past it; undo restores the part ----
    {
        const std::size_t undo = w.document()->undoCount();
        REQUIRE(w.tree()->rollTo(1));
        REQUIRE(QTest::qWaitFor([&] { return w.document()->undoCount() == undo + 1 && w.tree()->rollbackIndex() == 1; },
                                kTimeoutMs));
        w.settle();
        CHECK(w.document()->state()->rollback.has_value());
        CHECK(!w.tree()->get(1).value(QStringLiteral("rolledBack")).toBool());
        CHECK(w.tree()->get(2).value(QStringLiteral("rolledBack")).toBool());
        CHECK(w.status(2) == QStringLiteral("rolledBack"));
        REQUIRE(w.host->scene()->bodyCount() == 1);
        CHECK(w.volume() == Approx(10000).epsilon(1e-6)); // the block before the cut
        CHECK(w.host->scene()->meshOf(0)->faces.size() == 6);
        CHECK(w.stateHash() != finished); // document state: saved and undone with the rest
        w.shot("t098_rollback");
        // A datum plane added while rolled back lands right after the bar.
        openTool(w, "model.tool.datumPlane", "datum_plane");
        w.typeInto(QStringLiteral("offset"), QStringLiteral("5 mm"));
        w.previewed();
        w.commit();
        CHECK(w.featureTypes() == std::vector<std::string>{"sketch", "extrude", "datum_plane", "sketch", "extrude",
                                                           "sketch", "hole", "fillet"});
        CHECK(w.tree()->rollbackIndex() == 2);
        CHECK(w.volume() == Approx(10000).epsilon(1e-6));
        w.shot("rollback_insert");
        w.undo(); // the datum plane
        w.settle();
        CHECK(w.tree()->rollbackIndex() == 1);
        w.undo(); // the rollback
        w.settle();
        CHECK(w.tree()->rollbackIndex() == -1);
        CHECK(w.stateHash() == finished);
        CHECK(w.volume() == Approx(finalVolume).epsilon(1e-4));
    }

    // ---- Sketches in the view: the cut's sketch is hidden once used; the eye shows it again ----
    {
        const QString key =
            QStringLiteral("sketch:") + QString::fromStdString(circleSketch->value.toString()) + QStringLiteral(":g");
        CHECK(!w.tree()->sketchLines().has(key));
        w.tree()->setSketchShown(2, true);
        REQUIRE(QTest::qWaitFor([&] { return !w.tree()->busy() && w.tree()->sketchLines().has(key); }, kTimeoutMs));
        w.tree()->setSketchShown(2, false);
        REQUIRE(QTest::qWaitFor([&] { return !w.tree()->busy() && !w.tree()->sketchLines().has(key); }, kTimeoutMs));
    }

    // ---- A reorder that puts the extrude before its sketch is refused ----
    CHECK(!w.tree()->moveTo(1, 0));
    CHECK(!w.tree()->message().isEmpty());
    CHECK(w.stateHash() == finished);
    w.tree()->dismissMessage();

    // ---- Edit the first extrude by a double-click in the tree: 12 mm ----
    QQuickItem* row = nullptr;
    REQUIRE(QTest::qWaitFor([&] { return (row = w.find(QStringLiteral("featureRow.1"))) && row->isVisible(); }, 5000));
    w.doubleClick(row);
    REQUIRE(QTest::qWaitFor([&] { return w.tool()->active() && w.tool()->editing(); }, 5000));
    CHECK(w.tool()->type() == QStringLiteral("extrude"));
    w.previewed();
    // The preview never commits, and never stalls the UI thread while the values change.
    {
        const std::size_t undoBefore = w.document()->undoCount();
        ray::test::UiWatchdog dog; // ui_watchdog.h
        dog.start();
        for (const char* d : {"11 mm", "11.5 mm", "13 mm", "12 mm"}) {
            w.typeInto(QStringLiteral("distance"), QString::fromLatin1(d));
            ray::test::waitMs(60);
        }
        // As w.previewed(), without sleeping the UI thread between passes of the event loop.
        CHECK(ray::test::waitFor([&] { return !w.tool()->previewBusy(); }, kTimeoutMs));
        dog.stop();
        w.previewed();
        WARN("UI-thread watchdog while the extrude preview rebuilt the part: worst "
             << dog.worst().describe() << "; longest gap " << dog.worstWall().describe() << "; "
             << w.tool()->previewBuilds() << " preview builds");
        CHECK(dog.worstStallMs() < ray::test::uiBudgetMs(50));
        CHECK(w.document()->undoCount() == undoBefore);
        CHECK(w.stateHash() == finished);
    }
    w.commit();
    const double edited = 40.0 * 25 * 12 - kPi * 25 * 12 - kPi * (5.5 * 5.5 * 6 + 2.5 * 2.5 * 6) - fillets;
    CHECK(w.volume() == Approx(edited).epsilon(1e-4));
    for (int i = 0; i < w.tree()->count(); ++i)
        CHECK(w.status(i) == QStringLiteral("ok"));
    const std::string end = w.stateHash();

    // ---- Undo to the empty document, redo to the end ----
    while (w.document()->undoCount() > 0)
        w.undo();
    w.settle();
    CHECK(w.stateHash() == emptyHash);
    while (w.document()->redoCount() > 0)
        w.undo(true);
    w.settle();
    CHECK(w.stateHash() == end);
    CHECK(w.volume() == Approx(edited).epsilon(1e-4));
    w.checkNoWarnings();
}

TEST_CASE("cancel changes nothing; a failed fillet shows its message in the panel and the tree", "[model][fail]")
{
    RAY_REQUIRE_RHI();
    Window w;
    w.newDocument();
    firstExtrude(w, "10 mm");
    w.commit();
    const std::string hash = w.stateHash();
    const std::size_t undo = w.document()->undoCount();

    // The extrude's arrow handle: dragging it outwards lengthens the distance (the field and the preview follow).
    w.tree()->setCurrentIndex(1);
    REQUIRE(w.run("model.feature.edit"));
    REQUIRE(QTest::qWaitFor([&] { return w.tool()->active() && w.tool()->editing(); }, 5000));
    w.previewed();
    QQuickItem* knob = nullptr;
    REQUIRE(QTest::qWaitFor(
        [&] { return (knob = w.find(QStringLiteral("featureHandle.distance"))) && knob->isVisible(); }, 5000));
    {
        const QVariantMap h = w.tool()->handles().front().toMap();
        const QPointF dir(h.value(QStringLiteral("tipX")).toDouble() - h.value(QStringLiteral("x")).toDouble(),
                          h.value(QStringLiteral("tipY")).toDouble() - h.value(QStringLiteral("y")).toDouble());
        const double len = std::hypot(dir.x(), dir.y());
        REQUIRE(len > 1);
        w.grab();
        const QPointF from = knob->mapToScene(QPointF(knob->width() / 2, knob->height() / 2));
        const QPointF step = dir / len * 8.0;
        w.sendMouse(QEvent::MouseMove, from, Qt::NoButton, Qt::NoButton);
        w.sendMouse(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
        QPointF at = from;
        for (int i = 0; i < 6; ++i) {
            at += step;
            w.sendMouse(QEvent::MouseMove, at, Qt::LeftButton, Qt::LeftButton);
            QTest::qWait(30);
        }
        w.sendMouse(QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton);
    }
    const QString dragged = w.tool()->values().value(QStringLiteral("distance")).toString();
    INFO("distance after the drag: " << dragged.toStdString());
    CHECK(dragged != QStringLiteral("10 mm"));
    w.previewed();
    CHECK(w.tool()->overlay().itemCount() == 1);
    // Longer than before: the preview's body is taller than 10 mm.
    {
        const std::shared_ptr<const kernel::Mesh> mesh = w.tool()->overlay().meshes().front();
        double zMax = 0;
        for (std::size_t i = 2; i < mesh->positions.size(); i += 3)
            zMax = std::max(zMax, static_cast<double>(mesh->positions[i]));
        CHECK(zMax > 10.5);
    }
    w.shot("t098_extrude_handle_drag");
    w.tool()->cancel();
    CHECK(w.stateHash() == hash);

    // Every tool opened and cancelled: the document is as it was.
    w.tree()->setCurrentIndex(0); // the sketch
    openTool(w, "model.tool.revolve", "revolve");
    w.previewed("ok");
    w.shot("t098_revolve_panel");
    w.tool()->cancel();
    openTool(w, "model.tool.datumPlane", "datum_plane");
    w.typeInto(QStringLiteral("offset"), QStringLiteral("20 mm"));
    w.previewed();
    w.shot("t098_datum_panel");
    w.click("featureCancel");
    REQUIRE(QTest::qWaitFor([&] { return !w.tool()->active(); }, 5000));
    {
        const auto e = edgeAlongX(w, 0, 10);
        w.host->selection()->applyClick(
            render::PickHit{w.host->scene()->itemForBody(0), kernel::SubKind::Edge, e.first, e.second, 0},
            Qt::NoModifier);
    }
    openTool(w, "model.tool.chamfer", "chamfer");
    const bool picked = QTest::qWaitFor([&] { return w.tool()->pickCount() == 1; }, kTimeoutMs);
    INFO("selected " << w.host->selection()->count() << ", picking " << w.tool()->picking() << ", message "
                     << w.tool()->message().toStdString() << ", picks " << w.tool()->pickCount());
    REQUIRE(picked);
    w.previewed();
    w.shot("t098_chamfer_panel");
    // Idle with a tool open and its preview shown: nothing renders.
    QTest::qWait(500);
    CHECK(w.framesOver(1000) == 0);
    w.tool()->cancel();
    w.settle();
    CHECK(w.stateHash() == hash);
    CHECK(w.document()->undoCount() == undo);

    // A fillet too large for the 10 mm plate.
    {
        const auto e = edgeAlongX(w, 0, 10);
        w.host->selection()->applyClick(
            render::PickHit{w.host->scene()->itemForBody(0), kernel::SubKind::Edge, e.first, e.second, 0},
            Qt::NoModifier);
    }
    openTool(w, "model.tool.fillet", "fillet");
    REQUIRE(QTest::qWaitFor([&] { return w.tool()->pickCount() == 1; }, kTimeoutMs));
    w.typeInto(QStringLiteral("radius"), QStringLiteral("50 mm"));
    w.previewed("failed");
    CHECK(!w.tool()->message().isEmpty());
    w.shot("t098_fillet_failed_panel");
    w.commit(); // a feature that fails to build can be added; the tree says why
    const int last = w.tree()->count() - 1;
    CHECK(w.status(last) == QStringLiteral("failed"));
    CHECK(!w.tree()->get(last).value(QStringLiteral("message")).toString().isEmpty());
    QQuickItem* details = nullptr;
    REQUIRE(QTest::qWaitFor(
        [&] { return (details = w.find(QStringLiteral("featureDetails.%1").arg(last))) && details->isVisible(); },
        5000));
    w.shot("t098_failed_feature_tree");
    w.checkNoWarnings();
}

TEST_CASE("parameters: a rename rewrites the expressions that use it; a used parameter cannot be deleted",
          "[model][params]")
{
    RAY_REQUIRE_RHI();
    Window w;
    w.newDocument();
    firstExtrude(w, "10 mm");
    w.commit();
    ParamTableModel* params = w.params();

    std::size_t undo = w.document()->undoCount();
    REQUIRE(params->add(QStringLiteral("thk"), QStringLiteral("8 mm"), QStringLiteral("length")));
    REQUIRE(QTest::qWaitFor([&] { return params->count() == 1 && w.document()->undoCount() == undo + 1; }, kTimeoutMs));

    // The extrude takes the parameter.
    w.tree()->setCurrentIndex(1);
    REQUIRE(w.run("model.feature.edit"));
    REQUIRE(QTest::qWaitFor([&] { return w.tool()->active() && w.tool()->editing(); }, 5000));
    w.typeInto(QStringLiteral("distance"), QStringLiteral("thk"));
    w.previewed();
    w.commit();
    w.settle();
    CHECK(w.volume() == Approx(40.0 * 25 * 8).epsilon(1e-6));
    const auto distanceOf = [&] {
        const doc::Feature& f = w.document()->state()->features[1];
        return QJsonDocument::fromJson(QByteArray::fromStdString(f.propsJson))
            .object()
            .value(QStringLiteral("distance"))
            .toString();
    };
    CHECK(distanceOf() == QStringLiteral("thk"));

    // Rename: one step, the extrude's expression follows.
    undo = w.document()->undoCount();
    REQUIRE(params->rename(0, QStringLiteral("thickness")));
    REQUIRE(QTest::qWaitFor([&] { return w.document()->undoCount() == undo + 1; }, kTimeoutMs));
    w.settle();
    CHECK(params->rowOf(QStringLiteral("thickness")) == 0);
    CHECK(distanceOf() == QStringLiteral("thickness"));
    CHECK(w.status(1) == QStringLiteral("ok"));

    // Delete while used: refused, naming the user.
    undo = w.document()->undoCount();
    REQUIRE(params->remove(0));
    REQUIRE(QTest::qWaitFor([&] { return !params->busy(); }, kTimeoutMs));
    CHECK(params->message().contains(QStringLiteral("Extrude 1")));
    CHECK(params->count() == 1);
    CHECK(w.document()->undoCount() == undo);

    // A new value through the table changes the part.
    REQUIRE(params->setExpression(0, QStringLiteral("5 mm")));
    REQUIRE(QTest::qWaitFor([&] { return w.document()->undoCount() == undo + 1; }, kTimeoutMs));
    w.settle();
    CHECK(w.volume() == Approx(40.0 * 25 * 5).epsilon(1e-6));

    // ---- The tree: rename (F2), suppress, delete (the Delete key), each one undo step ----
    w.tree()->setCurrentIndex(1);
    REQUIRE(w.run("model.feature.rename"));
    QQuickItem* rename = nullptr;
    REQUIRE(QTest::qWaitFor(
        [&] { return (rename = w.find(QStringLiteral("featureRename.1"))) && rename->hasActiveFocus(); }, 5000));
    for (const char c : {'B', 'a', 's', 'e'})
        QTest::keyClick(w.window(), c);
    undo = w.document()->undoCount();
    QTest::keyClick(w.window(), Qt::Key_Return);
    REQUIRE(QTest::qWaitFor([&] { return w.document()->undoCount() == undo + 1; }, kTimeoutMs));
    CHECK(w.document()->state()->features[1].name == "Base");
    w.settle();

    REQUIRE(w.tree()->setSuppressed(1, true));
    REQUIRE(QTest::qWaitFor([&] { return w.document()->undoCount() == undo + 2; }, kTimeoutMs));
    w.settle();
    CHECK(w.status(1) == QStringLiteral("suppressed"));
    CHECK(w.host->scene()->bodyCount() == 0);
    w.undo();
    w.settle();
    CHECK(w.host->scene()->bodyCount() == 1);

    QQuickItem* list = w.find(QStringLiteral("featureList"));
    REQUIRE(list != nullptr);
    list->forceActiveFocus();
    w.tree()->setCurrentIndex(1);
    INFO("active focus " << (w.window()->activeFocusItem() ? w.window()->activeFocusItem()->objectName().toStdString()
                                                           : std::string("none"))
                         << ", list focus " << list->hasActiveFocus());
    QTest::keyClick(w.window(), Qt::Key_Delete);
    const bool deleted = QTest::qWaitFor([&] { return w.document()->state()->features.size() == 1; }, 10000);
    INFO("tree message '" << w.tree()->message().toStdString() << "', current " << w.tree()->currentIndex()
                          << ", features " << w.document()->state()->features.size());
    REQUIRE(deleted);
    w.undo();
    w.settle();
    CHECK(w.document()->state()->features.size() == 2);
    CHECK(w.volume() == Approx(40.0 * 25 * 5).epsilon(1e-6));

    // ---- Drag to reorder: a datum plane dragged to the top moves; the extrude dragged above its sketch does not ----
    REQUIRE(w.invoke("feature.datum_plane.add", api::sketch_edit::datumAddArgs(FeatureId{Uuid::generate()}, "Datum 1",
                                                                               "{\"principal\":\"xy\"}", 5)));
    w.settle();
    const auto dragRow = [&](int from, int to) {
        QQuickItem* a = nullptr;
        QQuickItem* b = nullptr;
        REQUIRE(QTest::qWaitFor(
            [&] {
                a = w.find(QStringLiteral("featureRow.%1").arg(from));
                b = w.find(QStringLiteral("featureRow.%1").arg(to));
                return a && b && a->isVisible() && b->isVisible();
            },
            5000));
        w.grab();
        const QPointF start = a->mapToScene(QPointF(a->width() / 2, a->height() / 2));
        const QPointF end = b->mapToScene(QPointF(b->width() / 2, b->height() * 0.25));
        w.sendMouse(QEvent::MouseMove, start, Qt::NoButton, Qt::NoButton);
        QTest::mousePress(w.window(), Qt::LeftButton, Qt::NoModifier, start.toPoint());
        for (int i = 1; i <= 8; ++i) {
            QTest::mouseMove(w.window(), (start + (end - start) * (i / 8.0)).toPoint());
            QTest::qWait(20);
        }
        QTest::mouseRelease(w.window(), Qt::LeftButton, Qt::NoModifier, end.toPoint());
        QCoreApplication::processEvents();
    };
    undo = w.document()->undoCount();
    dragRow(2, 0);
    const bool moved = QTest::qWaitFor([&] { return w.document()->undoCount() == undo + 1; }, 20000);
    INFO("tree message '" << w.tree()->message().toStdString() << "', checking " << w.tree()->checking() << ", current "
                          << w.tree()->currentIndex());
    REQUIRE(moved);
    CHECK(w.featureTypes() == std::vector<std::string>{"datum_plane", "sketch", "extrude"});
    w.settle();
    w.tree()->dismissMessage();
    dragRow(2, 0);
    REQUIRE(QTest::qWaitFor([&] { return !w.tree()->message().isEmpty() && !w.tree()->checking(); }, kTimeoutMs));
    CHECK(w.featureTypes() == std::vector<std::string>{"datum_plane", "sketch", "extrude"});
    CHECK(w.document()->undoCount() == undo + 1);
    w.shot("t098_reorder_refused");
    w.checkNoWarnings();
}

TEST_CASE("every model command is in the palette and the default keys do not clash", "[model][commands]")
{
    RAY_REQUIRE_RHI();
    Window w;
    for (const char* id : {"model.tool.extrude", "model.tool.revolve", "model.tool.fillet", "model.tool.chamfer",
                           "model.tool.hole", "model.tool.datumPlane", "model.feature.edit", "model.feature.rename",
                           "model.feature.suppress", "model.feature.delete", "model.rollback.end", "model.parameters"})
        CHECK(w.host->commands()->find(QString::fromLatin1(id)).has_value());
    CHECK(w.host->shortcuts()->shortcutText(QStringLiteral("model.tool.extrude")) == QStringLiteral("E"));
    for (const QString& c : w.host->shortcuts()->conflicts())
        UNSCOPED_INFO(c.toStdString());
    CHECK(w.host->shortcuts()->conflicts().isEmpty());
}

TEST_CASE("fillet edges are picked and unpicked in the view while the preview is shown", "[model][pick]")
{
    RAY_REQUIRE_RHI();
    Window w;
    w.newDocument();
    firstExtrude(w, "10 mm");
    w.commit();
    w.view()->setStandardView(QStringLiteral("iso"));
    w.view()->fitAll();
    w.settle();
    const render::ItemId body = w.host->scene()->itemForBody(0);
    REQUIRE(body != 0);
    const auto e1 = edgeAlongX(w, 0, 10);
    const auto e2 = edgeAlongX(w, 25, 10);

    openTool(w, "model.tool.fillet", "fillet");
    CHECK(w.tool()->picking());
    CHECK(w.tool()->pickCount() == 0);
    w.typeInto(QStringLiteral("radius"), QStringLiteral("2 mm"));
    const auto selectedEdges = [&] {
        std::set<std::uint32_t> out;
        for (const ui::SelectionEntry& e : w.host->selection()->entries()) {
            if (e.hit.kind == kernel::SubKind::Edge && e.hit.item == body)
                out.insert(e.hit.subIndex);
        }
        return out;
    };

    // A click on an edge in the view: picked, and the preview shows the fillet.
    w.clickScene(w.sceneOf(e1.second));
    REQUIRE(QTest::qWaitFor([&] { return w.tool()->pickCount() == 1; }, kTimeoutMs));
    w.previewed();
    REQUIRE(w.tool()->overlay().itemCount() == 1);
    CHECK(w.tool()->overlay().owner() == w.tool());
    render::IRenderViewport* viewport = w.host->scene()->viewport();
    REQUIRE(viewport != nullptr);
    // The preview never answers picks; the body it stands in for still does (hidden).
    CHECK(viewport->pickMode(w.tool()->overlay().items().front()) == render::PickMode::Never);
    CHECK(viewport->pickMode(body) == render::PickMode::Always);
    w.shot("fillet_pick_one");

    // With the preview shown, Shift adds the second edge (the click lands on the preview, the pick on the model).
    w.clickScene(w.sceneOf(e2.second), Qt::ShiftModifier);
    REQUIRE(QTest::qWaitFor([&] { return w.tool()->pickCount() == 2; }, kTimeoutMs));
    CHECK(selectedEdges() == std::set<std::uint32_t>{e1.first, e2.first});
    w.previewed();
    CHECK(w.tool()->overlay().itemCount() == 1);
    w.shot("fillet_pick_two_with_preview");

    // Ctrl takes the first one off again, still with the preview shown.
    w.clickScene(w.sceneOf(e1.second), Qt::ControlModifier);
    REQUIRE(QTest::qWaitFor([&] { return w.tool()->pickCount() == 1; }, kTimeoutMs));
    CHECK(selectedEdges() == std::set<std::uint32_t>{e2.first});
    w.previewed();
    w.shot("fillet_before_commit");
    {
        // The pick's preview is only scheduled when its references arrive: the tool counts as busy until then, so
        // waiting for "previewed" means the panel is final.
        QQuickItem* ok = w.find(QStringLiteral("featureOk"));
        REQUIRE(ok != nullptr);
        CHECK(ok->isEnabled());
        // OK stays where it is while the values are typed and their previews build: the status line and the
        // message about the values come and go below it. Typed a key at a time, every value on the way previewed:
        // the half-typed "2.5 m" is 2.5 metres, a radius far too large, so a message comes and goes as well.
        w.grab();
        const QPointF centre(ok->width() / 2, ok->height() / 2);
        const QPointF before = ok->mapToScene(centre);
        // What the panel shows, for a failure.
        const auto layout = [&] {
            std::ostringstream out;
            out << "OK at " << ok->mapToScene(centre).x() << "," << ok->mapToScene(centre).y() << " (before "
                << before.x() << "," << before.y() << "), status '" << w.tool()->previewStatus().toStdString()
                << "', message '" << w.tool()->message().toStdString() << "' (field '"
                << w.tool()->messageField().toStdString() << "'); the panel's rows:";
            for (QQuickItem* row : ok->parentItem()->parentItem()->childItems()) {
                if (row->isVisible())
                    out << "\n  " << row->objectName().toStdString() << " (" << row->metaObject()->className()
                        << ") at " << row->y() << ", " << row->height() << " high";
            }
            return out.str();
        };
        QQuickItem* field = w.find(QStringLiteral("featureInput.radius"));
        REQUIRE(field != nullptr);
        field->forceActiveFocus();
        QMetaObject::invokeMethod(field, "selectAll");
        bool messageShown = false;
        for (const QChar c : QStringLiteral("2.5 mm")) {
            QTest::keyClick(w.window(), c.toLatin1());
            INFO("typed '" << c.toLatin1() << "', the field says '" << field->property("text").toString().toStdString()
                           << "'");
            REQUIRE(QTest::qWaitFor(
                [&] { return w.tool()->previewStatus() == QStringLiteral("building") || !w.tool()->previewBusy(); },
                kTimeoutMs));
            w.grab();
            INFO("building: " << layout());
            CHECK(ok->mapToScene(centre) == before);
            REQUIRE(QTest::qWaitFor([&] { return !w.tool()->previewBusy(); }, kTimeoutMs));
            w.grab();
            INFO("built: " << layout());
            CHECK(ok->mapToScene(centre) == before);
            if (!w.tool()->message().isEmpty()) {
                messageShown = true;
                QQuickItem* message = w.find(QStringLiteral("featureMessage"));
                REQUIRE(message != nullptr);
                CHECK(message->isVisible());
                CHECK(message->mapToScene(QPointF(0, 0)).y() > before.y()); // below OK
                CHECK(field->property("invalid").toBool() == (w.tool()->messageField() == QStringLiteral("radius")));
            }
        }
        CHECK(messageShown);
        CHECK(w.tool()->previewStatus() == QStringLiteral("ok"));
        w.typeInto(QStringLiteral("radius"), QStringLiteral("2 mm"));
        w.previewed();
        w.grab();
        INFO(layout());
        CHECK(ok->mapToScene(centre) == before);
    }
    w.commit();
    const double fillet = (1 - kPi / 4) * 2 * 2 * 40;
    CHECK(w.volume() == Approx(10000 - fillet).epsilon(1e-4));
    // The preview slot is free and the body answers picks as it is drawn again.
    CHECK(w.tool()->overlay().owner() == nullptr);
    CHECK(viewport->pickMode(w.host->scene()->itemForBody(0)) == render::PickMode::WhenVisible);
    w.checkNoWarnings();
}

TEST_CASE("editing a fillet picks in the model before it: its edges are selected, Ctrl takes one off, Shift adds",
          "[model][pick][edit]")
{
    RAY_REQUIRE_RHI();
    Window w;
    w.newDocument();
    firstExtrude(w, "10 mm");
    w.commit();
    w.view()->setStandardView(QStringLiteral("iso"));
    w.view()->fitAll();
    w.settle();
    // Edges of the block (the model before the fillet keeps these indices: it is the same build).
    const auto e1 = edgeAlongX(w, 0, 10);
    const auto e2 = edgeAlongX(w, 25, 10);
    const auto e3 = edgeAlongX(w, 0, 0);
    const auto selectedEdges = [&] {
        std::set<std::uint32_t> out;
        const render::ItemId item = w.host->scene()->itemForBody(0);
        for (const ui::SelectionEntry& e : w.host->selection()->entries()) {
            if (e.hit.kind == kernel::SubKind::Edge && e.hit.item == item)
                out.insert(e.hit.subIndex);
        }
        return out;
    };
    const auto clickEdge = [&](const std::pair<std::uint32_t, Vec3>& e, Qt::KeyboardModifiers mods) {
        w.host->selection()->applyClick(
            render::PickHit{w.host->scene()->itemForBody(0), kernel::SubKind::Edge, e.first, e.second, 0}, mods);
    };

    // A fillet on e1 and e2.
    openTool(w, "model.tool.fillet", "fillet");
    clickEdge(e1, Qt::NoModifier);
    clickEdge(e2, Qt::ShiftModifier);
    REQUIRE(QTest::qWaitFor([&] { return w.tool()->pickCount() == 2; }, kTimeoutMs));
    w.typeInto(QStringLiteral("radius"), QStringLiteral("2 mm"));
    w.previewed();
    w.commit();
    const double fillet = (1 - kPi / 4) * 2 * 2 * 40;
    CHECK(w.volume() == Approx(10000 - 2 * fillet).epsilon(1e-4));
    REQUIRE(w.host->scene()->meshOf(0) != nullptr);
    CHECK(w.host->scene()->meshOf(0)->faces.size() > 6); // the filleted block
    const doc::Feature filletFeature = w.document()->state()->features.back();
    REQUIRE(filletFeature.type == "fillet");
    const QJsonArray storedEdges = QJsonDocument::fromJson(QByteArray::fromStdString(filletFeature.propsJson))
                                       .object()
                                       .value(QStringLiteral("edges"))
                                       .toArray();
    REQUIRE(storedEdges.size() == 2);

    // Edit it: the view shows the block as it was before the fillet (6 faces) under the preview, and the fillet's
    // own edges are selected in it.
    w.tree()->setCurrentIndex(w.tree()->count() - 1);
    REQUIRE(w.run("model.feature.edit"));
    REQUIRE(QTest::qWaitFor([&] { return w.tool()->active() && w.tool()->editing(); }, 5000));
    w.previewed(); // busy until the model before it is shown and its edges are selected
    CHECK(w.tool()->editBaseReady());
    CHECK(w.host->session()->showingModel());
    REQUIRE(w.host->scene()->bodyCount() == 1);
    REQUIRE(w.host->scene()->meshOf(0) != nullptr);
    CHECK(w.host->scene()->meshOf(0)->faces.size() == 6);
    CHECK(w.tool()->pickCount() == 2);
    CHECK(selectedEdges() == std::set<std::uint32_t>{e1.first, e2.first});
    CHECK(w.tool()->overlay().owner() == w.tool());
    CHECK(w.tool()->overlay().itemCount() == 1); // the preview stands in for the block
    render::IRenderViewport* viewport = w.host->scene()->viewport();
    REQUIRE(viewport != nullptr);
    CHECK(viewport->pickMode(w.host->scene()->itemForBody(0)) == render::PickMode::Always);
    w.shot("fillet_edit_stored_edges");
    // Unchanged, the props are the stored ones exactly.
    {
        auto props = w.tool()->propsJson();
        REQUIRE(props);
        const QJsonObject p = QJsonDocument::fromJson(QByteArray::fromStdString(*props)).object();
        CHECK(p.value(QStringLiteral("edges")).toArray() == storedEdges);
    }

    // Ctrl-click takes e1 off; Shift-click adds e3 (with the preview shown, on the model before the fillet).
    clickEdge(e1, Qt::ControlModifier);
    REQUIRE(QTest::qWaitFor([&] { return w.tool()->pickCount() == 1; }, kTimeoutMs));
    w.previewed();
    CHECK(selectedEdges() == std::set<std::uint32_t>{e2.first});
    clickEdge(e3, Qt::ShiftModifier);
    REQUIRE(QTest::qWaitFor([&] { return w.tool()->pickCount() == 2; }, kTimeoutMs));
    w.previewed();
    CHECK(selectedEdges() == std::set<std::uint32_t>{e2.first, e3.first});
    w.shot("fillet_edit_changed_edges");
    w.commit();

    // e2 keeps its stored reference as it was; e3's is new. The view shows the document's build again.
    const doc::Feature& edited = w.document()->state()->features.back();
    const QJsonArray edges = QJsonDocument::fromJson(QByteArray::fromStdString(edited.propsJson))
                                 .object()
                                 .value(QStringLiteral("edges"))
                                 .toArray();
    REQUIRE(edges.size() == 2);
    CHECK(edges.at(0) == storedEdges.at(1));
    CHECK(edges.at(1) != storedEdges.at(0));
    CHECK(edges.at(1) != storedEdges.at(1));
    CHECK(!w.host->session()->showingModel());
    for (int i = 0; i < w.tree()->count(); ++i)
        CHECK(w.status(i) == QStringLiteral("ok"));
    CHECK(w.volume() == Approx(10000 - 2 * fillet).epsilon(1e-4)); // two 40 mm edges again
    CHECK(w.host->scene()->meshOf(0)->faces.size() > 6);

    // Undo restores the first fillet; the edit is one step.
    w.undo();
    w.settle();
    CHECK(w.volume() == Approx(10000 - 2 * fillet).epsilon(1e-4));
    {
        const QJsonArray back =
            QJsonDocument::fromJson(QByteArray::fromStdString(w.document()->state()->features.back().propsJson))
                .object()
                .value(QStringLiteral("edges"))
                .toArray();
        CHECK(back == storedEdges);
    }
    w.checkNoWarnings();
}

TEST_CASE("datum planes are drawn in the view, hidden by their eye, and picked where a plane is asked for",
          "[model][datum]")
{
    RAY_REQUIRE_RHI();
    Window w;
    w.newDocument();
    firstExtrude(w, "10 mm");
    w.commit();
    // A datum plane 20 mm above XY.
    openTool(w, "model.tool.datumPlane", "datum_plane");
    w.typeInto(QStringLiteral("offset"), QStringLiteral("20 mm"));
    w.previewed();
    w.commit();
    const FeatureId datum = w.document()->state()->features.back().id;
    REQUIRE(w.document()->state()->features.back().type == "datum_plane");
    const QString base = QStringLiteral("datum:") + QString::fromStdString(datum.value.toString());
    const QString frame = base + QStringLiteral(":frame");
    const auto drawn = [&] {
        return !w.tree()->busy() && w.tree()->datumLines().has(frame);
    };
    const auto gone = [&] {
        return !w.tree()->busy() && !w.tree()->datumLines().has(frame);
    };
    REQUIRE(QTest::qWaitFor(drawn, kTimeoutMs));
    CHECK(w.tree()->datumLines().has(base + QStringLiteral(":inner")));
    CHECK(w.tree()->datumLines().has(base + QStringLiteral(":normal")));
    // A guide: it never takes a viewport pick.
    render::IRenderViewport* viewport = w.host->scene()->viewport();
    REQUIRE(viewport != nullptr);
    CHECK(viewport->pickMode(w.tree()->datumLines().itemOf(frame)) == render::PickMode::Never);
    const int row = w.tree()->rowOf(QString::fromStdString(datum.value.toString()));
    REQUIRE(row >= 0);
    CHECK(w.tree()->get(row).value(QStringLiteral("isDatum")).toBool());
    CHECK(w.tree()->get(row).value(QStringLiteral("datumShown")).toBool());
    CHECK(!w.tree()->get(1).value(QStringLiteral("isDatum")).toBool());
    w.view()->setStandardView(QStringLiteral("iso"));
    w.view()->fitAll();
    w.settle();
    w.shot("datum_plane_in_view");

    // Its eye in the tree hides it and shows it again.
    QQuickItem* eye = nullptr;
    REQUIRE(QTest::qWaitFor(
        [&] { return (eye = w.find(QStringLiteral("datumVisibility.%1").arg(row))) && eye->isVisible(); }, 5000));
    w.click(eye);
    REQUIRE(QTest::qWaitFor(gone, kTimeoutMs));
    CHECK(!w.tree()->get(row).value(QStringLiteral("datumShown")).toBool());
    w.tree()->setDatumShown(row, true);
    REQUIRE(QTest::qWaitFor(drawn, kTimeoutMs));

    // Rolled back to before it: not built, not drawn.
    REQUIRE(w.tree()->rollTo(1));
    REQUIRE(QTest::qWaitFor([&] { return w.tree()->rollbackIndex() == 1; }, kTimeoutMs));
    w.settle();
    REQUIRE(QTest::qWaitFor(gone, kTimeoutMs));
    REQUIRE(w.tree()->rollTo(-1));
    REQUIRE(QTest::qWaitFor([&] { return w.tree()->rollbackIndex() == -1; }, kTimeoutMs));
    w.settle();
    REQUIRE(QTest::qWaitFor(drawn, kTimeoutMs));

    // A point on the plane inside its frame, beside the block: the frame is centred over the model.
    const Vec3 onDatum{35, 30, 20};
    const QPointF at = w.view()->mapFromScene(w.sceneOf(onDatum));
    // Nothing asks for a plane: a click there picks nothing.
    CHECK(!w.tree()->planeWanted());
    CHECK(!w.tree()->pickDatumAt(at.x(), at.y()));

    // The datum plane tool asks for a base: a click on the datum takes it.
    openTool(w, "model.tool.datumPlane", "datum_plane");
    REQUIRE(QTest::qWaitFor(drawn, kTimeoutMs));
    CHECK(w.tree()->planeWanted());
    w.clickScene(w.sceneOf(onDatum));
    REQUIRE(QTest::qWaitFor([&] { return w.tool()->values().value(QStringLiteral("base")).toString() == base; }, 5000));
    w.tool()->cancel();

    // The sketch mode's plane picker: a click on the datum puts the new sketch on it.
    w.sketch()->newSketch();
    REQUIRE(w.sketch()->placing());
    CHECK(w.tree()->planeWanted());
    w.clickScene(w.sceneOf(onDatum));
    REQUIRE(QTest::qWaitFor([&] { return w.sketch()->active(); }, kTimeoutMs));
    CHECK(w.sketch()->plane().origin.z == Approx(20));
    CHECK(std::abs(w.sketch()->plane().normal.z) == Approx(1));
    w.shot("sketch_on_datum_plane");
    w.sketch()->finish();
    REQUIRE(QTest::qWaitFor([&] { return !w.sketch()->active(); }, kTimeoutMs));
    w.checkNoWarnings();
}

TEST_CASE("editing an extrude shows the model before it under the preview; cancel shows the part again",
          "[model][edit]")
{
    RAY_REQUIRE_RHI();
    Window w;
    w.newDocument();
    firstExtrude(w, "10 mm");
    w.commit();
    const std::string hash = w.stateHash();
    w.tree()->setCurrentIndex(1);
    REQUIRE(w.run("model.feature.edit"));
    REQUIRE(QTest::qWaitFor([&] { return w.tool()->active() && w.tool()->editing(); }, 5000));
    w.previewed();
    // Nothing was built before the first extrude: the scene shows no body, the preview shows the edited block.
    CHECK(w.tool()->editBaseReady());
    CHECK(w.host->session()->showingModel());
    CHECK(w.host->scene()->bodyCount() == 0);
    CHECK(w.tool()->overlay().itemCount() == 1);
    w.tool()->cancel();
    w.settle();
    CHECK(!w.host->session()->showingModel());
    REQUIRE(w.host->scene()->bodyCount() == 1);
    CHECK(w.volume() == Approx(10000).epsilon(1e-6));
    CHECK(w.stateHash() == hash);
    w.checkNoWarnings();
}

TEST_CASE("extrude through all: a cut that stays through after the block grows; the panel and the edit path",
          "[model][throughall]")
{
    RAY_REQUIRE_RHI();
    Window w;
    w.newDocument();
    firstExtrude(w, "10 mm");
    w.commit();
    const FeatureId block = w.document()->state()->features[1].id;
    // A circle (r 5 mm) sketched on XY, under the block.
    const FeatureId circle{Uuid::generate()};
    {
        sketch::Sketch m;
        const SketchEntId centre{Uuid::generate()};
        m.entities.push_back(sketch::Entity{centre, sketch::Point{Vec2{12, 12.5}}, false});
        m.entities.push_back(sketch::Entity{SketchEntId{Uuid::generate()}, sketch::Circle{centre, 5.0}, false});
        std::sort(m.entities.begin(), m.entities.end(),
                  [](const sketch::Entity& a, const sketch::Entity& b) { return a.id < b.id; });
        REQUIRE(w.invoke("feature.sketch.add",
                         api::sketch_edit::sketchAddArgs(circle, "Circle", "{\"principal\":\"xy\"}", m)));
        w.settle();
    }
    openTool(w, "model.tool.extrude", "extrude");
    w.tool()->setValue(QStringLiteral("sketch"), QString::fromStdString(circle.value.toString()));
    w.tool()->setValue(QStringLiteral("operation"), QStringLiteral("cut"));
    w.tool()->setValue(QStringLiteral("extent"), QStringLiteral("through_all"));
    QQuickItem* distance = nullptr;
    REQUIRE(
        QTest::qWaitFor([&] { return (distance = w.find(QStringLiteral("featureField.distance"))) != nullptr; }, 5000));
    REQUIRE(QTest::qWaitFor([&] { return !distance->isVisible(); }, 5000)); // no length to give
    w.previewed();
    CHECK(w.tool()->handles().isEmpty()); // and no length to drag
    w.shot("through_all_panel");
    w.commit();
    {
        const doc::Feature& cut = w.document()->state()->features.back();
        const QJsonObject p = QJsonDocument::fromJson(QByteArray::fromStdString(cut.propsJson)).object();
        CHECK(p.value(QStringLiteral("extent")).toString() == QStringLiteral("through_all"));
        CHECK(!p.contains(QStringLiteral("distance")));
    }
    const double r2 = 5.0 * 5.0;
    CHECK(w.volume() == Approx(10000 - kPi * r2 * 10).epsilon(1e-6));

    // The block made 20 mm deep upstream: the cut still goes through.
    REQUIRE(w.invoke("feature.extrude.edit",
                     "{\"id\":\"" + block.value.toString() + "\",\"props\":{\"distance\":\"20 mm\"}}"));
    w.settle();
    for (int i = 0; i < w.tree()->count(); ++i)
        CHECK(w.status(i) == QStringLiteral("ok"));
    CHECK(w.volume() == Approx(20000 - kPi * r2 * 20).epsilon(1e-6));

    // Edited back to a 5 mm blind cut in the panel: the extent goes, the distance comes.
    w.tree()->setCurrentIndex(w.tree()->count() - 1);
    REQUIRE(w.run("model.feature.edit"));
    REQUIRE(QTest::qWaitFor([&] { return w.tool()->active() && w.tool()->editing(); }, 5000));
    CHECK(w.tool()->values().value(QStringLiteral("extent")).toString() == QStringLiteral("through_all"));
    w.tool()->setValue(QStringLiteral("extent"), QStringLiteral("distance"));
    w.typeInto(QStringLiteral("distance"), QStringLiteral("5 mm"));
    w.previewed();
    w.commit();
    {
        const doc::Feature& cut = w.document()->state()->features.back();
        const QJsonObject p = QJsonDocument::fromJson(QByteArray::fromStdString(cut.propsJson)).object();
        CHECK(!p.contains(QStringLiteral("extent")));
        CHECK(p.value(QStringLiteral("distance")).toString() == QStringLiteral("5 mm"));
    }
    CHECK(w.volume() == Approx(20000 - kPi * r2 * 5).epsilon(1e-6));
    w.checkNoWarnings();
}

TEST_CASE("a document edit re-meshes only the bodies whose geometry changed", "[model][mesh-reuse]")
{
    RAY_REQUIRE_RHI();
    Window w;
    w.newDocument();
    ui::SceneController* scene = w.host->scene();
    const auto meshesDone = [&] {
        REQUIRE(QTest::qWaitFor([&] { return scene->ready() && !scene->refining(); }, kTimeoutMs));
    };
    // Four plates, each a rectangle sketch extruded into a body of its own.
    constexpr int kBodies = 4;
    std::vector<FeatureId> extrudes;
    for (int b = 0; b < kBodies; ++b) {
        sketch::Sketch m;
        std::vector<SketchEntId> p;
        const double x0 = 50.0 * b;
        for (const Vec2 c : {Vec2{x0, 0}, Vec2{x0 + 40, 0}, Vec2{x0 + 40, 25}, Vec2{x0, 25}}) {
            p.push_back(SketchEntId{Uuid::generate()});
            m.entities.push_back(sketch::Entity{p.back(), sketch::Point{c}, false});
        }
        for (int i = 0; i < 4; ++i)
            m.entities.push_back(
                sketch::Entity{SketchEntId{Uuid::generate()}, sketch::Line{p[i], p[(i + 1) % 4]}, false});
        std::sort(m.entities.begin(), m.entities.end(),
                  [](const sketch::Entity& a, const sketch::Entity& c) { return a.id < c.id; });
        const FeatureId sketchId{Uuid::generate()};
        REQUIRE(w.invoke("feature.sketch.add",
                         api::sketch_edit::sketchAddArgs(sketchId, "Plate sketch", "{\"principal\":\"xy\"}", m)));
        extrudes.push_back(FeatureId{Uuid::generate()});
        REQUIRE(w.invoke("feature.extrude.add", "{\"id\":\"" + extrudes.back().value.toString() +
                                                    "\",\"props\":{\"sketch\":\"" + sketchId.value.toString() +
                                                    "\",\"distance\":\"10 mm\",\"operation\":\"new\",\"target\":\"" +
                                                    Uuid::generate().toString() + "\"}}"));
    }
    w.settle();
    meshesDone();
    REQUIRE(scene->bodyCount() == kBodies);
    std::map<BodyId, render::ItemId> items;
    for (int i = 0; i < kBodies; ++i) {
        REQUIRE(scene->itemForBody(i) != 0);
        items[scene->body(i)->id] = scene->itemForBody(i);
    }
    const std::uint64_t before = scene->bodiesMeshed();

    // The third plate made 20 mm thick: one body rebuilt, so one coarse and one fine tessellation, and the other
    // bodies keep their viewport items.
    REQUIRE(w.invoke("feature.extrude.edit",
                     "{\"id\":\"" + extrudes[2].value.toString() + "\",\"props\":{\"distance\":\"20 mm\"}}"));
    w.settle();
    meshesDone();
    REQUIRE(scene->bodyCount() == kBodies);
    CHECK(scene->bodiesMeshed() - before == 2);
    int kept = 0;
    for (int i = 0; i < kBodies; ++i) {
        const auto it = items.find(scene->body(i)->id);
        REQUIRE(it != items.end());
        kept += scene->itemForBody(i) == it->second ? 1 : 0;
        CHECK(scene->meshIsFine(i));
    }
    CHECK(kept == kBodies - 1);

    // Undo gives back the earlier shape objects (a rebuild cache hit): nothing is meshed for the three plates the
    // edit did not touch, and the thick plate is meshed again.
    const std::uint64_t edited = scene->bodiesMeshed();
    w.undo();
    w.settle();
    meshesDone();
    CHECK(scene->bodiesMeshed() - edited == 2);
    w.checkNoWarnings();
}

TEST_CASE("the palette lists each command once; default keys never collide within a context", "[model][commands]")
{
    RAY_REQUIRE_RHI();
    Window w;
    w.newDocument();
    ui::CommandCatalog& commands = *w.host->commands();
    ui::ShortcutManager& keys = *w.host->shortcuts();

    // One entry per title in the palette (a registry action a UI command stands for is not listed again).
    const auto normalized = [](QString t) {
        t = t.toLower();
        t.remove(QChar(0x2026));
        t.remove(QStringLiteral("..."));
        return t.trimmed();
    };
    std::map<QString, QString> seen;
    for (const ui::Command& c : commands.commands()) {
        const auto [it, inserted] = seen.emplace(normalized(c.title), c.id);
        INFO(c.id.toStdString() << " and " << it->second.toStdString() << " are both '" << c.title.toStdString()
                                << "'");
        CHECK(inserted);
    }
    int extrudes = 0;
    for (const ui::Command& c : commands.search(QStringLiteral("extrude")))
        extrudes += normalized(c.title) == QStringLiteral("extrude") ? 1 : 0;
    CHECK(extrudes == 1);
    REQUIRE(commands.find(QStringLiteral("feature.extrude.add")).has_value()); // still there for scripts and agents
    for (const ui::Command& c : commands.commands())
        CHECK(c.id != QStringLiteral("feature.extrude.add"));

    // Every default key, declared where its command is registered, collides with no other in an overlapping context.
    std::vector<ui::Command> all = commands.commands();
    for (std::size_t i = 0; i < all.size(); ++i) {
        const QString a = keys.defaultShortcutText(all[i].id);
        if (a.isEmpty())
            continue;
        for (std::size_t j = i + 1; j < all.size(); ++j) {
            if (keys.defaultShortcutText(all[j].id) != a || !ui::contextsOverlap(all[i].context, all[j].context))
                continue;
            FAIL_CHECK(all[i].id.toStdString() << " and " << all[j].id.toStdString() << " share " << a.toStdString());
        }
    }
    for (const QString& c : keys.conflicts())
        UNSCOPED_INFO(c.toStdString());
    CHECK(keys.conflicts().isEmpty());
    for (const ui::UiCommand& u : ui::allUiCommands()) {
        INFO(u.id);
        if (QString::fromLatin1(u.id).startsWith(QLatin1String("model.tool.")))
            CHECK(u.context == ui::CommandContext::Model);
        if (QString::fromLatin1(u.id).startsWith(QLatin1String("sketch.tool.")) ||
            QString::fromLatin1(u.id).startsWith(QLatin1String("sketch.constrain.")))
            CHECK(u.context == ui::CommandContext::Sketch);
    }

    // The context decides what a key runs: E in the model, P (the point tool) only in a sketch.
    CHECK(keys.commandFor(QStringLiteral("E")) == QStringLiteral("model.tool.extrude"));
    CHECK(keys.commandFor(QStringLiteral("P")).isEmpty());
    // A model and a sketch command may share a key; a key that works everywhere may not be shared.
    CHECK(keys.assign(QStringLiteral("model.tool.revolve"), QStringLiteral("L")).value(QStringLiteral("ok")).toBool());
    CHECK(keys.conflicts().isEmpty());
    const QVariantMap clash = keys.assign(QStringLiteral("model.tool.revolve"), QStringLiteral("Ctrl+S"));
    CHECK(!clash.value(QStringLiteral("ok")).toBool());
    CHECK(clash.value(QStringLiteral("conflictId")).toString() == QStringLiteral("file.save"));
    keys.resetAll();

    // In a sketch: P picks the point tool and a click places a free point (the hole positions of a part).
    const FeatureId sk = rectangleSketch(w, 40, 25);
    w.settle();
    REQUIRE(w.sketch()->editFeature(QString::fromStdString(sk.value.toString())));
    REQUIRE(QTest::qWaitFor([&] { return w.sketch()->active(); }, 20000));
    CHECK(keys.commandFor(QStringLiteral("P")) == QStringLiteral("sketch.tool.point"));
    CHECK(keys.commandFor(QStringLiteral("E")).isEmpty());
    CHECK(keys.commandFor(QStringLiteral("L")) == QStringLiteral("sketch.tool.line"));
    w.view()->setStandardView(QStringLiteral("top"));
    w.grab();
    QTest::keyClick(w.window(), Qt::Key_P);
    REQUIRE(QTest::qWaitFor([&] { return w.sketch()->tool() == QStringLiteral("point"); }, 5000));
    const auto at = w.sketch()->toView(Vec2{17, 9});
    REQUIRE(at.has_value());
    w.clickScene(w.view()->mapToScene(*at));
    REQUIRE(QTest::qWaitFor([&] { return w.sketch()->inFlight() == 0 && !w.sketch()->solving(); }, 20000));
    auto model = api::sketch_edit::modelOf(*w.document()->state(), sk);
    REQUIRE(model);
    int points = 0;
    for (const sketch::Entity& e : model->entities) {
        if (const auto* p = std::get_if<sketch::Point>(&e.geom))
            points += std::abs(p->p.x - 17) < 0.5 && std::abs(p->p.y - 9) < 0.5 ? 1 : 0;
    }
    CHECK(points == 1);
    w.shot("sketch_point_tool");
    REQUIRE(w.run("sketch.mode.finish"));
    w.checkNoWarnings();
}

// The whole flow in the real window, step by step with screenshots (RAY_T098_SHOTS): sketch with a point, extrude,
// through-all cut, hole at the point, fillet with its edges changed while the preview shows, rollback and a feature
// inserted at the bar. Hidden ("[.scenario]"): run it by name.
TEST_CASE("scenario: sketch with a point, extrude, through-all cut, hole, fillet, rollback and insert", "[.scenario]")
{
    RAY_REQUIRE_RHI();
    Window w;
    w.newDocument();
    firstExtrude(w, "10 mm");
    w.commit();
    w.shot("app_01_extrude");

    // Sketch on the top face: a circle (10 mm) and a free point (P), both drawn in the sketch mode.
    const std::uint32_t top = faceAtZ(w, 10);
    w.host->selection()->applyClick(
        render::PickHit{w.host->scene()->itemForBody(0), kernel::SubKind::Face, top, Vec3{20, 12.5, 10}, 0},
        Qt::NoModifier);
    REQUIRE(w.run("sketch.mode.new"));
    QQuickItem* face = nullptr;
    REQUIRE(QTest::qWaitFor([&] { return (face = w.find(QStringLiteral("sketchPlane.face"))) != nullptr; }, 5000));
    w.click(face);
    REQUIRE(QTest::qWaitFor([&] { return w.sketch()->active(); }, 20000));
    w.view()->setCamera(w.view()->framed(w.view()->camera(), Box3{{-5, -5, 10}, {45, 30, 10}}));
    w.grab();
    REQUIRE(w.run("sketch.tool.circle"));
    {
        const auto centre = w.sketch()->toView(Vec2{12, 12.5});
        const auto rim = w.sketch()->toView(Vec2{15, 12.5});
        REQUIRE(centre.has_value());
        REQUIRE(rim.has_value());
        w.clickScene(w.view()->mapToScene(*centre));
        w.sendMouse(QEvent::MouseMove, w.view()->mapToScene(*rim), Qt::NoButton, Qt::NoButton);
        QCoreApplication::processEvents();
        for (const char c : {'1', '0', 'm', 'm'})
            QTest::keyClick(w.window(), c);
        QTest::keyClick(w.window(), Qt::Key_Return);
    }
    REQUIRE(QTest::qWaitFor([&] { return w.sketch()->inFlight() == 0 && !w.sketch()->solving(); }, 20000));
    QTest::keyClick(w.window(), Qt::Key_P); // the point tool
    REQUIRE(QTest::qWaitFor([&] { return w.sketch()->tool() == QStringLiteral("point"); }, 5000));
    {
        const auto at = w.sketch()->toView(Vec2{30, 12.5});
        REQUIRE(at.has_value());
        w.clickScene(w.view()->mapToScene(*at));
    }
    REQUIRE(QTest::qWaitFor([&] { return w.sketch()->inFlight() == 0 && !w.sketch()->solving(); }, 20000));
    w.shot("app_02_sketch_circle_and_point");
    const std::optional<FeatureId> sketch2 = w.sketch()->featureId();
    REQUIRE(sketch2.has_value());
    REQUIRE(w.run("sketch.mode.finish"));
    w.settle();

    // The extrude offered for the sketch: the circle cut through all.
    REQUIRE(QTest::qWaitFor([&] { return !w.tool()->offerSketch().isEmpty(); }, 5000));
    w.click("extrudeOfferAccept");
    REQUIRE(QTest::qWaitFor([&] { return w.tool()->active(); }, 5000));
    w.tool()->setValue(QStringLiteral("operation"), QStringLiteral("cut"));
    w.tool()->setValue(QStringLiteral("extent"), QStringLiteral("through_all"));
    w.view()->setStandardView(QStringLiteral("iso"));
    w.previewed();
    w.shot("app_03_through_all_cut_panel");
    w.commit();
    CHECK(w.volume() == Approx(10000 - kPi * 25 * 10).epsilon(1e-5));

    // A hole at the sketch point (the circle's centre is not a hole position while a free point exists).
    openTool(w, "model.tool.hole", "hole");
    w.tool()->setValue(QStringLiteral("sketch"), QString::fromStdString(sketch2->value.toString()));
    REQUIRE(
        QTest::qWaitFor([&] { return w.tool()->pickText().startsWith(QStringLiteral("Every point")); }, kTimeoutMs));
    w.typeInto(QStringLiteral("diameter"), QStringLiteral("6 mm"));
    w.previewed();
    w.shot("app_04_hole_panel");
    w.commit();
    CHECK(w.volume() == Approx(10000 - kPi * 25 * 10 - kPi * 9 * 10).epsilon(1e-5));

    // Fillet: one edge picked, then with the preview shown a second added and the first taken off.
    w.view()->setStandardView(QStringLiteral("iso"));
    w.view()->fitAll();
    w.settle();
    const auto e1 = edgeAlongX(w, 0, 10);
    const auto e2 = edgeAlongX(w, 25, 10);
    openTool(w, "model.tool.fillet", "fillet");
    w.typeInto(QStringLiteral("radius"), QStringLiteral("2 mm"));
    w.clickScene(w.sceneOf(e1.second));
    REQUIRE(QTest::qWaitFor([&] { return w.tool()->pickCount() == 1; }, kTimeoutMs));
    w.previewed();
    w.shot("app_05_fillet_one_edge_preview");
    w.clickScene(w.sceneOf(e2.second), Qt::ShiftModifier);
    REQUIRE(QTest::qWaitFor([&] { return w.tool()->pickCount() == 2; }, kTimeoutMs));
    w.previewed();
    w.shot("app_06_fillet_edge_added_with_preview");
    w.clickScene(w.sceneOf(e1.second), Qt::ControlModifier);
    REQUIRE(QTest::qWaitFor([&] { return w.tool()->pickCount() == 1; }, kTimeoutMs));
    w.previewed();
    w.shot("app_07_fillet_edge_removed_with_preview");
    w.commit();
    for (int i = 0; i < w.tree()->count(); ++i)
        CHECK(w.status(i) == QStringLiteral("ok"));
    w.shot("app_08_finished_part");

    // Roll back to after Extrude 1 and insert a datum plane at the bar.
    REQUIRE(w.tree()->rollTo(1));
    REQUIRE(QTest::qWaitFor([&] { return w.tree()->rollbackIndex() == 1; }, kTimeoutMs));
    w.settle();
    w.shot("app_09_rolled_back");
    openTool(w, "model.tool.datumPlane", "datum_plane");
    w.typeInto(QStringLiteral("offset"), QStringLiteral("5 mm"));
    w.previewed();
    w.shot("app_10_datum_at_rollback_bar");
    w.commit();
    CHECK(w.tree()->rollbackIndex() == 2);
    CHECK(w.document()->state()->features[2].type == "datum_plane");
    w.shot("app_11_inserted_at_bar");
    REQUIRE(w.run("model.rollback.end"));
    REQUIRE(QTest::qWaitFor([&] { return w.tree()->rollbackIndex() == -1; }, kTimeoutMs));
    w.settle();
    for (int i = 0; i < w.tree()->count(); ++i)
        CHECK(w.status(i) == QStringLiteral("ok"));
    w.shot("app_12_rolled_forward");
    w.checkNoWarnings();
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
