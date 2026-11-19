// The sketch drawing tools in the desktop app (task T-092), on the real Qt Quick window with the real application
// session:
//
//   - New Sketch, the XY plane from the picker; a rectangle and a circle drawn by pointer input (the circle's diameter
//     typed); the document's sketch holds them with the inferred constraints (horizontal, vertical, diameter); a line
//     from a corner shares the corner and snaps horizontal; every step is one undo step (undo / redo through the
//     action), and undoing the first step leaves the mode open
//   - dragging a point of a 200-curve coupled sketch moves it in the document; the UI thread never stalls over the
//     PLAN.md 9 budget (50 ms) while the drag worker solves
//   - an open sketch renders no frames while idle
//   - entering and leaving a sketch without drawing leaves the document hash as it was (new and existing sketches)
//   - an offset in the plane picker makes a datum plane first; a sketch on a picked planar face of an imported part;
//     a line dragged by its middle follows the pointer
//   - every sketch command is in the palette and the default keys do not clash
//
// Screenshots: with RAY_T092_SHOTS set to a folder, the window is saved there.
#include "../scene_graph_probe.h"
#include "app_context.h"
#include "app_host.h"
#include "app_options.h"
#include "files.h"
#include "palette_commands.h"
#include "scene_controller.h"
#include "selection_model.h"
#include "session.h"
#include "shortcuts.h"
#include "sketch/sketch_mode.h"
#include "sketch/sketch_overlay.h"
#include "viewport_item.h"

#include <ray_api/sketch_edit.h>
#include <ray_api/viewer.h>
#include <ray_render/viewport.h>

#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QPointer>
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
using ray::ui::SketchModel;

