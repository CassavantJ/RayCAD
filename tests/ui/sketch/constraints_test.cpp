// Constraints and dimensions in the desktop app (task T-093), on the real Qt Quick window with the real session:
//
//   - a rectangle drawn by pointer; two sides dimensioned with the smart dimension tool (D, clicks, the value typed
//     into the field); one edited to a document parameter's name; the model holds the dimensions and the label
//     offsets; changing the parameter moves the geometry; bad input shows its error and keeps the field; each step is
//     one undo step (undo / redo)
//   - every geometric constraint applied from a fitting selection lands in the model; buttons say what to select when
//     the selection does not fit; hovering a glyph highlights its entities; a glyph selected and Delete removes it
//   - a dimension made driven and driving again; a redundant new dimension is offered as driven
//   - a conflicting dimension: the offenders list matches the solver's own output, a click selects one, and the view
//     keeps the last good shape
//   - an open sketch with dimensions shown renders no frames while idle
//
// Screenshots: with RAY_T093_SHOTS set to a folder, a fully dimensioned profile is saved there.
#include "../scene_graph_probe.h"
#include "../window_focus.h"
#include "app_context.h"
#include "app_host.h"
#include "app_options.h"
#include "files.h"
#include "palette_commands.h"
#include "scene_controller.h"
#include "session.h"
#include "shortcuts.h"
#include "sketch/constraints_model.h"
#include "sketch/dimensions_overlay.h"
#include "sketch/sketch_mode.h"
#include "sketch/sketch_overlay.h"
#include "theme_object.h"
#include "viewport_item.h"

#include <ray_api/sketch_edit.h>
#include <ray_render/viewport.h>

#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QTemporaryDir>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>
#include <QtGui/QMouseEvent>
#include <QtQml/QQmlApplicationEngine>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>
#include <QtTest/QTest>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace ray;
using Catch::Approx;
using ray::ui::AppHost;
using ray::ui::AppOptions;
using ray::ui::SketchConstraintsModel;
using ray::ui::SketchModel;

