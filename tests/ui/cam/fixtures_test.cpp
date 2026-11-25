// The fixtures panel (task T-077) on the real Qt Quick window with the real application session: a corpus plate with a
// setup in the CAM workspace; the panel opened with its key (Shift+F) and from the CAM tree's Fixtures button; the
// shipped generic vise added from the library (its two bodies become fixtures of the setup); the vise's fixed jaw
// mated to the plate's front face with clicks in the view, lowered with a typed Z, previewed, applied (one undo step)
// and undone; the moving jaw mated to the fixed jaw with the plate's width as the gap, so it closes on the plate; Esc
// cancels a placement and then closes the panel; the operation panel refuses a face of a fixture as geometry and takes
// the plate's. Screenshots go to build/shots/t077_*.png, in both themes. After T-077, the vise and parallels placed on
// the setup's stock with "Place on stock" (one undo step, across Y then X): build/shots/fixsnap_*.png.
// First, on their own: operation_tool.h's QPointer members need these classes whole.
#include "cam/operation_view.h"
#include "cam/setup_tool.h"

#include "../scene_graph_probe.h"
#include "../window_focus.h"
#include "app_context.h"
#include "app_host.h"
#include "app_options.h"
#include "cam/fixtures_tool.h"
#include "cam/operation_tool.h"
#include "cam/setup_tree.h"
#include "files.h"
#include "scene_controller.h"
#include "selection_model.h"
#include "session.h"
#include "theme_object.h"
#include "viewport_item.h"

#include <ray_api/cam_setup.h>

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QTemporaryDir>
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
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <string>

using namespace ray;
using Catch::Matchers::ContainsSubstring;
using ray::ui::AppHost;
using ray::ui::AppOptions;
using ray::ui::FixturesToolModel;
using ray::ui::OperationToolModel;
using ray::ui::SetupTreeModel;

