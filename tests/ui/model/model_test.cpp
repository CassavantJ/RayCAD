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
#include "viewport_item.h"
#include "viewport_navigation.h"

#include <ray_api/sketch_edit.h>
#include <ray_api/viewer.h>
#include <ray_render/viewport.h>

#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
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
#include <memory>
#include <optional>
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
    void settle() const
    {
        REQUIRE(QTest::qWaitFor(
            [&] {
                doc::Document* d = document();
                return d != nullptr && !d->rebuildPending() && !host->scene()->meshing() && !tree()->busy() &&
                       sketch()->inFlight() == 0 && !sketch()->solving();
            },
            kTimeoutMs));
        QTest::qWait(50);
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
        REQUIRE(
            QTest::qWaitFor([&] { return !tool()->active() && document()->undoCount() == before + 1; }, kTimeoutMs));
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
    for (std::uint32_t i = 0; i < body->shape.count(kernel::SubKind::Face); ++i) {
        auto info = api::viewer::describeElement(body->shape, kernel::SubId{kernel::SubKind::Face, i}, ctx);
        REQUIRE(info);
        if (info->surface && info->surface->type == kernel::SurfaceType::Plane &&
            std::abs(info->surface->centroid.z - z) < 1e-6 && info->surface->axis.z > 0.5)
            return i;
    }
    FAIL("no planar face at that height");
    return 0;
}

// The straight edge of body 0 along X at (y, z); its index and midpoint.
std::pair<std::uint32_t, Vec3> edgeAlongX(const Window& w, double y, double z)
{
    const api::viewer::Body* body = w.host->scene()->body(0);
    REQUIRE(body != nullptr);
    JobContext ctx;
    for (std::uint32_t i = 0; i < body->shape.count(kernel::SubKind::Edge); ++i) {
        auto info = api::viewer::describeElement(body->shape, kernel::SubId{kernel::SubKind::Edge, i}, ctx);
        REQUIRE(info);
        if (!info->curve || info->curve->type != kernel::CurveType::Line)
            continue;
        const kernel::CurveInfo& c = *info->curve;
        if (std::abs(c.start.y - y) < 1e-6 && std::abs(c.end.y - y) < 1e-6 && std::abs(c.start.z - z) < 1e-6 &&
            std::abs(c.end.z - z) < 1e-6 && std::abs(c.start.x - c.end.x) > 1)
            return {i, Vec3{(c.start.x + c.end.x) / 2, y, z}};
    }
    FAIL("no edge there");
    return {};
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

TEST_CASE("a part by hand: extrude, cut, hole, fillet; edit; undo and redo", "[model][e2e]")
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
    w.clickScene(w.sceneOf(Vec3{plane.origin.x + 12, plane.origin.y + 12.5, plane.origin.z}));
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
    w.tool()->setPicking(false);
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

    // ---- Rolled back to after Extrude 1: the view shows the plain block, later features are greyed ----
    w.tree()->setRollback(1);
    REQUIRE(
        QTest::qWaitFor([&] { return w.tree()->rollbackOverlay().itemCount() == 1 && !w.tree()->busy(); }, kTimeoutMs));
    CHECK(!w.tree()->get(1).value(QStringLiteral("rolledBack")).toBool());
    CHECK(w.tree()->get(2).value(QStringLiteral("rolledBack")).toBool());
    CHECK(w.tree()->rollbackOverlay().meshes().front()->faces.size() == 6); // the block before the cut
    w.shot("t098_rollback");
    CHECK(w.stateHash() == finished); // a view state: the document does not change
    w.tree()->setRollback(-1);
    REQUIRE(QTest::qWaitFor([&] { return w.tree()->rollbackOverlay().itemCount() == 0; }, 5000));

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
        QElapsedTimer clock;
        qint64 last = 0, worst = 0;
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
        for (const char* d : {"11 mm", "11.5 mm", "13 mm", "12 mm"}) {
            w.typeInto(QStringLiteral("distance"), QString::fromLatin1(d));
            QTest::qWait(60);
        }
        w.previewed();
        tick.stop();
        WARN("UI-thread watchdog while the extrude preview rebuilt the part: worst gap "
             << worst << " ms, " << w.tool()->previewBuilds() << " preview builds");
        CHECK(worst < 50);
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

int main(int argc, char** argv)
{
    // The real user profile is never touched.
    QTemporaryDir profile;
    qputenv("RAYCAD_USER_DATA_DIR", QDir::toNativeSeparators(profile.path()).toUtf8());
    QGuiApplication app(argc, argv);
    QGuiApplication::setQuitOnLastWindowClosed(false);
    return Catch::Session().run(argc, argv);
}
