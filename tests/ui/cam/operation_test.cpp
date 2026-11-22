// The operation panel of the CAM workspace (task T-045) in the desktop app, on the real Qt Quick window with the real
// application session:
//
//   - a drill operation by the UI on a part with holes: the type from the registry's list, a library tool from the
//     tool library's picker (embedded in the same step), holes picked by any of their faces (a face that is no hole and
//     a hole picked twice refused with the reason in the status line), "Use calculated" from computeFeeds, the toolpath
//     preview and its backplot in the view, OK with no confirmation, the tree up to date; undo and redo
//   - the tool picker (ToolPicker.qml): the types the cycle calls for (x-toolTypes), every tool on request, a tool
//     written into the library folder or saved there while the panel is open shows at once, the tool library manager
//     from the picker, the library tool embedded on OK as the library has it then
//   - editing: a parameter changes the preview, a field that does not matter is hidden until it does, Esc changes
//     nothing, Apply (Enter) is one undo step
//   - progress: a generation started from the panel shows its progress there and in the tree, which follows the
//     engine's notifications (it polls no more)
//   - validation: an out-of-range parameter and a tap that does not fit the hole's thread block OK with the message
//     (the drill type's own words), warnings do not, a field's reset brings its default back
//   - every registered operation type gets a working form from its schema (the test iterates the registry, with
//     test-only types and a synthetic schema of every construct, those the form cannot edit included)
//   - [scale] no label is cut short at the scale CTest runs it at (1, 1.5, 2), in both themes
//   - [wallclock] the UI thread stays within its budget while previews build
//
// Screenshots: with RAY_T045_SHOTS set to a folder, the window is saved there in both themes.
#include "../scene_graph_probe.h"
#include "../ui_budget.h"
#include "app_context.h"
#include "app_host.h"
#include "app_options.h"
#include "cam/operation_tool.h"
#include "cam/operation_view.h"
#include "cam/setup_tool.h"
#include "cam/setup_tree.h"
#include "cam/tools_model.h"
#include "engine_test_util.h" // the CAM engine tests' test-only operation type and library JSON (tests/unit/cam)
#include "files.h"
#include "palette_commands.h"
#include "scene_controller.h"
#include "selection_model.h"
#include "session.h"
#include "theme_object.h"
#include "ui_commands.h"
#include "units.h"
#include "viewport_item.h"
#include "viewport_navigation.h"

#include <ray_api/cam_operation.h>
#include <ray_api/cam_tools.h>
#include <ray_api/viewer.h>

#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
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
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace ray;
using Catch::Approx;
using Catch::Matchers::ContainsSubstring;
using ray::ui::AppHost;
using ray::ui::AppOptions;
using ray::ui::OperationToolModel;
using ray::ui::SetupTreeModel;
using ray::ui::ToolpathDisplay;
namespace ct = ray::cam::test;

namespace Catch {
template <>
struct StringMaker<QString> {
    static std::string convert(const QString& s) { return '"' + s.toStdString() + '"'; }
};
} // namespace Catch

namespace {

constexpr int kTimeoutMs = 60000;
// plate_thru_holes_1 (tools/corpus_gen): a 120 x 80 x 10 mm plate with six 6.6 mm through holes.
const QString kPlate = QStringLiteral(RAY_CORPUS_DIR "/raycad-synthetic/plate_thru_holes_1.stp");
// Library items (resources/library): the 6 mm jobber drill, the M6x1 and M8x1.25 taps, 6061-T6, a flat end mill.
const QString kDrill6 = QStringLiteral("3d2ce280-3863-4869-9ef4-b75ed93ce20e");
const QString kTapM6 = QStringLiteral("8c62b95b-6890-46f4-a990-cacd8715acf8");
const QString kTapM8 = QStringLiteral("273e25d7-979b-4f0a-b01f-2c06b758d0a0");
const std::string kAl6061 = "b72c5171-c258-44d2-91ae-ee049b636621";

// A test-only type that reports progress while it waits at the engine tests' gate (as in cam_test.cpp).
class ProgressOperation final : public cam::IOperationType {
public:
    std::string_view typeName() const override { return "ui.progress"; }
    std::string_view paramsSchemaJson() const override
    {
        return R"({"type":"object","title":"Progress test","properties":{"steps":{"type":"integer","title":"Steps",)"
               R"("description":"How many","minimum":1,"default":3}}})";
    }
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
        cam::Move feed{};
        feed.kind = cam::MoveKind::Linear;
        feed.role = cam::MoveRole::Cut;
        feed.x = toNm(10.0);
        feed.z = toNm(in.op.heights.bottom);
        feed.feedMicronPerMin = static_cast<std::int32_t>(in.op.cutting.feedCut * 1000.0);
        tp.moves = {rapid, feed};
        return tp;
    }
};