namespace {

// plate_thru_holes_1 (tools/corpus_gen): a 120 x 80 x 10 mm plate with through holes.
const QString kPlate = QStringLiteral(RAY_CORPUS_DIR "/raycad-synthetic/plate_thru_holes_1.stp");

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
    SketchModel* sketch() const
    {
        auto* s =
            host->engine()->singletonInstance<SketchModel*>(QStringLiteral("RayCad.Ui"), QStringLiteral("SketchTool"));
        REQUIRE(s != nullptr);
        return s;
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
    ui::SketchOverlay* overlay() const
    {
        auto* o = qobject_cast<ui::SketchOverlay*>(find(QStringLiteral("sketchOverlay")));
        REQUIRE(o != nullptr);
        return o;
    }
    void sendMouse(QEvent::Type type, QPointF scenePos, Qt::MouseButton button, Qt::MouseButtons buttons,
                   Qt::KeyboardModifiers mods = Qt::NoModifier) const
    {
        QMouseEvent e(type, scenePos, window()->mapToGlobal(scenePos), button, buttons, mods);
        QGuiApplication::sendEvent(window(), &e);
    }
    void click(QQuickItem* item) const
    {
        REQUIRE(item != nullptr);
        grab();
        const QPointF c = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
        sendMouse(QEvent::MouseMove, c, Qt::NoButton, Qt::NoButton);
        QTest::qWait(20);
        sendMouse(QEvent::MouseButtonPress, c, Qt::LeftButton, Qt::LeftButton);
        sendMouse(QEvent::MouseButtonRelease, c, Qt::LeftButton, Qt::NoButton);
    }
    // A sketch-plane point in window coordinates.
    QPointF scenePoint(Vec2 uv) const
    {
        const auto v = sketch()->toView(uv);
        REQUIRE(v.has_value());
        return overlay()->mapToScene(*v);
    }
    void moveTo(Vec2 uv) const
    {
        sendMouse(QEvent::MouseMove, scenePoint(uv), Qt::NoButton, Qt::NoButton);
        QCoreApplication::processEvents();
    }
    void clickAt(Vec2 uv) const
    {
        const QPointF p = scenePoint(uv);
        sendMouse(QEvent::MouseMove, p, Qt::NoButton, Qt::NoButton);
        QCoreApplication::processEvents();
        sendMouse(QEvent::MouseButtonPress, p, Qt::LeftButton, Qt::LeftButton);
        sendMouse(QEvent::MouseButtonRelease, p, Qt::LeftButton, Qt::NoButton);
        QCoreApplication::processEvents();
    }
    // The snap distance in mm where the view shows `uv` (the theme's spacing.s, 8 px).
    double snapMm(Vec2 uv) const
    {
        const auto v = sketch()->toView(uv);
        REQUIRE(v.has_value());
        const auto a = sketch()->toPlane(*v);
        const auto b = sketch()->toPlane(*v + QPointF(host->theme()->spacingS(), 0));
        REQUIRE(a.has_value());
        REQUIRE(b.has_value());
        return std::hypot(a->x - b->x, a->y - b->y);
    }
    bool settled() const
    {
        return QTest::qWaitFor(
            [&] {
                SketchModel* s = sketch();
                return s->inFlight() == 0 && !s->solving() && !s->dragging();
            },
            20000);
    }
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
    // Shows the sketch-plane box (lo, hi) in the view.
    void frame(Vec2 lo, Vec2 hi) const
    {
        const kernel::Plane& p = sketch()->plane();
        const Vec3 y{p.normal.y * p.xDir.z - p.normal.z * p.xDir.y, p.normal.z * p.xDir.x - p.normal.x * p.xDir.z,
                     p.normal.x * p.xDir.y - p.normal.y * p.xDir.x};
        Box3 box{{1e300, 1e300, 1e300}, {-1e300, -1e300, -1e300}};
        for (const Vec2 c : {lo, hi, Vec2{lo.x, hi.y}, Vec2{hi.x, lo.y}}) {
            const Vec3 w{p.origin.x + p.xDir.x * c.x + y.x * c.y, p.origin.y + p.xDir.y * c.x + y.y * c.y,
                         p.origin.z + p.xDir.z * c.x + y.z * c.y};
            box.min = {std::min(box.min.x, w.x), std::min(box.min.y, w.y), std::min(box.min.z, w.z)};
            box.max = {std::max(box.max.x, w.x), std::max(box.max.y, w.y), std::max(box.max.z, w.z)};
        }
        view()->setCamera(view()->framed(view()->camera(), box));
        grab();
    }
    bool run(const char* id) const { return host->commands()->trigger(QString::fromLatin1(id)); }
    void undo(bool redo = false) const
    {
        const std::size_t before = document()->undoCount();
        REQUIRE(host->context()->invoke(redo ? QStringLiteral("edit.redo") : QStringLiteral("edit.undo")));
        REQUIRE(QTest::qWaitFor([&] { return document()->undoCount() != before; }, 20000));
        REQUIRE(settled());
        // The sketch mode follows the document (its listener runs after the undo).
        REQUIRE(QTest::qWaitFor(
            [&] {
                SketchModel* s = sketch();
                const auto id = s->featureId();
                if (!id)
                    return true;
                auto model = api::sketch_edit::modelOf(*document()->state(), *id);
                return model ? s->featureInDocument() && s->entityCount() == static_cast<int>(model->entities.size())
                             : !s->featureInDocument() && s->entityCount() == 0;
            },
            20000));
    }
    void shot(const char* name) const
    {
        const QString dir = qEnvironmentVariable("RAY_T092_SHOTS");
        if (dir.isEmpty())
            return;
        QDir().mkpath(dir);
        QTest::qWait(250);
        grab().save(QDir(dir).filePath(QString::fromLatin1(name) + QStringLiteral(".png")), "PNG");
    }
    void checkNoWarnings() const
    {
        for (const QString& warning : host->warnings())
            UNSCOPED_INFO(warning.toStdString());
        CHECK(host->warnings().isEmpty());
    }
};

int count(const sketch::Sketch& s, sketch::ConstraintType type)
{
    int n = 0;
    for (const sketch::Constraint& c : s.constraints)
        n += c.type == type ? 1 : 0;
    return n;
}

template <class T>
int countOf(const sketch::Sketch& s)
{
    int n = 0;
    for (const sketch::Entity& e : s.entities)
        n += std::holds_alternative<T>(e.geom) ? 1 : 0;
    return n;
}

sketch::Sketch documentSketch(const Window& w)
{
    const auto id = w.sketch()->featureId();
    REQUIRE(id.has_value());
    auto model = api::sketch_edit::modelOf(*w.document()->state(), *id);
    REQUIRE(model);
    return *model;
}

void enterXy(Window& w)
{
    REQUIRE(w.run("sketch.mode.new"));
    REQUIRE(w.sketch()->placing());
    QQuickItem* xy = nullptr;
    REQUIRE(QTest::qWaitFor([&] { return (xy = w.find(QStringLiteral("sketchPlane.xy"))) != nullptr; }, 5000));
    w.click(xy);
    REQUIRE(QTest::qWaitFor([&] { return w.sketch()->active(); }, 5000));
    w.frame({-20, -20}, {100, 60});
}

} // namespace

