// The Simulate workspace (task T-074) on the real Qt Quick window with the real application session. Real documents:
// the corpus plate (120 x 80 x 10 mm, six 6.6 mm through holes) with a box stock 1.5 mm above it, a facing operation (6
// mm end mill) and a peck drilling operation on two holes with the shipped 6 mm drill in a 30 mm holder, all generated
// by the CAM engine and simulated through ray_api/cam_sim (buildSimProgram):
//   - the acceptance script: the drill held 6 mm out of its holder (a seeded collision: the holder goes into the part);
//     Simulate is run from the panel, the collision's marker is found on the timeline and clicked; the timeline's
//     position is the event's place, the tool glyph's pose puts its tip at the event's position, and after posting from
//     the panel's offer the G-code viewer's current line is the first line PostOutput::lineMap gives for that move (all
//     of its lines marked). The clean twin (held 40 mm out): no event, no marker;
//   - on the clean twin: play, pause, the speed, scrubbing (the track and the API), one move per step (Right, Left,
//     Home, End, Space and ] in the workspace), the four visibility toggles (with a vise from the library as the
//     fixture), cancel mid-run (the button and Esc), out of date after an operation is edited, run again;
//   - a second setup turned over with its stock from the first: its state in the CAM tree (no "simulated first"), its
//     WCS shown from the carried stock, the setup panel's preview of that stock, and its simulation;
//   - [wallclock] a larger program on a real pocket part: the UI thread while it runs, plays at 100x and scrubs, with
//   the
//     run time, frames and seek times measured.
// Screenshots go to build/shots/t074_*.png in both themes.
// First, on their own: post_tool.h and operation_tool.h need these classes whole.
#include "cam/operation_view.h"
#include "cam/setup_tool.h"

#include "../scene_graph_probe.h"
#include "../ui_budget.h"
#include "../ui_watchdog.h"
#include "../window_focus.h"
#include "app_context.h"
#include "app_host.h"
#include "app_options.h"
#include "cam/fixtures_tool.h"
#include "cam/post_tool.h"
#include "cam/post_view.h"
#include "cam/setup_tree.h"
#include "cam/sim_tool.h"
#include "files.h"
#include "palette_commands.h"
#include "scene_controller.h"
#include "selection_model.h"
#include "session.h"
#include "theme_object.h"
#include "ui_commands.h"
#include "viewport_item.h"

#include <ray_api/cam_operation.h>
#include <ray_api/cam_setup.h>
#include <ray_api/cam_tools.h>

#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
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
using ray::ui::GcodeModel;
using ray::ui::PostToolModel;
using ray::ui::SetupToolModel;
using ray::ui::SetupTreeModel;
using ray::ui::SimToolModel;
namespace cs = ray::api::cam_sim;

namespace {

constexpr int kTimeoutMs = 120000;
const QString kPlate = QStringLiteral(RAY_CORPUS_DIR "/raycad-synthetic/plate_thru_holes_1.stp");
const QString kPocketPart = QStringLiteral(RAY_CORPUS_DIR "/raycad-synthetic/closed_pocket_1.stp");
const std::string kVmc = "b903ede7-5123-4c26-bc15-466e92fba4c6";    // the shipped "Generic VMC"
const std::string kDrill6 = "3d2ce280-3863-4869-9ef4-b75ed93ce20e"; // 6 mm HSS-Co jobber drill

std::string id(char kind, std::uint64_t n)
{
    static const char* const hex = "0123456789abcdef";
    std::string tail(12, '0');
    for (int i = 11; i >= 0; --i) {
        tail[static_cast<std::size_t>(i)] = hex[n % 16];
        n /= 16;
    }
    return std::string(1, kind) + "0000000-0000-4000-8000-" + tail;
}
OperationId opId(std::uint64_t n)
{
    return OperationId{Uuid::fromString(id('c', n))};
}
SetupId setupId(std::uint64_t n)
{
    return SetupId{Uuid::fromString(id('a', n))};
}
std::string jsonText(const QJsonObject& o)
{
    return QJsonDocument(o).toJson(QJsonDocument::Compact).toStdString();
}
QJsonObject object(const std::string& text)
{
    return QJsonDocument::fromJson(QByteArray::fromStdString(text)).object();
}
Vec3 apply(const Xform& xf, Vec3 p)
{
    const double* m = xf.m;
    return Vec3{m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3], m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
                m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]};
}
double distance(Vec3 a, Vec3 b)
{
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}

struct Window {
    QTemporaryDir cache;
    QTemporaryDir userData;
    QTemporaryDir out; // the post's output folder
    std::unique_ptr<AppHost> host;