// A test-only type whose schema has every construct of the form's subset and some it cannot edit.
class SchemaOperation final : public cam::IOperationType {
public:
    std::string_view typeName() const override { return "ui.schema"; }
    std::string_view paramsSchemaJson() const override
    {
        return R"({"type":"object","title":"Every construct","x-geometry":{"kind":"face","count":"any"},
          "x-heightsUsed":["clearance","top","bottom"],"x-cuttingUsed":["rpm","feedCut","feedPlunge","coolant"],
          "x-propertyOrder":["mode","depth"],"properties":{
          "mode":{"title":"Mode","description":"How","enum":["a","b"],"x-enumTitles":["Alpha","Beta"],"default":"a"},
          "depth":{"title":"Depth","description":"How deep","type":"number","exclusiveMinimum":0,"default":2,
                   "x-unit":"length","x-relevantWhen":{"mode":["b"]}},
          "angle":{"title":"Angle","description":"Tilt","type":"number","default":45,"x-unit":"angle"},
          "passes":{"title":"Passes","description":"Count","type":"integer","minimum":1,"default":2},
          "climb":{"title":"Climb","description":"Direction","type":"boolean","default":true},
          "label":{"title":"Label","description":"Text","type":"string","default":"A"},
          "nested":{"title":"Nested","description":"An object","type":"object","default":{}},
          "either":{"title":"Either","description":"One of","oneOf":[{"type":"number"}],"default":1}},
          "allOf":[]})";
    }
    Status validate(const cam::GenerateInput&) const override { return {}; }
    Result<cam::Toolpath> generate(const cam::GenerateInput& in, JobContext&) const override
    {
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

    explicit Window(bool testTypes = false, QSize size = QSize(1440, 900))
    {
        AppOptions options;
        options.theme = QStringLiteral("dark");
        options.glass = false;
        options.size = size;
        host = std::make_unique<AppHost>(options, cache.path());
        REQUIRE(host->optionError().isEmpty());
        REQUIRE(host->load());
        host->show();
        REQUIRE(QTest::qWaitForWindowExposed(host->window()));
        REQUIRE(host->viewport() != nullptr);
        REQUIRE(host->startSession(userData.path().toStdString()));
        // Operation types go in before the first document (api.h).
        if (testTypes) {
            host->app()->operationTypes()->add(std::make_unique<ct::TestOperation>());
            host->app()->operationTypes()->add(std::make_unique<ProgressOperation>());
            host->app()->operationTypes()->add(std::make_unique<SchemaOperation>());
        }
        host->app()->setConfirmHook([this](const api::ActionInfo&, std::string_view, std::string_view) {
            ++confirms;
            return true;
        });
        host->files()->setUseNativeDialogs(false);
        host->viewport()->setAnimationsEnabled(false);
        host->scene()->setPreset(render::presetFor(render::Tier::Medium));
        ct::Gate::instance().reset();
        grab();
    }
    ~Window() { ct::Gate::instance().open = true; }

    QQuickWindow* window() const { return host->window(); }
    ui::ViewportItem* view() const { return host->viewport(); }
    template <class T>
    T* singleton(const char* name) const
    {
        auto* s = host->engine()->singletonInstance<T*>(QStringLiteral("RayCad.Ui"), QString::fromLatin1(name));
        REQUIRE(s != nullptr);
        return s;
    }
    OperationToolModel* op() const { return singleton<OperationToolModel>("CamOperation"); }
    SetupTreeModel* tree() const { return singleton<SetupTreeModel>("CamTreeModel"); }
    ToolpathDisplay* display() const { return singleton<ToolpathDisplay>("CamToolpaths"); }
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
    QPointF centreOf(QQuickItem* item) const
    {
        return item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
    }
    // Scrolls the panel so `item` is in view (the panel is taller than the window).
    void inView(QQuickItem* item) const
    {
        QQuickItem* flick = find(QStringLiteral("operationScroll"));
        if (flick == nullptr || !flick->isAncestorOf(item))
            return;
        const double contentY = flick->property("contentY").toDouble();
        const double h = flick->height();
        const double y = item->mapToItem(flick, QPointF(0, 0)).y();
        const double maxY = std::max(0.0, flick->property("contentHeight").toDouble() - h);
        double target = contentY;
        if (y < 0 || y + item->height() > h)
            target = std::clamp(contentY + y - h / 3, 0.0, maxY);
        // On whole device pixels, as the panel's own scrolling (pixelAligned).
        const double dpr = window()->effectiveDevicePixelRatio();
        target = std::round(target * dpr) / dpr;
        flick->setProperty("contentY", target);
        QCoreApplication::processEvents();
    }
    void click(QQuickItem* item) const
    {
        REQUIRE(item != nullptr);
        inView(item);
        grab();
        clickScene(centreOf(item));
    }
    void click(const QString& objectName) const { click(shown(objectName)); }
    void doubleClick(QQuickItem* item) const
    {
        REQUIRE(item != nullptr);
        grab();
        const QPointF c = centreOf(item);
        sendMouse(QEvent::MouseMove, c, Qt::NoButton, Qt::NoButton);
        QTest::mouseDClick(window(), Qt::LeftButton, Qt::NoModifier, c.toPoint());
        QCoreApplication::processEvents();
    }
    void typeInto(const QString& objectName, const QString& text) const
    {
        QQuickItem* field = shown(objectName);
        inView(field);
        field->forceActiveFocus();
        QMetaObject::invokeMethod(field, "selectAll");
        if (text.isEmpty())
            QTest::keyClick(window(), Qt::Key_Backspace);
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
    // The panel's preview and check are done; its status is `status`.
    void previewed(const char* status = "ok") const
    {
        const bool done = QTest::qWaitFor(
            [&] { return !op()->previewBusy() && op()->previewStatus() == QLatin1String(status); }, kTimeoutMs);
        INFO("operation preview: status '" << op()->previewStatus().toStdString() << "', busy " << op()->previewBusy()
                                           << ", message '" << op()->message().toStdString() << "', errors '"
                                           << op()->errors().join(QStringLiteral(" | ")).toStdString() << "'");
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
    QString bodyId(int i = 0) const
    {
        const api::viewer::Body* b = host->scene()->body(i);
        REQUIRE(b != nullptr);
        return QString::fromStdString(b->id.value.toString());
    }
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
    bool freeOfPanel(QPointF p) const
    {
        for (const char* name : {"operationPanel", "setupPanel"}) {
            QQuickItem* panel = find(QLatin1String(name));
            if (panel != nullptr && panel->isVisible()) {
                const QRectF r(panel->mapToScene(QPointF(0, 0)), QSizeF(panel->width(), panel->height()));
                if (r.adjusted(-8, -8, 8, 8).contains(p))
                    return false;
            }
        }
        const QRectF v(view()->mapToScene(QPointF(0, 0)), QSizeF(view()->width(), view()->height()));
        return v.adjusted(40, 80, -200, -40).contains(p);
    }
    QPointF sceneOf(Vec3 world) const
    {
        const auto v = ui::nav::project(view()->camera(), world, view()->width(), view()->height());
        REQUIRE(v.has_value());
        return view()->mapToScene(*v);
    }
    void shot(const QString& name) const
    {
        const QString dir = qEnvironmentVariable("RAY_T045_SHOTS");
        if (dir.isEmpty())
            return;
        QDir().mkpath(dir);
        const QPointF away(window()->width() / 2.0, window()->height() - 4.0);
        sendMouse(QEvent::MouseMove, away, Qt::NoButton, Qt::NoButton);
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

std::string jsonText(const QJsonObject& o)
{
    return QJsonDocument(o).toJson(QJsonDocument::Compact).toStdString();
}

// The JSON of a library item as the commands' "library" member takes it.
std::string libraryMaterial(const Window& w, const std::string& id)
{
    const lib::Material* m = w.host->app()->libraries()->material(MaterialId{Uuid::fromString(id)});
    REQUIRE(m != nullptr);
    return api::cam_operation::materialJson(*m);
}
std::string libraryTool(const Window& w, const QString& id)
{
    const lib::Tool* t = w.host->app()->libraries()->tool(ToolId{Uuid::fromString(id.toStdString())});
    REQUIRE(t != nullptr);
    return api::cam_operation::toolJson(*t);
}

// A setup on body 0 through the API: machine 1 of the engine tests and 6061-T6 as its stock material.
void addSetup(const Window& w, int n = 1)
{
    const std::string args = R"({"id":")" + ct::id('a', static_cast<std::uint64_t>(n)) + R"(","name":"Setup )" +
                             std::to_string(n) + R"(","partBodies":[")" + w.bodyId().toStdString() +
                             R"("],"machine":")" + ct::id('9', 1) + R"(","material":")" + kAl6061 +
                             R"(","library":{"machines":[)" + ct::machineJson(1) + "],\"materials\":[" +
                             libraryMaterial(w, kAl6061) + "]}}";
    REQUIRE(w.invoke("cam.setup.add", args));
    REQUIRE(QTest::qWaitFor([&] { return w.tree()->setupCount() >= n; }, kTimeoutMs));
}

// The status line under OK and Cancel.
QString statusLine(const Window& w)
{
    QQuickItem* line = w.find(QStringLiteral("operationPreviewState"));
    REQUIRE(line != nullptr);
    return line->isVisible() ? line->property("text").toString() : QString();
}

// Opens the panel for a new operation from the setup's row in the tree, as the user does.
void newOperation(const Window& w, int setupRow = 0)
{
    w.click(w.shown(QStringLiteral("setupRow.%1").arg(setupRow)));
    REQUIRE(QTest::qWaitFor([&] { return w.tree()->currentIndex() == setupRow; }, 5000));
    w.click(w.shown(QStringLiteral("camRowNewOperation.%1").arg(setupRow)));
    REQUIRE(QTest::qWaitFor([&] { return w.op()->active(); }, 5000));
    w.shown(QStringLiteral("operationPanel"));
}

// The list of the panel's tool picker (qml/cam/ToolPicker.qml, the tool library's).
ui::ToolListModel* pickerList(const Window& w)
{
    QQuickItem* view = w.shown(QStringLiteral("toolPickerView"));
    auto* list = qobject_cast<ui::ToolListModel*>(view->property("model").value<QObject*>());
    REQUIRE(list != nullptr);
    return list;
}

// The type names the picker lists now.
std::set<QString> pickerTypes(const Window& w)
{
    ui::ToolListModel* list = pickerList(w);
    std::set<QString> out;
    for (int i = 0; i < list->count(); ++i)
        out.insert(list->get(i).value(QStringLiteral("type")).toString());
    return out;
}

// The picker's row of a tool (the document's copy first), or -1.
int pickerRow(const Window& w, const QString& id)
{
    ui::ToolListModel* list = pickerList(w);
    const int row = list->rowOf(QStringLiteral("document"), id);
    return row >= 0 ? row : list->rowOf(QStringLiteral("library"), id);
}

// Chooses a tool in the panel's picker by searching for its name and clicking its row.
void chooseTool(const Window& w, const QString& id, const QString& search)
{
    w.typeInto(QStringLiteral("toolPickerSearch"), search);
    int row = -1;
    const bool listed = QTest::qWaitFor([&] { return (row = pickerRow(w, id)) >= 0; }, 5000);
    INFO("tool " << id.toStdString() << " after searching '" << search.toStdString() << "'");
    REQUIRE(listed);
    // The list scrolls to the row (it shows a few at a time), which must be the tool's (the list follows the search).
    QQuickItem* view = w.shown(QStringLiteral("toolPickerView"));
    QQuickItem* item = nullptr;
    const bool shown = QTest::qWaitFor(
        [&] {
            QMetaObject::invokeMethod(view, "positionViewAtIndex", Q_ARG(int, row), Q_ARG(int, 4 /* Contain */));
            item = w.find(QStringLiteral("toolPickerRow.%1").arg(row));
            return item != nullptr && item->isVisible() && item->property("itemId").toString() == id;
        },
        5000);
    REQUIRE(shown);
    w.click(item);
    REQUIRE(QTest::qWaitFor([&] { return w.op()->toolId() == id; }, 5000));
}

// Steps a list (a combo box with the keyboard focus) down until `done` holds, as the arrow key does.
void stepDown(const Window& w, const QString& objectName, const std::function<bool()>& done)
{
    QQuickItem* combo = w.shown(objectName);
    w.inView(combo);
    combo->forceActiveFocus();
    for (int i = 0; i < 12 && !done(); ++i) {
        QTest::keyClick(w.window(), Qt::Key_Down);
        QTest::qWait(30);
    }
    REQUIRE(QTest::qWaitFor(done, 5000));
}

// Picks hole `i` of the panel's holes by a face that is not its bore wall (any face of a hole picks it), as a click
// on it in the view does (the selection's own entry point for a pick).
void pickHole(const Window& w, std::size_t i, bool otherFace = true)
{
    REQUIRE(QTest::qWaitFor([&] { return w.op()->holeCount() > i; }, kTimeoutMs));
    const api::cam_operation::Hole& h = w.op()->holes()[i];
    std::uint32_t face = h.boreFace;
    for (std::uint32_t f : h.faces) {
        if (!otherFace || f != h.boreFace)
            face = f;
    }
    w.host->selection()->applyClick(
        render::PickHit{w.host->scene()->itemForBody(0), kernel::SubKind::Face, face, h.top, 0}, Qt::NoModifier);
    QCoreApplication::processEvents();
}

// The panel's text reads in full: no label cut short; headings drawn inside the panel (their ink).
QRect inkBox(const QImage& img, QQuickItem* item)
{
    const qreal dpr = img.devicePixelRatio();
    const QPointF tl = item->mapToScene(QPointF(0, 0)) * dpr;
    const QRect r(tl.toPoint(), QSize(int(item->width() * dpr), int(item->height() * dpr)));
    const QRect bounded = r.intersected(img.rect());
    if (bounded.isEmpty())
        return {};
    const QColor bg = img.pixelColor(bounded.topLeft());
    QRect box;
    for (int y = bounded.top(); y <= bounded.bottom(); ++y) {
        for (int x = bounded.left(); x <= bounded.right(); ++x) {
            const QColor c = img.pixelColor(x, y);
            const int d =
                std::abs(c.red() - bg.red()) + std::abs(c.green() - bg.green()) + std::abs(c.blue() - bg.blue());
            if (d > 90)
                box = box.united(QRect(x, y, 1, 1));
        }
    }
    return box;
}

// The footer (OK, Cancel, what blocks OK) is wholly inside the panel and the window, and the header on top, when the
// form is taller than the panel: at the top of the scroll and at its bottom.
void checkFooter(const Window& w)
{
    QQuickItem* panel = w.shown(QStringLiteral("operationPanel"));
    QQuickItem* flick = w.shown(QStringLiteral("operationScroll"));
    INFO("form " << flick->property("contentHeight").toDouble() << " px in a scroll area of " << flick->height());
    REQUIRE(flick->property("contentHeight").toDouble() > flick->height()); // taller than the window allows
    const qreal dpr = w.window()->effectiveDevicePixelRatio();
    const QRectF panelRect(panel->mapToScene(QPointF(0, 0)), QSizeF(panel->width(), panel->height()));
    const QRectF windowRect(QPointF(0, 0), QSizeF(w.window()->width(), w.window()->height()));
    for (const double at : {0.0, 1.0}) {
        const double maxY = flick->property("contentHeight").toDouble() - flick->height();
        flick->setProperty("contentY", std::round(at * maxY * dpr) / dpr);
        QCoreApplication::processEvents();
        const QImage img = w.grab();
        for (const char* name : {"operationOk", "operationCancel", "operationPanelTitle", "operationPanelSubject"}) {
            QQuickItem* item = w.shown(QLatin1String(name));
            const QRectF r(item->mapToScene(QPointF(0, 0)), QSizeF(item->width(), item->height()));
            INFO(name << " at scroll " << at << ": " << r.x() << "," << r.y() << " " << r.width() << " x " << r.height()
                      << " in panel " << panelRect.y() << ".." << panelRect.bottom());
            CHECK(panelRect.contains(r));
            CHECK(windowRect.contains(r));
            // Drawn: the text's ink inside the item.
            const QRect ink = inkBox(img, item);
            CHECK_FALSE(ink.isEmpty());
            CHECK(QRectF(r.x() * dpr - 1, r.y() * dpr - 1, r.width() * dpr + 2, r.height() * dpr + 2)
                      .contains(QRectF(ink)));
        }
        // Neither header nor footer scrolls.
        CHECK_FALSE(flick->isAncestorOf(w.find(QStringLiteral("operationOk"))));
        CHECK_FALSE(flick->isAncestorOf(w.find(QStringLiteral("operationPanelSubject"))));
    }
    flick->setProperty("contentY", 0.0);
}

void checkPanelLayout(const Window& w)
{
    QQuickItem* panel = w.shown(QStringLiteral("operationPanel"));
    std::vector<QQuickItem*> items{panel};
    for (std::size_t i = 0; i < items.size(); ++i) {
        for (QQuickItem* c : items[i]->childItems())
            items.push_back(c);
    }
    int texts = 0;
    // The tool picker's rows are the tool library's (ToolPicker.qml): a long name elides there, in full in the row's
    // accessible name; the panel's own labels never do.
    QQuickItem* picker = w.find(QStringLiteral("operationToolPicker"));
    REQUIRE(picker != nullptr);
    for (QQuickItem* item : items) {
        const QVariant truncated = item->property("truncated");
        if (!truncated.isValid() || !item->isVisible() || item->property("text").toString().isEmpty() ||
            item->objectName() == QStringLiteral("placeholder") || item->objectName() == QStringLiteral("label") ||
            picker->isAncestorOf(item))
            continue;
        ++texts;
        INFO("label '" << item->property("text").toString().toStdString() << "' (" << item->objectName().toStdString()
                       << ")");
        CHECK_FALSE(truncated.toBool());
    }
    CHECK(texts > 20);
    // The section headings and the status line are drawn, whole, inside the panel.
    const QImage img = w.grab();
    const qreal dpr = img.devicePixelRatio();
    const QRectF panelRect(panel->mapToScene(QPointF(0, 0)) * dpr, QSizeF(panel->width(), panel->height()) * dpr);
    for (const char* name : {"operationSection.tool", "operationSection.parameters", "operationPanelTitle"}) {
        QQuickItem* heading = w.find(QLatin1String(name));
        REQUIRE(heading != nullptr);
        w.inView(heading);
        const QImage now = w.grab();
        const QRect ink = inkBox(now, heading);
        INFO(name << ": ink " << ink.width() << " x " << ink.height());
        REQUIRE(!ink.isEmpty());
        CHECK(panelRect.contains(QRectF(ink)));
        // Not cut at the right: the ink ends before the item's own end, with the glyphs' height.
        CHECK(ink.right() < static_cast<int>(heading->mapToScene(QPointF(heading->width(), 0)).x() * dpr) + 1);
        CHECK(ink.height() >= static_cast<int>(6 * dpr));
    }
    // Clear of the view cube's axis triad in the bottom left corner.
    QQuickItem* triad = w.find(QStringLiteral("axisTriad"));
    REQUIRE(triad != nullptr);
    CHECK(panel->mapToScene(QPointF(0, panel->height())).y() <= triad->mapToScene(QPointF(0, 0)).y());
}

// The holes' bore faces of the plate as references (model.reference), for operations added through the API.
QJsonArray holeRefs(const Window& w, std::size_t count)
{
    JobContext ctx;
    auto holes = api::cam_operation::holesOf(w.document()->state(), w.document()->build(),
                                             SetupId{Uuid::fromString(ct::id('a', 1))}, ctx);
    REQUIRE(holes);
    REQUIRE(holes->size() >= count);
    QJsonArray refs;
    for (std::size_t i = 0; i < count; ++i) {
        const QJsonObject args{{QStringLiteral("body"), QString::fromStdString((*holes)[i].body.value.toString())},
                               {QStringLiteral("kind"), QStringLiteral("face")},
                               {QStringLiteral("index"), static_cast<int>((*holes)[i].boreFace)}};
        const QJsonObject r =
            QJsonDocument::fromJson(QByteArray::fromStdString(w.invokeResult("model.reference", jsonText(args))))
                .object();
        refs.push_back(r.value(QStringLiteral("ref")));
    }
    return refs;
}

// A drill operation c1 on the first `holes` holes through the API, with the 6 mm drill from the library.
void addDrill(const Window& w, std::size_t holes, const QJsonObject& params = {})
{
    QJsonObject args{{QStringLiteral("id"), QString::fromStdString(ct::id('c', 1))},
                     {QStringLiteral("setup"), QString::fromStdString(ct::id('a', 1))},
                     {QStringLiteral("type"), QStringLiteral("drill")},
                     {QStringLiteral("name"), QStringLiteral("Drill 1")},
                     {QStringLiteral("tool"), QJsonObject{{QStringLiteral("tool"), kDrill6}}},
                     {QStringLiteral("cutting"), QJsonObject{{QStringLiteral("rpm"), 1500},
                                                             {QStringLiteral("feedCut"), 150},
                                                             {QStringLiteral("feedPlunge"), 150},
                                                             {QStringLiteral("coolant"), QStringLiteral("flood")}}},
                     {QStringLiteral("heights"), QJsonObject{{QStringLiteral("clearance"), 15},
                                                             {QStringLiteral("retract"), 5},
                                                             {QStringLiteral("feedPlane"), 5},
                                                             {QStringLiteral("top"), 0},
                                                             {QStringLiteral("bottom"), 0}}},
                     {QStringLiteral("geometry"), holeRefs(w, holes)},
                     {QStringLiteral("params"), params}};
    args.insert(
        QStringLiteral("library"),
        QJsonObject{
            {QStringLiteral("tools"),
             QJsonArray{QJsonDocument::fromJson(QByteArray::fromStdString(libraryTool(w, kDrill6))).object()}}});
    REQUIRE(w.invoke("cam.operation.add", jsonText(args)));
}

int cycleMoves(const cam::Toolpath& tp)
{
    return static_cast<int>(std::count_if(tp.moves.begin(), tp.moves.end(), [](const cam::Move& m) {
        return m.kind >= cam::MoveKind::CycleDrill && m.kind <= cam::MoveKind::CycleReam;
    }));
}

// A point of the plate's top surface at a window point the panel does not cover.
std::optional<QPointF> topPoint(const Window& w)
{
    const Box3 b = kernel::bounds(w.host->scene()->body(0)->shape);
    for (const double fx : {0.92, 0.85, 0.75, 0.65}) {
        const Vec3 p{b.min.x + fx * (b.max.x - b.min.x), (b.min.y + b.max.y) / 2, b.max.z};
        const QPointF s = w.sceneOf(p);
        if (w.freeOfPanel(s))
            return s;
    }
    return std::nullopt;
}

} // namespace

TEST_CASE("a drill operation by the UI: holes, a library tool, calculated feeds, the preview, OK", "[cam][e2e][t045]")
{
    RAY_REQUIRE_RHI();
    Window w;
    w.open(kPlate);
    w.toWorkspace(ui::kCamWorkspace);
    addSetup(w);
    const std::string before = w.stateHash();
    const std::size_t undoBefore = w.undoCount();

    // A new operation from the setup's row: the type is the registry's (drill, the only built-in type).
    newOperation(w);
    CHECK(w.op()->type() == QStringLiteral("drill"));
    CHECK(w.op()->typeTitle() == QStringLiteral("Drilling"));
    CHECK(w.shown(QStringLiteral("operationCombo.type"))->property("displayText").toString() ==
          QStringLiteral("Drilling"));
    CHECK(w.op()->geometryKind() == QStringLiteral("hole"));
    // Picking holes needs faces (any face of a hole picks it): the selection takes faces only while picking.
    CHECK(w.op()->picking());
    CHECK(w.host->selection()->filterMask() == static_cast<std::uint8_t>(render::PickFilter::Faces));
    // The header names what is edited, whatever the scroll.
    CHECK(w.shown(QStringLiteral("operationPanelSubject"))->property("text").toString() ==
          QStringLiteral("Drilling 1 · Drilling"));
    // Lengths at rest in the app's display format (Units), in full while edited; the stored value exact.
    {
        auto* units = w.singleton<ui::UnitsObject>("Units");
        QQuickItem* clearance = w.shown(QStringLiteral("operationInput.clearance"));
        CHECK(clearance->property("text").toString() == units->formatLength(15.24));
        QQuickItem* breakthrough = w.shown(QStringLiteral("schemaInput.breakthrough"));
        CHECK(breakthrough->property("text").toString() == units->formatLength(1.0));
        w.inView(breakthrough);
        breakthrough->forceActiveFocus();
        QCoreApplication::processEvents();
        CHECK(breakthrough->property("text").toString() ==
              ui::camui::numberText(1.0, ui::camui::Unit::Length, units->unitLabel()));
        clearance->forceActiveFocus();
        QCoreApplication::processEvents();
        CHECK(breakthrough->property("text").toString() == units->formatLength(1.0));
        CHECK(clearance->property("text").toString() ==
              ui::camui::numberText(15.24, ui::camui::Unit::Length, units->unitLabel()));
        w.window()->contentItem()->forceActiveFocus();
        QCoreApplication::processEvents();
    }
    // The heights the type reads, and no others.
    CHECK(w.find(QStringLiteral("operationField.clearance"))->isVisible());
    CHECK(w.find(QStringLiteral("operationField.feedPlane")) == nullptr);
    // The status line names the next step.
    w.previewed("incomplete");
    CHECK(statusLine(w) == QStringLiteral("Choose a tool"));
    CHECK_FALSE(w.shown(QStringLiteral("operationOk"))->isEnabled());

    // The tool: the library's picker offers drills for the drill cycle; search, click.
    REQUIRE(QTest::qWaitFor([&] { return pickerList(w)->count() > 0; }, 10000));
    CHECK(pickerTypes(w) == std::set<QString>{QStringLiteral("drill")});
    chooseTool(w, kDrill6, QStringLiteral("6 mm HSS"));
    CHECK(w.shown(QStringLiteral("operationToolText"))->property("text").toString() ==
          QStringLiteral("6 mm HSS-Co jobber drill"));
    w.previewed("incomplete");
    CHECK(statusLine(w) == QStringLiteral("Click any face of each hole to machine."));

    // A click on the top face, which is no hole: refused, with the reason.
    REQUIRE(QTest::qWaitFor([&] { return w.op()->holeCount() == 6; }, kTimeoutMs));
    if (const auto p = topPoint(w)) {
        w.clickScene(*p);
        REQUIRE(QTest::qWaitFor([&] { return statusLine(w).contains(QLatin1String("not part of a hole")); }, 10000));
        CHECK(w.op()->picks().isEmpty());
    } else {
        WARN("no point of the top face clear of the panel: the refused click was not made");
    }
    // Three holes, each by a face other than its bore wall; the same hole again is refused.
    for (std::size_t i = 0; i < 3; ++i) {
        pickHole(w, i);
        REQUIRE(QTest::qWaitFor([&] { return w.op()->picks().size() == static_cast<int>(i + 1); }, kTimeoutMs));
    }
    w.previewed("incomplete");
    CHECK(statusLine(w) == QStringLiteral("Enter the spindle speed and feed, or use the calculated ones"));
    pickHole(w, 1, false);
    REQUIRE(QTest::qWaitFor([&] { return statusLine(w) == QStringLiteral("That hole is picked already"); }, 10000));
    CHECK(w.op()->picks().size() == 3);
    CHECK(w.shown(QStringLiteral("operationPick.0"))->isVisible());
    CHECK_THAT(w.op()->picks()[0].toMap().value(QStringLiteral("text")).toString().toStdString(),
               ContainsSubstring("through at"));

    // Feeds: calculated for the drill in the setup's 6061 on its machine; typed values would be the user's.
    CHECK(w.op()->materialSource() == QStringLiteral("setup"));
    // The library's name as it is ("6061-T6"), in the list and drawn: the hyphen is there.
    {
        QQuickItem* material = w.shown(QStringLiteral("operationCombo.material"));
        CHECK(material->property("displayText").toString() == QStringLiteral("6061-T6"));
        QQuickItem* label = Window::findItem(material, QStringLiteral("label"));
        REQUIRE(label != nullptr);
        CHECK(label->property("text").toString() == QStringLiteral("6061-T6"));
    }
    REQUIRE(w.op()->calculated().value(QStringLiteral("available")).toBool());
    CHECK(w.op()->feedsState() == QStringLiteral("none"));
    w.click(QStringLiteral("operationUseCalculated"));
    REQUIRE(QTest::qWaitFor([&] { return w.op()->feedsState() == QStringLiteral("calculated"); }, 5000));
    const QString rpmText = w.op()->values().value(QStringLiteral("rpm")).toString();
    CHECK_FALSE(rpmText.isEmpty());
    CHECK(w.shown(QStringLiteral("operationCalculatedRpm"))->isVisible());
    CHECK(w.shown(QStringLiteral("operationCalculatedLimit"))->property("text").toString().size() > 0);

    // The preview: generated on a worker, drawn in the view; nothing is committed.
    w.previewed("ok");
    REQUIRE(w.op()->previewToolpath() != nullptr);
    CHECK(cycleMoves(*w.op()->previewToolpath()) == 3);
    REQUIRE(w.op()->previewBackplot() != nullptr);
    CHECK(w.op()->previewBackplot()->lineVertices.size() > 3);
    REQUIRE(QTest::qWaitFor(
        [&] {
            const auto shown = w.display()->shown();
            return std::any_of(shown.begin(), shown.end(), [](const ToolpathDisplay::Shown& s) { return s.preview; });
        },
        10000));
    CHECK(w.stateHash() == before);
    CHECK(w.op()->errors().isEmpty());
    REQUIRE(QTest::qWaitFor([&] { return statusLine(w) == QStringLiteral("Ready: OK adds the operation"); }, 10000));
    CHECK(w.shown(QStringLiteral("operationOk"))->isEnabled());
    checkPanelLayout(w);
    w.shotsBothThemes(QStringLiteral("t045_new_drill"));

    // OK: one undo step, the user's own click not asked about; the tree up to date at once (the preview made it).
    const QString id = w.op()->operationId();
    w.click(QStringLiteral("operationOk"));
    REQUIRE(QTest::qWaitFor([&] { return !w.op()->active() && w.undoCount() == undoBefore + 1; }, kTimeoutMs));
    CHECK(w.confirms == 0);
    const doc::DocStatePtr state = w.document()->state();
    REQUIRE(state->cam);
    REQUIRE(state->cam->operations.size() == 1);
    const cam::Operation& op = state->cam->operations.begin()->second;
    CHECK(op.type == "drill");
    CHECK(op.name == "Drilling 1");
    CHECK(op.geometry.size() == 3);
    CHECK(QString::fromStdString(op.tool.tool.value.toString()) == kDrill6);
    CHECK(state->cam->tools.count(op.tool.tool) == 1); // embedded on first use
    REQUIRE(op.calculated.has_value());
    CHECK(op.calculated->spindleRpm == op.cutting.spindleRpm);
    CHECK(op.calculated->feedCut == op.cutting.feedCut);
    CHECK(op.calculated->material == MaterialId{Uuid::fromString(kAl6061)});
    CHECK(op.paramsJson.find("\"cycle\":\"drill\"") != std::string::npos); // the defaults are stored
    CHECK(op.paramsJson.find("\"breakthrough\":1,") != std::string::npos); // exact, though shown rounded
    CHECK(op.heights.clearance == 15.24);
    const int row = w.tree()->rowOf(id);
    REQUIRE(row >= 0);
    w.waitStatus(row, "upToDate");
    CHECK(w.tree()->currentIndex() == row); // the new operation is selected
    REQUIRE(QTest::qWaitFor(
        [&] {
            const auto shown = w.display()->shown();
            return shown.size() == 1 && !shown[0].preview && !shown[0].stale && shown[0].toolpath != nullptr;
        },
        kTimeoutMs));
    w.shotsBothThemes(QStringLiteral("t045_tree_up_to_date"));

    // Undo and redo.
    w.undo();
    CHECK(w.document()->state()->cam->operations.empty());
    REQUIRE(QTest::qWaitFor([&] { return w.tree()->rowOf(id) < 0; }, 10000));
    w.undo(true);
    REQUIRE(QTest::qWaitFor([&] { return w.tree()->rowOf(id) >= 0; }, 10000));
    w.waitStatus(w.tree()->rowOf(id), "upToDate");
    w.checkNoWarnings();
}

TEST_CASE("the panel's tool picker: the types by cycle, the library as it changes, the tool library one click away",
          "[cam][e2e][t045][picker]")
{
    RAY_REQUIRE_RHI();
    Window w;
    w.open(kPlate);
    w.toWorkspace(ui::kCamWorkspace);
    addSetup(w);
    lib::LibraryStack* stack = w.host->app()->libraries();
    const auto roots = stack->roots();
    std::optional<std::size_t> mine; // My library: the last writable root
    for (std::size_t i = 0; i < roots.size(); ++i) {
        if (roots[i].writable)
            mine = i;
    }
    REQUIRE(mine.has_value());
    const lib::Tool* drill6 = stack->tool(ToolId{Uuid::fromString(kDrill6.toStdString())});
    REQUIRE(drill6 != nullptr);
    // A drill written into My library's folder while no panel is open: opening the panel reads the folders again.
    lib::Tool fresh = *drill6;
    fresh.id = ToolId{Uuid::generate()};
    fresh.name = "Picker test drill";
    fresh.diameter = 6.1;
    const QString freshId = QString::fromStdString(fresh.id.value.toString());
    {
        const QString dir = QString::fromStdString(roots[*mine].dir) + QStringLiteral("/tools");
        REQUIRE(QDir().mkpath(dir));
        QFile f(dir + QStringLiteral("/") + freshId + QStringLiteral(".tool.json"));
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(QByteArray::fromStdString(api::cam_tools::toolJson(fresh)));
    }
    newOperation(w);
    w.typeInto(QStringLiteral("toolPickerSearch"), QStringLiteral("picker test"));
    REQUIRE(QTest::qWaitFor([&] { return pickerRow(w, freshId) >= 0; }, 10000));
    w.typeInto(QStringLiteral("toolPickerSearch"), QString());
    REQUIRE(QTest::qWaitFor([&] { return pickerList(w)->count() > 1; }, 5000));

    // The types follow the cycle (x-toolTypes of the drill schema); "Show every tool" lifts the filter.
    CHECK(pickerTypes(w) == std::set<QString>{QStringLiteral("drill")});
    stepDown(w, QStringLiteral("schemaCombo.cycle"), // spot, [drill], peck, chipBreak, tap
             [&] { return w.op()->form()->valueOf(QStringLiteral("cycle")).toString() == QStringLiteral("tap"); });
    REQUIRE(QTest::qWaitFor([&] { return pickerTypes(w) == std::set<QString>{QStringLiteral("tap")}; }, 5000));
    w.op()->form()->setValue(QStringLiteral("cycle"), QStringLiteral("spot"));
    REQUIRE(QTest::qWaitFor([&] { return pickerTypes(w).count(QStringLiteral("spotDrill")) == 1; }, 5000));
    for (const QString& t : pickerTypes(w)) {
        INFO("type " << t.toStdString());
        CHECK((t == QStringLiteral("spotDrill") || t == QStringLiteral("drill") || t == QStringLiteral("chamferMill")));
    }
    w.click(QStringLiteral("operationShowAllTools"));
    REQUIRE(QTest::qWaitFor([&] { return w.op()->showAllTools(); }, 5000));
    REQUIRE(QTest::qWaitFor([&] { return pickerList(w)->count() == pickerList(w)->total(); }, 5000));
    CHECK(pickerTypes(w).count(QStringLiteral("flatEndMill")) == 1);
    w.click(QStringLiteral("operationShowAllTools"));
    w.op()->form()->setValue(QStringLiteral("cycle"), QStringLiteral("drill"));
    REQUIRE(QTest::qWaitFor([&] { return pickerTypes(w) == std::set<QString>{QStringLiteral("drill")}; }, 5000));

    // The new drill, chosen from the picker: the panel reads it from the library (its name, its feeds).
    chooseTool(w, freshId, QStringLiteral("picker test"));
    CHECK(w.shown(QStringLiteral("operationToolText"))->property("text").toString() ==
          QStringLiteral("Picker test drill"));
    CHECK(w.find(QStringLiteral("toolPickerRow.0"))->property("itemId").toString() == freshId);
    REQUIRE(QTest::qWaitFor([&] { return w.op()->calculated().value(QStringLiteral("available")).toBool(); }, 5000));
    // Changed in the library while the panel is open (a save in the manager, another program's through the watch):
    // the picker and the panel follow, through the stack's listeners.
    lib::Tool renamed = fresh;
    renamed.name = "Picker test drill, reground";
    REQUIRE(stack->library(*mine)->save(renamed));
    stack->remerge();
    REQUIRE(QTest::qWaitFor(
        [&] {
            return w.shown(QStringLiteral("operationToolText"))->property("text").toString() ==
                   QStringLiteral("Picker test drill, reground");
        },
        5000));
    REQUIRE(QTest::qWaitFor(
        [&] {
            const int row = pickerRow(w, freshId);
            return row >= 0 && pickerList(w)->get(row).value(QStringLiteral("name")).toString() ==
                                   QStringLiteral("Picker test drill, reground");
        },
        5000));
    lib::Tool another = fresh;
    another.id = ToolId{Uuid::generate()};
    another.name = "Picker test drill two";
    REQUIRE(stack->library(*mine)->save(another));
    stack->remerge();
    REQUIRE(
        QTest::qWaitFor([&] { return pickerRow(w, QString::fromStdString(another.id.value.toString())) >= 0; }, 5000));

    // The tool library one click away, over the panel; closing it leaves the panel as it was.
    auto* tools = w.singleton<ui::ToolLibraryModel>("CamTools");
    w.click(QStringLiteral("toolPickerOpenLibrary"));
    REQUIRE(QTest::qWaitFor([&] { return tools->shown(); }, 5000));
    w.shown(QStringLiteral("toolLibraryClose"));
    CHECK(w.op()->active());
    w.shotsBothThemes(QStringLiteral("t045_picker_library"));
    tools->hide();
    REQUIRE(QTest::qWaitFor([&] { return !tools->shown(); }, 5000));
    CHECK(w.op()->active());
    CHECK(w.op()->toolId() == freshId);

    // OK: the library tool is embedded in the same undo step, as the library has it now.
    pickHole(w, 0);
    REQUIRE(QTest::qWaitFor([&] { return w.op()->picks().size() == 1; }, kTimeoutMs));
    w.click(QStringLiteral("operationUseCalculated"));
    w.previewed("ok");
    const std::size_t undoBefore = w.undoCount();
    w.click(QStringLiteral("operationOk"));
    REQUIRE(QTest::qWaitFor([&] { return !w.op()->active() && w.undoCount() == undoBefore + 1; }, kTimeoutMs));
    const doc::DocStatePtr state = w.document()->state();
    REQUIRE(state->cam->tools.count(fresh.id) == 1);
    CHECK(state->cam->tools.at(fresh.id).item.name == "Picker test drill, reground");
    CHECK(state->cam->operations.begin()->second.tool.tool == fresh.id);
    w.checkNoWarnings();
}

TEST_CASE("editing an operation: the preview follows a parameter, relevance, Esc, Apply", "[cam][e2e][t045]")
{
    RAY_REQUIRE_RHI();
    Window w;
    w.open(kPlate);
    w.toWorkspace(ui::kCamWorkspace);
    addSetup(w);
    addDrill(w, 2);
    const QString id = QString::fromStdString(ct::id('c', 1));
    REQUIRE(QTest::qWaitFor([&] { return w.tree()->rowOf(id) >= 0; }, 10000));
    const int row = w.tree()->rowOf(id);
    // Generated from the tree: its badge goes up to date.
    REQUIRE(w.tree()->generate(row));
    w.waitStatus(row, "upToDate");
    const std::string before = w.stateHash();
    const std::size_t undoBefore = w.undoCount();

    // Double-click: the panel edits it, with its values.
    w.doubleClick(w.shown(QStringLiteral("operationRow.%1").arg(row)));
    REQUIRE(QTest::qWaitFor([&] { return w.op()->active() && w.op()->editing(); }, 5000));
    CHECK(w.op()->title() == QStringLiteral("Edit Drill 1"));
    CHECK(w.find(QStringLiteral("operationChoice.type"))->isVisible() == false); // the type stays
    REQUIRE(QTest::qWaitFor([&] { return w.op()->picks().size() == 2; }, kTimeoutMs));
    w.previewed("ok");
    // Editing an operation that has its holes: not picking, the user's own filter; the pick button picks faces.
    CHECK_FALSE(w.op()->picking());
    const std::uint8_t userFilter = w.host->selection()->filterMask();
    w.click(QStringLiteral("operationPickButton"));
    REQUIRE(QTest::qWaitFor([&] { return w.op()->picking(); }, 5000));
    CHECK(w.host->selection()->filterMask() == static_cast<std::uint8_t>(render::PickFilter::Faces));
    w.click(QStringLiteral("operationPickButton"));
    REQUIRE(QTest::qWaitFor([&] { return !w.op()->picking(); }, 5000));
    CHECK(w.host->selection()->filterMask() == userFilter);
    CHECK(w.shown(QStringLiteral("operationPanelSubject"))->property("text").toString() ==
          QStringLiteral("Drill 1 · Drilling"));
    REQUIRE(w.op()->previewToolpath() != nullptr);
    const Nm z0 = w.op()->previewToolpath()->moves.at(1).z;
    CHECK(statusLine(w) == QStringLiteral("Ready: Apply changes the operation"));
    CHECK(w.stateHash() == before);

    // A parameter: 2 mm more breakthrough takes the tip 2 mm deeper, in the preview only.
    const int shown = w.op()->previewsShown();
    w.typeInto(QStringLiteral("schemaInput.breakthrough"), QStringLiteral("3 mm"));
    REQUIRE(QTest::qWaitFor([&] { return w.op()->previewsShown() > shown; }, kTimeoutMs));
    w.previewed("ok");
    CHECK(w.op()->previewToolpath()->moves.at(1).z == z0 - 2'000'000);
    CHECK(w.shown(QStringLiteral("schemaReset.breakthrough"))->isEnabled()); // not the default any more
    CHECK(w.stateHash() == before);
    // A field that does not matter yet is hidden: the depth, until the depth is given as a value.
    CHECK_FALSE(w.find(QStringLiteral("schemaRow.depth"))->isVisible());
    stepDown(w, QStringLiteral("schemaCombo.depthFrom"), [&] {
        return w.op()->form()->valueOf(QStringLiteral("depthFrom")).toString() == QStringLiteral("value");
    });
    REQUIRE(QTest::qWaitFor([&] { return w.find(QStringLiteral("schemaRow.depth"))->isVisible(); }, 5000));
    CHECK(w.op()->form()->valueOf(QStringLiteral("depthFrom")).toString() == QStringLiteral("value"));
    w.shotsBothThemes(QStringLiteral("t045_edit_drill"));

    // Esc: the panel closes and the document is as it was. (While a job of the session runs, Esc cancels that job
    // first, as everywhere in the shell: wait for the preview of the last change.)
    w.previewed("ok");
    REQUIRE(QTest::qWaitFor([&] { return !w.host->session()->property("jobActive").toBool(); }, kTimeoutMs));
    // From the panel's name field (the screenshots above moved the pointer and switched the theme).
    QQuickItem* name = w.shown(QStringLiteral("operationInput.name"));
    w.inView(name);
    name->forceActiveFocus();
    QCoreApplication::processEvents();
    QTest::keyClick(w.window(), Qt::Key_Escape);
    REQUIRE(QTest::qWaitFor([&] { return !w.op()->active(); }, 5000));
    CHECK(w.stateHash() == before);
    CHECK(w.undoCount() == undoBefore);

    // Again, and Apply with Enter from a field: one undo step.
    REQUIRE(w.op()->edit(id));
    w.previewed("ok");
    w.typeInto(QStringLiteral("schemaInput.breakthrough"), QStringLiteral("2 mm"));
    w.previewed("ok");
    QTest::keyClick(w.window(), Qt::Key_Return);
    REQUIRE(QTest::qWaitFor([&] { return !w.op()->active() && w.undoCount() == undoBefore + 1; }, kTimeoutMs));
    const cam::Operation& op =
        w.document()->state()->cam->operations.at(OperationId{Uuid::fromString(id.toStdString())});
    CHECK(op.paramsJson.find("\"breakthrough\":2") != std::string::npos);
    CHECK_FALSE(op.calculated.has_value()); // typed values: no record
    w.waitStatus(w.tree()->rowOf(id), "upToDate");
    w.checkNoWarnings();
}

TEST_CASE("generation shows its progress in the panel and in the tree, which follows the engine", "[cam][e2e][t045]")
{
    RAY_REQUIRE_RHI();
    Window w(true);
    w.open(kPlate);
    w.toWorkspace(ui::kCamWorkspace);
    addSetup(w);
    ct::Gate::instance().open = false;
    newOperation(w);
    // The type from the list: the test type that reports progress while it waits.
    stepDown(w, QStringLiteral("operationCombo.type"), [&] { return w.op()->type() == QStringLiteral("ui.progress"); });
    CHECK(w.find(QStringLiteral("operationSection.geometry"))->isVisible() == false); // it takes no geometry
    CHECK(w.shown(QStringLiteral("schemaInput.steps"))->property("text").toString() == QStringLiteral("3"));
    // Any tool suits it; typed speed and feed (the user's own).
    w.shown(QStringLiteral("toolPickerRow.0"));
    w.click(QStringLiteral("toolPickerRow.0"));
    REQUIRE(QTest::qWaitFor([&] { return !w.op()->toolId().isEmpty(); }, 5000));
    w.typeInto(QStringLiteral("operationInput.rpm"), QStringLiteral("2000"));
    w.typeInto(QStringLiteral("operationInput.feedCut"), QStringLiteral("8"));
    CHECK(w.op()->feedsState() == QStringLiteral("user"));

    // The preview generation waits at the gate: its progress shows in the panel.
    REQUIRE(QTest::qWaitFor([&] { return w.op()->progress() > 0.2; }, kTimeoutMs));
    CHECK(w.shown(QStringLiteral("operationProgress"))->isVisible());
    CHECK(statusLine(w).startsWith(QStringLiteral("Generating the preview…")));
    w.shotsBothThemes(QStringLiteral("t045_preview_progress"));
    // Checked already (its check does not wait for the generator): OK works.
    REQUIRE(QTest::qWaitFor([&] { return w.op()->canCommit(); }, kTimeoutMs));
    const QString id = w.op()->operationId();
    const int notices = w.tree()->notices();
    w.click(QStringLiteral("operationOk"));
    REQUIRE(QTest::qWaitFor([&] { return !w.op()->active(); }, kTimeoutMs));
    // The committed operation is generated (the preview was cancelled with the panel): the tree shows it, with its
    // progress, from the engine's notices.
    REQUIRE(QTest::qWaitFor([&] { return w.tree()->rowOf(id) >= 0; }, 10000));
    const int row = w.tree()->rowOf(id);
    w.waitStatus(row, "generating");
    REQUIRE(QTest::qWaitFor(
        [&] { return w.tree()->get(row).value(QStringLiteral("statusText")).toString().contains(QLatin1Char('%')); },
        kTimeoutMs));
    CHECK(w.tree()->notices() > notices);
    w.shotsBothThemes(QStringLiteral("t045_tree_generating"));
    ct::Gate::instance().open = true;
    w.waitStatus(row, "upToDate");
    w.checkNoWarnings();
}

TEST_CASE("validation: a parameter out of range and a tap that does not fit the thread block OK", "[cam][e2e][t045]")
{
    RAY_REQUIRE_RHI();
    Window w;
    // A 60 x 40 x 15 mm block with an M6x1 tapped hole 12 mm deep at (15, 20), built by the feature actions.
    REQUIRE(w.host->app()->newDocument(""));
    REQUIRE(QTest::qWaitFor([&] { return w.host->session()->hasDocument(); }, kTimeoutMs));
    const auto f = [](int n) {
        return ct::id('f', static_cast<std::uint64_t>(n));
    };
    const auto e = [](int n) {
        return ct::id('e', static_cast<std::uint64_t>(n));
    };
    const auto pt = [&](int n, double x, double y) {
        return R"({"id":")" + e(n) + R"(","type":"point","x":)" + std::to_string(x) + R"(,"y":)" + std::to_string(y) +
               "}";
    };
    const auto line = [&](int n, int a, int b) {
        return R"({"id":")" + e(n) + R"(","type":"line","a":")" + e(a) + R"(","b":")" + e(b) + R"("})";
    };
    const std::string rect = pt(1, 0, 0) + "," + pt(2, 60, 0) + "," + pt(3, 60, 40) + "," + pt(4, 0, 40) + "," +
                             line(5, 1, 2) + "," + line(6, 2, 3) + "," + line(7, 3, 4) + "," + line(8, 4, 1);
    REQUIRE(w.invoke("feature.sketch.add", R"({"id":")" + f(2) +
                                               R"(","props":{"plane":{"principal":"xy"},"sketch":{"schema":1,)"
                                               R"("entities":[)" +
                                               rect + R"(],"constraints":[]}}})"));
    REQUIRE(w.invoke("feature.extrude.add", R"({"id":")" + f(3) + R"(","props":{"sketch":")" + f(2) +
                                                R"(","distance":15,"operation":"new","target":")" + ct::id('b', 1) +
                                                R"("}})"));
    REQUIRE(w.invoke("feature.datum_plane.add",
                     R"({"id":")" + f(4) + R"(","props":{"base":{"principal":"xy"},"offset":15}})"));
    REQUIRE(w.invoke("feature.sketch.add", R"({"id":")" + f(5) + R"(","props":{"plane":{"datum":")" + f(4) +
                                               R"("},"sketch":{"schema":1,"entities":[)" + pt(9, 15, 20) +
                                               R"(],"constraints":[]}}})"));
    REQUIRE(w.invoke("feature.hole.add",
                     R"({"id":")" + f(6) + R"(","props":{"sketch":")" + f(5) + R"(","target":")" + ct::id('b', 1) +
                         R"(","kind":"tapped","end":"blind","depth":12,"thread":{"designation":"M6x1",)"
                         R"("standard":"custom","nominalDiameter":6,"pitch":1,"tapDrill":5,"depth":8}}})"));
    w.settle();
    REQUIRE(w.host->scene()->bodyCount() == 1);
    w.view()->fitAll();
    w.toWorkspace(ui::kCamWorkspace);
    addSetup(w);

    newOperation(w);
    // Tap: the cycle, then the M8 tap, whose thread is not the hole's.
    stepDown(w, QStringLiteral("schemaCombo.cycle"), // spot, [drill], peck, chipBreak, tap
             [&] { return w.op()->form()->valueOf(QStringLiteral("cycle")).toString() == QStringLiteral("tap"); });
    REQUIRE(QTest::qWaitFor([&] { return pickerTypes(w) == std::set<QString>{QStringLiteral("tap")}; }, 5000));
    chooseTool(w, kTapM8, QStringLiteral("M8x1.25"));
    REQUIRE(QTest::qWaitFor([&] { return w.op()->holeCount() == 1; }, kTimeoutMs));
    pickHole(w, 0);
    REQUIRE(QTest::qWaitFor([&] { return w.op()->picks().size() == 1; }, kTimeoutMs));
    CHECK_THAT(w.op()->picks()[0].toMap().value(QStringLiteral("text")).toString().toStdString(),
               ContainsSubstring("M6x1"));
    w.click(QStringLiteral("operationUseCalculated"));
    REQUIRE(QTest::qWaitFor([&] { return w.op()->feedsState() == QStringLiteral("calculated"); }, 5000));
    // The drill type's own words, under the parameters; OK stays off.
    REQUIRE(QTest::qWaitFor([&] { return !w.op()->errors().isEmpty() && !w.op()->previewBusy(); }, kTimeoutMs));
    CHECK_THAT(w.op()->errors().front().toStdString(),
               ContainsSubstring("The tap's pitch (1.25 mm) does not match the M6x1 thread"));
    CHECK(w.shown(QStringLiteral("operationError.0"))->property("text").toString() == w.op()->errors().front());
    CHECK_FALSE(w.op()->canCommit());
    CHECK_FALSE(w.shown(QStringLiteral("operationOk"))->isEnabled());
    CHECK(statusLine(w) == QStringLiteral("Fix the error above first"));
    // The blocking message is in the footer beside OK, in view whatever the scroll.
    {
        QQuickItem* error = w.shown(QStringLiteral("operationError.0"));
        CHECK_FALSE(w.find(QStringLiteral("operationScroll"))->isAncestorOf(error));
        CHECK(w.find(QStringLiteral("operationFooter"))->isAncestorOf(error));
        QQuickItem* panel = w.find(QStringLiteral("operationPanel"));
        const QRectF p(panel->mapToScene(QPointF(0, 0)), QSizeF(panel->width(), panel->height()));
        const QRectF r(error->mapToScene(QPointF(0, 0)), QSizeF(error->width(), error->height()));
        CHECK(p.contains(r));
    }
    // A tap advances by its pitch per revolution, not a chip load per tooth.
    {
        QQuickItem* pitch = w.shown(QStringLiteral("operationCalculatedChipLoad"));
        CHECK(pitch->property("label").toString() == QStringLiteral("Pitch"));
        CHECK(pitch->property("value").toString().endsWith(QStringLiteral(" in/rev"))); // an inch document
    }
    w.shotsBothThemes(QStringLiteral("t045_tap_mismatch"));

    // The tool changed since the feeds were calculated: an update is offered, not made.
    chooseTool(w, kTapM6, QStringLiteral("M6x1"));
    REQUIRE(QTest::qWaitFor([&] { return w.op()->feedsState() == QStringLiteral("offer"); }, 5000));
    CHECK(w.shown(QStringLiteral("operationUseCalculated"))->property("text").toString() ==
          QStringLiteral("Update to calculated"));
    const QString keptRpm = w.op()->values().value(QStringLiteral("rpm")).toString();
    // The M6x1 tap fits: no error, OK works (a warning would not stop it either).
    w.previewed("ok");
    CHECK(w.op()->errors().isEmpty());
    REQUIRE(QTest::qWaitFor([&] { return w.op()->canCommit(); }, 10000));
    CHECK(w.op()->values().value(QStringLiteral("rpm")).toString() == keptRpm); // not replaced without the click
    w.click(QStringLiteral("operationUseCalculated"));
    REQUIRE(QTest::qWaitFor([&] { return w.op()->feedsState() == QStringLiteral("calculated"); }, 5000));

    // A parameter out of range: the message beside it, OK off; reset brings the default back.
    w.typeInto(QStringLiteral("schemaInput.dwell"), QStringLiteral("-1"));
    REQUIRE(QTest::qWaitFor([&] { return w.find(QStringLiteral("schemaError.dwell"))->isVisible(); }, 5000));
    CHECK(w.find(QStringLiteral("schemaError.dwell"))->property("text").toString() ==
          QStringLiteral("Must be at least 0 s"));
    REQUIRE(QTest::qWaitFor([&] { return !w.op()->canCommit(); }, 5000));
    CHECK_FALSE(w.shown(QStringLiteral("operationOk"))->isEnabled());
    CHECK(statusLine(w).startsWith(QStringLiteral("Dwell:")));
    w.click(QStringLiteral("schemaReset.dwell"));
    REQUIRE(QTest::qWaitFor([&] { return !w.find(QStringLiteral("schemaError.dwell"))->isVisible(); }, 5000));
    w.previewed("ok");
    REQUIRE(QTest::qWaitFor([&] { return w.op()->canCommit(); }, 10000));
    // A height out of order is named too.
    w.typeInto(QStringLiteral("operationInput.retract"), QStringLiteral("100 mm"));
    REQUIRE(QTest::qWaitFor([&] { return statusLine(w).contains(QLatin1String("must not be below")); }, 5000));
    CHECK_FALSE(w.op()->canCommit());
    w.click(QStringLiteral("operationCancel"));
    REQUIRE(QTest::qWaitFor([&] { return !w.op()->active(); }, 5000));
    CHECK(w.document()->state()->cam->operations.empty());
    w.checkNoWarnings();
}