namespace {

constexpr int kTimeoutMs = 60000;
// plate_thru_holes_1 (tools/corpus_gen): a 120 x 80 x 10 mm plate with six 6.6 mm through holes.
const QString kPlate = QStringLiteral(RAY_CORPUS_DIR "/raycad-synthetic/plate_thru_holes_1.stp");
const std::string kVmc = "b903ede7-5123-4c26-bc15-466e92fba4c6"; // the shipped "Generic VMC"
const std::string kSetup = "a0000000-0000-4000-8000-000000000001";

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
        REQUIRE(ray::test::focusWindow(host->window()));
        REQUIRE(host->viewport() != nullptr);
        REQUIRE(host->startSession(userData.path().toStdString()));
        host->app()->setConfirmHook([](const api::ActionInfo&, std::string_view, std::string_view) { return true; });
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
    FixturesToolModel* fixtures() const { return singleton<FixturesToolModel>("CamFixtures"); }
    OperationToolModel* operation() const { return singleton<OperationToolModel>("CamOperation"); }
    SetupTreeModel* tree() const { return singleton<SetupTreeModel>("CamTreeModel"); }
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
        QQuickItem* item = nullptr;
        const bool ok = QTest::qWaitFor([&] { return (item = find(objectName)) && item->isVisible(); }, 10000);
        INFO("item " << objectName.toStdString());
        REQUIRE(ok);
        return item;
    }
    void sendMouse(QEvent::Type type, QPointF scenePos, Qt::MouseButton button, Qt::MouseButtons buttons) const
    {
        QMouseEvent e(type, scenePos, window()->mapToGlobal(scenePos), button, buttons, Qt::NoModifier);
        QGuiApplication::sendEvent(window(), &e);
    }
    void clickScene(QPointF p) const
    {
        sendMouse(QEvent::MouseMove, p, Qt::NoButton, Qt::NoButton);
        QCoreApplication::processEvents();
        sendMouse(QEvent::MouseButtonPress, p, Qt::LeftButton, Qt::LeftButton);
        sendMouse(QEvent::MouseButtonRelease, p, Qt::LeftButton, Qt::NoButton);
        QCoreApplication::processEvents();
    }
    void click(const QString& objectName) const
    {
        QQuickItem* item = shown(objectName);
        grab();
        clickScene(item->mapToScene(QPointF(item->width() / 2, item->height() / 2)));
    }
    void key(Qt::Key k, Qt::KeyboardModifiers modifiers = Qt::NoModifier) const
    {
        if (QGuiApplication::focusWindow() != window())
            ray::test::focusWindow(window());
        QTest::keyClick(window(), k, modifiers);
        QCoreApplication::processEvents();
    }
    void typeInto(const QString& objectName, const QString& text) const
    {
        QQuickItem* field = shown(objectName);
        if (QGuiApplication::focusWindow() != window())
            ray::test::focusWindow(window());
        field->forceActiveFocus();
        QMetaObject::invokeMethod(field, "selectAll");
        QTest::keyClick(window(), Qt::Key_Delete);
        for (const QChar c : text)
            QTest::keyClick(window(), c.toLatin1());
        QCoreApplication::processEvents();
    }
    doc::Document* document() const
    {
        api::OpenDocument* od = host->app()->active();
        REQUIRE(od != nullptr);
        return od->document.get();
    }
    bool settled() const
    {
        doc::Document* d = host->app()->active() ? host->app()->active()->document.get() : nullptr;
        return d != nullptr && !d->rebuildPending() && !host->files()->busy() && host->scene()->ready() &&
               !host->scene()->meshing() && !view()->animating();
    }
    void settle() const
    {
        for (int pass = 0; pass < 2; ++pass) {
            REQUIRE(QTest::qWaitFor([&] { return settled(); }, kTimeoutMs));
            QTest::qWait(50);
        }
    }
    void open(const QString& path) const
    {
        REQUIRE(QFileInfo::exists(path));
        REQUIRE(host->files()->open(path));
        REQUIRE(QTest::qWaitFor([&] { return host->session()->hasDocument() && settled(); }, kTimeoutMs));
        settle();
        view()->fitAll();
        grab();
    }
    void toWorkspace(int index) const
    {
        click(QStringLiteral("workspaceTab.%1").arg(index));
        REQUIRE(QTest::qWaitFor([&] { return host->context()->workspace() == index; }, 5000));
        grab();
    }
    std::string invokeResult(const std::string& action, const std::string& args) const
    {
        JobHandle<api::ActionResult> job = host->app()->actions().invoke(action, args, api::Caller::User);
        REQUIRE(QTest::qWaitFor([&] { return job.done(); }, kTimeoutMs));
        auto r = job.wait();
        INFO(action << ": " << (r ? std::string() : r.error().message));
        REQUIRE(r);
        return r->resultJson;
    }
    // The fixtures panel has done what it was asked (an import, a pick, a preview, a commit) and the scene shows it.
    void idle() const
    {
        REQUIRE(QTest::qWaitFor([&] { return !fixtures()->busy() && settled(); }, kTimeoutMs));
        QTest::qWait(50);
        REQUIRE(QTest::qWaitFor([&] { return !fixtures()->busy() && settled(); }, kTimeoutMs));
    }
    int bodyIndex(const BodyId& id) const
    {
        for (int i = 0; i < host->scene()->bodyCount(); ++i) {
            const api::viewer::Body* b = host->scene()->body(i);
            if (b != nullptr && b->id == id)
                return i;
        }
        FAIL_CHECK("the scene does not show body " << id.value.toString());
        return -1;
    }
    BodyId bodyNamed(const std::string& name) const
    {
        for (const doc::Body& b : document()->state()->bodies) {
            if (b.name == name)
                return b.id;
        }
        FAIL_CHECK("no body " << name);
        return {};
    }
    // The planar face of a shown body looking along `n` (an axis) whose centroid is at `along` on it.
    kernel::SubId face(const BodyId& id, Vec3 n, double along, double area = 0) const
    {
        const api::viewer::Body* b = host->scene()->body(bodyIndex(id));
        REQUIRE(b != nullptr);
        for (std::uint32_t i = 0; i < b->shape.count(kernel::SubKind::Face); ++i) {
            const kernel::SubId sub{kernel::SubKind::Face, i};
            auto s = kernel::faceSample(b->shape, sub);
            auto info = kernel::surfaceInfo(b->shape, sub);
            if (!s || !info || info->type != kernel::SurfaceType::Plane)
                continue;
            const double at = info->centroid.x * n.x + info->centroid.y * n.y + info->centroid.z * n.z;
            if (std::abs(s->normal.x - n.x) < 1e-9 && std::abs(s->normal.y - n.y) < 1e-9 &&
                std::abs(s->normal.z - n.z) < 1e-9 && std::abs(at - along) < 1e-6 &&
                (area == 0 || std::abs(info->area - area) < 1e-6))
                return sub;
        }
        FAIL_CHECK("no such face");
        return {};
    }
    // A click in the view on that face: the selection takes the hit as a click of the viewport would.
    void clickFace(const BodyId& id, kernel::SubId sub) const
    {
        const int index = bodyIndex(id);
        const render::ItemId item = host->scene()->itemForBody(index);
        REQUIRE(item != 0);
        host->selection()->applyClick(render::PickHit{item, kernel::SubKind::Face, sub.index, Vec3{}, 0},
                                      Qt::NoModifier);
        QCoreApplication::processEvents();
    }
    Box3 boxOf(const BodyId& id) const
    {
        const doc::BuildResultPtr b = document()->build();
        REQUIRE(b != nullptr);
        return kernel::bounds(b->bodies.at(id).shape);
    }
    void shot(const QString& name) const
    {
        const QString dir = QStringLiteral(RAY_SHOTS_DIR);
        QDir().mkpath(dir);
        sendMouse(QEvent::MouseMove, QPointF(window()->width() / 2.0, window()->height() - 4.0), Qt::NoButton,
                  Qt::NoButton);
        QTest::qWait(300);
        grab().save(QDir(dir).filePath(name + QStringLiteral(".png")), "PNG");
    }
    void shotsBothThemes(const QString& name) const
    {
        const QString was = host->theme()->property("mode").toString();
        host->theme()->setProperty("mode", QStringLiteral("dark"));
        shot(name + QStringLiteral("_dark"));
        host->theme()->setProperty("mode", QStringLiteral("light"));
        shot(name + QStringLiteral("_light"));
        host->theme()->setProperty("mode", was);
    }
    void checkNoWarnings() const
    {
        for (const QString& warning : host->warnings())
            UNSCOPED_INFO(warning.toStdString());
        CHECK(host->warnings().isEmpty());
    }
};