    explicit Window(render::Tier tier = render::Tier::Medium)
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
        host->scene()->setPreset(render::presetFor(tier));
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
    SimToolModel* sim() const { return singleton<SimToolModel>("CamSim"); }
    PostToolModel* post() const { return singleton<PostToolModel>("CamPost"); }
    GcodeModel* gcode() const { return singleton<GcodeModel>("CamGcode"); }
    SetupTreeModel* tree() const { return singleton<SetupTreeModel>("CamTreeModel"); }
    SetupToolModel* setupTool() const { return singleton<SetupToolModel>("CamSetup"); }
    ui::FixturesToolModel* fixtures() const { return singleton<ui::FixturesToolModel>("CamFixtures"); }
    QImage grab() const { return host->window()->grabWindow(); }
    QQuickItem* find(const QString& objectName, QQuickItem* in = nullptr) const
    {
        return findItem(in ? in : window()->contentItem(), objectName);
    }
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
    // The first item of that name that is visible (each workspace has a G-code viewer of its own).
    static QQuickItem* findVisible(QQuickItem* item, const QString& objectName)
    {
        if (!item->isVisible())
            return nullptr;
        if (item->objectName() == objectName)
            return item;
        for (QQuickItem* child : item->childItems()) {
            if (QQuickItem* found = findVisible(child, objectName))
                return found;
        }
        return nullptr;
    }
    // Brings line `line` of the viewer in `host` into sight and clicks it, as the user does.
    void clickGcodeLine(QQuickItem* viewerHost, int line) const
    {
        GcodeModel* g = gcode();
        QQuickItem* list = find(QStringLiteral("gcodeList"), viewerHost);
        REQUIRE(list != nullptr);
        // In the middle of the list (an edge of it may lie under the timeline or a panel).
        REQUIRE(QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, line), Q_ARG(int, 1 /* Center */)));
        // The row laid out where the list shows it (a pooled delegate keeps its old place until the list polishes).
        QPointF at;
        const bool there = QTest::qWaitFor(
            [&] {
                QQuickItem* item = findVisible(viewerHost, QStringLiteral("gcodeLine.%1").arg(line));
                if (item == nullptr)
                    return false;
                at = item->mapToScene(QPointF(std::min(item->width() / 2, 160.0), item->height() / 2));
                const QPointF inList = list->mapFromScene(at);
                return inList.y() > 0 && inList.y() < list->height() && inList.x() > 0 && inList.x() < list->width();
            },
            5000);
        INFO("G-code line " << line);
        REQUIRE(there);
        grab();
        clickScene(at);
        REQUIRE(QTest::qWaitFor([&] { return g->currentLine() == line; }, 5000));
    }
    // A visible item, searched in the Simulate workspace first (the CAM workspace has a G-code viewer too).
    QQuickItem* shown(const QString& objectName) const
    {
        QQuickItem* item = nullptr;
        const bool ok = QTest::qWaitFor(
            [&] {
                QQuickItem* ws = find(QStringLiteral("simWorkspace"));
                item = ws && ws->isVisible() ? find(objectName, ws) : nullptr;
                if (!item || !item->isVisible())
                    item = find(objectName);
                return item && item->isVisible();
            },
            10000);
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
    std::map<OperationId, cam::OpState> statuses() const
    {
        return host->app()->cam()->status(document()->state(), document()->build());
    }
    void generate(std::uint64_t op) const
    {
        settle();
        auto job = host->app()->cam()->generate(document()->state(), document()->build(), opId(op));
        REQUIRE(QTest::qWaitFor([&] { return job.done(); }, kTimeoutMs * 3));
        auto r = job.wait();
        INFO("operation " << op << ": " << (r ? std::string() : r.error().message));
        REQUIRE(r);
        REQUIRE(QTest::qWaitFor([&] { return statuses().at(opId(op)).status == cam::OpStatus::UpToDate; }, kTimeoutMs));
    }
    // The simulation has done what it was asked (a run, a seek, its gate).
    void simIdle() const
    {
        REQUIRE(QTest::qWaitFor([&] { return !sim()->busy(); }, kTimeoutMs * 2));
        QTest::qWait(30);
        REQUIRE(QTest::qWaitFor([&] { return !sim()->busy(); }, kTimeoutMs * 2));
    }
    void runSim() const
    {
        simIdle();
        REQUIRE(QTest::qWaitFor([&] { return sim()->canRun(); }, kTimeoutMs));
        click(QStringLiteral("simRun"));
        REQUIRE(QTest::qWaitFor([&] { return sim()->state() != QLatin1String("running"); }, kTimeoutMs * 3));
        INFO("simulation: " << sim()->state().toStdString() << ": " << sim()->message().toStdString());
        REQUIRE(sim()->state() == QLatin1String("done"));
        simIdle();
    }
    BodyId plate() const { return host->scene()->body(0)->id; }
    // The planar face of the shown body `index` looking along `n` (an axis) whose centroid lies at `along` on it.
    std::optional<kernel::SubId> face(int index, Vec3 n, double along) const
    {
        const api::viewer::Body* b = host->scene()->body(index);
        REQUIRE(b != nullptr);
        for (std::uint32_t i = 0; i < b->shape.count(kernel::SubKind::Face); ++i) {
            const kernel::SubId sub{kernel::SubKind::Face, i};
            auto s = kernel::faceSample(b->shape, sub);
            auto info = kernel::surfaceInfo(b->shape, sub);
            if (!s || !info || info->type != kernel::SurfaceType::Plane)
                continue;
            const double at = info->centroid.x * n.x + info->centroid.y * n.y + info->centroid.z * n.z;
            if (std::abs(s->normal.x - n.x) < 1e-9 && std::abs(s->normal.y - n.y) < 1e-9 &&
                std::abs(s->normal.z - n.z) < 1e-9 && std::abs(at - along) < 1e-6)
                return sub;
        }
        return std::nullopt;
    }
    QJsonObject faceRef(kernel::SubId sub) const
    {
        const QJsonObject args{{QStringLiteral("body"), QString::fromStdString(plate().value.toString())},
                               {QStringLiteral("kind"), QStringLiteral("face")},
                               {QStringLiteral("index"), static_cast<int>(sub.index)}};
        return object(invokeResult("model.reference", jsonText(args))).value(QStringLiteral("ref")).toObject();
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

// The plate job: setup 1 (box stock 2 mm around, 5 mm above and 1 mm below the plate, the shipped VMC), a 6 mm flat
// end mill and a 30 mm holder embedded; peck drilling of two holes with the shipped drill 40 mm out of the holder, its
// R plane `drillRetract` above the hole tops (c2), THEN facing the 5 mm off the top (c1); both generated. With the R
// plane 2 mm above the plate (3 mm inside the stock that is still there: drilled before facing) the cycle's rapid down
// to it goes into the stock, a seeded collision (RapidIntoStock); 8 mm above it (above the stock) it is the clean twin.
void buildPlateJob(const Window& w, double drillRetract)
{
    const lib::MachineDef* m = w.host->app()->libraries()->machine(MachineId{Uuid::fromString(kVmc)});
    REQUIRE(m != nullptr);
    (void)w.invokeResult("cam.setup.add",
                         R"({"id":")" + id('a', 1) + R"(","name":"Op 1 - top","partBodies":[")" +
                             w.plate().value.toString() + R"("],"machine":")" + kVmc +
                             R"(","stock":{"type":"box","xNeg":2,"xPos":2,"yNeg":2,"yPos":2,"zNeg":1,"zPos":5},)"
                             R"("library":{"machines":[)" +
                             api::cam_setup::machineJson(*m) + "]}}");
    REQUIRE(QTest::qWaitFor([&] { return w.tree()->setupCount() >= 1; }, kTimeoutMs));
    const lib::Tool* drill = w.host->app()->libraries()->tool(ToolId{Uuid::fromString(kDrill6)});
    REQUIRE(drill != nullptr);
    const std::string mill = R"({"schema":1,"id":")" + id('d', 1) +
                             R"(","name":"6 mm end mill","type":"flatEndMill","flutes":3,"diameter":6,)"
                             R"("shankDiameter":6,"fluteLength":20,"shoulderLength":25,"overallLength":60,)"
                             R"("defaultToolNumber":1})";
    const std::string holder = R"({"schema":1,"id":")" + id('e', 1) +
                               R"(","name":"30 mm holder","gaugeLength":40,)"
                               R"("segments":[{"height":40,"lowerDiameter":30,"upperDiameter":40}]})";
    (void)w.invokeResult("cam.library.embed", R"({"tools":[)" + mill + "," + api::cam_tools::toolJson(*drill) +
                                                  R"(],"holders":[)" + holder + "]}");
    // The first two holes' bores.
    w.settle();
    JobContext ctx;
    auto holes = api::cam_operation::holesOf(w.document()->state(), w.document()->build(), setupId(1), ctx);
    REQUIRE(holes);
    REQUIRE(holes->size() >= 2);
    QJsonArray refs;
    for (std::size_t i = 0; i < 2; ++i) {
        const QJsonObject args{{QStringLiteral("body"), QString::fromStdString((*holes)[i].body.value.toString())},
                               {QStringLiteral("kind"), QStringLiteral("face")},
                               {QStringLiteral("index"), static_cast<int>((*holes)[i].boreFace)}};
        refs.push_back(object(w.invokeResult("model.reference", jsonText(args))).value(QStringLiteral("ref")));
    }
    QJsonObject drillOp{
        {QStringLiteral("id"), QString::fromStdString(id('c', 2))},
        {QStringLiteral("setup"), QString::fromStdString(id('a', 1))},
        {QStringLiteral("type"), QStringLiteral("drill")},
        {QStringLiteral("name"), QStringLiteral("Drill")},
        {QStringLiteral("tool"), QJsonObject{{QStringLiteral("tool"), QString::fromStdString(kDrill6)},
                                             {QStringLiteral("holder"), QString::fromStdString(id('e', 1))},
                                             {QStringLiteral("stickOut"), 40.0},
                                             {QStringLiteral("number"), 2}}},
        {QStringLiteral("cutting"), QJsonObject{{QStringLiteral("rpm"), 1500},
                                                {QStringLiteral("feedCut"), 150},
                                                {QStringLiteral("feedPlunge"), 150}}},
        {QStringLiteral("heights"), QJsonObject{{QStringLiteral("clearance"), 15},
                                                {QStringLiteral("retract"), drillRetract},
                                                {QStringLiteral("feedPlane"), drillRetract},
                                                {QStringLiteral("top"), 0},
                                                {QStringLiteral("bottom"), 0}}},
        {QStringLiteral("geometry"), refs},
        {QStringLiteral("params"), QJsonObject{{QStringLiteral("cycle"), QStringLiteral("peck")}}}};
    (void)w.invokeResult("cam.operation.add", jsonText(drillOp));
    (void)w.invokeResult("cam.operation.add",
                         R"({"id":")" + id('c', 1) + R"(","setup":")" + id('a', 1) +
                             R"(","type":"face","name":"Face","tool":{"tool":")" + id('d', 1) + R"(","holder":")" +
                             id('e', 1) +
                             R"(","stickOut":30,"number":1},"cutting":{"rpm":6000,"feedCut":1000,"feedPlunge":300},)"
                             R"("heights":{"clearance":15,"retract":5,"feedPlane":2,"top":0,"bottom":0},)"
                             R"("geometry":[],"params":{"stepdown":1}})");
    w.generate(1);
    w.generate(2);
}

// Setup 2 of the plate: turned over (its Z face is the plate's bottom), its stock what setup 1 leaves; facing the 1 mm
// under the plate (c3), generated.
void addTurnedOverSetup(const Window& w)
{
    const Box3 box = kernel::bounds(w.document()->build()->bodies.at(w.plate()).shape);
    const auto bottom = w.face(0, {0, 0, -1}, -box.min.z);
    REQUIRE(bottom.has_value());
    const QJsonObject ref = w.faceRef(*bottom);
    (void)w.invokeResult("cam.setup.add", R"({"id":")" + id('a', 2) + R"(","name":"Op 2 - bottom","partBodies":[")" +
                                              w.plate().value.toString() + R"("],"machine":")" + kVmc +
                                              R"(","stock":{"type":"fromSetup","setup":")" + id('a', 1) +
                                              R"("},"wcs":{"zFace":)" +
                                              QJsonDocument(ref).toJson(QJsonDocument::Compact).toStdString() +
                                              R"(,"origin":{"mode":"stockCorner","corner":0},"workOffset":2}})");
    (void)w.invokeResult("cam.operation.add",
                         R"({"id":")" + id('c', 3) + R"(","setup":")" + id('a', 2) +
                             R"(","type":"face","name":"Face bottom","tool":{"tool":")" + id('d', 1) +
                             R"(","holder":")" + id('e', 1) +
                             R"(","stickOut":30,"number":1},"cutting":{"rpm":6000,"feedCut":1000,"feedPlunge":300},)"
                             R"("heights":{"clearance":15,"retract":5,"feedPlane":2,"top":0,"bottom":0},)"
                             R"("geometry":[],"params":{"stepdown":1}})");
    w.generate(3);
}