TEST_CASE("every registered operation type gets a working form with no type-specific QML", "[cam][e2e][t045]")
{
    RAY_REQUIRE_RHI();
    Window w(true);
    w.open(kPlate);
    w.toWorkspace(ui::kCamWorkspace);
    addSetup(w);
    const auto types = api::cam_operation::operationTypes(*w.host->app());
    REQUIRE(types.size() >= 4); // drill and the test types
    for (const api::cam_operation::TypeInfo& t : types) {
        INFO("operation type " << t.name);
        REQUIRE(w.op()->startFor(QString::fromStdString(ct::id('a', 1))));
        w.op()->setType(QString::fromStdString(t.name));
        REQUIRE(w.op()->type() == QString::fromStdString(t.name));
        w.shown(QStringLiteral("operationPanel"));
        QCoreApplication::processEvents();
        w.grab();
        // Every field has its row and its control; what the form cannot edit says so.
        for (const ui::camui::FormField& field : w.op()->form()->schema().fields) {
            INFO("parameter " << field.key.toStdString());
            QQuickItem* row = w.find(QStringLiteral("schemaRow.") + field.key);
            REQUIRE(row != nullptr);
            if (!w.op()->form()->isRelevant(field.key)) {
                CHECK_FALSE(row->isVisible());
                continue;
            }
            CHECK(row->isVisible());
            using K = ui::camui::FormField::Kind;
            const char* control = field.kind == K::Enum          ? "schemaCombo."
                                  : field.kind == K::Boolean     ? "schemaCheck."
                                  : field.kind == K::Unsupported ? "schemaUnsupported."
                                                                 : "schemaInput.";
            QQuickItem* c = w.find(QLatin1String(control) + field.key);
            REQUIRE(c != nullptr);
            CHECK(c->isVisible());
            if (field.kind == K::Unsupported)
                CHECK(c->property("text").toString().startsWith(QStringLiteral("Cannot edit this parameter here")));
        }
        for (int i = 0; i < w.op()->form()->problems().size(); ++i)
            CHECK(w.shown(QStringLiteral("schemaProblem.%1").arg(i))->isVisible());
        // Sections from the annotations: the heights the type reads, its geometry picker or none.
        CHECK(w.find(QStringLiteral("operationSection.geometry"))->isVisible() ==
              (w.op()->geometryKind() != QStringLiteral("none")));
        for (const QVariant& h : w.op()->heights())
            CHECK(w.find(QStringLiteral("operationField.") + h.toMap().value(QStringLiteral("key")).toString())
                      ->isVisible());
        if (t.name == "ui.schema") {
            CHECK(w.op()->heights().size() == 3);
            CHECK(w.find(QStringLiteral("operationField.feedPlunge"))->isVisible());
            CHECK_FALSE(w.find(QStringLiteral("operationField.feedRamp"))->isVisible());
            CHECK(w.find(QStringLiteral("schemaUnsupported.nested"))->isVisible());
            CHECK(w.op()->form()->problems().size() == 1); // "allOf"
            w.shotsBothThemes(QStringLiteral("t045_every_construct"));
        }
        w.op()->cancel();
        REQUIRE_FALSE(w.op()->active());
    }
    w.checkNoWarnings();
}