const cam::Setup& setupOf(const doc::DocState& state)
{
    REQUIRE(state.cam != nullptr);
    REQUIRE(!state.cam->setups.empty());
    return state.cam->setups.front();
}

int transformCount(const doc::DocState& state)
{
    int n = 0;
    for (const doc::Feature& f : state.features)
        n += f.type == "transform_body" ? 1 : 0;
    return n;
}

} // namespace

TEST_CASE("the fixtures panel adds the generic vise, mates it to the part and closes its jaw on it",
          "[ui][cam][fixtures]")
{
    RAY_REQUIRE_RHI();
    Window w;
    w.open(kPlate);
    const BodyId plate = w.host->scene()->body(0)->id;
    const Box3 part = w.boxOf(plate);
    // A setup on the plate with the shipped VMC.
    const lib::MachineDef* m = w.host->app()->libraries()->machine(MachineId{Uuid::fromString(kVmc)});
    REQUIRE(m != nullptr);
    (void)w.invokeResult("cam.setup.add", R"({"id":")" + kSetup + R"(","name":"Op 1 - top","partBodies":[")" +
                                              plate.value.toString() + R"("],"machine":")" + kVmc +
                                              R"(","library":{"machines":[)" + api::cam_setup::machineJson(*m) + "]}}");
    w.toWorkspace(1);
    REQUIRE(QTest::qWaitFor([&] { return w.tree()->setupCount() == 1; }, kTimeoutMs));

    // ---- Open with the key; nothing there yet; the library lists the shipped models ----
    w.key(Qt::Key_F, Qt::ShiftModifier);
    REQUIRE(QTest::qWaitFor([&] { return w.fixtures()->shown(); }, 5000));
    w.shown(QStringLiteral("fixturesPanel"));
    w.shown(QStringLiteral("fixturesEmpty"));
    const QVariantList library = w.fixtures()->library();
    int viseIndex = -1;
    for (int i = 0; i < library.size(); ++i) {
        if (library[i].toMap().value(QStringLiteral("name")).toString() == QStringLiteral("Generic vise 6in"))
            viseIndex = i;
    }
    REQUIRE(viseIndex >= 0);
    w.shotsBothThemes(QStringLiteral("t077_panel_empty"));

    // ---- Add the vise: two fixture bodies, both fixtures of the setup, the vise opened for placing ----
    const std::size_t undoBefore = w.document()->undoCount();
    QQuickItem* combo = w.shown(QStringLiteral("fixturesLibrary"));
    combo->setProperty("currentIndex", viseIndex);
    w.click(QStringLiteral("fixturesAdd"));
    REQUIRE(QTest::qWaitFor([&] { return w.fixtures()->fixtures().size() == 2 && !w.fixtures()->placing().isEmpty(); },
                            kTimeoutMs));
    w.idle();
    const BodyId vise = w.bodyNamed("Generic 6 in vise");
    const BodyId jaw = w.bodyNamed("Generic 6 in vise moving jaw");
    CHECK(w.fixtures()->placing().toStdString() == vise.value.toString());
    CHECK(setupOf(*w.document()->state()).fixtureBodies == std::vector<BodyId>{vise, jaw});
    CHECK(w.document()->undoCount() == undoBefore + 2); // the import, and the setup's fixture bodies
    for (const doc::Body& b : w.document()->state()->bodies) {
        if (b.id == vise || b.id == jaw)
            CHECK(b.role == doc::Body::Role::Fixture);
    }
    CHECK(w.host->scene()->body(w.bodyIndex(vise))->fixture);

    // ---- Mate: the fixed jaw's clamping face, then the plate's front face; 5 mm down ----
    REQUIRE(w.fixtures()->pickMode() == QStringLiteral("face"));
    // A face of the plate is refused for the fixture's face, saying why.
    w.clickFace(plate, w.face(plate, {0, -1, 0}, -part.min.y));
    w.idle();
    CHECK_THAT(w.fixtures()->message().toStdString(), ContainsSubstring("the fixture being placed"));
    CHECK_FALSE(w.fixtures()->hasFace());
    w.clickFace(vise, w.face(vise, {0, 1, 0}, 0, 152.4 * 44.45));
    REQUIRE(QTest::qWaitFor([&] { return w.fixtures()->hasFace(); }, kTimeoutMs));
    REQUIRE(w.fixtures()->pickMode() == QStringLiteral("target"));
    w.clickFace(plate, w.face(plate, {0, -1, 0}, -part.min.y));
    REQUIRE(QTest::qWaitFor([&] { return w.fixtures()->hasTarget(); }, kTimeoutMs));
    w.typeInto(QStringLiteral("fixtureInput.z"), QStringLiteral("-5"));
    w.idle();
    INFO(w.fixtures()->message().toStdString());
    REQUIRE(w.fixtures()->previewStatus() == QStringLiteral("ok"));
    CHECK(w.fixtures()->previewsShown() > 0);
    CHECK(w.fixtures()->canCommit());
    // The preview never touched the document.
    CHECK(transformCount(*w.document()->state()) == 0);
    w.view()->fitAll();
    w.idle();
    w.shotsBothThemes(QStringLiteral("t077_place_vise_preview"));

    // ---- Apply: one undo step; the jaw face on the plate's front face, centred, 5 mm below centre ----
    const std::size_t undoBeforeApply = w.document()->undoCount();
    w.click(QStringLiteral("fixtureApply"));
    REQUIRE(QTest::qWaitFor([&] { return w.fixtures()->placing().isEmpty(); }, kTimeoutMs));
    w.idle();
    CHECK(w.document()->undoCount() == undoBeforeApply + 1);
    CHECK(transformCount(*w.document()->state()) == 1);
    const double cx = (part.min.x + part.max.x) / 2, cz = (part.min.z + part.max.z) / 2;
    Box3 v = w.boxOf(vise);
    CHECK(v.min.y == Catch::Approx(part.min.y - 60).margin(1e-5)); // the vise's back is 60 behind its jaw face
    CHECK((v.min.x + v.max.x) / 2 == Catch::Approx(cx).margin(1e-5));
    CHECK(v.max.z == Catch::Approx(cz + 44.45 / 2 - 5).margin(1e-5));
    // Undo puts it back where the file had it; redo places it again.
    (void)w.invokeResult("edit.undo", "{}");
    w.settle();
    CHECK(w.boxOf(vise).max.z == Catch::Approx(44.45).margin(1e-5));
    (void)w.invokeResult("edit.redo", "{}");
    w.settle();
    CHECK(w.boxOf(vise).max.z == Catch::Approx(cz + 44.45 / 2 - 5).margin(1e-5));

    // ---- Esc cancels a placement, a second Esc closes the panel; the CAM tree's button opens it again ----
    REQUIRE(w.fixtures()->place(QString::fromStdString(jaw.value.toString())));
    w.idle();
    const Hash256 before = w.document()->state()->stateHash();
    w.key(Qt::Key_Escape);
    REQUIRE(QTest::qWaitFor([&] { return w.fixtures()->placing().isEmpty(); }, 5000));
    CHECK(w.fixtures()->shown());
    CHECK(w.document()->state()->stateHash() == before);
    w.key(Qt::Key_Escape);
    REQUIRE(QTest::qWaitFor([&] { return !w.fixtures()->shown(); }, 5000));
    w.click(QStringLiteral("camTreeFixtures"));
    REQUIRE(QTest::qWaitFor([&] { return w.fixtures()->shown(); }, 5000));

    // ---- The moving jaw onto the fixed jaw, the plate's width apart: it closes on the plate ----
    w.click(QStringLiteral("fixturePlace.1"));
    REQUIRE(QTest::qWaitFor([&] { return w.fixtures()->placing().toStdString() == jaw.value.toString(); }, 5000));
    w.idle();
    w.clickFace(jaw, w.face(jaw, {0, -1, 0}, -50.8));
    REQUIRE(QTest::qWaitFor([&] { return w.fixtures()->hasFace(); }, kTimeoutMs));
    w.clickFace(vise, w.face(vise, {0, 1, 0}, part.min.y, 152.4 * 44.45));
    REQUIRE(QTest::qWaitFor([&] { return w.fixtures()->hasTarget(); }, kTimeoutMs));
    w.typeInto(QStringLiteral("fixtureInput.gap"), QString::number(part.max.y - part.min.y) + QStringLiteral(" mm"));
    w.idle();
    REQUIRE(w.fixtures()->previewStatus() == QStringLiteral("ok"));
    w.click(QStringLiteral("fixtureApply"));
    REQUIRE(QTest::qWaitFor([&] { return w.fixtures()->placing().isEmpty(); }, kTimeoutMs));
    w.idle();
    const Box3 j = w.boxOf(jaw);
    CHECK(j.min.y == Catch::Approx(part.max.y).margin(1e-5));
    CHECK(j.max.z == Catch::Approx(cz + 44.45 / 2 - 5).margin(1e-5));
    // The new jaw feature went after the vise's, so it follows the vise: move the vise 2 mm up and the jaw comes too.
    CHECK(transformCount(*w.document()->state()) == 2);
    REQUIRE(w.fixtures()->place(QString::fromStdString(vise.value.toString())));
    w.idle();
    CHECK(w.fixtures()->placingExisting());
    w.typeInto(QStringLiteral("fixtureInput.z"), QStringLiteral("-3"));
    w.idle();
    w.click(QStringLiteral("fixtureApply"));
    REQUIRE(QTest::qWaitFor([&] { return w.fixtures()->placing().isEmpty(); }, kTimeoutMs));
    w.idle();
    CHECK(w.boxOf(jaw).max.z == Catch::Approx(cz + 44.45 / 2 - 3).margin(1e-5));
    CHECK(w.boxOf(vise).max.z == Catch::Approx(cz + 44.45 / 2 - 3).margin(1e-5));
    w.view()->fitAll();
    w.idle();
    w.shotsBothThemes(QStringLiteral("t077_vise_closed"));
    w.fixtures()->close();

    // ---- The operation panel's picker: a fixture's face is refused as machining geometry, the plate's is taken ----
    REQUIRE(w.operation()->startFor(QString::fromStdString(kSetup)));
    w.operation()->setType(QStringLiteral("pocket2d"));
    REQUIRE(QTest::qWaitFor([&] { return !w.operation()->previewBusy(); }, kTimeoutMs));
    w.operation()->setPicking(true);
    REQUIRE(w.operation()->picking());
    // The refusal is a message in the panel's status (a later preview status may replace it): record every one.
    bool refused = false;
    const QMetaObject::Connection watch = QObject::connect(w.operation(), &OperationToolModel::statusChanged, [&] {
        refused = refused || w.operation()->message().contains(QStringLiteral("not a part body of the setup"));
    });
    w.clickFace(vise, w.face(vise, {0, 0, 1}, cz + 44.45 / 2 - 3, 152.4 * 60));
    REQUIRE(QTest::qWaitFor([&] { return refused; }, kTimeoutMs));
    REQUIRE(QTest::qWaitFor([&] { return !w.operation()->previewBusy(); }, kTimeoutMs));
    QObject::disconnect(watch);
    CHECK(w.operation()->picks().isEmpty());
    w.shotsBothThemes(QStringLiteral("t077_operation_refuses_fixture"));
    w.operation()->cancel();
    w.checkNoWarnings();
}