// The plate held in the library's vise (T-077, resources/library/fixtures), placed with plain moves of transform_body:
// the fixed jaw's clamping face against the plate's front (y = 0), the moving jaw against its back (y = 80), the vise's
// floor 14.45 mm below the plate's bottom so the jaws' top is at z = 30, 20 mm above the plate. Setup 1 (box stock 2 mm
// around in X only, none in Y where the jaws are; the vise and its jaw as fixtures); peck drilling of the two front
// holes (y = 26.7) with the shipped 6 mm drill 25 mm out of a 60 mm holder (c1), generated: the holder reaches 3.3 mm
// over the fixed jaw, and once the tip is 5 mm into the plate it comes down on the jaw. A real HolderHitsFixture,
// nothing seeded.
void buildViseJob(const Window& w)
{
    const BodyId plateId = w.plate();
    const QString vise = QStringLiteral(RAY_SOURCE_DIR "/resources/library/fixtures/generic-vise-6in.step");
    REQUIRE(QFileInfo::exists(vise));
    (void)w.invokeResult("file.import", jsonText(QJsonObject{{QStringLiteral("path"), vise},
                                                             {QStringLiteral("role"), QStringLiteral("fixture")}}));
    w.settle();
    BodyId viseId, jawId;
    for (const doc::Body& b : w.document()->state()->bodies) {
        if (b.name == "Generic 6 in vise")
            viseId = b.id;
        else if (b.name == "Generic 6 in vise moving jaw")
            jawId = b.id;
    }
    REQUIRE(viseId.value != Uuid{});
    REQUIRE(jawId.value != Uuid{});
    (void)w.invokeResult("feature.transform_body.add", R"({"id":")" + id('f', 8) + R"(","props":{"body":")" +
                                                           viseId.value.toString() + R"(","x":60,"y":0,"z":-14.45}})");
    (void)w.invokeResult("feature.transform_body.add", R"({"id":")" + id('f', 9) + R"(","props":{"body":")" +
                                                           jawId.value.toString() +
                                                           R"(","x":60,"y":29.2,"z":-14.45}})");
    w.settle();
    const lib::MachineDef* m = w.host->app()->libraries()->machine(MachineId{Uuid::fromString(kVmc)});
    REQUIRE(m != nullptr);
    (void)w.invokeResult("cam.setup.add",
                         R"({"id":")" + id('a', 1) + R"(","name":"In the vise","partBodies":[")" +
                             plateId.value.toString() + R"("],"fixtureBodies":[")" + viseId.value.toString() +
                             R"(",")" + jawId.value.toString() + R"("],"machine":")" + kVmc +
                             R"(","stock":{"type":"box","xNeg":2,"xPos":2,"yNeg":0,"yPos":0,"zNeg":0,"zPos":0},)"
                             R"("library":{"machines":[)" +
                             api::cam_setup::machineJson(*m) + "]}}");
    REQUIRE(QTest::qWaitFor([&] { return w.tree()->setupCount() >= 1; }, kTimeoutMs));
    const lib::Tool* drill = w.host->app()->libraries()->tool(ToolId{Uuid::fromString(kDrill6)});
    REQUIRE(drill != nullptr);
    const std::string holder = R"({"schema":1,"id":")" + id('e', 2) +
                               R"(","name":"60 mm holder","gaugeLength":40,)"
                               R"("segments":[{"height":40,"lowerDiameter":60,"upperDiameter":60}]})";
    (void)w.invokeResult("cam.library.embed",
                         R"({"tools":[)" + api::cam_tools::toolJson(*drill) + R"(],"holders":[)" + holder + "]}");
    w.settle();
    JobContext ctx;
    auto holes = api::cam_operation::holesOf(w.document()->state(), w.document()->build(), setupId(1), ctx);
    REQUIRE(holes);
    QJsonArray refs;
    for (const api::cam_operation::Hole& h : *holes) {
        if (h.top.y > 40 || refs.size() >= 2)
            continue; // the front row only
        const QJsonObject args{{QStringLiteral("body"), QString::fromStdString(h.body.value.toString())},
                               {QStringLiteral("kind"), QStringLiteral("face")},
                               {QStringLiteral("index"), static_cast<int>(h.boreFace)}};
        refs.push_back(object(w.invokeResult("model.reference", jsonText(args))).value(QStringLiteral("ref")));
    }
    REQUIRE(refs.size() == 2);
    QJsonObject drillOp{
        {QStringLiteral("id"), QString::fromStdString(id('c', 1))},
        {QStringLiteral("setup"), QString::fromStdString(id('a', 1))},
        {QStringLiteral("type"), QStringLiteral("drill")},
        {QStringLiteral("name"), QStringLiteral("Drill")},
        {QStringLiteral("tool"), QJsonObject{{QStringLiteral("tool"), QString::fromStdString(kDrill6)},
                                             {QStringLiteral("holder"), QString::fromStdString(id('e', 2))},
                                             {QStringLiteral("stickOut"), 25.0},
                                             {QStringLiteral("number"), 1}}},
        {QStringLiteral("cutting"), QJsonObject{{QStringLiteral("rpm"), 1500},
                                                {QStringLiteral("feedCut"), 150},
                                                {QStringLiteral("feedPlunge"), 150}}},
        // Clearance above the jaws (the generator refuses one below a fixture's top); R 8 above the plate.
        {QStringLiteral("heights"), QJsonObject{{QStringLiteral("clearance"), 25},
                                                {QStringLiteral("retract"), 8},
                                                {QStringLiteral("feedPlane"), 8},
                                                {QStringLiteral("top"), 0},
                                                {QStringLiteral("bottom"), 0}}},
        {QStringLiteral("geometry"), refs},
        {QStringLiteral("params"), QJsonObject{{QStringLiteral("cycle"), QStringLiteral("peck")}}}};
    (void)w.invokeResult("cam.operation.add", jsonText(drillOp));
    w.generate(1);
}

// Posts setup `setup` through the panel's offer (the post dialog, its gate), into the window's output folder.
void postFromPanel(const Window& w)
{
    w.click(QStringLiteral("simGcodePost"));
    PostToolModel* p = w.post();
    REQUIRE(QTest::qWaitFor([&] { return p->active(); }, 5000));
    w.shown(QStringLiteral("postDialog"));
    const bool read = QTest::qWaitFor([&] { return p->postState() != QLatin1String("loading"); }, kTimeoutMs);
    INFO("post '" << p->postId().toStdString() << "': " << p->postMessage().toStdString());
    REQUIRE(read);
    REQUIRE(p->postState() == QLatin1String("ok"));
    p->setValue(QStringLiteral("directory"), QDir::toNativeSeparators(w.out.path()));
    p->setValue(QStringLiteral("overwrite"), QStringLiteral("replace"));
    REQUIRE(QTest::qWaitFor([&] { return p->canPost(); }, kTimeoutMs));
    REQUIRE(p->post());
    const bool done = QTest::qWaitFor([&] { return p->state() == QLatin1String("done"); }, kTimeoutMs * 2);
    INFO("post state '" << p->state().toStdString() << "', " << p->statusText().toStdString());
    REQUIRE(done);
    p->close();
    REQUIRE(QTest::qWaitFor([&] { return !p->active(); }, 5000));
}

} // namespace