TEST_CASE("the operation panel's labels are not cut short at this scale", "[scale]")
{
    RAY_REQUIRE_RHI();
    const QString scale =
        qEnvironmentVariableIsSet("RAY_TEST_SCALE") ? qEnvironmentVariable("RAY_TEST_SCALE") : QStringLiteral("native");
    Window w(false, QSize(1280, 860));
    w.open(kPlate);
    w.toWorkspace(ui::kCamWorkspace);
    addSetup(w);
    newOperation(w);
    chooseTool(w, kDrill6, QStringLiteral("6 mm HSS"));
    pickHole(w, 0);
    REQUIRE(QTest::qWaitFor([&] { return w.op()->picks().size() == 1; }, kTimeoutMs));
    w.click(QStringLiteral("operationUseCalculated"));
    w.previewed("ok");
    for (const char* theme : {"dark", "light"}) {
        INFO("theme " << theme << ", scale " << scale.toStdString());
        w.host->theme()->setProperty("mode", QLatin1String(theme));
        QTest::qWait(100);
        checkPanelLayout(w);
        checkFooter(w);
        w.shot(QStringLiteral("t045_scale_%1_%2")
                   .arg(scale, QLatin1String(theme))
                   .replace(QLatin1Char('.'), QLatin1Char('_')));
    }
    w.op()->cancel();
}

