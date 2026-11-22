// The CAM workspace and its setups (task T-044) in the desktop app, on the real Qt Quick window with the real
// application session and an imported corpus part:
//
//   - a setup in four interactions, counted: New setup, a click on the top face, a click on an origin marker of the
//     previewed stock, OK (the default stock accepted as it is); the setup the document gets; no confirmation asked
//   - the preview (stock and WCS triad) is there before the first pick and follows every change; a curved face is
//     refused with the reason; flip X turns the frame; editing a setup from the tree; Esc and leaving the workspace
//     change nothing; undo and redo; closing the document while a preview runs
//   - each status badge (up to date, stale, generating with progress, failed, broken reference, suppressed) against
//     the engine with the CAM engine tests' test-only operation type (and one here that reports progress while it
//     waits), following a model edit in the Model workspace and undo
//   - [wallclock] the preview's latency on a large corpus part, axis-aligned and tilted, with the UI-thread watchdog
//
// Screenshots: with RAY_T044_SHOTS set to a folder, the window is saved there in both themes.
#include "../scene_graph_probe.h"
#include "../ui_budget.h"
#include "app_context.h"
#include "app_host.h"
#include "app_options.h"
#include "cam/setup_tool.h"
#include "cam/setup_tree.h"
#include "engine_test_util.h" // the CAM engine tests' test-only operation type and library JSON (tests/unit/cam)
#include "files.h"
#include "palette_commands.h"
#include "palette_model.h"
#include "scene_controller.h"
#include "selection_model.h"
#include "session.h"
#include "shortcuts.h"
#include "theme_object.h"
#include "ui_commands.h"
#include "units.h"
#include "viewport_item.h"
#include "viewport_navigation.h"

#include <ray_api/cam_setup.h>
#include <ray_api/viewer.h>
#include <ray_render/viewport.h>

#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFileInfo>
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

#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace ray;
using Catch::Approx;
using ray::ui::AppHost;
using ray::ui::AppOptions;
using ray::ui::SetupToolModel;
using ray::ui::SetupTreeModel;
namespace ct = ray::cam::test;

namespace {

constexpr int kTimeoutMs = 60000;
// plate_thru_holes_1 (tools/corpus_gen): a 120 x 80 x 10 mm plate with six 6.6 mm through holes.
const QString kPlate = QStringLiteral(RAY_CORPUS_DIR "/raycad-synthetic/plate_thru_holes_1.stp");
// The largest committed corpus file (about 0.86 MB).
const QString kLarge = QStringLiteral(RAY_CORPUS_DIR "/nist-pmi/nist_ctc_05_asme1_ap242-e1.stp");

// A test-only operation type that reports progress while it waits at the engine tests' gate (ct::Gate): the badge of a
// generating operation shows how far it is.
class ProgressOperation final : public cam::IOperationType {
public:
    std::string_view typeName() const override { return "ui.progress"; }
    std::string_view paramsSchemaJson() const override { return R"({"type":"object"})"; }
    Status validate(const cam::GenerateInput&) const override { return {}; }
    Result<cam::Toolpath> generate(const cam::GenerateInput& in, JobContext& ctx) const override
    {
        ct::Gate& gate = ct::Gate::instance();
        ++gate.waiting;
        double fraction = 0.2;
        while (!gate.open.load()) {
            if (auto st = ctx.cancel.check(); !st) {
                --gate.waiting;
                return tl::unexpected(st.error());
            }
            fraction = std::min(0.9, fraction + 0.001);
            ctx.progress.set(fraction, "Waiting");
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        --gate.waiting;
        cam::Toolpath tp{};
        cam::Move rapid{};
        rapid.kind = cam::MoveKind::Rapid;
        rapid.role = cam::MoveRole::Approach;
        rapid.z = toNm(in.op.heights.clearance);
        tp.moves = {rapid};
        return tp;
    }
};

struct Window {
    QTemporaryDir cache;
    QTemporaryDir userData;
    std::unique_ptr<AppHost> host;
    int confirms = 0; // the confirm hook: automation only, never the user's own clicks