TEST_CASE("a rectangle and a circle drawn by pointer, with inferred constraints, undo per step", "[sketch][e2e]")
{
    RAY_REQUIRE_RHI();
    Window w;
    w.newDocument();
    const std::string emptyHash = w.stateHash();
    enterXy(w);
    SketchModel& s = *w.sketch();
    CHECK(s.tool() == QStringLiteral("line")); // a new sketch starts with the line tool
    CHECK(!s.featureInDocument());             // nothing in the document until something is drawn
    CHECK(w.stateHash() == emptyHash);

    // Rectangle: R, then two corners.
    REQUIRE(w.run("sketch.tool.rectangle"));
    CHECK(s.tool() == QStringLiteral("rectangle"));
    w.clickAt({0, 0});
    w.moveTo({40, 25});
    w.shot("t092_rectangle_rubber_band");
    w.clickAt({40, 25});
    REQUIRE(w.settled());
    REQUIRE(s.featureInDocument());
    {
        const sketch::Sketch m = documentSketch(w);
        CHECK(countOf<sketch::Line>(m) == 4);
        CHECK(countOf<sketch::Point>(m) == 4);
        CHECK(count(m, sketch::ConstraintType::Horizontal) == 2);
        CHECK(count(m, sketch::ConstraintType::Vertical) == 2);
    }
    const std::size_t afterRectangle = w.document()->undoCount();

    // Circle: centre by pointer, diameter typed.
    REQUIRE(w.run("sketch.tool.circle"));
    w.clickAt({70, 15});
    w.moveTo({75, 15});
    for (const char c : {'2', '0', 'm', 'm'}) // a new document is in inches: the unit is typed
        QTest::keyClick(w.window(), c);
    QTest::keyClick(w.window(), Qt::Key_Return);
    REQUIRE(w.settled());
    {
        const sketch::Sketch m = documentSketch(w);
        REQUIRE(countOf<sketch::Circle>(m) == 1);
        for (const sketch::Entity& e : m.entities) {
            if (const auto* c = std::get_if<sketch::Circle>(&e.geom))
                CHECK(c->radius == Approx(10).epsilon(1e-9));
        }
        CHECK(count(m, sketch::ConstraintType::Diameter) == 1);
    }
    CHECK(w.document()->undoCount() == afterRectangle + 1);

    // A line from the corner (40, 25) that ends a little off horizontal: it reuses the corner and snaps horizontal.
    REQUIRE(w.run("sketch.tool.line"));
    const double tol = w.snapMm({60, 25});
    w.clickAt({40, 25});
    w.moveTo({55, 25 + tol * 0.3});
    w.clickAt({55, 25 + tol * 0.3});
    QTest::keyClick(w.window(), Qt::Key_Escape); // ends the chain
    REQUIRE(w.settled());
    {
        const sketch::Sketch m = documentSketch(w);
        CHECK(countOf<sketch::Line>(m) == 5);
        CHECK(countOf<sketch::Point>(m) == 4 + 1 + 1); // rectangle, circle centre, the line's new end only
        CHECK(count(m, sketch::ConstraintType::Horizontal) == 3);
    }
    w.shot("t092_profile");

    // Undo and redo step by step: the line, the circle, the rectangle (the feature goes; the mode stays open).
    w.undo();
    CHECK(countOf<sketch::Line>(documentSketch(w)) == 4);
    w.undo();
    CHECK(countOf<sketch::Circle>(documentSketch(w)) == 0);
    w.undo();
    CHECK(s.active());
    CHECK(!s.featureInDocument());
    CHECK(s.entityCount() == 0);
    CHECK(w.stateHash() == emptyHash);
    w.undo(true);
    REQUIRE(s.featureInDocument());
    CHECK(countOf<sketch::Line>(documentSketch(w)) == 4);
    w.undo(true);
    w.undo(true);
    CHECK(countOf<sketch::Line>(documentSketch(w)) == 5);
    CHECK(s.entityCount() == static_cast<int>(documentSketch(w).entities.size()));

    REQUIRE(w.run("sketch.mode.finish"));
    CHECK(!s.active());
    w.checkNoWarnings();
}