TEST_CASE("the UI thread stays within its budget while previews build", "[cam][stall][wallclock]")
{
    RAY_REQUIRE_RHI();
    Window w;
    w.open(kPlate);
    w.toWorkspace(ui::kCamWorkspace);
    addSetup(w);
    newOperation(w);
    chooseTool(w, kDrill6, QStringLiteral("6 mm HSS"));
    for (std::size_t i = 0; i < 6; ++i) {
        pickHole(w, i);
        REQUIRE(QTest::qWaitFor([&] { return w.op()->picks().size() == static_cast<int>(i + 1); }, kTimeoutMs));
    }
    w.click(QStringLiteral("operationUseCalculated"));
    w.previewed("ok");

    // The field in view first: scrolling the panel is not what is measured.
    w.inView(w.shown(QStringLiteral("schemaInput.breakthrough")));
    QTest::qWait(200);
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
    std::vector<double> ms;
    for (int i = 0; i < 6; ++i) {
        QElapsedTimer t;
        t.start();
        const int shown = w.op()->previewsShown();
        w.typeInto(QStringLiteral("schemaInput.breakthrough"),
                   QString::number(1 + i % 3 + 0.5 * i) + QStringLiteral(" mm"));
        REQUIRE(QTest::qWaitFor([&] { return w.op()->previewsShown() > shown && !w.op()->previewBusy(); }, kTimeoutMs));
        ms.push_back(static_cast<double>(t.elapsed()));
    }
    tick.stop();
    std::sort(ms.begin(), ms.end());
    WARN("operation preview latency (six holes, drill): median " << ms[ms.size() / 2] << " ms, worst " << ms.back()
                                                                 << " ms");
    INFO("worst UI-thread gap " << worst << " ms");
    WARN("UI-thread watchdog while operation previews build: worst gap " << worst << " ms");
    CHECK(worst < ray::test::uiBudgetMs(50));
    CHECK(ms[ms.size() / 2] < ray::test::uiBudgetMs(1000));
    w.op()->cancel();
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