    explicit Window(bool testOperation = false)
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
        // Operation types go in before the first document (api.h).
        if (testOperation) {
            host->app()->operationTypes()->add(std::make_unique<ct::TestOperation>());
            host->app()->operationTypes()->add(std::make_unique<ProgressOperation>());
        }
        host->app()->setConfirmHook([this](const api::ActionInfo&, std::string_view, std::string_view) {
            ++confirms;
            return true;
        });
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
    SetupToolModel* tool() const { return singleton<SetupToolModel>("CamSetup"); }
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
    // One interaction of the user's: a click at a window point (counted by the caller).
    void clickScene(QPointF p) const
    {
        sendMouse(QEvent::MouseMove, p, Qt::NoButton, Qt::NoButton);
        QCoreApplication::processEvents();
        sendMouse(QEvent::MouseButtonPress, p, Qt::LeftButton, Qt::LeftButton);
        sendMouse(QEvent::MouseButtonRelease, p, Qt::LeftButton, Qt::NoButton);
        QCoreApplication::processEvents();
    }
    QPointF centreOf(QQuickItem* item) const
    {
        return item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
    }
    void click(QQuickItem* item) const
    {
        REQUIRE(item != nullptr);
        grab();
        clickScene(centreOf(item));
    }
    void click(const char* objectName) const { click(shown(QString::fromLatin1(objectName))); }
    void doubleClick(QQuickItem* item) const
    {
        REQUIRE(item != nullptr);
        grab();
        const QPointF c = centreOf(item);
        sendMouse(QEvent::MouseMove, c, Qt::NoButton, Qt::NoButton);
        QTest::mouseDClick(window(), Qt::LeftButton, Qt::NoModifier, c.toPoint());
        QCoreApplication::processEvents();
    }
    void typeInto(const QString& key, const QString& text) const
    {
        QQuickItem* field = shown(QStringLiteral("setupInput.") + key);
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
    doc::Document* document() const
    {
        api::OpenDocument* od = host->app()->active();
        REQUIRE(od != nullptr);
        return od->document.get();
    }
    std::string stateHash() const { return document()->state()->stateHash().hex(); }
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
        REQUIRE(host->scene()->bodyCount() >= 1);
        view()->fitAll();
        grab();
    }
    void toWorkspace(int index) const
    {
        click(shown(QStringLiteral("workspaceTab.%1").arg(index)));
        REQUIRE(QTest::qWaitFor([&] { return host->context()->workspace() == index; }, 5000));
        grab();
    }
    // The panel's preview is built and shown.
    void previewed(const char* status = "ok") const
    {
        const bool done = QTest::qWaitFor(
            [&] { return !tool()->previewBusy() && tool()->previewStatus() == QLatin1String(status); }, kTimeoutMs);
        INFO("setup preview: status '" << tool()->previewStatus().toStdString() << "', busy " << tool()->previewBusy()
                                       << ", message '" << tool()->message().toStdString() << "', shown "
                                       << tool()->previewsShown());
        REQUIRE(done);
    }
    std::size_t undoCount() const { return document()->undoCount(); }
    bool invoke(const std::string& action, const std::string& args) const
    {
        JobHandle<api::ActionResult> job = host->app()->actions().invoke(action, args, api::Caller::User);
        REQUIRE(QTest::qWaitFor([&] { return job.done(); }, kTimeoutMs));
        auto r = job.wait();
        if (!r)
            UNSCOPED_INFO(action << ": " << r.error().message);
        return r.has_value();
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
    void undo(bool redo = false) const
    {
        const std::size_t before = undoCount();
        REQUIRE(host->context()->invoke(redo ? QStringLiteral("edit.redo") : QStringLiteral("edit.undo")));
        REQUIRE(QTest::qWaitFor([&] { return undoCount() != before; }, kTimeoutMs));
        settle();
    }
    const cam::Setup* setup(std::size_t i = 0) const
    {
        const doc::DocStatePtr s = document()->state();
        return s->cam && i < s->cam->setups.size() ? &s->cam->setups[i] : nullptr;
    }
    QString bodyId(int i = 0) const
    {
        const api::viewer::Body* b = host->scene()->body(i);
        REQUIRE(b != nullptr);
        return QString::fromStdString(b->id.value.toString());
    }
    // The tree row's status key, once the statuses after `after` runs have arrived.
    QString rowStatus(int row) const { return tree()->get(row).value(QStringLiteral("status")).toString(); }
    bool waitStatus(int row, const char* status) const
    {
        const bool ok = QTest::qWaitFor([&] { return rowStatus(row) == QLatin1String(status); }, kTimeoutMs);
        INFO("row " << row << ": '" << rowStatus(row).toStdString() << "' ("
                    << tree()->get(row).value(QStringLiteral("statusText")).toString().toStdString() << "), wanted '"
                    << status << "'");
        CHECK(ok);
        return ok;
    }
    // Whether a window point is free of the panel (so a click there reaches the view or its markers).
    bool freeOfPanel(QPointF p) const
    {
        QQuickItem* panel = find(QStringLiteral("setupPanel"));
        if (panel != nullptr && panel->isVisible()) {
            const QRectF r(panel->mapToScene(QPointF(0, 0)), QSizeF(panel->width(), panel->height()));
            if (r.adjusted(-8, -8, 8, 8).contains(p))
                return false;
        }
        const QRectF v(view()->mapToScene(QPointF(0, 0)), QSizeF(view()->width(), view()->height()));
        return v.adjusted(40, 80, -200, -40).contains(p); // clear of the selection filter and the view cube
    }
    void shot(const char* name) const
    {
        const QString dir = qEnvironmentVariable("RAY_T044_SHOTS");
        if (dir.isEmpty())
            return;
        QDir().mkpath(dir);
        const QPointF away(window()->width() / 2.0, window()->height() - 4.0);
        sendMouse(QEvent::MouseMove, away, Qt::NoButton, Qt::NoButton);
        QTest::qWait(300);
        grab().save(QDir(dir).filePath(QString::fromLatin1(name) + QStringLiteral(".png")), "PNG");
    }
    void shotsBothThemes(const char* name) const
    {
        const QString was = host->theme()->property("mode").toString();
        host->theme()->setProperty("mode", QStringLiteral("dark"));
        shot((std::string(name) + "_dark").c_str());
        host->theme()->setProperty("mode", QStringLiteral("light"));
        shot((std::string(name) + "_light").c_str());
        host->theme()->setProperty("mode", was);
    }
    void checkNoWarnings() const
    {
        for (const QString& warning : host->warnings())
            UNSCOPED_INFO(warning.toStdString());
        CHECK(host->warnings().isEmpty());
    }
};

// The planar faces of body 0 whose outward normal is `normal`, by centroid; and a straight edge.
struct Element {
    std::uint32_t index = 0;
    Vec3 at{};
};
std::vector<Element> faces(const Window& w, const std::function<bool(const kernel::SurfaceInfo&)>& pick)
{
    const api::viewer::Body* body = w.host->scene()->body(0);
    REQUIRE(body != nullptr);
    JobContext ctx;
    std::vector<Element> out;
    for (std::uint32_t i = 0; i < body->shape.count(kernel::SubKind::Face); ++i) {
        auto info = api::viewer::describeElement(body->shape, kernel::SubId{kernel::SubKind::Face, i}, ctx);
        REQUIRE(info);
        if (info->surface && pick(*info->surface))
            out.push_back(Element{i, info->surface->centroid});
    }
    return out;
}

// The plate's top face (planar, normal +Z, the highest).
Element topFace(const Window& w)
{
    auto top = faces(w, [](const kernel::SurfaceInfo& s) {
        return s.type == kernel::SurfaceType::Plane && (s.reversed ? -s.axis.z : s.axis.z) > 0.99;
    });
    REQUIRE(!top.empty());
    std::sort(top.begin(), top.end(), [](const Element& a, const Element& b) { return a.at.z > b.at.z; });
    return top.front();
}

// A point of the plate's top surface (z = top) at a window point the panel does not cover.
QPointF topPoint(const Window& w, double z)
{
    const Box3 b = kernel::bounds(w.host->scene()->body(0)->shape);
    // Between the rows of holes (y = 80/3 and 160/3), off the holes' columns.
    std::optional<QPointF> found;
    for (const double fx : {0.92, 0.85, 0.75, 0.08, 0.15}) {
        const Vec3 p{b.min.x + fx * (b.max.x - b.min.x), (b.min.y + b.max.y) / 2, z};
        const QPointF s = w.sceneOf(p);
        if (!found && w.freeOfPanel(s))
            found = s;
    }
    INFO("a point of the top face that the panel does not cover");
    REQUIRE(found.has_value());
    return found.value_or(QPointF());
}

// A marker the panel does not cover; its key.
QString freeMarker(const Window& w, const QString& prefer = {})
{
    QString best;
    for (const QVariant& m : w.tool()->markers()) {
        const QVariantMap mm = m.toMap();
        const QString key = mm.value(QStringLiteral("key")).toString();
        QQuickItem* item = w.find(QStringLiteral("setupMarker.") + key);
        if (item == nullptr || !item->isVisible() || !w.freeOfPanel(w.centreOf(item)))
            continue;
        if (mm.value(QStringLiteral("current")).toBool())
            continue; // a marker that changes something
        if (key == prefer)
            return key;
        if (best.isEmpty())
            best = key;
    }
    REQUIRE(!best.isEmpty());
    return best;
}

std::string jsonText(const QJsonObject& o)
{
    return QJsonDocument(o).toJson(QJsonDocument::Compact).toStdString();
}

// A setup on the plate through the API (machine 1 of the engine tests, embedded in the same step).
void addSetup(const Window& w, int n = 1)
{
    const std::string args = R"({"id":")" + ct::id('a', static_cast<std::uint64_t>(n)) + R"(","name":"Setup )" +
                             std::to_string(n) + R"(","partBodies":[")" + w.bodyId().toStdString() +
                             R"("],"machine":")" + ct::id('9', 1) + R"(","library":{"machines":[)" +
                             ct::machineJson(1) + "]}}";
    REQUIRE(w.invoke("cam.setup.add", args));
}

} // namespace

TEST_CASE("a setup on an imported part in four interactions", "[cam][e2e]")
{
    RAY_REQUIRE_RHI();
    Window w;
    w.open(kPlate);
    const Element top = topFace(w);
    const double topZ = top.at.z;
    w.toWorkspace(ui::kCamWorkspace);
    // The empty CAM tree says how to start.
    w.shown(QStringLiteral("camTreeEmpty"));
    CHECK(w.find(QStringLiteral("featurePanelHost"))->isVisible() == false); // the Design tools stay in Design
    w.shotsBothThemes("t044_empty");
    const std::string before = w.stateHash();
    const std::size_t undoBefore = w.undoCount();

    int interactions = 0;
    // 1. New setup.
    w.click("camTreeEmptyNewSetup");
    ++interactions;
    REQUIRE(QTest::qWaitFor([&] { return w.tool()->active(); }, 5000));
    w.shown(QStringLiteral("setupPanel"));
    CHECK(w.tool()->pickMode() == QStringLiteral("face"));
    // The preview is there before any pick: the zero-offset box around the part, the WCS at its top front left.
    w.previewed();
    REQUIRE(w.tool()->lastPreview().has_value());
    CHECK(w.tool()->triadLines().has(QStringLiteral("triad.x")));
    CHECK(w.tool()->triadLines().has(QStringLiteral("triad.z")));
    CHECK(w.host->scene()->preview().owner() == w.tool());
    CHECK(w.tool()->markers().size() == 9);
    CHECK(w.tool()->triadLabels().size() == 3);
    CHECK(w.stateHash() == before); // the preview commits nothing

    // 2. The top face, clicked in the view.
    const QPointF topAt = topPoint(w, topZ);
    w.clickScene(topAt);
    ++interactions;
    REQUIRE(QTest::qWaitFor([&] { return w.tool()->hasZFace(); }, kTimeoutMs));
    w.previewed();
    const api::cam_setup::Preview& p = *w.tool()->lastPreview();
    CHECK(p.z.z == Approx(1.0));
    CHECK(p.origin.z == Approx(topZ)); // corner 0 is on the top of the stock, which is the part's top

    // 3. The default stock is accepted as it is; an origin marker on it is clicked.
    const QString key = freeMarker(w, QStringLiteral("corner2"));
    const int shownBefore = w.tool()->previewsShown();
    w.click(w.shown(QStringLiteral("setupMarker.") + key));
    ++interactions;
    REQUIRE(
        QTest::qWaitFor([&] { return w.tool()->values().value(QStringLiteral("origin")).toString() == key; }, 5000));
    w.previewed();
    CHECK(w.tool()->previewsShown() > shownBefore); // the triad moved to the new origin
    w.shotsBothThemes("t044_setup_panel");

    // 4. OK.
    w.click("setupOk");
    ++interactions;
    REQUIRE(QTest::qWaitFor([&] { return !w.tool()->active() && w.undoCount() == undoBefore + 1; }, kTimeoutMs));
    CHECK(interactions == 4);
    CHECK(interactions <= 4);
    w.settle();

    const cam::Setup* s = w.setup();
    REQUIRE(s != nullptr);
    CHECK(s->name == "Setup 1");
    REQUIRE(s->partBodies.size() == 1);
    CHECK(QString::fromStdString(s->partBodies.front().value.toString()) == w.bodyId());
    CHECK(s->wcs.zFace.has_value());
    CHECK_FALSE(s->wcs.xEdge.has_value());
    cam::Wcs::OriginMode mode{};
    int corner = -1;
    REQUIRE(ui::camui::originFromKey(key, &mode, &corner));
    CHECK(s->wcs.originMode == mode);
    if (mode == cam::Wcs::OriginMode::StockCorner)
        CHECK(s->wcs.stockCornerIndex == corner);
    const auto* box = std::get_if<cam::BoxStock>(&s->stock);
    REQUIRE(box != nullptr);
    CHECK(box->xNeg == 0.0);
    CHECK(box->zPos == 0.0);
    CHECK(s->machine.value != Uuid{}); // a machine of the library, embedded with the setup
    CHECK(w.document()->state()->cam->machines.count(s->machine) == 1);
    CHECK(!s->postId.empty());
    // The user's own OK was not asked about: the confirmation is for automation.
    CHECK(w.confirms == 0);
    // The panel's preview is gone with it; the tree lists the setup.
    CHECK(w.host->scene()->preview().owner() == nullptr);
    CHECK(w.tool()->triadLines().count() == 0);
    REQUIRE(QTest::qWaitFor([&] { return w.tree()->setupCount() == 1; }, 5000));
    w.shotsBothThemes("t044_tree_one_setup");
    w.checkNoWarnings();
}

TEST_CASE("the live preview, picks, flips, editing, Esc, workspaces, undo and redo", "[cam][e2e]")
{
    RAY_REQUIRE_RHI();
    Window w;
    w.open(kPlate);
    const Element top = topFace(w);
    w.toWorkspace(ui::kCamWorkspace);
    const std::string empty = w.stateHash();

    SECTION("the preview follows the fields; a curved face is refused; Esc changes nothing")
    {
        REQUIRE(w.host->commands()->trigger(QStringLiteral("cam.tool.newSetup")));
        w.previewed();
        const Box3 first = w.tool()->lastPreview()->resolved.stockBounds;
        // An offset of 2 above the part, a plain number in the document's unit: the stock grows by it at once.
        const double unitMm = w.host->units()->system() == UnitSystem::Inch ? 25.4 : 1.0;
        w.typeInto(QStringLiteral("zPos"), QStringLiteral("2"));
        w.previewed();
        const Box3 grown = w.tool()->lastPreview()->resolved.stockBounds;
        CHECK((grown.max.z - grown.min.z) - (first.max.z - first.min.z) == Approx(2.0 * unitMm).margin(1e-9));
        // Expressions as the feature panels take them (with units, whatever the display unit).
        w.typeInto(QStringLiteral("xNeg"), QStringLiteral("1 mm + 2 mm"));
        w.previewed();
        const Box3 wider = w.tool()->lastPreview()->resolved.stockBounds;
        CHECK((wider.max.x - wider.min.x) - (first.max.x - first.min.x) == Approx(3.0).margin(1e-9));
        // A field that cannot be read says so under the field, and the last good preview stays.
        w.typeInto(QStringLiteral("yNeg"), QStringLiteral("nonsense"));
        REQUIRE(QTest::qWaitFor([&] { return w.tool()->messageField() == QStringLiteral("yNeg"); }, 10000));
        CHECK(w.find(QStringLiteral("setupFieldError.yNeg"))->isVisible());
        CHECK_FALSE(w.tool()->canCommit());
        CHECK(w.tool()->lastPreview().has_value());
        w.typeInto(QStringLiteral("yNeg"), QStringLiteral("0"));
        w.previewed();

        // A cylindrical face (a hole's wall) is refused with the reason; the Z face stays as it was.
        const auto walls =
            faces(w, [](const kernel::SurfaceInfo& s) { return s.type == kernel::SurfaceType::Cylinder; });
        REQUIRE(!walls.empty());
        w.host->selection()->applyClick(render::PickHit{w.host->scene()->itemForBody(0), kernel::SubKind::Face,
                                                        walls.front().index, walls.front().at, 0},
                                        Qt::NoModifier);
        REQUIRE(QTest::qWaitFor([&] { return w.tool()->message().contains(QStringLiteral("not flat")); }, 10000));
        CHECK(w.tool()->message().contains(QStringLiteral("cylinder")));
        CHECK_FALSE(w.tool()->hasZFace());
        CHECK(w.find(QStringLiteral("setupMessage"))->isVisible());
        // A planar one is taken (here through the selection too).
        w.host->selection()->applyClick(
            render::PickHit{w.host->scene()->itemForBody(0), kernel::SubKind::Face, top.index, top.at, 0},
            Qt::NoModifier);
        REQUIRE(QTest::qWaitFor([&] { return w.tool()->hasZFace(); }, 10000));
        CHECK(w.tool()->message().isEmpty());
        w.previewed();
        // OK and Cancel stay where they are while a preview builds.
        QQuickItem* ok = w.shown(QStringLiteral("setupOk"));
        const QPointF okAt = w.centreOf(ok);
        w.typeInto(QStringLiteral("zPos"), QStringLiteral("5"));
        CHECK(w.centreOf(ok) == okAt);
        w.previewed();
        CHECK(w.centreOf(ok) == okAt);

        // Cylinder stock: a first cylinder is sized round the part, along Z, and previewed.
        w.tool()->setValue(QStringLiteral("stock"), QStringLiteral("cylinder"));
        CHECK(!w.tool()->values().value(QStringLiteral("diameter")).toString().isEmpty());
        w.previewed();
        const Box3 cyl = w.tool()->lastPreview()->resolved.stockBounds;
        const Box3 part = w.tool()->lastPreview()->resolved.partBounds;
        CHECK(cyl.max.x - cyl.min.x == Approx(cyl.max.y - cyl.min.y)); // round across Z
        CHECK(cyl.max.x - cyl.min.x >= std::hypot(part.max.x - part.min.x, part.max.y - part.min.y) - 1e-6);
        w.tool()->setValue(QStringLiteral("stock"), QStringLiteral("box"));
        w.previewed();

        // Esc: the panel closes and the document is as it was.
        QTest::keyClick(w.window(), Qt::Key_Escape);
        REQUIRE(QTest::qWaitFor([&] { return !w.tool()->active(); }, 5000));
        CHECK(w.stateHash() == empty);
        CHECK(w.host->scene()->preview().owner() == nullptr);
        CHECK(w.tool()->triadLines().count() == 0);
        CHECK(w.confirms == 0);
    }
    SECTION("flip X, editing from the tree, undo and redo")
    {
        REQUIRE(w.host->commands()->trigger(QStringLiteral("cam.tool.newSetup")));
        w.previewed();
        const Vec3 x0 = w.tool()->lastPreview()->x;
        const Vec3 y0 = w.tool()->lastPreview()->y;
        w.click("setupCheck.flipX");
        REQUIRE(QTest::qWaitFor([&] { return w.tool()->values().value(QStringLiteral("flipX")).toBool(); }, 5000));
        w.previewed();
        const Vec3 x1 = w.tool()->lastPreview()->x;
        const Vec3 y1 = w.tool()->lastPreview()->y;
        CHECK(x1.x == -x0.x);
        CHECK(x1.y == -x0.y);
        CHECK(y1.x == -y0.x);
        CHECK(y1.y == -y0.y);
        CHECK(w.tool()->lastPreview()->z.z == Approx(1.0)); // Z stays
        const std::size_t undo0 = w.undoCount();
        w.click("setupOk");
        REQUIRE(QTest::qWaitFor([&] { return !w.tool()->active() && w.undoCount() == undo0 + 1; }, kTimeoutMs));
        w.settle();
        REQUIRE(w.setup() != nullptr);
        CHECK(w.setup()->wcs.flipX);
        const std::string added = w.stateHash();

        // Edit it from the tree: double-click its row; the panel holds what the setup has.
        REQUIRE(QTest::qWaitFor([&] { return w.tree()->count() >= 1; }, 5000));
        w.doubleClick(w.shown(QStringLiteral("setupRow.0")));
        REQUIRE(QTest::qWaitFor([&] { return w.tool()->active() && w.tool()->editing(); }, 5000));
        CHECK(w.tool()->values().value(QStringLiteral("flipX")).toBool());
        CHECK(w.find(QStringLiteral("setupOk"))->property("text").toString() == QStringLiteral("Apply"));
        w.previewed();
        CHECK(w.tool()->lastPreview()->x.x == x1.x);
        w.typeInto(QStringLiteral("name"), QStringLiteral("Top side"));
        w.typeInto(QStringLiteral("zPos"), QStringLiteral("1.5 mm"));
        w.previewed();
        w.shotsBothThemes("t044_edit_setup");
        w.click("setupOk");
        REQUIRE(QTest::qWaitFor([&] { return !w.tool()->active() && w.undoCount() == undo0 + 2; }, kTimeoutMs));
        w.settle();
        REQUIRE(w.setup() != nullptr);
        CHECK(w.setup()->name == "Top side");
        CHECK(std::get<cam::BoxStock>(w.setup()->stock).zPos == Approx(1.5));
        CHECK(w.setup()->wcs.flipX);
        REQUIRE(QTest::qWaitFor(
            [&] { return w.tree()->get(0).value(QStringLiteral("rowName")).toString() == QStringLiteral("Top side"); },
            5000));

        // Undo and redo: the document and the tree follow, one step each.
        w.undo();
        CHECK(w.stateHash() == added);
        REQUIRE(QTest::qWaitFor(
            [&] { return w.tree()->get(0).value(QStringLiteral("rowName")).toString() == QStringLiteral("Setup 1"); },
            5000));
        w.undo();
        CHECK(w.stateHash() == empty);
        REQUIRE(QTest::qWaitFor([&] { return w.tree()->setupCount() == 0; }, 5000));
        w.shown(QStringLiteral("camTreeEmpty"));
        w.undo(true);
        w.undo(true);
        REQUIRE(QTest::qWaitFor(
            [&] { return w.tree()->get(0).value(QStringLiteral("rowName")).toString() == QStringLiteral("Top side"); },
            5000));
        // An edit open while its setup is undone away closes the panel.
        REQUIRE(w.tool()->edit(QString::fromStdString(w.setup()->id.value.toString())));
        w.previewed();
        w.undo();
        w.undo();
        REQUIRE(QTest::qWaitFor([&] { return !w.tool()->active(); }, 5000));
        CHECK(w.confirms == 0);
    }
    SECTION("leaving the workspace mid-edit cancels; the CAM commands work only in CAM")
    {
        REQUIRE(w.host->commands()->trigger(QStringLiteral("cam.tool.newSetup")));
        w.previewed();
        w.typeInto(QStringLiteral("zPos"), QStringLiteral("3"));
        w.toWorkspace(0);
        REQUIRE(QTest::qWaitFor([&] { return !w.tool()->active(); }, 5000));
        CHECK(w.stateHash() == empty);
        CHECK(w.host->scene()->preview().owner() == nullptr);
        // Design: the model tree is back, and the CAM key does nothing there (the extrude key works).
        CHECK(w.find(QStringLiteral("modelTree"))->isVisible());
        CHECK(w.host->shortcuts()->commandFor(QStringLiteral("Shift+N")).isEmpty());
        CHECK(w.host->shortcuts()->commandFor(QStringLiteral("E")) == QStringLiteral("model.tool.extrude"));
        CHECK(w.host->commands()->activeContext() == ui::CommandContext::Model);
        // CAM: the CAM key opens the panel, the model's does not run there.
        w.toWorkspace(ui::kCamWorkspace);
        CHECK(w.host->commands()->activeContext() == ui::CommandContext::Cam);
        CHECK(w.host->shortcuts()->commandFor(QStringLiteral("Shift+N")) == QStringLiteral("cam.tool.newSetup"));
        CHECK(w.host->shortcuts()->commandFor(QStringLiteral("E")).isEmpty());
        CHECK(w.find(QStringLiteral("camTree"))->isVisible());
        CHECK_FALSE(w.find(QStringLiteral("modelTree"))->isVisible());
        // The palette lists the CAM commands in CAM only.
        ui::CommandCatalog& commands = *w.host->commands();
        const auto setupCommand = commands.find(QStringLiteral("cam.tool.newSetup"));
        REQUIRE(setupCommand.has_value());
        CHECK(commands.inPalette(*setupCommand));
        CHECK_FALSE(commands.inPalette(*commands.find(QStringLiteral("model.tool.extrude"))));
        w.host->context()->setWorkspace(0);
        CHECK_FALSE(commands.inPalette(*setupCommand));
        CHECK(commands.inPalette(*commands.find(QStringLiteral("model.tool.extrude"))));
        w.toWorkspace(ui::kCamWorkspace);
        // The key itself.
        w.host->viewport()->forceActiveFocus();
        QTest::keyClick(w.window(), Qt::Key_N, Qt::ShiftModifier);
        REQUIRE(QTest::qWaitFor([&] { return w.tool()->active(); }, 5000));
        w.previewed();
        w.tool()->cancel();
        // Every default key collides with no other in an overlapping context, the CAM ones included.
        for (const QString& c : w.host->shortcuts()->conflicts())
            UNSCOPED_INFO(c.toStdString());
        CHECK(w.host->shortcuts()->conflicts().isEmpty());
    }
    SECTION("closing the document while a preview is being made")
    {
        REQUIRE(w.host->commands()->trigger(QStringLiteral("cam.tool.newSetup")));
        w.typeInto(QStringLiteral("zPos"), QStringLiteral("4"));
        CHECK(w.tool()->previewBusy());
        api::OpenDocument* od = w.host->app()->active();
        REQUIRE(od != nullptr);
        REQUIRE(w.host->app()->close(*od));
        REQUIRE(QTest::qWaitFor([&] { return !w.host->session()->hasDocument(); }, kTimeoutMs));
        REQUIRE(QTest::qWaitFor([&] { return !w.tool()->active(); }, 5000));
        QTest::qWait(300); // what was on its way arrives, and is dropped
        CHECK(w.host->scene()->preview().owner() == nullptr);
        CHECK(w.tool()->triadLines().count() == 0);
        w.shown(QStringLiteral("camTreeEmpty"));
    }
    w.checkNoWarnings();
}

TEST_CASE("status badges match the engine and follow model edits, undo and redo", "[cam][e2e]")
{
    RAY_REQUIRE_RHI();
    ct::Gate::instance().reset();
    Window w(true);
    w.open(kPlate);
    addSetup(w);
    REQUIRE(w.invoke("cam.library.embed", ct::library(ct::toolJson(1))));
    // A face reference for an operation, and one that cannot resolve (its names changed).
    const Element top = topFace(w);
    const QJsonObject faceRef =
        QJsonDocument::fromJson(
            QByteArray::fromStdString(w.invokeResult(
                "model.reference", jsonText(QJsonObject{{QStringLiteral("body"), w.bodyId()},
                                                        {QStringLiteral("kind"), QStringLiteral("face")},
                                                        {QStringLiteral("index"), static_cast<int>(top.index)}}))))
            .object()
            .value(QStringLiteral("ref"))
            .toObject();
    REQUIRE(!faceRef.isEmpty());
    QJsonObject brokenRef = faceRef;
    QJsonArray names;
    for (int i = 0; i < faceRef.value(QStringLiteral("names")).toArray().size(); ++i)
        names.append(QStringLiteral("0102030405060708091011121314151617181920212223242526272829303132"));
    brokenRef.insert(QStringLiteral("names"), names);
    brokenRef.remove(QStringLiteral("neighbourNames"));
    brokenRef.remove(QStringLiteral("hint"));
    const auto geometry = [](const QJsonObject& ref) {
        return QJsonDocument(QJsonArray{ref}).toJson(QJsonDocument::Compact).toStdString();
    };
    // Operations 1..5: plain, blocking, failing, on a broken reference, suppressed.
    const auto add = [&](std::uint64_t n, const std::string& geom, const char* mode, const std::string& extra = {},
                         bool progress = false) {
        doc::Command c = ct::opAdd(n, 1, 1, geom, std::string(R"({"mode":")") + mode + "\"}", extra);
        if (progress) // the type that reports progress while it waits
            c.argsJson.replace(c.argsJson.find("\"test.op\""), 9, "\"ui.progress\"");
        REQUIRE(w.invoke(c.name, c.argsJson));
    };
    add(1, geometry(faceRef), "plain");
    add(2, "[]", "block", {}, true);
    add(3, "[]", "fail");
    add(4, geometry(brokenRef), "plain");
    add(5, "[]", "plain", R"(,"suppressed":true)");
    w.settle();
    w.toWorkspace(ui::kCamWorkspace);
    REQUIRE(QTest::qWaitFor([&] { return w.tree()->count() == 6; }, 10000)); // the setup and its five operations

    // Before anything is generated.
    w.waitStatus(1, "stale");
    w.waitStatus(2, "stale");
    w.waitStatus(3, "stale");
    w.waitStatus(4, "broken");
    w.waitStatus(5, "suppressed");
    w.waitStatus(0, "broken"); // the setup shows its worst operation
    CHECK(!w.tree()->get(4).value(QStringLiteral("message")).toString().isEmpty());
    CHECK(w.find(QStringLiteral("camRowDetails.4"))->isVisible()); // the reason under the row
    // Every badge is what the engine says.
    const auto engineSays = [&](std::uint64_t n) {
        const auto all = w.host->app()->cam()->status(w.document()->state(), w.document()->build());
        return all.at(ct::opId(n)).status;
    };
    CHECK(engineSays(4) == cam::OpStatus::ReferenceBroken);
    CHECK(engineSays(5) == cam::OpStatus::Suppressed);

    // Generate operation 1 (the command on the current row): up to date.
    w.click(w.shown(QStringLiteral("operationRow.1")));
    REQUIRE(w.tree()->currentIndex() == 1);
    REQUIRE(w.host->commands()->trigger(QStringLiteral("cam.tree.generate")));
    w.waitStatus(1, "upToDate");
    CHECK(engineSays(1) == cam::OpStatus::UpToDate);
    // Operation 3 fails, with the generator's message.
    w.click(w.shown(QStringLiteral("operationRow.3")));
    REQUIRE(w.host->commands()->trigger(QStringLiteral("cam.tree.generate")));
    w.waitStatus(3, "failed");
    CHECK(w.tree()->get(3).value(QStringLiteral("message")).toString() == QStringLiteral("Test generator failed"));
    CHECK(engineSays(3) == cam::OpStatus::Failed);
    // Operation 2 waits at its gate: generating, with its progress, until the gate opens.
    ct::Gate::instance().open = false;
    w.click(w.shown(QStringLiteral("operationRow.2")));
    REQUIRE(w.host->commands()->trigger(QStringLiteral("cam.tree.generate")));
    w.waitStatus(2, "generating");
    CHECK(engineSays(2) == cam::OpStatus::Generating);
    REQUIRE(QTest::qWaitFor([&] { return ct::Gate::instance().waiting.load() == 1; }, kTimeoutMs));
    // The generator reports its progress while it waits: the badge shows it.
    REQUIRE(QTest::qWaitFor([&] { return w.tree()->get(2).value(QStringLiteral("progress")).toDouble() > 0.3; },
                            kTimeoutMs));
    CHECK(w.tree()->get(2).value(QStringLiteral("statusText")).toString().startsWith(QStringLiteral("Generating ")));
    CHECK(w.tree()->get(2).value(QStringLiteral("statusText")).toString().endsWith(QStringLiteral("%")));
    w.shotsBothThemes("t044_badges");
    ct::Gate::instance().open = true;
    w.waitStatus(2, "upToDate");
    w.waitStatus(0, "broken");

    // A model edit in the Model workspace: a fillet on an edge of the plate makes every operation on it stale.
    w.toWorkspace(0);
    const api::viewer::Body* body = w.host->scene()->body(0);
    std::optional<std::uint32_t> edge;
    {
        JobContext ctx;
        for (std::uint32_t i = 0; i < body->shape.count(kernel::SubKind::Edge) && !edge; ++i) {
            auto info = api::viewer::describeElement(body->shape, kernel::SubId{kernel::SubKind::Edge, i}, ctx);
            REQUIRE(info);
            if (info->curve && info->curve->type == kernel::CurveType::Line &&
                std::abs(info->curve->start.z - top.at.z) < 1e-6 && std::abs(info->curve->end.z - top.at.z) < 1e-6)
                edge = i;
        }
    }
    REQUIRE(edge.has_value());
    const QJsonObject edgeRef =
        QJsonDocument::fromJson(
            QByteArray::fromStdString(w.invokeResult(
                "model.reference", jsonText(QJsonObject{{QStringLiteral("body"), w.bodyId()},
                                                        {QStringLiteral("kind"), QStringLiteral("edge")},
                                                        {QStringLiteral("index"), static_cast<int>(*edge)}}))))
            .object()
            .value(QStringLiteral("ref"))
            .toObject();
    const QJsonObject fillet{
        {QStringLiteral("id"), QString::fromStdString(Uuid::generate().toString())},
        {QStringLiteral("props"), QJsonObject{{QStringLiteral("edges"), QJsonArray{edgeRef}},
                                              {QStringLiteral("radius"), QStringLiteral("1 mm")}}}};
    const int runs = w.tree()->statusRuns();
    REQUIRE(w.invoke("feature.fillet.add", jsonText(fillet)));
    w.settle();
    w.toWorkspace(ui::kCamWorkspace);
    REQUIRE(QTest::qWaitFor([&] { return w.tree()->statusRuns() > runs; }, kTimeoutMs));
    w.waitStatus(1, "stale");
    w.waitStatus(2, "stale");
    CHECK(engineSays(1) == cam::OpStatus::Stale);
    // Undo the fillet: the toolpaths made for the model before it are found again.
    w.undo();
    w.waitStatus(1, "upToDate");
    w.waitStatus(2, "upToDate");
    w.undo(true);
    w.waitStatus(1, "stale");
    // Suppress and unsuppress from the tree (one undo step each).
    const std::size_t undoBefore = w.undoCount();
    REQUIRE(w.tree()->setSuppressed(1, true));
    REQUIRE(QTest::qWaitFor([&] { return w.undoCount() == undoBefore + 1; }, kTimeoutMs));
    w.waitStatus(1, "suppressed");
    REQUIRE(w.tree()->setSuppressed(1, false));
    w.waitStatus(1, "stale");
    // Reorder and remove from the tree.
    REQUIRE(w.tree()->moveDown(1));
    REQUIRE(QTest::qWaitFor(
        [&] {
            return w.tree()->get(2).value(QStringLiteral("rowId")).toString() == QString::fromStdString(ct::id('c', 1));
        },
        kTimeoutMs));
    REQUIRE(w.tree()->remove(2));
    REQUIRE(QTest::qWaitFor([&] { return w.tree()->count() == 5; }, kTimeoutMs));
    CHECK(w.confirms == 0); // the tree's actions are the user's
    w.checkNoWarnings();
}

TEST_CASE("the preview on a large part: latency, tilted frames and the UI thread", "[cam][stall][wallclock]")
{
    RAY_REQUIRE_RHI();
    Window w;
    w.open(kLarge);
    const api::viewer::Body* body = w.host->scene()->body(0);
    REQUIRE(body != nullptr);
    // A planar face whose normal is no model axis gives a tilted frame (the part is measured in it), and the largest
    // axis-aligned one the usual case.
    const auto planar = faces(w, [](const kernel::SurfaceInfo& s) { return s.type == kernel::SurfaceType::Plane; });
    REQUIRE(!planar.empty());
    std::optional<Element> tilted;
    {
        JobContext ctx;
        for (const Element& f : planar) {
            auto info = api::viewer::describeElement(body->shape, kernel::SubId{kernel::SubKind::Face, f.index}, ctx);
            const Vec3 n = info->surface->axis;
            const double big = std::max({std::abs(n.x), std::abs(n.y), std::abs(n.z)});
            if (big < 0.999 && big > 0.5) {
                tilted = f;
                break;
            }
        }
    }
    w.toWorkspace(ui::kCamWorkspace);

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

    // The time from a field change to the preview shown, over a few changes.
    const auto latency = [&](const char* what) {
        std::vector<double> ms;
        for (int i = 0; i < 4; ++i) {
            QElapsedTimer t;
            t.start();
            w.typeInto(QStringLiteral("zPos"), QString::number(1 + i));
            w.previewed();
            ms.push_back(static_cast<double>(t.elapsed()));
        }
        std::sort(ms.begin(), ms.end());
        WARN("setup preview latency on " << kLarge.toStdString() << ", " << what << ": median " << ms[ms.size() / 2]
                                         << " ms, worst " << ms.back() << " ms");
        return ms[ms.size() / 2];
    };
    REQUIRE(w.host->commands()->trigger(QStringLiteral("cam.tool.newSetup")));
    w.previewed();
    const double aligned = latency("axis-aligned frame");
    double tiltedMs = 0;
    if (tilted) {
        w.host->selection()->applyClick(
            render::PickHit{w.host->scene()->itemForBody(0), kernel::SubKind::Face, tilted->index, tilted->at, 0},
            Qt::NoModifier);
        REQUIRE(QTest::qWaitFor([&] { return w.tool()->hasZFace(); }, kTimeoutMs));
        w.previewed();
        tiltedMs = latency("tilted frame");
    } else {
        WARN("no tilted planar face on the large part: the tilted frame was not measured");
    }
    tick.stop();
    // The resolve alone (api::cam_setup::previewSetup, what the panel's job runs), on the document's preview of the
    // setup, for the report: the frame, the part measured in it and the stock solid.
    {
        doc::Document* d = w.document();
        REQUIRE(d->previewState() != nullptr);
        REQUIRE(d->previewBuild() != nullptr);
        std::vector<double> ms;
        for (int i = 0; i < 5; ++i) {
            JobContext ctx;
            QElapsedTimer t;
            t.start();
            auto p = api::cam_setup::previewSetup(d->previewState(), d->previewBuild(), *w.tool()->setupId(), ctx);
            ms.push_back(static_cast<double>(t.nsecsElapsed()) / 1e6);
            REQUIRE(p);
        }
        std::sort(ms.begin(), ms.end());
        WARN("previewSetup alone on the large part (" << (tilted ? "tilted" : "axis-aligned") << " frame, "
                                                      << body->shape.count(kernel::SubKind::Face) << " faces): median "
                                                      << ms[2] << " ms");
    }
    // What a tilted frame cost before T-044 (the body moved into the frame by kernel::transformed: a copy, checked)
    // against kernel::boundsIn (the move as a location), on the same body, for the report.
    {
        const double c = std::cos(0.3), s = std::sin(0.3);
        const Xform tilt{{1, 0, 0, 0, 0, c, -s, 0, 0, s, c, 0}};
        const auto median = [](std::vector<double> v) {
            std::sort(v.begin(), v.end());
            return v[v.size() / 2];
        };
        std::vector<double> copied, located;
        for (int i = 0; i < 3; ++i) {
            QElapsedTimer t;
            t.start();
            auto moved = kernel::transformed(body->shape, tilt);
            REQUIRE(moved);
            const Box3 b1 = kernel::bounds(moved->shape);
            copied.push_back(static_cast<double>(t.nsecsElapsed()) / 1e6);
            t.restart();
            auto b2 = kernel::boundsIn(body->shape, tilt);
            located.push_back(static_cast<double>(t.nsecsElapsed()) / 1e6);
            REQUIRE(b2);
            CHECK(std::abs(b1.max.z - b2->max.z) < 1e-6);
            CHECK(std::abs(b1.min.y - b2->min.y) < 1e-6);
        }
        WARN("the large part in a tilted frame: transformed + bounds " << median(copied) << " ms, boundsIn "
                                                                       << median(located) << " ms");
    }
    INFO("worst UI-thread gap " << worst << " ms");
    WARN("UI-thread watchdog while the setup preview builds on a large part: worst gap " << worst << " ms");
    CHECK(worst < ray::test::uiBudgetMs(50));
    // An interactive preview: well under a second per change on either frame.
    CHECK(aligned < ray::test::uiBudgetMs(1000));
    CHECK(tiltedMs < ray::test::uiBudgetMs(1000));
    w.tool()->cancel();
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