namespace {

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
    SketchModel* sketch() const
    {
        auto* s =
            host->engine()->singletonInstance<SketchModel*>(QStringLiteral("RayCad.Ui"), QStringLiteral("SketchTool"));
        REQUIRE(s != nullptr);
        return s;
    }
    SketchConstraintsModel* constraints() const
    {
        auto* c = host->engine()->singletonInstance<SketchConstraintsModel*>(QStringLiteral("RayCad.Ui"),
                                                                             QStringLiteral("SketchConstraints"));
        REQUIRE(c != nullptr);
        return c;
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
    ui::SketchDimensionOverlay* dimensionOverlay() const
    {
        auto* o = qobject_cast<ui::SketchDimensionOverlay*>(find(QStringLiteral("dimensionOverlay")));
        REQUIRE(o != nullptr);
        return o;
    }
    void sendMouse(QEvent::Type type, QPointF scenePos, Qt::MouseButton button, Qt::MouseButtons buttons,
                   Qt::KeyboardModifiers mods = Qt::NoModifier) const
    {
        QMouseEvent e(type, scenePos, window()->mapToGlobal(scenePos), button, buttons, mods);
        QGuiApplication::sendEvent(window(), &e);
    }
    void clickScene(QPointF c, Qt::KeyboardModifiers mods = Qt::NoModifier) const
    {
        sendMouse(QEvent::MouseMove, c, Qt::NoButton, Qt::NoButton, mods);
        QCoreApplication::processEvents();
        sendMouse(QEvent::MouseButtonPress, c, Qt::LeftButton, Qt::LeftButton, mods);
        sendMouse(QEvent::MouseButtonRelease, c, Qt::LeftButton, Qt::NoButton, mods);
        QCoreApplication::processEvents();
    }
    void click(QQuickItem* item) const
    {
        REQUIRE(item != nullptr);
        grab();
        clickScene(item->mapToScene(QPointF(item->width() / 2, item->height() / 2)));
    }
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
    void clickAt(Vec2 uv, Qt::KeyboardModifiers mods = Qt::NoModifier) const { clickScene(scenePoint(uv), mods); }
    bool settled() const
    {
        return QTest::qWaitFor(
            [&] {
                SketchModel* s = sketch();
                return s->inFlight() == 0 && !s->solving() && !s->dragging() && constraints()->pending() == 0;
            },
            20000);
    }
    doc::Document* document() const
    {
        api::OpenDocument* od = host->app()->active();
        REQUIRE(od != nullptr);
        return od->document.get();
    }
    void newDocument() const
    {
        host->files()->newPart();
        REQUIRE(QTest::qWaitFor([&] { return host->session()->hasDocument(); }, 20000));
        grab();
    }
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
    // A registry action with its arguments, waited for; then the sketch has caught up.
    void action(const char* id, const std::string& args) const
    {
        auto job = host->app()->actions().invoke(id, args, api::Caller::User);
        REQUIRE(QTest::qWaitFor([&] { return job.done(); }, 20000));
        auto r = job.wait();
        if (!r)
            FAIL(id << " failed: " << r.error().message);
        REQUIRE(settled());
        REQUIRE(QTest::qWaitFor([&] { return sketchMatchesDocument(); }, 20000));
        REQUIRE(settled());
    }
    bool sketchMatchesDocument() const
    {
        const auto id = sketch()->featureId();
        if (!id)
            return true;
        auto model = api::sketch_edit::modelOf(*document()->state(), *id);
        if (!model)
            return !sketch()->featureInDocument();
        const sketch::Sketch& mine = sketch()->model();
        if (mine.constraints.size() != model->constraints.size() || mine.entities.size() != model->entities.size())
            return false;
        for (std::size_t i = 0; i < mine.constraints.size(); ++i) {
            const sketch::Constraint& a = mine.constraints[i];
            const sketch::Constraint& b = model->constraints[i];
            if (a.id != b.id || a.driven != b.driven || a.value != b.value || a.expression != b.expression)
                return false;
        }
        return true;
    }
    void undo(bool redo = false) const
    {
        const std::size_t before = document()->undoCount();
        REQUIRE(host->context()->invoke(redo ? QStringLiteral("edit.redo") : QStringLiteral("edit.undo")));
        REQUIRE(QTest::qWaitFor([&] { return document()->undoCount() != before; }, 20000));
        REQUIRE(settled());
        REQUIRE(QTest::qWaitFor([&] { return sketchMatchesDocument(); }, 20000));
        REQUIRE(settled());
    }
    void type(const QString& text) const
    {
        for (const QChar c : text)
            QTest::keyClick(window(), c.toLatin1());
    }
    void shot(const char* name) const
    {
        const QString dir = qEnvironmentVariable("RAY_T093_SHOTS");
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

sketch::Sketch documentSketch(const Window& w)
{
    const auto id = w.sketch()->featureId();
    REQUIRE(id.has_value());
    auto model = api::sketch_edit::modelOf(*w.document()->state(), *id);
    REQUIRE(model);
    return *model;
}

// Copies (the sketch is often a temporary).
std::vector<std::optional<sketch::Constraint>> ofType(const sketch::Sketch& s, sketch::ConstraintType type)
{
    std::vector<std::optional<sketch::Constraint>> out;
    for (const sketch::Constraint& c : s.constraints) {
        if (c.type == type)
            out.push_back(c);
    }
    return out;
}

void enterXy(Window& w)
{
    REQUIRE(w.run("sketch.mode.new"));
    QQuickItem* xy = nullptr;
    REQUIRE(QTest::qWaitFor([&] { return (xy = w.find(QStringLiteral("sketchPlane.xy"))) != nullptr; }, 5000));
    w.click(xy);
    REQUIRE(QTest::qWaitFor([&] { return w.sketch()->active(); }, 5000));
    w.frame({-30, -30}, {110, 70});
}

// A rectangle from (0, 0) to (40, 25) by pointer; its four lines by position: bottom, right, top, left.
struct Rect {
    SketchEntId bottom, right, top, left;
};

Rect drawRectangle(Window& w, Vec2 lo = {0, 0}, Vec2 hi = {40, 25})
{
    REQUIRE(w.run("sketch.tool.rectangle"));
    w.clickAt(lo);
    w.clickAt(hi);
    REQUIRE(w.settled());
    REQUIRE(w.run("sketch.tool.select"));
    const ui::sketchui::SketchView& view = w.sketch()->displayView();
    Rect r;
    for (const ui::sketchui::ViewCurve& c : view.curves()) {
        if (c.kind != ui::sketchui::ViewCurve::Kind::Line)
            continue;
        const Vec2 m{(c.a.x + c.b.x) / 2, (c.a.y + c.b.y) / 2};
        const auto near = [](double a, double b) {
            return std::abs(a - b) < 1e-6;
        };
        if (near(m.y, lo.y) && m.x > lo.x && m.x < hi.x)
            r.bottom = c.id;
        else if (near(m.y, hi.y) && m.x > lo.x && m.x < hi.x)
            r.top = c.id;
        else if (near(m.x, lo.x) && m.y > lo.y && m.y < hi.y)
            r.left = c.id;
        else if (near(m.x, hi.x) && m.y > lo.y && m.y < hi.y)
            r.right = c.id;
    }
    REQUIRE(r.bottom.value != Uuid{});
    REQUIRE(r.left.value != Uuid{});
    return r;
}

double lineLength(const ui::sketchui::SketchView& view, const SketchEntId& line)
{
    const ui::sketchui::ViewCurve* c = view.curve(line);
    REQUIRE(c != nullptr);
    return std::hypot(c->b.x - c->a.x, c->b.y - c->a.y);
}

// The solved length of a line once the solve of the current model is back; waits a while for `expected` (the
// sketch follows the document's listener, which may run after the step reports done).
double solvedLength(const Window& w, const SketchEntId& line, double expected)
{
    double length = -1;
    (void)QTest::qWaitFor(
        [&] {
            const auto solved = w.sketch()->solved();
            if (!solved || !w.settled())
                return false;
            length = lineLength(ui::sketchui::SketchView(*solved), line);
            return std::abs(length - expected) < 1e-6;
        },
        10000);
    return length;
}

// The smart dimension tool: picks by click, then the label placed at `label`; the value typed and Enter.
void dimensionByClicks(Window& w, const std::vector<Vec2>& picks, Vec2 label, const QString& value)
{
    REQUIRE(w.run("sketch.constrain.dimension"));
    REQUIRE(w.constraints()->dimensionTool());
    for (const Vec2 p : picks) {
        const std::size_t before = w.constraints()->picks().size();
        w.clickAt(p);
        INFO("pick at " << p.x << ", " << p.y);
        CHECK(w.constraints()->picks().size() == before + 1);
    }
    w.moveTo(label);
    REQUIRE(w.constraints()->candidate().has_value());
    INFO("dimension type " << static_cast<int>(w.constraints()->candidate()->type) << " on "
                           << w.constraints()->candidate()->refs.size() << " refs; the sketch is "
                           << w.sketch()->status().toStdString());
    w.clickAt(label);
    REQUIRE(w.constraints()->editing());
    REQUIRE(w.constraints()->editNew());
    QQuickItem* field = w.find(QStringLiteral("dimensionField"));
    REQUIRE(field != nullptr);
    REQUIRE(QTest::qWaitFor([&] { return field->hasActiveFocus(); }, 5000));
    // The trial solve (does it over-constrain?) is back before Enter.
    REQUIRE(QTest::qWaitFor([&] { return !w.constraints()->editChecking(); }, 20000));
    w.type(value);
    QTest::keyClick(w.window(), Qt::Key_Return);
    INFO("the field says: " << w.constraints()->editError().toStdString() << "; over-constrains "
                            << w.constraints()->editOverConstrains());
    REQUIRE(!w.constraints()->editing());
    REQUIRE(w.settled());
    REQUIRE(QTest::qWaitFor([&] { return w.sketchMatchesDocument(); }, 20000));
}

QQuickItem* labelItem(const Window& w, const Uuid& id)
{
    QQuickItem* item = nullptr;
    REQUIRE(QTest::qWaitFor(
        [&] { return (item = w.find(QStringLiteral("dimensionLabel.") + QString::fromStdString(id.toString()))); },
        5000));
    return item;
}

} // namespace

TEST_CASE("smart dimensions on a rectangle, an expression with a parameter, undo per step",
          "[sketch][constraints][dimensions]")
{
    RAY_REQUIRE_RHI();
    Window w;
    w.newDocument();
    w.action("param.set", R"({"id":"a0000000-0000-4000-8000-000000000001","name":"height_p","expression":"30 mm",)"
                          R"("kind":"length"})");
    enterXy(w);
    const Rect r = drawRectangle(w);
    SketchConstraintsModel& c = *w.constraints();
    const std::size_t afterRect = w.document()->undoCount();

    // D, the bottom line, the label below it: its length.
    dimensionByClicks(w, {{20, 0}}, {20, -8}, QStringLiteral("50 mm"));
    CHECK(c.dimensionTool()); // the tool stays on for the next dimension
    {
        const sketch::Sketch m = documentSketch(w);
        const auto d = ofType(m, sketch::ConstraintType::Distance);
        REQUIRE(d.size() == 1);
        CHECK(d[0]->refs == std::vector<SketchEntId>{r.bottom});
        CHECK(d[0]->value == Approx(50));
        CHECK(!d[0]->expression);
        // The label sits where it was placed: below the line.
        const auto labels = api::sketch_edit::labelsOf(*w.document()->state(), *w.sketch()->featureId());
        REQUIRE(labels.count(d[0]->id) == 1);
        CHECK(labels.at(d[0]->id).y < 0);
    }
    CHECK(w.document()->undoCount() == afterRect + 1);
    CHECK(solvedLength(w, r.bottom, 50) == Approx(50).epsilon(1e-9));

    // Two points (the left line's ends, where the solve put them) with the label beside them: the vertical distance.
    {
        const ui::sketchui::ViewCurve* left = w.sketch()->displayView().curve(r.left);
        REQUIRE(left != nullptr);
        dimensionByClicks(w, {left->a, left->b}, {left->a.x - 10, (left->a.y + left->b.y) / 2},
                          QStringLiteral("20 mm"));
    }
    {
        const sketch::Sketch m = documentSketch(w);
        const auto d = ofType(m, sketch::ConstraintType::DistanceY);
        REQUIRE(d.size() == 1);
        CHECK(d[0]->value == Approx(20));
    }
    CHECK(w.document()->undoCount() == afterRect + 2);
    CHECK(solvedLength(w, r.left, 20) == Approx(20).epsilon(1e-9));
    QTest::keyClick(w.window(), Qt::Key_Escape); // leaves the tool
    CHECK(!c.dimensionTool());

    // Double-click the vertical dimension's label: the field opens; bad input keeps it open with the error.
    const Uuid vertical = ofType(documentSketch(w), sketch::ConstraintType::DistanceY)[0]->id;
    QQuickItem* label = labelItem(w, vertical);
    w.grab();
    const QPointF at = label->mapToScene(QPointF(label->width() / 2, label->height() / 2));
    w.sendMouse(QEvent::MouseMove, at, Qt::NoButton, Qt::NoButton);
    QTest::mouseDClick(w.window(), Qt::LeftButton, Qt::NoModifier, at.toPoint());
    REQUIRE(QTest::qWaitFor([&] { return c.editing(); }, 5000));
    CHECK(!c.editNew());
    CHECK(c.editId() == QString::fromStdString(vertical.toString()));
    QQuickItem* field = w.find(QStringLiteral("dimensionField"));
    REQUIRE(QTest::qWaitFor([&] { return field->hasActiveFocus(); }, 5000));
    w.type(QStringLiteral("nosuch*2"));
    QTest::keyClick(w.window(), Qt::Key_Return);
    CHECK(c.editing());
    CHECK(!c.editError().isEmpty());
    QQuickItem* error = w.find(QStringLiteral("dimensionError"));
    REQUIRE(error != nullptr);
    CHECK(error->isVisible());
    CHECK(w.document()->undoCount() == afterRect + 2);
    w.shot("t093_value_field_error");

    // A parameter's name: the dimension takes the expression.
    QTest::keyClick(w.window(), 'a', Qt::ControlModifier);
    w.type(QStringLiteral("height_p"));
    QTest::keyClick(w.window(), Qt::Key_Return);
    REQUIRE(!c.editing());
    REQUIRE(w.settled());
    REQUIRE(QTest::qWaitFor([&] { return w.sketchMatchesDocument(); }, 20000));
    {
        const auto d = ofType(documentSketch(w), sketch::ConstraintType::DistanceY);
        REQUIRE(d.size() == 1);
        REQUIRE(d[0]->expression.has_value());
        CHECK(*d[0]->expression == "height_p");
        CHECK(d[0]->value == Approx(30));
    }
    CHECK(w.document()->undoCount() == afterRect + 3);
    CHECK(solvedLength(w, r.left, 30) == Approx(30).epsilon(1e-9));

    // Changing the parameter moves the geometry.
    w.action("param.set", R"({"id":"a0000000-0000-4000-8000-000000000001","expression":"45 mm"})");
    REQUIRE(QTest::qWaitFor(
        [&] {
            const auto s = w.sketch()->solved();
            return s && std::abs(lineLength(ui::sketchui::SketchView(*s), r.left) - 45) < 1e-6;
        },
        20000));
    // The label shows the parameter's value now.
    REQUIRE(QTest::qWaitFor(
        [&] {
            return c.labels() != nullptr &&
                   static_cast<ui::ConstraintRows*>(c.labels())
                       ->find(QString::fromStdString(vertical.toString()))
                       .value(QStringLiteral("text"))
                       .toString()
                       .contains(QStringLiteral("1.7717")); // 45 mm in the new document's inches
        },
        5000));

    // Undo and redo step by step: the parameter, the expression, the vertical dimension, the length.
    w.undo();
    CHECK(solvedLength(w, r.left, 30) == Approx(30).epsilon(1e-9));
    w.undo();
    CHECK(!ofType(documentSketch(w), sketch::ConstraintType::DistanceY)[0]->expression);
    CHECK(solvedLength(w, r.left, 20) == Approx(20).epsilon(1e-9));
    w.undo();
    CHECK(ofType(documentSketch(w), sketch::ConstraintType::DistanceY).empty());
    w.undo();
    CHECK(ofType(documentSketch(w), sketch::ConstraintType::Distance).empty());
    CHECK(w.document()->undoCount() == afterRect);
    w.undo(true);
    w.undo(true);
    CHECK(ofType(documentSketch(w), sketch::ConstraintType::DistanceY).size() == 1);
    CHECK(solvedLength(w, r.bottom, 50) == Approx(50).epsilon(1e-9));
    w.checkNoWarnings();
}

TEST_CASE("every geometric constraint from a fitting selection; glyphs hover, select and delete",
          "[sketch][constraints]")
{
    RAY_REQUIRE_RHI();
    Window w;
    w.newDocument();
    enterXy(w);
    // One line by pointer makes the feature; the rest is added as free geometry (no inferred constraints).
    REQUIRE(w.run("sketch.tool.line"));
    w.clickAt({-20, -20});
    w.clickAt({-10, -22});
    QTest::keyClick(w.window(), Qt::Key_Escape);
    REQUIRE(w.settled());
    REQUIRE(w.run("sketch.tool.select"));
    const FeatureId feature = *w.sketch()->featureId();

    std::vector<sketch::Entity> add;
    const auto point = [&](double x, double y) {
        const SketchEntId id{Uuid::generate()};
        add.push_back({id, sketch::Point{{x, y}}, false});
        return id;
    };
    const auto line = [&](Vec2 a, Vec2 b) {
        const SketchEntId pa = point(a.x, a.y), pb = point(b.x, b.y), id{Uuid::generate()};
        add.push_back({id, sketch::Line{pa, pb}, false});
        return id;
    };
    const auto circle = [&](Vec2 c, double r) {
        const SketchEntId pc = point(c.x, c.y), id{Uuid::generate()};
        add.push_back({id, sketch::Circle{pc, r}, false});
        return id;
    };
    const SketchEntId l1 = line({0, 0}, {30, 2});
    const SketchEntId l2 = line({0, 10}, {28, 14});
    const SketchEntId l3 = line({40, 0}, {42, 20});
    const SketchEntId l4 = line({50, 0}, {70, 1});
    const SketchEntId l5 = line({0, 50}, {20, 52});
    const SketchEntId l6 = line({30, 49}, {50, 47});
    const SketchEntId c1 = circle({80, 10}, 5);
    const SketchEntId c2 = circle({82, 30}, 3);
    const SketchEntId arcCentre = point(100, 10), arcStart = point(105, 10), arcEnd = point(100, 15);
    const SketchEntId a1{Uuid::generate()};
    add.push_back({a1, sketch::Arc{arcCentre, arcStart, arcEnd, true}, false});
    const SketchEntId p1 = point(60, 6), p2 = point(10, 30), p3 = point(15, 40), p4 = point(60, 30), p5 = point(61, 31),
                      p6 = point(88, 12), p7 = point(70, 40), p8 = point(80, 41);
    w.action("sketch.entity.add", api::sketch_edit::entityAddArgs(feature, add, {}));
    SketchModel& s = *w.sketch();
    SketchConstraintsModel& c = *w.constraints();

    const auto apply = [&](const char* kind, std::vector<SketchEntId> selection) {
        INFO(kind);
        s.selectEntities(selection);
        REQUIRE(s.selectedIds().size() == selection.size());
        CHECK(c.reason(QString::fromLatin1(kind)).isEmpty());
        const std::size_t before = w.document()->undoCount();
        REQUIRE(w.run((std::string("sketch.constrain.") + kind).c_str()));
        REQUIRE(QTest::qWaitFor([&] { return w.document()->undoCount() == before + 1; }, 20000));
        REQUIRE(w.settled());
        REQUIRE(QTest::qWaitFor([&] { return w.sketchMatchesDocument(); }, 20000));
        s.clearSelection();
    };
    const auto has = [&](sketch::ConstraintType type, std::vector<SketchEntId> refs) {
        std::sort(refs.begin(), refs.end());
        for (const sketch::Constraint& k : documentSketch(w).constraints) {
            std::vector<SketchEntId> r = k.refs;
            std::sort(r.begin(), r.end());
            if (k.type == type && r == refs)
                return true;
        }
        return false;
    };
    using T = sketch::ConstraintType;

    // A selection that does not fit says what to select.
    s.selectEntities({c1});
    CHECK(c.reason(QStringLiteral("horizontal")) == QStringLiteral("Select one or more lines, or two points"));
    CHECK(c.reason(QStringLiteral("perpendicular")) == QStringLiteral("Select two lines"));
    const QVariantList actions = c.actions();
    REQUIRE(actions.size() == 12);
    for (const QVariant& a : actions) {
        const QVariantMap m = a.toMap();
        if (m.value(QStringLiteral("name")) == QStringLiteral("horizontal")) {
            CHECK(!m.value(QStringLiteral("enabled")).toBool());
            CHECK(!m.value(QStringLiteral("reason")).toString().isEmpty());
        }
        if (m.value(QStringLiteral("name")) == QStringLiteral("fix"))
            CHECK(m.value(QStringLiteral("enabled")).toBool());
    }
    s.clearSelection();

    apply("horizontal", {l1});
    CHECK(has(T::Horizontal, {l1}));
    apply("vertical", {l3});
    CHECK(has(T::Vertical, {l3}));
    apply("parallel", {l1, l2});
    CHECK(has(T::Parallel, {l1, l2}));
    apply("perpendicular", {l5, l6});
    CHECK(has(T::Perpendicular, {l5, l6}));
    apply("tangent", {l4, c1});
    CHECK(has(T::Tangent, {l4, c1}));
    apply("equal", {c1, c2});
    CHECK(has(T::Equal, {c1, c2}));
    apply("midpoint", {p1, l4});
    CHECK(has(T::Midpoint, {p1, l4}));
    apply("concentric", {c2, a1});
    CHECK(has(T::Concentric, {c2, a1}));
    {
        const std::size_t onCurve = ofType(documentSketch(w), T::PointOnCurve).size();
        apply("collinear", {l2, l5});
        CHECK(ofType(documentSketch(w), T::PointOnCurve).size() == onCurve + 2); // sketch.h has no Collinear
    }
    apply("symmetric", {p2, p3, l3});
    CHECK(has(T::Symmetric, {p2, p3, l3}));
    apply("coincident", {p4, p5});
    CHECK(has(T::Coincident, {p4, p5}));
    apply("coincident", {p6, c1});
    CHECK(has(T::PointOnCurve, {p6, c1}));
    apply("fix", {p7});
    CHECK(has(T::Fix, {p7}));
    // The same again is refused: it is there already.
    s.selectEntities({p7});
    CHECK(c.reason(QStringLiteral("fix")) == QStringLiteral("The selection has this constraint already"));
    s.clearSelection();

    // By clicks and a key: two points (Shift adds to the selection), then H.
    w.clickAt({70, 40});
    w.clickAt({80, 41}, Qt::ShiftModifier);
    REQUIRE(s.selectedIds().size() == 2);
    {
        const std::size_t before = w.document()->undoCount();
        QTest::keyClick(w.window(), Qt::Key_H);
        REQUIRE(QTest::qWaitFor([&] { return w.document()->undoCount() == before + 1; }, 20000));
        REQUIRE(w.settled());
        CHECK(has(T::Horizontal, {p7, p8}));
    }
    s.clearSelection();
    w.shot("t093_constraint_glyphs");

    // The glyph of the fix: hover highlights, click selects, Delete removes (one step).
    Uuid fixId;
    for (const sketch::Constraint& k : documentSketch(w).constraints) {
        if (k.type == T::Fix)
            fixId = k.id;
    }
    const QString cid = QString::fromStdString(fixId.toString());
    QQuickItem* glyph = nullptr;
    REQUIRE(QTest::qWaitFor([&] { return (glyph = w.find(QStringLiteral("constraintGlyph.") + cid + ".0")); }, 5000));
    w.grab();
    const QPointF g = glyph->mapToScene(QPointF(glyph->width() / 2, glyph->height() / 2));
    w.sendMouse(QEvent::MouseMove, g, Qt::NoButton, Qt::NoButton);
    REQUIRE(QTest::qWaitFor([&] { return c.hovered() == cid; }, 5000));
    {
        // Its entity is drawn highlighted.
        bool highlighted = false;
        const auto pv = s.toView(*s.displayView().point(p7));
        REQUIRE(pv.has_value());
        for (const ui::SketchOverlayScene::Point& pt : s.overlay().points) {
            if (pt.style == ui::SketchOverlayScene::Style::Highlight &&
                std::hypot(pt.at.x() - pv->x(), pt.at.y() - pv->y()) < 1)
                highlighted = true;
        }
        CHECK(highlighted);
    }
    w.clickScene(g);
    REQUIRE(QTest::qWaitFor([&] { return c.selected() == cid; }, 5000));
    {
        const std::size_t before = w.document()->undoCount();
        QTest::keyClick(w.window(), Qt::Key_Delete);
        REQUIRE(QTest::qWaitFor([&] { return w.document()->undoCount() == before + 1; }, 20000));
        REQUIRE(w.settled());
        REQUIRE(QTest::qWaitFor([&] { return w.sketchMatchesDocument(); }, 20000));
    }
    CHECK(!has(T::Fix, {p7}));
    CHECK(c.selected().isEmpty());
    w.undo();
    CHECK(has(T::Fix, {p7}));
    w.checkNoWarnings();
}

TEST_CASE("driven dimensions; a redundant one is offered as driven; conflicts are listed from the solver",
          "[sketch][constraints][dimensions]")
{
    RAY_REQUIRE_RHI();
    Window w;
    w.newDocument();
    enterXy(w);
    const Rect r = drawRectangle(w); // with horizontal and vertical constraints: top and bottom stay equal
    SketchConstraintsModel& c = *w.constraints();
    dimensionByClicks(w, {{20, 0}}, {20, -8}, QStringLiteral("40 mm"));
    QTest::keyClick(w.window(), Qt::Key_Escape);
    const Uuid width = ofType(documentSketch(w), sketch::ConstraintType::Distance)[0]->id;
    const QString widthId = QString::fromStdString(width.toString());

    // Driven and driving again, from the selection.
    w.click(labelItem(w, width));
    REQUIRE(QTest::qWaitFor([&] { return c.selected() == widthId; }, 5000));
    CHECK(c.selectedIsDimension());
    REQUIRE(w.run("sketch.constrain.driven"));
    REQUIRE(QTest::qWaitFor(
        [&] {
            return w.document()->state() && documentSketch(w).constraints.size() > 0 &&
                   ofType(documentSketch(w), sketch::ConstraintType::Distance)[0]->driven;
        },
        20000));
    REQUIRE(w.settled());
    REQUIRE(QTest::qWaitFor([&] { return c.selectedDriven(); }, 5000));
    {
        const QVariantMap row = static_cast<ui::ConstraintRows*>(c.labels())->find(widthId);
        CHECK(row.value(QStringLiteral("driven")).toBool());
        CHECK(row.value(QStringLiteral("text")).toString().startsWith(QLatin1Char('(')));
    }
    REQUIRE(w.run("sketch.constrain.driven"));
    REQUIRE(QTest::qWaitFor([&] { return !ofType(documentSketch(w), sketch::ConstraintType::Distance)[0]->driven; },
                            20000));
    REQUIRE(w.settled());
    CHECK(ofType(documentSketch(w), sketch::ConstraintType::Distance)[0]->value == Approx(40).epsilon(1e-9));
    QTest::keyClick(w.window(), Qt::Key_Escape); // clears the selection

    // The top line is as long as the bottom one already: a dimension on it would over-constrain.
    REQUIRE(w.run("sketch.constrain.dimension"));
    w.clickAt({20, 25});
    w.moveTo({20, 33});
    w.clickAt({20, 33});
    REQUIRE(c.editing());
    REQUIRE(QTest::qWaitFor([&] { return !c.editChecking(); }, 20000));
    CHECK(c.editOverConstrains());
    CHECK(!c.commitEdit(QStringLiteral("40 mm")));
    CHECK(!c.editError().isEmpty());
    QQuickItem* driven = w.find(QStringLiteral("dimensionAddDriven"));
    REQUIRE(driven != nullptr);
    REQUIRE(QTest::qWaitFor([&] { return driven->isVisible(); }, 5000));
    w.click(driven);
    REQUIRE(QTest::qWaitFor([&] { return ofType(documentSketch(w), sketch::ConstraintType::Distance).size() == 2; },
                            20000));
    REQUIRE(w.settled());
    {
        const auto d = ofType(documentSketch(w), sketch::ConstraintType::Distance);
        const auto top =
            std::find_if(d.begin(), d.end(), [&](const auto& k) { return k->refs == std::vector<SketchEntId>{r.top}; });
        for (const auto& k : d)
            UNSCOPED_INFO("distance on " << k->refs.size() << " refs, first " << k->refs[0].value.toString()
                                         << (k->driven ? " driven" : ""));
        UNSCOPED_INFO("top " << r.top.value.toString() << " bottom " << r.bottom.value.toString());
        REQUIRE(top != d.end());
        CHECK((*top)->driven);
    }
    CHECK(w.sketch()->status() == QStringLiteral("under"));
    QTest::keyClick(w.window(), Qt::Key_Escape);
    w.shot("t093_driven");

    // A driving dimension on the top line that disagrees: conflicting. The list is the solver's own answer.
    const double before = lineLength(w.sketch()->displayView(), r.bottom);
    sketch::Constraint bad;
    bad.id = Uuid::generate();
    bad.type = sketch::ConstraintType::Distance;
    bad.refs = {r.top};
    bad.value = 30;
    w.action("sketch.constraint.add", api::sketch_edit::constraintAddArgs(*w.sketch()->featureId(), {bad}));
    REQUIRE(QTest::qWaitFor(
        [&] {
            return w.sketch()->status() == QStringLiteral("conflict") || w.sketch()->status() == QStringLiteral("over");
        },
        20000));
    auto solver = api::sketch_edit::makeSolver();
    JobContext ctx;
    auto expected =
        solver->solve(api::sketch_edit::withEvaluatedDimensions(*w.document()->state(), w.sketch()->model()), ctx);
    REQUIRE(expected);
    std::set<std::string> want;
    for (const auto* ids : {&expected->conflicting, &expected->redundant, &expected->partiallyRedundant}) {
        for (const Uuid& id : *ids)
            want.insert(id.toString());
    }
    REQUIRE(!want.empty());
    REQUIRE(QTest::qWaitFor([&] { return static_cast<std::size_t>(c.offenders().size()) == want.size(); }, 5000));
    std::set<std::string> got;
    for (const QVariant& v : c.offenders())
        got.insert(v.toMap().value(QStringLiteral("cid")).toString().toStdString());
    CHECK(got == want);
    for (const std::string& id : want) {
        // Their labels or glyphs are drawn in the conflict colour.
        const QVariantMap row = static_cast<ui::ConstraintRows*>(c.labels())->find(QString::fromStdString(id));
        if (!row.isEmpty())
            CHECK(row.value(QStringLiteral("bad")).toBool());
    }
    // The view keeps the last good shape.
    CHECK(lineLength(w.sketch()->displayView(), r.bottom) == Approx(before).epsilon(1e-9));
    QQuickItem* issues = w.find(QStringLiteral("constraintIssues"));
    REQUIRE(issues != nullptr);
    REQUIRE(QTest::qWaitFor([&] { return issues->isVisible(); }, 5000));
    w.shot("t093_conflict");
    QQuickItem* first = w.find(QStringLiteral("constraintIssue.0"));
    REQUIRE(first != nullptr);
    w.click(first);
    REQUIRE(
        QTest::qWaitFor([&] { return c.selected() == c.offenders().value(0).toMap().value("cid").toString(); }, 5000));
    // Deleting the disagreeing one solves again.
    c.selectConstraint(QString::fromStdString(bad.id.toString()));
    REQUIRE(w.run("sketch.constrain.delete"));
    REQUIRE(QTest::qWaitFor([&] { return w.sketch()->status() == QStringLiteral("under"); }, 20000));
    REQUIRE(QTest::qWaitFor([&] { return c.offenders().isEmpty(); }, 5000));
    w.checkNoWarnings();
}

TEST_CASE("a fully dimensioned profile renders nothing while idle", "[sketch][constraints][idle]")
{
    RAY_REQUIRE_RHI();
    Window w;
    w.newDocument();
    enterXy(w);
    const Rect r = drawRectangle(w);
    (void)r;
    // Fix the corner at the origin, dimension the width and height, a circle inside with its diameter and position.
    w.clickAt({0, 0});
    REQUIRE(w.run("sketch.constrain.fix"));
    REQUIRE(w.settled());
    QTest::keyClick(w.window(), Qt::Key_Escape);
    dimensionByClicks(w, {{20, 0}}, {20, -8}, QStringLiteral("60 mm"));
    dimensionByClicks(w, {{0, 12}}, {-9, 12}, QStringLiteral("35 mm"));
    QTest::keyClick(w.window(), Qt::Key_Escape);
    REQUIRE(w.run("sketch.tool.circle"));
    w.clickAt({45, 15});
    w.clickAt({50, 15});
    REQUIRE(w.settled());
    REQUIRE(w.run("sketch.tool.select"));
    dimensionByClicks(w, {{50, 15}}, {57, 24}, QStringLiteral("12 mm"));
    // The centre to the right side and to the bottom side: perpendicular distances.
    dimensionByClicks(w, {{45, 15}, {60, 10}}, {52, 30}, QStringLiteral("15 mm"));
    dimensionByClicks(w, {{45, 15}, {30, 0}}, {70, 7}, QStringLiteral("17.5 mm"));
    QTest::keyClick(w.window(), Qt::Key_Escape);
    REQUIRE(w.settled());
    REQUIRE(QTest::qWaitFor([&] { return w.sketch()->status() == QStringLiteral("full"); }, 20000));
    CHECK(w.sketch()->freeDof() == 0);
    CHECK(w.sketch()->statusText() == QStringLiteral("Fully constrained"));
    CHECK(static_cast<ui::ConstraintRows*>(w.constraints()->labels())->count() == 5);
    w.frame({-25, -20}, {85, 50});
    w.moveTo({100, 60});
    w.shot("t093_fully_dimensioned");

    REQUIRE(QTest::qWaitFor(
        [&] {
            doc::Document* d = w.document();
            return d != nullptr && !d->rebuildPending() && !w.host->session()->jobActive();
        },
        20000));
    QTest::qWait(800);
    int swapped = 0;
    QObject::connect(w.window(), &QQuickWindow::frameSwapped, w.window(), [&] { ++swapped; }, Qt::DirectConnection);
    const int paints = w.dimensionOverlay()->paints();
    const int sketchPaints = w.overlay()->paints();
    QTest::qWait(1000);
    CHECK(swapped == 0);
    CHECK(w.dimensionOverlay()->paints() == paints);
    CHECK(w.overlay()->paints() == sketchPaints);
    w.checkNoWarnings();
}

TEST_CASE("the constraint commands are in the palette with keys that do not clash", "[sketch][constraints][palette]")
{
    RAY_REQUIRE_RHI();
    Window w;
    ui::CommandCatalog& commands = *w.host->commands();
    for (const char* id :
         {"sketch.constrain.dimension", "sketch.constrain.coincident", "sketch.constrain.horizontal",
          "sketch.constrain.vertical", "sketch.constrain.parallel", "sketch.constrain.perpendicular",
          "sketch.constrain.tangent", "sketch.constrain.equal", "sketch.constrain.midpoint",
          "sketch.constrain.concentric", "sketch.constrain.collinear", "sketch.constrain.symmetric",
          "sketch.constrain.fix", "sketch.constrain.driven", "sketch.constrain.delete", "sketch.dimension.place"}) {
        INFO(id);
        CHECK(commands.find(QString::fromLatin1(id)).has_value());
    }
    CHECK(!commands.disabledReason(*commands.find(QStringLiteral("sketch.constrain.horizontal"))).isEmpty());
    CHECK(w.host->shortcuts()->conflicts().isEmpty());
    CHECK(w.host->shortcuts()->sequence(QStringLiteral("sketch.constrain.dimension")) == QStringLiteral("D"));
}