TEST_CASE("an open sketch renders nothing while idle; entering and leaving changes nothing", "[sketch][idle]")
{
    RAY_REQUIRE_RHI();
    Window w;
    w.newDocument();
    const std::string before = w.stateHash();
    const std::size_t undoBefore = w.document()->undoCount();

    // A new sketch entered and left without drawing.
    enterXy(w);
    REQUIRE(w.run("sketch.mode.finish"));
    CHECK(!w.sketch()->active());
    CHECK(w.stateHash() == before);
    CHECK(w.document()->undoCount() == undoBefore);

    // Draw something, then re-enter it from the document and leave: the hash stays.
    enterXy(w);
    REQUIRE(w.run("sketch.tool.rectangle"));
    w.clickAt({0, 0});
    w.clickAt({30, 20});
    REQUIRE(w.settled());
    const auto id = w.sketch()->featureId();
    REQUIRE(id.has_value());
    REQUIRE(w.run("sketch.mode.finish"));
    const std::string drawn = w.stateHash();
    const std::size_t undoDrawn = w.document()->undoCount();
    REQUIRE(w.sketch()->editFeature(QString::fromStdString(id->value.toString())));
    REQUIRE(QTest::qWaitFor([&] { return w.sketch()->active(); }, 5000));
    REQUIRE(w.settled());
    CHECK(w.sketch()->entityCount() == 8);
    CHECK(w.sketch()->tool() == QStringLiteral("select"));

    // Idle: the cursor rests over the sketch; no frame for a second.
    w.frame({-10, -10}, {50, 40});
    w.moveTo({15, 30});
    // Everything the document does after the edit (its rebuild, the status line) is over before the count starts.
    REQUIRE(QTest::qWaitFor(
        [&] {
            doc::Document* d = w.document();
            return d != nullptr && !d->rebuildPending() && !w.host->session()->jobActive();
        },
        20000));
    QTest::qWait(800);
    int swapped = 0;
    QElapsedTimer since;
    since.start();
    std::vector<qint64> swapTimes;
    QObject::connect(
        w.window(), &QQuickWindow::frameSwapped, w.window(),
        [&] {
            ++swapped;
            swapTimes.push_back(since.elapsed());
        },
        Qt::DirectConnection);
    const int paints = w.overlay()->paints();
    const std::uint64_t frames = w.view()->viewport()->info().framesRendered;
    QTest::qWait(1000);
    for (const qint64 at : swapTimes)
        UNSCOPED_INFO("a frame at " << at << " ms");
    CHECK(swapped == 0);
    CHECK(w.overlay()->paints() == paints);
    CHECK(w.view()->viewport()->info().framesRendered == frames);

    REQUIRE(w.run("sketch.mode.finish"));
    CHECK(w.stateHash() == drawn);
    CHECK(w.document()->undoCount() == undoDrawn);
    w.checkNoWarnings();
}