// ============================================================================================================
// acceptance
TEST_CASE(
    "acceptance: a seeded collision is found on the timeline; its marker shows the tool there and its G-code line",
    "[ui][cam][sim][acceptance]")
{
    RAY_REQUIRE_RHI();
    Window w;
    w.open(kPlate);
    w.toWorkspace(ui::kCamWorkspace);
    buildPlateJob(w, 2.0); // the R plane inside the stock: drilled before facing
    w.toWorkspace(ui::kSimWorkspace);
    SimToolModel* s = w.sim();
    w.shown(QStringLiteral("simPanel"));
    REQUIRE(QTest::qWaitFor([&] { return s->setupIdText().toStdString() == id('a', 1); }, 5000));
    w.simIdle();
    CHECK(s->canRun());
    CHECK(s->blockers().isEmpty());
    CHECK_FALSE(s->hasResult());
    CHECK_FALSE(w.find(QStringLiteral("simTimeline"))->isVisible()); // nothing to show before a run
    w.shotsBothThemes(QStringLiteral("t074_ready"));

    // ---- Run from the panel ----
    w.runSim();
    const auto res = s->result();
    REQUIRE(res);
    CHECK(res->setup == setupId(1));
    REQUIRE(res->ops.size() == 2);
    CHECK(res->cellMm == Catch::Approx(render::presetFor(render::Tier::Medium).simCellMm));
    CHECK(s->resolutionText().contains(QStringLiteral("0.10 mm")));
    CHECK(s->stockItem() != 0);
    CHECK(s->stockVisible());
    CHECK(s->triangleBudget() == 250'000);
    // The setup's plate is hidden while the stock (the plate as cut) is shown.
    const std::vector<int> hidden = s->hiddenBodies();
    CHECK(std::find(hidden.begin(), hidden.end(), 0) != hidden.end());
    for (std::size_t i = 0; i < res->events.size(); ++i)
        UNSCOPED_INFO(ui::camui::eventKindName(static_cast<int>(res->events[i].kind))
                      << " op " << res->places[i].opIndex << " move " << res->events[i].moveIndex << " at "
                      << res->places[i].sec << " s: " << res->events[i].message);
    REQUIRE(s->errorCount() > 0);
    // The drill's rapid down to its R plane, into the stock not faced yet: RapidIntoStock on the drilling.
    std::optional<int> collision;
    for (std::size_t i = 0; i < res->events.size() && !collision; ++i) {
        if (res->events[i].kind == sim::EventKind::RapidIntoStock)
            collision = static_cast<int>(i);
    }
    REQUIRE(collision.has_value());
    CHECK(res->places[static_cast<std::size_t>(*collision)].opIndex == 0); // the drilling comes first
    CHECK(s->summary().contains(QStringLiteral("error")));
    w.shown(QStringLiteral("simTimeline"));
    w.shown(QStringLiteral("simEvent.0"));

    // ---- The G-code: none posted yet; the panel offers to post (the post dialog and its gate) ----
    CHECK(s->gcodeState() == QStringLiteral("none"));
    w.shown(QStringLiteral("simGcodePost"));
    postFromPanel(w);
    REQUIRE(QTest::qWaitFor([&] { return s->gcodeState() == QStringLiteral("current"); }, 5000));

    // ---- Find the marker of the collision on the timeline and click it ----
    QQuickItem* track = w.shown(QStringLiteral("simTrack"));
    const QVariantList markers = s->markers(track->width());
    int markerIndex = -1;
    for (int k = 0; k < markers.size() && markerIndex < 0; ++k) {
        if (markers[k].toMap().value(QStringLiteral("events")).toList().contains(*collision))
            markerIndex = k;
    }
    REQUIRE(markerIndex >= 0);
    const QVariantMap marker = markers[markerIndex].toMap();
    CHECK(marker.value(QStringLiteral("severity")).toString() == QStringLiteral("error"));
    const int chosen = marker.value(QStringLiteral("first")).toInt();
    w.click(QStringLiteral("simMarker.%1").arg(markerIndex));
    REQUIRE(QTest::qWaitFor([&] { return s->currentEvent() == chosen; }, 5000));
    w.simIdle();

    // The timeline's position is the event's place.
    const api::cam_sim::EventPlace& place = res->places[static_cast<std::size_t>(chosen)];
    const sim::SimEvent& event = res->events[static_cast<std::size_t>(chosen)];
    CHECK(s->position().opIndex == place.opIndex);
    CHECK(s->position().moveIndex == place.moveIndex);
    CHECK(s->position().t01 == Catch::Approx(place.t01).margin(1e-9));
    CHECK(s->positionSec() == Catch::Approx(place.sec).margin(1e-6));
    QQuickItem* playhead = w.shown(QStringLiteral("simPlayhead"));
    const double headX = playhead->x() + playhead->width() / 2;
    CHECK(headX == Catch::Approx(place.sec / res->timeline.totalSec * track->width()).margin(1.0));
    // The tool glyph there: its pose puts the tip at the event's position, the operation's own glyph shown.
    const Xform pose = s->toolPose();
    const Vec3 tip{pose.m[3], pose.m[7], pose.m[11]};
    const Vec3 want = apply(res->modelFromWcs, event.position);
    INFO("tip " << tip.x << ", " << tip.y << ", " << tip.z << "; event " << want.x << ", " << want.y << ", " << want.z);
    CHECK(distance(tip, want) < 0.05);
    CHECK(s->shownOp() == static_cast<int>(place.opIndex));
    CHECK(s->toolVisible());
    CHECK(s->holderVisible());
    CHECK(s->toolItem(place.opIndex) != 0);
    CHECK(s->holderItem(place.opIndex) != 0);
    // The G-code line: the first PostOutput::lineMap gives for that move, every line of the move marked.
    GcodeModel* g = w.gcode();
    REQUIRE(g->isOpen());
    const ui::PostedProgram* program = g->program();
    REQUIRE(program != nullptr);
    std::optional<std::uint32_t> section;
    for (std::size_t i = 0; i < program->sectionOps.size(); ++i) {
        if (program->sectionOps[i] == res->ops[place.opIndex])
            section = static_cast<std::uint32_t>(i);
    }
    REQUIRE(section.has_value());
    int first = -1, last = -1;
    for (const post::GcodeLineMap& m : program->index->map) {
        if (m.sectionIndex == *section && m.moveIndex == place.moveIndex) {
            first = first < 0 ? static_cast<int>(m.line) : std::min(first, static_cast<int>(m.line));
            last = std::max(last, static_cast<int>(m.line));
        }
    }
    REQUIRE(first >= 0);
    CHECK(g->currentLine() == first);
    CHECK(g->pickedFirst() == first);
    CHECK(g->pickedLast() == last);
    CHECK(s->gcodeLine() == first);
    // In the Simulate workspace's viewer: the line in sight.
    QQuickItem* viewer = w.shown(QStringLiteral("simGcodeViewHost"));
    REQUIRE(
        QTest::qWaitFor([&] { return w.find(QStringLiteral("gcodeLine.%1").arg(first), viewer) != nullptr; }, 5000));
    CHECK(w.find(QStringLiteral("gcodeLine.%1").arg(first), viewer)->isVisible());
    w.view()->fitAll();
    w.simIdle();
    w.shotsBothThemes(QStringLiteral("t074_marker_gcode"));

    // A row of the list does the same; Ctrl+Right goes to the next problem.
    if (res->events.size() > 1) {
        w.view()->forceActiveFocus();
        w.key(Qt::Key_Right, Qt::ControlModifier);
        REQUIRE(QTest::qWaitFor([&] { return s->currentEvent() != chosen; }, 5000));
        w.simIdle();
        CHECK(s->positionSec() >= place.sec - 1e-6);
    }
    w.click(QStringLiteral("simEvent.%1").arg(*collision));
    REQUIRE(QTest::qWaitFor([&] { return s->currentEvent() == *collision; }, 5000));
    w.simIdle();
    CHECK(s->position().moveIndex == res->places[static_cast<std::size_t>(*collision)].moveIndex);

    // ---- The other way (added after T-074): a line clicked in the viewer takes the simulation to its move ----
    // A line of the facing (operation 1) part way through its section: the stock and the tool go to the end of the move
    // it writes (the first, were there several), and the line clicked stays the viewer's current line.
    std::optional<std::uint32_t> faceSection;
    for (std::size_t i = 0; i < program->sectionOps.size(); ++i) {
        if (program->sectionOps[i] == res->ops[1])
            faceSection = static_cast<std::uint32_t>(i);
    }
    REQUIRE(faceSection.has_value());
    std::vector<int> faceLines;
    for (const post::GcodeLineMap& m : program->index->map) {
        if (m.sectionIndex == *faceSection)
            faceLines.push_back(static_cast<int>(m.line));
    }
    REQUIRE(faceLines.size() > 8);
    const int faceLine = faceLines[faceLines.size() / 2];
    const auto faceMove = program->index->moveOf(static_cast<std::size_t>(faceLine));
    REQUIRE(faceMove.has_value());
    const auto faceStatus = w.statuses().at(opId(1));
    REQUIRE(faceStatus.toolpath);
    REQUIRE(faceMove->move < faceStatus.toolpath->moves.size());
    w.clickGcodeLine(viewer, faceLine);
    w.simIdle();
    CHECK(g->currentLine() == faceLine);
    // The line's move, without the CAM workspace's "its toolpath is not shown" (here the simulation draws it).
    CHECK(g->lineInfo() == QStringLiteral("Line %1: Face, move %2").arg(faceLine + 1).arg(faceMove->move + 1));
    CHECK(s->currentEvent() == -1); // no problem chosen any more
    CHECK(s->position().opIndex == 1);
    CHECK(s->position().moveIndex == faceMove->move);
    CHECK(s->position().t01 == 1);
    CHECK(s->positionSec() == Catch::Approx(res->timeline.secondsAt({1, faceMove->move, 1})).margin(1e-9));
    {
        const cam::Move& mv = faceStatus.toolpath->moves[faceMove->move];
        const Vec3 end = apply(res->modelFromWcs, Vec3{mv.x / 1e6, mv.y / 1e6, mv.z / 1e6});
        const Vec3 at{s->toolPose().m[3], s->toolPose().m[7], s->toolPose().m[11]};
        INFO("tip " << at.x << ", " << at.y << ", " << at.z << "; move end " << end.x << ", " << end.y << ", "
                    << end.z);
        CHECK(distance(at, end) < 1e-3);
    }
    w.shotsBothThemes(QStringLiteral("simgaps_gcode_line"));
    // A line that writes no move (the program's start, a comment, a tool change): nothing moves.
    int quiet = 0;
    while (quiet < g->lineCount() && program->index->moveOf(static_cast<std::size_t>(quiet)).has_value())
        ++quiet;
    REQUIRE(quiet < g->lineCount());
    const sim::TimelinePosition before = s->position();
    const int framesBefore = s->framesApplied();
    w.clickGcodeLine(viewer, quiet);
    w.simIdle();
    CHECK(g->currentLine() == quiet);
    CHECK(s->framesApplied() == framesBefore);
    CHECK(s->position().opIndex == before.opIndex);
    CHECK(s->position().moveIndex == before.moveIndex);
    CHECK(s->position().t01 == before.t01);
    // The CAM workspace's viewer is as it was: a line clicked there is current and moves no simulation.
    w.toWorkspace(ui::kCamWorkspace);
    QQuickItem* camViewer = nullptr;
    REQUIRE(QTest::qWaitFor(
        [&] {
            camViewer = Window::findVisible(w.window()->contentItem(), QStringLiteral("gcodeViewHost"));
            return camViewer != nullptr;
        },
        5000));
    const int camLine = faceLines[faceLines.size() / 4];
    w.clickGcodeLine(camViewer, camLine);
    w.simIdle();
    CHECK(g->currentLine() == camLine);
    CHECK_FALSE(s->seeking());
    CHECK(s->framesApplied() == framesBefore);
    CHECK(s->position().moveIndex == before.moveIndex);
    w.toWorkspace(ui::kSimWorkspace);
    w.checkNoWarnings();
}

TEST_CASE("the clean twin: no marker; play, pause, speed, scrub, steps, the toggles, cancel, out of date",
          "[ui][cam][sim][twin]")
{
    RAY_REQUIRE_RHI();
    Window w;
    w.open(kPlate);
    w.toWorkspace(ui::kCamWorkspace);
    buildPlateJob(w, 8.0); // the R plane above the stock
    w.toWorkspace(ui::kSimWorkspace);
    SimToolModel* s = w.sim();
    w.runSim();
    const auto res = s->result();
    REQUIRE(res);
    for (std::size_t i = 0; i < res->events.size(); ++i)
        UNSCOPED_INFO(ui::camui::eventKindName(static_cast<int>(res->events[i].kind))
                      << " op " << res->places[i].opIndex << " move " << res->events[i].moveIndex << ": "
                      << res->events[i].message);
    CHECK(res->events.empty());
    CHECK(s->markers(500).isEmpty());
    CHECK(s->summary().startsWith(QStringLiteral("No collision")));
    w.view()->fitAll();
    w.shotsBothThemes(QStringLiteral("t074_clean"));
    const double total = res->timeline.totalSec;
    REQUIRE(total > 10);

    // ---- Steps: Home, Right, Right, Left, End (the workspace's keys) ----
    w.view()->forceActiveFocus();
    w.key(Qt::Key_Home);
    w.simIdle();
    CHECK(s->positionSec() == 0);
    CHECK(res->timeline.movesDone(s->position()) == 0);
    w.key(Qt::Key_Right);
    w.simIdle();
    CHECK(res->timeline.movesDone(s->position()) == 1);
    w.key(Qt::Key_Right);
    w.simIdle();
    CHECK(res->timeline.movesDone(s->position()) == 2);
    w.key(Qt::Key_Left);
    w.simIdle();
    CHECK(res->timeline.movesDone(s->position()) == 1);
    w.click(QStringLiteral("simStepForward"));
    w.simIdle();
    CHECK(res->timeline.movesDone(s->position()) == 2);
    w.key(Qt::Key_End);
    w.simIdle();
    CHECK(s->positionSec() == Catch::Approx(total));
    CHECK(res->timeline.movesDone(s->position()) == res->timeline.moveCount);

    // ---- Scrub: the API and a press on the track ----
    s->scrubTo(0.5);
    // While the seek runs on its worker: the place being sought is shown (the UI thread does not wait for it).
    CHECK(s->seeking());
    CHECK(s->targetSec() == Catch::Approx(total / 2).margin(total * 1e-9));
    CHECK(w.find(QStringLiteral("simTarget"))->isVisible());
    w.simIdle();
    CHECK_FALSE(s->seeking());
    CHECK(s->positionSec() == Catch::Approx(total / 2).margin(total * 1e-9));
    QQuickItem* scrub = w.shown(QStringLiteral("simScrub"));
    const QPointF at = scrub->mapToScene(QPointF(scrub->width() * 0.25, scrub->height() / 2));
    w.sendMouse(QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
    const QPointF to = scrub->mapToScene(QPointF(scrub->width() * 0.3, scrub->height() / 2));
    w.sendMouse(QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton);
    w.sendMouse(QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
    w.simIdle();
    CHECK(s->positionSec() == Catch::Approx(total * 0.3).margin(total / scrub->width() + 1e-6));
    // Many scrub requests at once: the seeks coalesce to the latest; the stock ends where the last one asked.
    const int framesBefore = s->framesApplied();
    for (int i = 0; i <= 40; ++i)
        s->scrubTo(i / 40.0);
    w.simIdle();
    CHECK(s->positionSec() == Catch::Approx(total));
    CHECK(s->framesApplied() - framesBefore < 41);

    // ---- Play, pause and the speed ----
    w.key(Qt::Key_Home);
    w.simIdle();
    s->setSpeedIndex(3); // 10x
    w.key(Qt::Key_Space);
    REQUIRE(s->playing());
    QElapsedTimer played;
    played.start();
    QTest::qWait(1500);
    const double during = s->positionSec();
    const double wall = static_cast<double>(played.elapsed()) / 1000.0;
    CHECK(during > 0);
    CHECK(during <= 10 * wall + 1e-6);
    CHECK(during >= 10 * wall * 0.3); // the stock follows the clock (frames dropped, never time)
    w.shotsBothThemes(QStringLiteral("t074_playing"));
    w.key(Qt::Key_Space);
    REQUIRE_FALSE(s->playing());
    w.simIdle();
    const double paused = s->positionSec();
    QTest::qWait(300);
    CHECK(s->positionSec() == paused);
    // ] and [ change the speed; the button plays from where it stopped, faster.
    w.key(Qt::Key_BracketRight);
    CHECK(s->speedIndex() == 4);
    w.key(Qt::Key_BracketLeft);
    CHECK(s->speedIndex() == 3);
    s->setSpeedIndex(6); // 100x
    w.click(QStringLiteral("simPlay"));
    REQUIRE(s->playing());
    QTest::qWait(800);
    w.click(QStringLiteral("simPlay"));
    w.simIdle();
    CHECK(s->positionSec() > paused);
    INFO("frames applied " << s->framesApplied() << ", most triangles in a frame " << s->mostFrameTriangles()
                           << ", worst seek " << s->worstSeekMs() << " ms");
    CHECK(s->framesApplied() > 3);

    // ---- The command palette: the simulation's commands in this workspace only; they run ----
    ui::CommandCatalog* commands = w.singleton<ui::CommandCatalog>("Commands");
    for (const char* cmd : {"cam.sim.run", "cam.sim.play", "cam.sim.stepForward", "cam.sim.nextEvent",
                            "cam.sim.toggleStock", "cam.sim.toggleFixtures", "cam.sim.open"}) {
        const auto c = commands->find(QString::fromLatin1(cmd));
        INFO(cmd);
        REQUIRE(c.has_value());
        CHECK(commands->inPalette(*c));
    }
    REQUIRE(commands->trigger(QStringLiteral("cam.sim.toggleTool")));
    CHECK_FALSE(s->showTool());
    REQUIRE(commands->trigger(QStringLiteral("cam.sim.toggleTool")));
    CHECK(s->showTool());
    w.toWorkspace(ui::kCamWorkspace);
    CHECK_FALSE(commands->inPalette(*commands->find(QStringLiteral("cam.sim.play"))));
    CHECK(commands->inPalette(*commands->find(QStringLiteral("cam.sim.open")))); // shows the workspace from anywhere
    REQUIRE(commands->trigger(QStringLiteral("cam.sim.open")));
    REQUIRE(QTest::qWaitFor([&] { return w.host->context()->workspace() == ui::kSimWorkspace; }, 5000));

    // ---- The visibility toggles ----
    w.key(Qt::Key_End);
    w.simIdle();
    REQUIRE(s->stockVisible());
    w.click(QStringLiteral("simShow.stock"));
    CHECK_FALSE(s->showStock());
    CHECK_FALSE(s->stockVisible());
    CHECK(s->hiddenBodies().empty()); // the plate shows instead
    w.click(QStringLiteral("simShow.stock"));
    CHECK(s->stockVisible());
    w.click(QStringLiteral("simShow.tool"));
    CHECK_FALSE(s->toolVisible());
    CHECK(s->holderVisible());
    w.click(QStringLiteral("simShow.holder"));
    CHECK_FALSE(s->holderVisible());
    w.shotsBothThemes(QStringLiteral("t074_toggles_off"));
    w.click(QStringLiteral("simShow.tool"));
    w.click(QStringLiteral("simShow.holder"));
    CHECK(s->toolVisible());
    CHECK(s->holderVisible());

    // ---- The toolpaths (added after T-074): shown by default, the moves done in full, those to come dimmed ----
    {
        const std::pair<std::uint32_t, std::uint32_t> none{1, 0};
        REQUIRE(res->backplots.size() == 2);
        CHECK(s->showToolpath());
        CHECK(s->toolpathVisible());
        const auto n0 = static_cast<std::uint32_t>(res->timeline.ops[0].moves());
        const auto n1 = static_cast<std::uint32_t>(res->timeline.ops[1].moves());
        for (std::size_t op = 0; op < 2; ++op) {
            CHECK(s->pathItem(op, false) != 0);
            CHECK(s->pathItem(op, true) != 0);
        }
        // At the end: everything done.
        CHECK(s->pathRange(0, false) == std::make_pair(0u, n0 - 1));
        CHECK(s->pathRange(0, true) == none);
        CHECK(s->pathRange(1, false) == std::make_pair(0u, n1 - 1));
        CHECK(s->pathRange(1, true) == none);
        // At the start: nothing done.
        w.view()->forceActiveFocus();
        w.key(Qt::Key_Home);
        w.simIdle();
        CHECK(s->pathRange(0, false) == none);
        CHECK(s->pathRange(0, true) == std::make_pair(0u, n0 - 1));
        CHECK(s->pathRange(1, true) == std::make_pair(0u, n1 - 1));
        // Part way through the facing: its moves up to the one under way done, the rest ahead; the drilling all done.
        const cs::TimelineOperation& facing = res->timeline.ops[1];
        s->scrubTo((facing.firstMoveSec() + facing.endSec) / 2 / total);
        w.simIdle();
        const sim::TimelinePosition p = s->position();
        REQUIRE(p.opIndex == 1);
        const std::uint32_t begun = p.moveIndex + (p.t01 > 0 ? 1u : 0u);
        CHECK(s->pathRange(0, false) == std::make_pair(0u, n0 - 1));
        CHECK(s->pathRange(0, true) == none);
        CHECK(s->pathRange(1, false) == std::make_pair(0u, begun - 1));
        CHECK(s->pathRange(1, true) == std::make_pair(begun, n1 - 1));
        w.view()->fitAll();
        w.simIdle();
        // The picture part way through the facing's first layer (later layers lie under the ones done): the passes
        // made in full, those to come over the rest of the plate dimmed.
        s->scrubTo((facing.firstMoveSec() + 0.08 * (facing.endSec - facing.firstMoveSec())) / total);
        w.simIdle();
        w.shotsBothThemes(QStringLiteral("simgaps_toolpath"));
        // The toggle (the panel's fifth) and its command.
        w.click(QStringLiteral("simShow.toolpath"));
        CHECK_FALSE(s->showToolpath());
        CHECK_FALSE(s->toolpathVisible());
        CHECK(s->stockVisible()); // the others as they were
        w.click(QStringLiteral("simShow.toolpath"));
        CHECK(s->toolpathVisible());
        ui::CommandCatalog* catalog = w.singleton<ui::CommandCatalog>("Commands");
        REQUIRE(catalog->trigger(QStringLiteral("cam.sim.toggleToolpath")));
        CHECK_FALSE(s->toolpathVisible());
        REQUIRE(catalog->trigger(QStringLiteral("cam.sim.toggleToolpath")));
        CHECK(s->toolpathVisible());
        // Only in the Simulate workspace.
        w.toWorkspace(ui::kCamWorkspace);
        CHECK_FALSE(s->toolpathVisible());
        w.toWorkspace(ui::kSimWorkspace);
        CHECK(s->toolpathVisible());
        w.simIdle(); // the gate checked again on coming back
    }

    // ---- Cancel mid-run: the button, then Esc ----
    REQUIRE(s->run());
    CHECK(s->running());
    CHECK_FALSE(s->hasResult()); // the old result is gone while the new one runs: never shown as current
    w.click(QStringLiteral("simCancel"));
    CHECK(s->state() == QStringLiteral("cancelled"));
    CHECK_FALSE(s->hasResult());
    CHECK(s->stockItem() == 0);
    w.simIdle();
    w.view()->forceActiveFocus();
    REQUIRE(s->run());
    w.key(Qt::Key_Escape);
    CHECK(s->state() == QStringLiteral("cancelled"));
    w.shotsBothThemes(QStringLiteral("t074_cancelled"));
    w.runSim(); // and a run after them works

    // ---- Out of date after an edit; run again ----
    (void)w.invokeResult("cam.operation.edit", R"({"id":")" + id('c', 1) + R"(","params":{"stepdown":0.75}})");
    REQUIRE(QTest::qWaitFor([&] { return s->outOfDate(); }, kTimeoutMs));
    CHECK(s->outOfDateReason().contains(QStringLiteral("Face")));
    w.shown(QStringLiteral("simOutOfDate"));
    CHECK_FALSE(s->canRun()); // the face toolpath is stale: generate first
    w.shown(QStringLiteral("simGenerateAll"));
    w.shotsBothThemes(QStringLiteral("t074_out_of_date"));
    w.click(QStringLiteral("simGenerateAll"));
    REQUIRE(QTest::qWaitFor([&] { return s->canRun(); }, kTimeoutMs * 2));
    CHECK(s->outOfDate()); // generated again: still not what was simulated
    w.runSim();
    CHECK_FALSE(s->outOfDate());

    // ---- The fixtures toggle: the shipped vise as a fixture of the setup ----
    w.toWorkspace(ui::kCamWorkspace);
    ui::FixturesToolModel* f = w.fixtures();
    REQUIRE(f->open());
    int vise = -1;
    const QVariantList library = f->library();
    for (int i = 0; i < library.size(); ++i) {
        if (library[i].toMap().value(QStringLiteral("name")).toString() == QStringLiteral("Generic vise 6in"))
            vise = i;
    }
    REQUIRE(vise >= 0);
    REQUIRE(f->addFromLibrary(vise));
    REQUIRE(QTest::qWaitFor([&] { return f->fixtures().size() == 2 && !f->busy(); }, kTimeoutMs));
    w.settle();
    f->cancel();
    f->close();
    w.toWorkspace(ui::kSimWorkspace);
    w.simIdle();
    CHECK(s->outOfDate()); // the setup's fixtures changed
    std::vector<int> fixtureBodies;
    for (int i = 0; i < w.host->scene()->bodyCount(); ++i) {
        if (w.host->scene()->body(i)->fixture)
            fixtureBodies.push_back(i);
    }
    REQUIRE(fixtureBodies.size() == 2);
    auto hiddenNow = s->hiddenBodies();
    for (const int b : fixtureBodies)
        CHECK(std::find(hiddenNow.begin(), hiddenNow.end(), b) == hiddenNow.end());
    w.click(QStringLiteral("simShow.fixtures"));
    CHECK_FALSE(s->showFixtures());
    hiddenNow = s->hiddenBodies();
    for (const int b : fixtureBodies)
        CHECK(std::find(hiddenNow.begin(), hiddenNow.end(), b) != hiddenNow.end());
    w.shotsBothThemes(QStringLiteral("t074_fixtures_hidden"));
    // Only in the Simulate workspace: leaving it shows them again (and the model tree never changed).
    w.toWorkspace(ui::kCamWorkspace);
    CHECK(s->hiddenBodies().empty());
    for (const int b : fixtureBodies)
        CHECK(w.host->scene()->bodyVisible(b));
    w.toWorkspace(ui::kSimWorkspace);
    hiddenNow = s->hiddenBodies();
    for (const int b : fixtureBodies)
        CHECK(std::find(hiddenNow.begin(), hiddenNow.end(), b) != hiddenNow.end());
    w.click(QStringLiteral("simShow.fixtures"));
    w.checkNoWarnings();
}

TEST_CASE(
    "a second setup turned over, its stock from the first: the CAM tree, the WCS, the setup panel, the simulation",
    "[ui][cam][sim][carried]")
{
    RAY_REQUIRE_RHI();
    Window w;
    w.open(kPlate);
    w.toWorkspace(ui::kCamWorkspace);
    buildPlateJob(w, 8.0);
    addTurnedOverSetup(w);

    // The CAM tree: setup 2 resolves (from the carried stock once setup 1 is up to date), no "simulated first".
    SetupTreeModel* tree = w.tree();
    REQUIRE(QTest::qWaitFor([&] { return tree->setupCount() == 2; }, kTimeoutMs));
    REQUIRE(QTest::qWaitFor(
        [&] {
            const auto st = tree->setupState(setupId(2));
            return !tree->busy() && st.has_value();
        },
        kTimeoutMs * 2));
    const auto state2 = tree->setupState(setupId(2));
    INFO((state2 && !*state2 ? state2->error().message : std::string()));
    CHECK((state2 && *state2));

    // Its WCS in the view (the tree's current setup), from the carried stock.
    const int row = tree->rowOf(QString::fromStdString(id('a', 2)));
    REQUIRE(row >= 0);
    tree->setCurrentIndex(row);
    REQUIRE(QTest::qWaitFor([&] { return w.setupTool()->wcsLines().count() >= 3; }, kTimeoutMs));
    w.shotsBothThemes(QStringLiteral("t074_setup2_wcs"));

    // The setup panel on setup 2: its stock is "from an earlier setup", previewed from the carried stock.
    SetupToolModel* panel = w.setupTool();
    REQUIRE(panel->edit(QString::fromStdString(id('a', 2))));
    CHECK(panel->values().value(QStringLiteral("stock")).toString() == QStringLiteral("fromSetup"));
    CHECK(panel->values().value(QStringLiteral("fromSetup")).toString().toStdString() == id('a', 1));
    REQUIRE(QTest::qWaitFor([&] { return !panel->previewBusy() && panel->lastPreview().has_value(); }, kTimeoutMs * 2));
    INFO(panel->message().toStdString());
    CHECK(panel->previewStatus() == QStringLiteral("ok"));
    REQUIRE(panel->lastPreview()->stockMesh);
    CHECK(panel->lastPreview()->stockMesh->indices.size() > 0);
    w.shown(QStringLiteral("setupChoice.fromSetup"));
    w.shotsBothThemes(QStringLiteral("t074_setup2_panel"));
    panel->cancel();

    // The simulation of setup 2, from what setup 1 leaves.
    w.toWorkspace(ui::kSimWorkspace);
    SimToolModel* s = w.sim();
    s->setSetup(QString::fromStdString(id('a', 2)));
    w.runSim();
    const auto res = s->result();
    REQUIRE(res);
    CHECK(res->setup == setupId(2));
    CHECK(s->stockItem() != 0);
    CHECK(res->timeline.moveCount > 0);
    for (std::size_t i = 0; i < res->events.size(); ++i)
        UNSCOPED_INFO(ui::camui::eventKindName(static_cast<int>(res->events[i].kind))
                      << ": " << res->events[i].message);
    w.view()->fitAll();
    w.shotsBothThemes(QStringLiteral("t074_setup2_simulated"));
    w.checkNoWarnings();
}

// =============================================================================================================
// added after T-074
TEST_CASE("a real holder collision: the library vise, its marker, the holder at the contact, the problem's row",
          "[ui][cam][sim][holder]")
{
    RAY_REQUIRE_RHI();
    Window w;
    w.open(kPlate);
    w.toWorkspace(ui::kCamWorkspace);
    buildViseJob(w);
    w.toWorkspace(ui::kSimWorkspace);
    SimToolModel* s = w.sim();
    w.shown(QStringLiteral("simPanel"));
    REQUIRE(QTest::qWaitFor([&] { return s->setupIdText().toStdString() == id('a', 1); }, 5000));
    w.runSim();
    const auto res = s->result();
    REQUIRE(res);
    for (std::size_t i = 0; i < res->events.size(); ++i)
        UNSCOPED_INFO(ui::camui::eventKindName(static_cast<int>(res->events[i].kind))
                      << " op " << res->places[i].opIndex << " move " << res->events[i].moveIndex << " at "
                      << res->places[i].sec << " s: " << res->events[i].message);
    std::optional<int> hit;
    for (std::size_t i = 0; i < res->events.size(); ++i) {
        // Nothing seeded: the stock is the plate's own size, the drill's rapids stay above it.
        CHECK(res->events[i].kind != sim::EventKind::RapidIntoStock);
        if (res->events[i].kind == sim::EventKind::HolderHitsFixture && !hit)
            hit = static_cast<int>(i);
    }
    REQUIRE(hit.has_value());
    CHECK(res->events[static_cast<std::size_t>(*hit)].severity == sim::Severity::Error);
    CHECK(s->errorCount() > 0);

    // ---- Its marker on the timeline, clicked ----
    QQuickItem* track = w.shown(QStringLiteral("simTrack"));
    const QVariantList markers = s->markers(track->width());
    int markerIndex = -1;
    for (int k = 0; k < markers.size() && markerIndex < 0; ++k) {
        if (markers[k].toMap().value(QStringLiteral("events")).toList().contains(*hit))
            markerIndex = k;
    }
    REQUIRE(markerIndex >= 0);
    const QVariantMap marker = markers[markerIndex].toMap();
    CHECK(marker.value(QStringLiteral("severity")).toString() == QStringLiteral("error"));
    QQuickItem* markerItem = w.shown(QStringLiteral("simMarker.%1").arg(markerIndex));
    CHECK(markerItem->property("colour").value<QColor>() == w.host->theme()->property("error").value<QColor>());
    const int chosen = marker.value(QStringLiteral("first")).toInt();
    w.click(QStringLiteral("simMarker.%1").arg(markerIndex));
    REQUIRE(QTest::qWaitFor([&] { return s->currentEvent() == chosen; }, 5000));
    w.simIdle();
    const cs::EventPlace& place = res->places[static_cast<std::size_t>(chosen)];
    const sim::SimEvent& event = res->events[static_cast<std::size_t>(chosen)];
    CHECK(event.kind == sim::EventKind::HolderHitsFixture);
    CHECK(s->position().opIndex == place.opIndex);
    CHECK(s->position().moveIndex == place.moveIndex);
    CHECK(s->position().t01 == Catch::Approx(place.t01).margin(1e-9));
    CHECK(s->positionSec() == Catch::Approx(place.sec).margin(1e-6));

    // ---- The holder shown at the contact ----
    const Xform pose = s->toolPose();
    const Vec3 tip{pose.m[3], pose.m[7], pose.m[11]};
    const Vec3 want = apply(res->modelFromWcs, event.position);
    INFO("tip " << tip.x << ", " << tip.y << ", " << tip.z << "; event " << want.x << ", " << want.y << ", " << want.z);
    CHECK(distance(tip, want) < 0.05);
    CHECK(s->shownOp() == static_cast<int>(place.opIndex));
    CHECK(s->holderItem(place.opIndex) != 0);
    CHECK(s->holderVisible());
    CHECK(s->toolVisible());
    // The holder glyph stands where the tool is (its pose is the tool's); its lowest ring is at the jaws' top (z 30 in
    // the model) and reaches over the fixed jaw's clamping face (y = 0).
    const sim::ToolShape& tool = res->tools[place.opIndex];
    REQUIRE_FALSE(tool.holderProfile.empty());
    double holderBottom = 1e9, holderRadius = 0;
    for (const Vec2& v : tool.holderProfile) {
        holderBottom = std::min(holderBottom, v.y);
        holderRadius = std::max(holderRadius, v.x);
    }
    INFO("holder bottom at z " << tip.z + holderBottom << ", radius " << holderRadius);
    CHECK(tip.z + holderBottom <= 30 + 0.05);
    CHECK(tip.z + holderBottom >= 30 - 0.5);
    CHECK(tip.y - holderRadius < 0);

    // ---- The problem's row ----
    QQuickItem* row = w.shown(QStringLiteral("simEvent.%1").arg(chosen));
    const QVariantMap e = s->events()[chosen].toMap();
    CHECK(e.value(QStringLiteral("title")).toString() == QStringLiteral("Holder hits a fixture"));
    CHECK(e.value(QStringLiteral("severity")).toString() == QStringLiteral("error"));
    CHECK(e.value(QStringLiteral("operation")).toString() == QStringLiteral("Drill"));
    CHECK(e.value(QStringLiteral("move")).toInt() == static_cast<int>(place.moveIndex) + 1);
    CHECK(e.value(QStringLiteral("time")).toString() == ui::camui::formatClock(place.sec));
    QQuickItem* message = w.find(QStringLiteral("simEventMessage.%1").arg(chosen), row);
    REQUIRE(message != nullptr);
    CHECK(message->property("text").toString() == QString::fromStdString(event.message));
    CHECK(row->property("current").toBool());
    CHECK(s->summary().contains(QStringLiteral("error")));
    w.view()->fitAll();
    w.simIdle();
    w.shotsBothThemes(QStringLiteral("simgaps_holder_collision"));
    // The row goes there too: from the start, a click on it.
    w.view()->forceActiveFocus();
    w.key(Qt::Key_Home);
    w.simIdle();
    REQUIRE(s->positionSec() == 0);
    w.click(QStringLiteral("simEvent.%1").arg(chosen));
    w.simIdle();
    CHECK(s->currentEvent() == chosen);
    CHECK(s->position().moveIndex == place.moveIndex);
    CHECK(s->position().t01 == Catch::Approx(place.t01).margin(1e-9));
    w.checkNoWarnings();
}

TEST_CASE("a warning the whole way: its amber marker and row, and both seek there", "[ui][cam][sim][warning]")
{
    RAY_REQUIRE_RHI();
    Window w;
    w.open(kPlate);
    w.toWorkspace(ui::kCamWorkspace);
    buildPlateJob(w, 8.0); // the clean twin: no event of its own
    w.toWorkspace(ui::kSimWorkspace);
    SimToolModel* s = w.sim();
    // No check raises a Warning yet: one is added where the simulator's events go (cam_sim::RunRequest::addedEvents),
    // on the facing (the program's operation 1), at the end of a straight feed part way through it.
    const auto face = w.statuses().at(opId(1)).toolpath;
    REQUIRE(face);
    std::uint32_t k = static_cast<std::uint32_t>(face->moves.size() / 2);
    while (k + 1 < face->moves.size() &&
           !(face->moves[k].kind == cam::MoveKind::Linear &&
             (face->moves[k].x != face->moves[k - 1].x || face->moves[k].y != face->moves[k - 1].y)))
        ++k;
    REQUIRE(k + 1 < face->moves.size());
    const cam::Move& mv = face->moves[k];
    const Vec3 at{mv.x / 1e6, mv.y / 1e6, mv.z / 1e6};
    s->setAddedEvents({sim::SimEvent{sim::EventKind::ExcessEngagement, sim::Severity::Warning, opId(1), k, at, 0,
                                     "The cutter's engagement went over its limit here (added by the test)"}});
    w.runSim();
    s->setAddedEvents({});
    const auto res = s->result();
    REQUIRE(res);
    REQUIRE(res->events.size() == 1);
    CHECK(s->errorCount() == 0);
    CHECK(s->warningCount() == 1);
    CHECK(s->summary().startsWith(QStringLiteral("1 warning.")));
    const QColor amber = w.host->theme()->property("warning").value<QColor>();
    CHECK(w.shown(QStringLiteral("simSummary"))->property("color").value<QColor>() == amber);
    const cs::EventPlace& place = res->places[0];
    CHECK(place.opIndex == 1);
    CHECK(place.moveIndex == k);

    // ---- The amber marker ----
    QQuickItem* track = w.shown(QStringLiteral("simTrack"));
    const QVariantList markers = s->markers(track->width());
    REQUIRE(markers.size() == 1);
    CHECK(markers[0].toMap().value(QStringLiteral("severity")).toString() == QStringLiteral("warning"));
    QQuickItem* markerItem = w.shown(QStringLiteral("simMarker.0"));
    CHECK(markerItem->property("colour").value<QColor>() == amber);
    CHECK(markerItem->x() + markerItem->width() / 2 ==
          Catch::Approx(place.sec / res->timeline.totalSec * track->width()).margin(1.0));
    w.click(QStringLiteral("simMarker.0"));
    REQUIRE(QTest::qWaitFor([&] { return s->currentEvent() == 0; }, 5000));
    w.simIdle();
    CHECK(s->position().opIndex == 1);
    CHECK(s->position().moveIndex == k);
    CHECK(s->position().t01 == Catch::Approx(place.t01).margin(1e-9));
    CHECK(s->positionSec() == Catch::Approx(place.sec).margin(1e-6));
    const Vec3 tip{s->toolPose().m[3], s->toolPose().m[7], s->toolPose().m[11]};
    CHECK(distance(tip, apply(res->modelFromWcs, at)) < 0.05);

    // ---- Its row: amber, the machinist's title, the message as raised ----
    QQuickItem* row = w.shown(QStringLiteral("simEvent.0"));
    const QVariantMap e = s->events()[0].toMap();
    CHECK(e.value(QStringLiteral("severity")).toString() == QStringLiteral("warning"));
    CHECK(e.value(QStringLiteral("title")).toString() ==
          ui::camui::eventTitle(static_cast<int>(sim::EventKind::ExcessEngagement)));
    CHECK(e.value(QStringLiteral("operation")).toString() == QStringLiteral("Face"));
    CHECK(e.value(QStringLiteral("move")).toInt() == static_cast<int>(k) + 1);
    QQuickItem* message = w.find(QStringLiteral("simEventMessage.0"), row);
    REQUIRE(message != nullptr);
    CHECK(message->property("text").toString().contains(QStringLiteral("added by the test")));
    CHECK(row->property("current").toBool());
    w.view()->fitAll();
    w.simIdle();
    w.shotsBothThemes(QStringLiteral("simgaps_warning"));
    // The row seeks too, and the previous / next problem keys find it.
    w.view()->forceActiveFocus();
    w.key(Qt::Key_End);
    w.simIdle();
    w.click(QStringLiteral("simEvent.0"));
    w.simIdle();
    CHECK(s->position().moveIndex == k);
    w.view()->forceActiveFocus();
    w.key(Qt::Key_Home);
    w.simIdle();
    w.key(Qt::Key_Right, Qt::ControlModifier);
    w.simIdle();
    CHECK(s->currentEvent() == 0);
    CHECK(s->positionSec() == Catch::Approx(place.sec).margin(1e-6));
    w.checkNoWarnings();
}

// =============================================================================================================
// wallclock
TEST_CASE("a larger program on a pocket part: the UI thread while it runs, plays and scrubs",
          "[ui][cam][sim][wallclock]")
{
    RAY_REQUIRE_RHI();
    Window w;
    w.open(kPocketPart);
    w.toWorkspace(ui::kCamWorkspace);
    // The pocket's floor: the highest upward planar face below the top.
    const Box3 box = kernel::bounds(w.document()->build()->bodies.at(w.plate()).shape);
    std::optional<kernel::SubId> floor;
    double floorZ = -1e9;
    const api::viewer::Body* body = w.host->scene()->body(0);
    for (std::uint32_t i = 0; i < body->shape.count(kernel::SubKind::Face); ++i) {
        const kernel::SubId sub{kernel::SubKind::Face, i};
        auto smp = kernel::faceSample(body->shape, sub);
        auto info = kernel::surfaceInfo(body->shape, sub);
        if (!smp || !info || info->type != kernel::SurfaceType::Plane || std::abs(smp->normal.z - 1) > 1e-9)
            continue;
        if (info->centroid.z < box.max.z - 1e-3 && info->centroid.z > floorZ) {
            floor = sub;
            floorZ = info->centroid.z;
        }
    }
    REQUIRE(floor.has_value());
    const lib::MachineDef* m = w.host->app()->libraries()->machine(MachineId{Uuid::fromString(kVmc)});
    REQUIRE(m != nullptr);
    (void)w.invokeResult("cam.setup.add",
                         R"({"id":")" + id('a', 1) + R"(","name":"Pocket part","partBodies":[")" +
                             w.plate().value.toString() + R"("],"machine":")" + kVmc +
                             R"(","stock":{"type":"box","xNeg":2,"xPos":2,"yNeg":2,"yPos":2,"zNeg":0,"zPos":1},)"
                             R"("library":{"machines":[)" +
                             api::cam_setup::machineJson(*m) + "]}}");
    const std::string mill = R"({"schema":1,"id":")" + id('d', 1) +
                             R"(","name":"3 mm end mill","type":"flatEndMill","flutes":2,"diameter":3,)"
                             R"("shankDiameter":6,"fluteLength":12,"shoulderLength":15,"overallLength":50,)"
                             R"("defaultToolNumber":1})";
    (void)w.invokeResult("cam.library.embed", R"({"tools":[)" + mill + "]}");
    (void)w.invokeResult("cam.operation.add",
                         R"({"id":")" + id('c', 1) + R"(","setup":")" + id('a', 1) +
                             R"(","type":"face","name":"Face","tool":{"tool":")" + id('d', 1) +
                             R"(","stickOut":20,"number":1},"cutting":{"rpm":8000,"feedCut":1500,"feedPlunge":400},)"
                             R"("heights":{"clearance":15,"retract":5,"feedPlane":2,"top":0,"bottom":0},)"
                             R"("geometry":[],"params":{"stepdown":0.5}})");
    (void)w.invokeResult(
        "cam.operation.add",
        R"({"id":")" + id('c', 2) + R"(","setup":")" + id('a', 1) +
            R"(","type":"pocket2d","name":"Pocket","tool":{"tool":")" + id('d', 1) +
            R"(","stickOut":20,"number":1},"cutting":{"rpm":8000,"feedCut":1500,"feedPlunge":400,"feedRamp":800},)"
            R"("heights":{"clearance":15,"retract":5,"feedPlane":2,"top":0,"bottom":0},"geometry":[)" +
            QJsonDocument(w.faceRef(*floor)).toJson(QJsonDocument::Compact).toStdString() +
            R"(],"params":{"stepdown":0.5,"stepover":25}})");
    // Adaptive clearing of the same pocket again (rest material is none: the moves are what counts here).
    (void)w.invokeResult(
        "cam.operation.add",
        R"({"id":")" + id('c', 3) + R"(","setup":")" + id('a', 1) +
            R"(","type":"adaptive2d","name":"Adaptive","tool":{"tool":")" + id('d', 1) +
            R"(","stickOut":20,"number":1},"cutting":{"rpm":8000,"feedCut":1500,"feedPlunge":400,"feedRamp":800},)"
            R"("heights":{"clearance":15,"retract":5,"feedPlane":2,"top":0,"bottom":0},"geometry":[)" +
            QJsonDocument(w.faceRef(*floor)).toJson(QJsonDocument::Compact).toStdString() +
            R"(],"params":{"stepover":8,"stepdown":1}})");
    w.generate(1);
    w.generate(2);
    w.generate(3);
    std::uint64_t moves = 0;
    for (const auto& [op, st] : w.statuses())
        moves += st.toolpath ? st.toolpath->moves.size() : 0;

    w.toWorkspace(ui::kSimWorkspace);
    SimToolModel* s = w.sim();
    w.simIdle();
    REQUIRE(s->canRun());
    // The same program at each tier preset's cell, budget and the UI thread's worst stall in each.
    for (const render::Tier tier : {render::Tier::Low, render::Tier::Medium, render::Tier::High}) {
        const render::TierPreset preset = render::presetFor(tier);
        w.host->scene()->setPreset(preset);
        s->resetStats();
        ray::test::UiWatchdog watchdog;
        watchdog.start();
        QElapsedTimer runClock;
        runClock.start();
        REQUIRE(s->run());
        REQUIRE(ray::test::waitFor([&] { return s->state() != QLatin1String("running"); }, kTimeoutMs * 3));
        const double runMs = static_cast<double>(runClock.elapsed());
        REQUIRE(s->state() == QStringLiteral("done"));
        REQUIRE(ray::test::waitFor([&] { return !s->busy(); }, kTimeoutMs));
        const double runStall = watchdog.worstStallMs();
        const auto res = s->result();
        REQUIRE(res);
        CHECK(res->cellMm == preset.simCellMm);

        // Playback at 100x for three seconds.
        s->toStart();
        REQUIRE(ray::test::waitFor([&] { return !s->busy(); }, kTimeoutMs));
        watchdog.reset();
        const int framesBefore = s->framesApplied();
        s->setSpeedIndex(6);
        s->play();
        ray::test::waitMs(3000);
        s->pause();
        const int playFrames = s->framesApplied() - framesBefore;
        const double playedSec = s->positionSec();
        REQUIRE(ray::test::waitFor([&] { return !s->busy(); }, kTimeoutMs));
        const double playStall = watchdog.worstStallMs();

        // Scrubbing: 60 places along the track, one every 16 ms, as a drag delivers them.
        watchdog.reset();
        const int framesScrub = s->framesApplied();
        for (int i = 0; i < 60; ++i) {
            s->scrubTo((i * 37 % 60) / 60.0);
            ray::test::waitMs(16);
        }
        REQUIRE(ray::test::waitFor([&] { return !s->busy(); }, kTimeoutMs));
        const double scrubStall = watchdog.worstStallMs();
        const int scrubFrames = s->framesApplied() - framesScrub;
        const std::string worst = watchdog.worst().describe();
        watchdog.stop();
        if (tier == render::Tier::Medium)
            w.shotsBothThemes(QStringLiteral("t074_pocket_part"));

        const double gpuMb = static_cast<double>(s->triangleBudget()) * ui::camui::kStockBytesPerTriangle / 1e6;
        WARN("pocket part at " << preset.simCellMm << " mm: " << moves << " moves (timeline " << res->timeline.moveCount
                               << "), " << res->timeline.totalSec << " s of machine time; run " << runMs
                               << " ms (program " << res->programSec * 1000 << " ms, simulation " << res->runSec * 1000
                               << " ms, timeline " << res->timelineSec * 1000 << " ms), first mesh "
                               << res->first.triangles << " triangles; budget " << s->triangleBudget() << " triangles ("
                               << gpuMb << " MB on the GPU)");
        WARN("  playback 100x for 3 s: " << playFrames << " frames, reached " << playedSec << " s; scrub: 60 requests, "
                                         << scrubFrames << " frames; worst seek " << s->worstSeekMs()
                                         << " ms, most triangles in one frame " << s->mostFrameTriangles());
        WARN("  worst UI-thread stall: run " << runStall << " ms, playback " << playStall << " ms, scrub " << scrubStall
                                             << " ms; " << worst);
        CHECK(runStall < ray::test::uiBudgetMs(50));
        CHECK(playStall < ray::test::uiBudgetMs(50));
        CHECK(scrubStall < ray::test::uiBudgetMs(50));
        CHECK(playFrames > 10);
        CHECK(scrubFrames <= 60); // never more frames than requests (coalesced when seeks are slower)
        CHECK(res->first.triangles <= s->triangleBudget());
    }
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