// After T-077: the vise placed on the setup's stock from the panel. The plate (120 x 80 x 10) with a setup whose stock
// is the plate; the generic vise and parallels added from the library (both fixtures of the setup); "Place on stock"
// with the grip on the shorter side (across Y, 80): the plate stands on the parallels, the moving jaw closed on its
// front face, the fixed jaw's back 60 behind its back face, centred along X; the jaws stand above the plate's top and
// the panel says so; one undo step, undone and redone; again across X (the four placements edited, not added); the
// vise's placement opened by hand shows its numbers.
TEST_CASE("the fixtures panel places the vise on the setup's stock in one step", "[ui][cam][fixtures]")
{
    RAY_REQUIRE_RHI();
    Window w;
    w.open(kPlate);
    const BodyId plate = w.host->scene()->body(0)->id;
    const Box3 part = w.boxOf(plate);
    const lib::MachineDef* m = w.host->app()->libraries()->machine(MachineId{Uuid::fromString(kVmc)});
    REQUIRE(m != nullptr);
    (void)w.invokeResult("cam.setup.add", R"({"id":")" + kSetup + R"(","name":"Op 1 - top","partBodies":[")" +
                                              plate.value.toString() + R"("],"machine":")" + kVmc +
                                              R"(","library":{"machines":[)" + api::cam_setup::machineJson(*m) + "]}}");
    w.toWorkspace(1);
    REQUIRE(QTest::qWaitFor([&] { return w.tree()->setupCount() == 1; }, kTimeoutMs));
    w.key(Qt::Key_F, Qt::ShiftModifier);
    REQUIRE(QTest::qWaitFor([&] { return w.fixtures()->shown(); }, 5000));
    // No vise yet: the section is not shown.
    CHECK(w.fixtures()->viseName().isEmpty());
    CHECK_FALSE(w.find(QStringLiteral("fixtureOnStock"))->isVisible());

    // ---- The vise and the parallels from the library; each opens for placing by hand, which is cancelled ----
    const QVariantList library = w.fixtures()->library();
    const auto add = [&](const QString& name, int fixturesAfter) {
        int index = -1;
        for (int i = 0; i < library.size(); ++i) {
            if (library[i].toMap().value(QStringLiteral("name")).toString() == name)
                index = i;
        }
        REQUIRE(index >= 0);
        w.shown(QStringLiteral("fixturesLibrary"))->setProperty("currentIndex", index);
        w.click(QStringLiteral("fixturesAdd"));
        REQUIRE(QTest::qWaitFor(
            [&] { return w.fixtures()->fixtures().size() == fixturesAfter && !w.fixtures()->placing().isEmpty(); },
            kTimeoutMs));
        w.idle();
        w.fixtures()->cancel();
        w.idle();
    };
    add(QStringLiteral("Generic vise 6in"), 2);
    add(QStringLiteral("Generic parallels 1x6in"), 4);
    const BodyId vise = w.bodyNamed("Generic 6 in vise");
    const BodyId jaw = w.bodyNamed("Generic 6 in vise moving jaw");
    const BodyId parA = w.bodyNamed("Generic parallel 1/8 x 1 x 6 in A");
    const BodyId parB = w.bodyNamed("Generic parallel 1/8 x 1 x 6 in B");
    CHECK(setupOf(*w.document()->state()).fixtureBodies.size() == 4);
    CHECK(transformCount(*w.document()->state()) == 0);
    CHECK(w.fixtures()->viseName() == QStringLiteral("Generic 6 in vise"));
    w.shown(QStringLiteral("fixtureOnStock"));
    QQuickItem* note = w.shown(QStringLiteral("fixtureOnStockNote"));
    CHECK_THAT(note->property("text").toString().toStdString(), ContainsSubstring("if the stock changes"));
    CHECK(w.fixtures()->grip() == QStringLiteral("shorter"));

    // ---- Place on stock: one undo step; the plate on the parallels, the jaws closed across Y ----
    const std::size_t undoBefore = w.document()->undoCount();
    w.click(QStringLiteral("fixturePlaceOnStock"));
    REQUIRE(QTest::qWaitFor([&] { return !w.fixtures()->placingOnStock() && !w.fixtures()->stockStatus().isEmpty(); },
                            kTimeoutMs));
    w.idle();
    INFO(w.fixtures()->stockMessage().toStdString());
    // The jaws stand 44.45 - 25.4 - 10 = 9.05 above the plate's top: said as a warning.
    REQUIRE(w.fixtures()->stockStatus() == QStringLiteral("warning"));
    CHECK_THAT(w.fixtures()->stockMessage().toStdString(), ContainsSubstring("above the stock's top"));
    CHECK_THAT(w.fixtures()->stockMessage().toStdString(), ContainsSubstring("parallels"));
    CHECK(w.document()->undoCount() == undoBefore + 1);
    CHECK(w.document()->undoCommand()->name == "cam.batch");
    CHECK(transformCount(*w.document()->state()) == 4);
    const double cx = (part.min.x + part.max.x) / 2;
    const auto placedAcrossY = [&] {
        const Box3 j = w.boxOf(jaw), v = w.boxOf(vise), a = w.boxOf(parA), b = w.boxOf(parB);
        CHECK(j.max.y == Catch::Approx(part.min.y).margin(1e-5));         // closed on the front face
        CHECK(v.max.y == Catch::Approx(part.max.y + 60).margin(1e-5));    // the fixed jaw on the back face
        CHECK((j.min.x + j.max.x) / 2 == Catch::Approx(cx).margin(1e-5)); // centred along the jaws
        CHECK(a.max.z == Catch::Approx(part.min.z).margin(1e-5));         // the plate on the parallels
        CHECK(b.max.z == Catch::Approx(part.min.z).margin(1e-5));
        CHECK(j.max.z == Catch::Approx(part.min.z - 25.4 + 44.45).margin(1e-5));
        CHECK(std::min(a.min.y, b.min.y) == Catch::Approx(part.min.y).margin(1e-5)); // against the jaws
        CHECK(std::max(a.max.y, b.max.y) == Catch::Approx(part.max.y).margin(1e-5));
    };
    placedAcrossY();
    w.view()->fitAll();
    w.idle();
    w.shotsBothThemes(QStringLiteral("fixsnap_on_stock"));

    // ---- Undo takes all of it back in one step; redo places it again ----
    (void)w.invokeResult("edit.undo", "{}");
    w.settle();
    CHECK(transformCount(*w.document()->state()) == 0);
    CHECK(w.boxOf(jaw).min.y == Catch::Approx(50.8).margin(1e-5)); // where the file has it
    (void)w.invokeResult("edit.redo", "{}");
    w.settle();
    placedAcrossY();

    // ---- Across X: the same placements, edited ----
    QQuickItem* grip = w.shown(QStringLiteral("fixtureGrip"));
    w.fixtures()->setGrip(QStringLiteral("x"));
    REQUIRE(QTest::qWaitFor([&] { return grip->property("currentIndex").toInt() == 1; }, 5000));
    w.click(QStringLiteral("fixturePlaceOnStock"));
    REQUIRE(QTest::qWaitFor([&] { return !w.fixtures()->placingOnStock() && !w.fixtures()->stockStatus().isEmpty(); },
                            kTimeoutMs));
    w.idle();
    CHECK(w.fixtures()->stockStatus() == QStringLiteral("warning"));
    CHECK(transformCount(*w.document()->state()) == 4);
    const Box3 j = w.boxOf(jaw);
    CHECK(j.min.x == Catch::Approx(part.max.x).margin(1e-5));
    CHECK((j.min.y + j.max.y) / 2 == Catch::Approx((part.min.y + part.max.y) / 2).margin(1e-5));
    CHECK(w.boxOf(vise).min.x == Catch::Approx(part.min.x - 60).margin(1e-5));
    w.view()->fitAll();
    w.idle();
    w.shotsBothThemes(QStringLiteral("fixsnap_on_stock_across_x"));

    // ---- Still an ordinary placement: opened by hand, it shows its numbers ----
    REQUIRE(w.fixtures()->place(QString::fromStdString(vise.value.toString())));
    w.idle();
    CHECK(w.fixtures()->placingExisting());
    CHECK(w.fixtures()->values().value(QStringLiteral("angle")).toString() == QStringLiteral("-90 deg"));
    CHECK_FALSE(w.fixtures()->hasFace());
    w.shown(QStringLiteral("fixtureField.tiltX"));
    w.shotsBothThemes(QStringLiteral("fixsnap_vise_by_hand"));
    w.fixtures()->cancel();
    w.idle();
    w.checkNoWarnings();
}