TEST_CASE("dragging a point of a 200-curve sketch: the model follows, the UI thread never stalls", "[sketch][drag]")
{
    RAY_REQUIRE_RHI();
    Window w;
    w.newDocument();
    enterXy(w);
    // One line to make the feature, then the 200-curve chain replaces the model (sketch.set).
    w.clickAt({0, 0});
    w.clickAt({10, 0});
    QTest::keyClick(w.window(), Qt::Key_Escape);
    REQUIRE(w.settled());
    const FeatureId feature = *w.sketch()->featureId();

    // A zig-zag chain of 200 lines with fixed lengths, its first point fixed: one coupled group.
    sketch::Sketch chain;
    std::vector<SketchEntId> points;
    for (int i = 0; i <= 200; ++i) {
        const SketchEntId id{Uuid::generate()};
        points.push_back(id);
        chain.entities.push_back(sketch::Entity{id, sketch::Point{{i * 0.5, (i % 2) * 0.4}}, false});
    }
    for (int i = 0; i < 200; ++i) {
        const SketchEntId line{Uuid::generate()};
        chain.entities.push_back(sketch::Entity{
            line, sketch::Line{points[static_cast<std::size_t>(i)], points[static_cast<std::size_t>(i + 1)]}, false});
        sketch::Constraint d;
        d.id = Uuid::generate();
        d.type = sketch::ConstraintType::Distance;
        d.refs = {line};
        d.value = std::hypot(0.5, 0.4);
        chain.constraints.push_back(d);
    }
    sketch::Constraint fix;
    fix.id = Uuid::generate();
    fix.type = sketch::ConstraintType::Fix;
    fix.refs = {points.front()};
    chain.constraints.push_back(fix);
    std::sort(chain.entities.begin(), chain.entities.end(),
              [](const sketch::Entity& a, const sketch::Entity& b) { return a.id < b.id; });
    const std::string args = api::sketch_edit::sketchSetArgs(feature, chain);
    auto job = w.host->app()->actions().invoke("sketch.set", args, api::Caller::User);
    REQUIRE(QTest::qWaitFor([&] { return job.done(); }, 20000));
    REQUIRE(job.wait());
    REQUIRE(QTest::qWaitFor([&] { return w.sketch()->entityCount() == 401; }, 20000));
    REQUIRE(w.settled());
    REQUIRE(w.run("sketch.tool.select"));
    w.frame({-10, -60}, {110, 60});

    const SketchEntId end = points.back();
    const Vec2 start = *w.sketch()->displayView().point(end);
    const Vec2 target{start.x - 20, start.y + 30};

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

    w.moveTo(start);
    w.sendMouse(QEvent::MouseButtonPress, w.scenePoint(start), Qt::LeftButton, Qt::LeftButton);
    const int steps = 60;
    QPointF at;
    for (int i = 1; i <= steps; ++i) {
        const double t = static_cast<double>(i) / steps;
        const Vec2 p{start.x + (target.x - start.x) * t, start.y + (target.y - start.y) * t};
        // The pointer positions are fixed in the window before the drag changes the view's content.
        at = w.scenePoint(p);
        w.sendMouse(QEvent::MouseMove, at, Qt::LeftButton, Qt::LeftButton);
        QTest::qWait(16);
    }
    CHECK(w.sketch()->dragging());
    // The drag itself ends at the release; what follows is the commit (the drag's end on the worker, then sketch.set:
    // the document command, its journal record and the rebuild scheduling), reported separately.
    const qint64 worstDrag = worst;
    worst = 0;
    w.sendMouse(QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton);
    REQUIRE(QTest::qWaitFor([&] { return !w.sketch()->dragging(); }, 30000));
    REQUIRE(w.settled());
    tick.stop();
    const qint64 worstCommit = worst;
    worst = worstDrag;
    INFO("worst UI-thread gap " << worst << " ms; drag steps solved " << w.sketch()->dragSteps() << ", slowest "
                                << w.sketch()->slowestDragStepMs() << " ms");
    WARN("UI-thread watchdog while dragging a 200-curve sketch: worst gap "
         << worst << " ms, " << w.sketch()->dragSteps() << " steps, slowest step " << w.sketch()->slowestDragStepMs()
         << " ms");
    WARN("the commit after the drag: worst UI-thread gap " << worstCommit << " ms");
    CHECK(worst < 50);
    CHECK(w.sketch()->dragSteps() > 0);

    // The document has the dragged geometry (one undo step).
    const sketch::Sketch m = documentSketch(w);
    Vec2 moved{};
    for (const sketch::Entity& e : m.entities) {
        if (e.id == end)
            moved = std::get<sketch::Point>(e.geom).p;
    }
    // The chain is 200 * 0.64 mm long, so the end reaches the target.
    CHECK(std::hypot(moved.x - target.x, moved.y - target.y) < 0.5);
    w.shot("t092_drag_200");
    REQUIRE(w.run("sketch.mode.finish"));
    w.checkNoWarnings();
}