// After T-077: the 10 mm plate on the 1 in parallels sits 9.05 below the jaws' top, so Place on stock names the library
// pair that would bring it above them (the 1-1/2 in: 6.35 held, the top 3.65 above), and changes nothing else. In a
// fresh document the vise and that pair from the library, Place on stock recognises the pair (any library pair in the
// setup, not only the 1 in) and stands the plate on it: its top above the jaws, no warning. Screenshots:
// build/shots/leftovers_taller_parallels_{suggested,placed}_{dark,light}.png.
TEST_CASE("Place on stock suggests taller parallels for a thin plate and stands it on them", "[ui][cam][fixtures]")
{
    RAY_REQUIRE_RHI();
    QString suggested;
    for (int round = 0; round < 2; ++round) {
        INFO("round " << round);
        Window w;
        w.open(kPlate); // 120 x 80 x 10
        const BodyId plate = w.host->scene()->body(0)->id;
        const Box3 part = w.boxOf(plate);
        REQUIRE(part.max.z - part.min.z == Catch::Approx(10).margin(1e-6));
        const lib::MachineDef* m = w.host->app()->libraries()->machine(MachineId{Uuid::fromString(kVmc)});
        REQUIRE(m != nullptr);
        (void)w.invokeResult("cam.setup.add", R"({"id":")" + kSetup + R"(","name":"Op 1 - top","partBodies":[")" +
                                                  plate.value.toString() + R"("],"machine":")" + kVmc +
                                                  R"(","library":{"machines":[)" + api::cam_setup::machineJson(*m) +
                                                  "]}}");
        w.toWorkspace(1);
        REQUIRE(QTest::qWaitFor([&] { return w.tree()->setupCount() == 1; }, kTimeoutMs));
        w.key(Qt::Key_F, Qt::ShiftModifier);
        REQUIRE(QTest::qWaitFor([&] { return w.fixtures()->shown(); }, 5000));
        const QVariantList library = w.fixtures()->library();
        const auto add = [&](const QString& name, int fixturesAfter) {
            int index = -1;
            for (int i = 0; i < library.size(); ++i) {
                if (library[i].toMap().value(QStringLiteral("name")).toString() == name)
                    index = i;
            }
            INFO("library item " << name.toStdString());
            REQUIRE(index >= 0);
            w.shown(QStringLiteral("fixturesLibrary"))->setProperty("currentIndex", index);
            w.click(QStringLiteral("fixturesAdd"));
            REQUIRE(QTest::qWaitFor(
                [&] { return w.fixtures()->fixtures().size() == fixturesAfter && !w.fixtures()->placing().isEmpty(); },
                kTimeoutMs));
            w.idle();
            w.fixtures()->cancel();
            w.idle();
        };
        // Every library pair is listed.
        for (const char* name : {"Generic parallels 1x6in", "Generic parallels 1.25x6in", "Generic parallels 1.5x6in",
                                 "Generic parallels 1.625x6in"}) {
            CHECK(std::any_of(library.begin(), library.end(), [&](const QVariant& v) {
                return v.toMap().value(QStringLiteral("name")).toString() == QLatin1String(name);
            }));
        }
        add(QStringLiteral("Generic vise 6in"), 2);
        const QString pairName = round == 0 ? QStringLiteral("Generic parallels 1x6in") : suggested;
        REQUIRE_FALSE(pairName.isEmpty());
        add(pairName, 4);
        const std::size_t undoBefore = w.document()->undoCount();
        w.click(QStringLiteral("fixturePlaceOnStock"));
        REQUIRE(QTest::qWaitFor(
            [&] { return !w.fixtures()->placingOnStock() && !w.fixtures()->stockStatus().isEmpty(); }, kTimeoutMs));
        w.idle();
        const QString message = w.fixtures()->stockMessage();
        INFO(message.toStdString());
        CHECK(w.document()->undoCount() == undoBefore + 1);
        CHECK(transformCount(*w.document()->state()) == 4); // the vise, its jaw and the pair: nothing swapped
        CHECK(setupOf(*w.document()->state()).fixtureBodies.size() == 4);
        const BodyId jaw = w.bodyNamed("Generic 6 in vise moving jaw");
        // The pair in the setup, whichever it is, under the plate.
        std::vector<BodyId> pair;
        for (const doc::Body& b : w.document()->state()->bodies) {
            if (b.name.rfind("Generic parallel 1/8 x ", 0) == 0)
                pair.push_back(b.id);
        }
        REQUIRE(pair.size() == 2);
        for (const BodyId& b : pair)
            CHECK(w.boxOf(b).max.z == Catch::Approx(part.min.z).margin(1e-5));
        const double height = part.min.z - w.boxOf(pair[0]).min.z;
        CHECK_THAT(message.toStdString(),
                   ContainsSubstring("The stock stands on the parallels (" + pairName.toStdString() + ")"));
        if (round == 0) {
            // On the 1 in pair: the jaws 9.05 above the plate's top, and the pair that would fix it named.
            CHECK(height == Catch::Approx(25.4).margin(1e-5));
            CHECK(w.fixtures()->stockStatus() == QStringLiteral("warning"));
            CHECK_THAT(message.toStdString(), ContainsSubstring("above the stock's top"));
            const QRegularExpressionMatch match =
                QRegularExpression(QStringLiteral("Taller parallels \\(([^)]+)\\) would bring the stock top above the "
                                                  "jaws\\."))
                    .match(message);
            REQUIRE(match.hasMatch());
            suggested = match.captured(1);
            CHECK(suggested == QStringLiteral("Generic parallels 1.5x6in"));
            CHECK(w.boxOf(jaw).max.z == Catch::Approx(part.max.z + 9.05).margin(1e-5));
            w.view()->fitAll();
            w.idle();
            w.shotsBothThemes(QStringLiteral("leftovers_taller_parallels_suggested"));
        } else {
            // On the suggested pair: the plate's top 3.65 above the jaws, 6.35 held, no warning and no suggestion.
            CHECK(height == Catch::Approx(38.1).margin(1e-5));
            CHECK(w.fixtures()->stockStatus() == QStringLiteral("ok"));
            CHECK(message.indexOf(QStringLiteral("parallels (")) == message.lastIndexOf(QStringLiteral("parallels (")));
            CHECK_FALSE(message.contains(QStringLiteral("Taller parallels")));
            CHECK(w.boxOf(jaw).max.z == Catch::Approx(part.max.z - 3.65).margin(1e-5));
            CHECK(w.boxOf(jaw).max.z - part.min.z == Catch::Approx(6.35).margin(1e-5));
            w.view()->fitAll();
            w.idle();
            w.shotsBothThemes(QStringLiteral("leftovers_taller_parallels_placed"));
        }
        w.checkNoWarnings();
    }
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