TEST_CASE("an offset in the plane picker makes a datum plane; the sketch lies on it", "[sketch][datum]")
{
    RAY_REQUIRE_RHI();
    Window w;
    w.newDocument();
    REQUIRE(w.run("sketch.mode.new"));
    QQuickItem* offset = nullptr;
    REQUIRE(QTest::qWaitFor([&] { return (offset = w.find(QStringLiteral("sketchPlaneOffset"))) != nullptr; }, 5000));
    offset->setProperty("text", QStringLiteral("10mm"));
    w.click(w.find(QStringLiteral("sketchPlane.xy")));
    REQUIRE(QTest::qWaitFor([&] { return w.sketch()->active(); }, 10000));
    CHECK(w.sketch()->plane().origin.z == Approx(10));
    CHECK(w.sketch()->plane().normal.z == Approx(1));
    const auto& features = w.document()->state()->features;
    REQUIRE(features.size() == 1);
    CHECK(features[0].type == "datum_plane");
    w.frame({-20, -20}, {60, 40});
    REQUIRE(w.run("sketch.tool.circle"));
    w.clickAt({0, 0});
    w.clickAt({5, 0});
    REQUIRE(w.settled());
    REQUIRE(w.document()->state()->features.size() == 2);
    CHECK(w.document()->state()->features[1].propsJson.find("\"datum\"") != std::string::npos);
    REQUIRE(w.run("sketch.mode.finish"));
    w.checkNoWarnings();
}

TEST_CASE("a sketch on a picked planar face; a curve dragged by its middle", "[sketch][face]")
{
    RAY_REQUIRE_RHI();
    Window w;
    REQUIRE(w.host->files()->open(kPlate));
    REQUIRE(QTest::qWaitFor(
        [&] { return !w.host->files()->busy() && w.host->scene()->ready() && w.host->scene()->bodyCount() == 1; },
        120000));
    // The plate's top face (z = 10).
    const api::viewer::Body* body = w.host->scene()->body(0);
    REQUIRE(body != nullptr);
    std::optional<std::uint32_t> top;
    JobContext ctx;
    for (std::uint32_t i = 0; i < body->shape.count(kernel::SubKind::Face); ++i) {
        auto info = api::viewer::describeElement(body->shape, kernel::SubId{kernel::SubKind::Face, i}, ctx);
        REQUIRE(info);
        if (info->surface && info->surface->type == kernel::SurfaceType::Plane &&
            std::abs(info->surface->centroid.z - 10) < 1e-6)
            top = i;
    }
    REQUIRE(top.has_value());
    w.host->selection()->applyClick(
        render::PickHit{w.host->scene()->itemForBody(0), kernel::SubKind::Face, *top, Vec3{}, 0}, Qt::NoModifier);
    REQUIRE(w.run("sketch.mode.new"));
    QQuickItem* face = nullptr;
    REQUIRE(QTest::qWaitFor([&] { return (face = w.find(QStringLiteral("sketchPlane.face"))) != nullptr; }, 5000));
    w.click(face);
    REQUIRE(QTest::qWaitFor([&] { return w.sketch()->active(); }, 20000));
    CHECK(w.sketch()->plane().origin.z == Approx(10));
    CHECK(w.sketch()->plane().normal.z == Approx(1));
    w.frame({-10, -10}, {130, 90});
    w.shot("t092_face_sketch_empty");

    // A line, then dragged by its middle: the line follows the pointer (a temporary point on it is dragged).
    REQUIRE(w.run("sketch.tool.line"));
    w.clickAt({20, 20});
    w.clickAt({60, 25});
    QTest::keyClick(w.window(), Qt::Key_Escape);
    REQUIRE(w.settled());
    REQUIRE(w.run("sketch.tool.select"));
    const Vec2 grab{40, 22.5};
    const Vec2 to{40, 50};
    w.moveTo(grab);
    w.sendMouse(QEvent::MouseButtonPress, w.scenePoint(grab), Qt::LeftButton, Qt::LeftButton);
    QPointF at;
    for (int i = 1; i <= 20; ++i) {
        at = w.scenePoint({grab.x, grab.y + (to.y - grab.y) * i / 20.0});
        w.sendMouse(QEvent::MouseMove, at, Qt::LeftButton, Qt::LeftButton);
        QTest::qWait(16);
    }
    w.sendMouse(QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton);
    REQUIRE(QTest::qWaitFor([&] { return !w.sketch()->dragging(); }, 30000));
    REQUIRE(w.settled());
    const sketch::Sketch m = documentSketch(w);
    CHECK(countOf<sketch::Point>(m) == 2); // the temporary point is gone
    CHECK(m.constraints.empty());
    // The line now passes through the drop point: both ends moved up.
    double ySum = 0;
    for (const sketch::Entity& e : m.entities) {
        if (const auto* p = std::get_if<sketch::Point>(&e.geom))
            ySum += p->p.y;
    }
    CHECK(ySum / 2 > 35);
    w.shot("t092_face_sketch");
    REQUIRE(w.run("sketch.mode.finish"));
    w.checkNoWarnings();
}

TEST_CASE("the sketch commands are in the palette; their keys do not clash", "[sketch][palette]")
{
    RAY_REQUIRE_RHI();
    Window w;
    ui::CommandCatalog& commands = *w.host->commands();
    for (const char* id :
         {"sketch.mode.new", "sketch.mode.finish", "sketch.tool.line", "sketch.tool.rectangle", "sketch.tool.circle",
          "sketch.tool.arc3", "sketch.tool.arcCenter", "sketch.tool.arcTangent", "sketch.tool.slot",
          "sketch.tool.spline", "sketch.tool.construction", "sketch.tool.trim", "sketch.tool.extend",
          "sketch.tool.offset", "sketch.tool.fillet", "sketch.extend", "sketch.offset", "sketch.fillet"}) {
        INFO(id);
        CHECK(commands.find(QString::fromLatin1(id)).has_value());
    }
    CHECK(!commands.disabledReason(*commands.find(QStringLiteral("sketch.tool.line"))).isEmpty());
    CHECK(w.host->shortcuts()->conflicts().isEmpty());
    CHECK(w.host->shortcuts()->sequence(QStringLiteral("sketch.tool.line")) == QStringLiteral("L"));
    w.newDocument();
    CHECK(commands.disabledReason(*commands.find(QStringLiteral("feature.sketch.add"))).isEmpty());
    // From the palette, New Sketch opens the plane picker (no argument form).
    CHECK(!commands.needsForm(*commands.find(QStringLiteral("feature.sketch.add"))));
    REQUIRE(commands.trigger(QStringLiteral("feature.sketch.add")));
    CHECK(w.sketch()->placing());
    w.sketch()->cancelPlacing();
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
