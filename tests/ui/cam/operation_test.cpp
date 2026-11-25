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
#include "../ui_watchdog.h"
#include "../window_focus.h"
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

#include <ray/kernel.h> // T-055: finding the faces and edges to pick

#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtGui/QFontMetricsF>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>
#include <QtGui/QMouseEvent>
#include <QtGui/QScreen>
#include <QtQml/QQmlApplicationEngine>
#include <QtQml/QQmlComponent>
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
#include <limits>
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
        // Keys and window shortcuts go to Qt's focus window: taken whether or not the desktop grants it
        // (window_focus.h).
        REQUIRE(ray::test::focusWindow(host->window()));
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
    // A CAM tree row is clicked on its name, where a user clicks to select it: its tool buttons are at its right end
    // (CamTree.qml), and a click at the row's centre could fall on one.
    QPointF rowNameAt(const QString& rowObjectName) const
    {
        QQuickItem* row = shown(rowObjectName);
        QQuickItem* name = findItem(row, QStringLiteral("camRowName"));
        REQUIRE(name != nullptr);
        const double w = std::min(name->width(), name->property("contentWidth").toDouble());
        return name->mapToScene(QPointF(w / 2, name->height() / 2));
    }
    void clickRow(const QString& rowObjectName) const
    {
        grab();
        clickScene(rowNameAt(rowObjectName));
    }
    void doubleClickRow(const QString& rowObjectName) const
    {
        grab();
        const QPointF c = rowNameAt(rowObjectName);
        sendMouse(QEvent::MouseMove, c, Qt::NoButton, Qt::NoButton);
        QTest::mouseDClick(window(), Qt::LeftButton, Qt::NoModifier, c.toPoint());
        QCoreApplication::processEvents();
    }
    void typeInto(const QString& objectName, const QString& text) const
    {
        QQuickItem* field = shown(objectName);
        inView(field);
        if (QGuiApplication::focusWindow() != window()) // taken by another program since (window_focus.h)
            ray::test::focusWindow(window());
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
    // A new document (inches, the default for new documents) with `path` imported into it: a STEP file opened on its
    // own takes the unit it declares (the plate is metric), one imported keeps the document's.
    void openInNewDocument(const QString& path) const
    {
        REQUIRE(QFileInfo::exists(path));
        host->files()->newPart();
        REQUIRE(QTest::qWaitFor(
            [&] {
                return host->session()->hasDocument() && host->app()->active() != nullptr && !host->files()->busy();
            },
            kTimeoutMs));
        host->files()->importFile(path);
        REQUIRE(QTest::qWaitFor([&] { return host->scene()->bodyCount() >= 1 && settled(); }, kTimeoutMs));
        settle();
        REQUIRE(host->app()->active()->document->state()->settings.units == UnitSystem::Inch);
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
    return api::cam_tools::toolJson(*t);
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

// Opens the panel for a new operation from the setup's row in the tree, as the user does, and makes it of `type` (the
// panel starts with the type last added in the session, else drill; "" keeps that).
void newOperation(const Window& w, int setupRow = 0, const QString& type = QStringLiteral("drill"))
{
    w.clickRow(QStringLiteral("setupRow.%1").arg(setupRow));
    REQUIRE(QTest::qWaitFor([&] { return w.tree()->currentIndex() == setupRow; }, 5000));
    w.click(w.shown(QStringLiteral("camRowNewOperation.%1").arg(setupRow)));
    REQUIRE(QTest::qWaitFor([&] { return w.op()->active(); }, 5000));
    w.shown(QStringLiteral("operationPanel"));
    if (!type.isEmpty() && w.op()->type() != type) {
        w.op()->setType(type);
        REQUIRE(w.op()->type() == type);
        QCoreApplication::processEvents();
    }
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

// The pixels of the first hyphen of a Text item's single line that differ from the background: the ink between the
// hyphen's advance, its middle three fifths, over the line's height (device pixels).
int hyphenInk(const QImage& img, QQuickItem* text)
{
    const qreal dpr = img.devicePixelRatio();
    const QString s = text->property("text").toString();
    const qsizetype h = s.indexOf(QLatin1Char('-'));
    REQUIRE(h >= 0);
    const QFontMetricsF fm(text->property("font").value<QFont>());
    const double x0 = fm.horizontalAdvance(s.left(h)), x1 = fm.horizontalAdvance(s.left(h + 1));
    const double top = text->property("verticalAlignment").toInt() == Qt::AlignVCenter
                           ? (text->height() - text->property("contentHeight").toDouble()) / 2
                           : 0.0;
    const QPointF a = text->mapToScene(QPointF(x0 + 0.2 * (x1 - x0), top)) * dpr;
    const QPointF b = text->mapToScene(QPointF(x1 - 0.2 * (x1 - x0), top + fm.height())) * dpr;
    const QColor bg = img.pixelColor(QPoint(int(a.x()), int(a.y())));
    int ink = 0;
    for (int y = int(a.y()); y <= int(b.y()); ++y) {
        for (int x = int(std::ceil(a.x())); x <= int(b.x()); ++x) {
            const QColor c = img.pixelColor(x, y);
            if (std::abs(c.red() - bg.red()) + std::abs(c.green() - bg.green()) + std::abs(c.blue() - bg.blue()) > 60)
                ++ink;
        }
    }
    return ink;
}

// How many of the window's text items showing exactly `text` (with a hyphen), wholly in sight (inside every clipping
// ancestor and the window), draw no ink where the hyphen is.
int hyphensLost(const Window& w, const QString& text)
{
    const QImage img = w.grab();
    std::vector<QQuickItem*> all{w.window()->contentItem()};
    for (std::size_t i = 0; i < all.size(); ++i) {
        for (QQuickItem* c : all[i]->childItems())
            all.push_back(c);
    }
    int lost = 0, seen = 0;
    for (QQuickItem* item : all) {
        if (!item->isVisible() || item->property("text").toString() != text)
            continue;
        const QRectF r = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
        bool inSight = QRectF(0, 0, w.window()->width(), w.window()->height()).contains(r);
        for (QQuickItem* p = item->parentItem(); p != nullptr && inSight; p = p->parentItem()) {
            if (p->clip())
                inSight = p->mapRectToScene(QRectF(0, 0, p->width(), p->height())).contains(r);
        }
        if (!inSight)
            continue;
        ++seen;
        if (hyphenInk(img, item) == 0) {
            ++lost;
            UNSCOPED_INFO("hyphen lost at scene " << r.x() << ", " << r.y());
        }
    }
    INFO("'" << text.toStdString() << "' in sight " << seen << " times");
    CHECK(seen > 0);
    return lost;
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
    w.openInNewDocument(kPlate); // in inches: a stored length then shows rounded (the exact value is checked)
    w.toWorkspace(ui::kCamWorkspace);
    addSetup(w);
    const std::string before = w.stateHash();
    const std::size_t undoBefore = w.undoCount();

    // A new operation from the setup's row, made a drilling one.
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

TEST_CASE("a 2D contour operation by the UI: edges picked in the view, a library end mill, the preview, OK",
          "[cam][e2e][t053]")
{
    // The same form as every type (no type-specific QML): the plate's four top edges picked as a user clicks them,
    // the 10 mm end mill from the picker, the bottom 5 mm below the edges, the preview drawn, OK.
    RAY_REQUIRE_RHI();
    Window w;
    w.open(kPlate); // metric
    w.toWorkspace(ui::kCamWorkspace);
    addSetup(w);
    const std::size_t undoBefore = w.undoCount();
    newOperation(w, 0, QStringLiteral("contour2d"));
    CHECK(w.op()->typeTitle() == QStringLiteral("2D contour"));
    CHECK(w.op()->geometryKind() == QStringLiteral("edge"));
    CHECK(w.op()->geometryKinds() == QStringList({QStringLiteral("edge"), QStringLiteral("face")}));
    CHECK(w.op()->picking());
    // Edges and faces (x-geometry names both since M3's gaps were closed).
    CHECK(w.host->selection()->filterMask() ==
          static_cast<std::uint8_t>(static_cast<std::uint8_t>(render::PickFilter::Edges) |
                                    static_cast<std::uint8_t>(render::PickFilter::Faces)));
    for (const char* key : {"clearance", "retract", "feedPlane", "top", "bottom"})
        CHECK(w.find(QStringLiteral("operationField.") + QLatin1String(key))->isVisible());
    // Relevance from the schema: the finish allowance only with a finish pass, the tab fields only with tabs.
    CHECK_FALSE(w.op()->form()->isRelevant(QStringLiteral("finishAllowance")));
    CHECK_FALSE(w.op()->form()->isRelevant(QStringLiteral("tabShape")));

    // The tool: end mills first (x-toolTypes).
    REQUIRE(QTest::qWaitFor([&] { return pickerList(w)->count() > 0; }, 10000));
    CHECK(pickerTypes(w).count(QStringLiteral("drill")) == 0);
    CHECK(pickerTypes(w).count(QStringLiteral("flatEndMill")) == 1);
    const QString mill10 = QStringLiteral("3f550375-46eb-47d0-9b8c-4fedbcb8466d"); // 10 mm 4FL carbide flat end mill
    chooseTool(w, mill10, QStringLiteral("10 mm 4FL"));

    // The four top edges of the plate, each as a click on it in the view would pick it.
    const kernel::Shape& shape = w.host->scene()->body(0)->shape;
    const Box3 b = kernel::bounds(shape);
    int picked = 0;
    for (std::uint32_t i = 0; i < shape.count(kernel::SubKind::Edge); ++i) {
        auto c = kernel::curveInfo(shape, kernel::SubId{kernel::SubKind::Edge, i});
        if (!c || c->type != kernel::CurveType::Line || std::abs(c->start.z - b.max.z) > 1e-6 ||
            std::abs(c->end.z - b.max.z) > 1e-6)
            continue;
        const Vec3 mid{(c->start.x + c->end.x) / 2, (c->start.y + c->end.y) / 2, b.max.z};
        const bool side = std::abs(mid.x - b.min.x) < 1e-6 || std::abs(mid.x - b.max.x) < 1e-6 ||
                          std::abs(mid.y - b.min.y) < 1e-6 || std::abs(mid.y - b.max.y) < 1e-6;
        if (!side)
            continue;
        w.host->selection()->applyClick(
            render::PickHit{w.host->scene()->itemForBody(0), kernel::SubKind::Edge, i, mid, 0}, Qt::NoModifier);
        QCoreApplication::processEvents();
        ++picked;
        REQUIRE(QTest::qWaitFor([&] { return w.op()->picks().size() == picked; }, kTimeoutMs));
    }
    REQUIRE(picked == 4);
    // Down 5 mm from the edges; feeds typed.
    w.typeInto(QStringLiteral("operationInput.bottom"), QStringLiteral("-5"));
    w.typeInto(QStringLiteral("operationInput.rpm"), QStringLiteral("8000"));
    w.typeInto(QStringLiteral("operationInput.feedCut"), QStringLiteral("1200"));
    w.window()->contentItem()->forceActiveFocus();
    QCoreApplication::processEvents();

    // The preview: the tool centre 5 mm outside the plate, drawn in the view.
    w.previewed("ok");
    const std::shared_ptr<const cam::Toolpath> tp = w.op()->previewToolpath();
    REQUIRE(tp != nullptr);
    bool outside = false;
    for (const cam::Move& m : tp->moves) {
        if (m.role == cam::MoveRole::Cut && m.x == -5'000'000)
            outside = true;
    }
    CHECK(outside);
    REQUIRE(QTest::qWaitFor(
        [&] {
            const auto shown = w.display()->shown();
            return std::any_of(shown.begin(), shown.end(), [](const ToolpathDisplay::Shown& s) { return s.preview; });
        },
        10000));
    REQUIRE(QTest::qWaitFor([&] { return statusLine(w) == QStringLiteral("Ready: OK adds the operation"); }, 10000));
    w.shotsBothThemes(QStringLiteral("t053_contour2d"));
    w.inView(w.shown(QStringLiteral("schemaRow.side"))); // the type's own parameters, from its schema
    w.shotsBothThemes(QStringLiteral("t053_contour2d_parameters"));

    w.click(QStringLiteral("operationOk"));
    REQUIRE(QTest::qWaitFor([&] { return !w.op()->active() && w.undoCount() == undoBefore + 1; }, kTimeoutMs));
    const doc::DocStatePtr state = w.document()->state();
    REQUIRE(state->cam);
    REQUIRE(state->cam->operations.size() == 1);
    const cam::Operation& op = state->cam->operations.begin()->second;
    CHECK(op.type == "contour2d");
    CHECK(op.geometry.size() == 4);
    CHECK(op.heights.bottom == -5);
    CHECK(op.paramsJson.find("\"direction\":\"climb\"") != std::string::npos); // the defaults are stored
    const int row = w.tree()->rowOf(QString::fromStdString(op.id.value.toString()));
    REQUIRE(row >= 0);
    w.waitStatus(row, "upToDate");
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
    // Every library row in sight draws the hyphen of its library's name (operation_test's scale case says why).
    for (const char* theme : {"dark", "light"}) {
        w.host->theme()->setProperty("mode", QLatin1String(theme));
        QTest::qWait(200);
        CHECK(hyphensLost(w, QStringLiteral("RayCAD built-in library")) == 0);
    }
    w.host->theme()->setProperty("mode", QStringLiteral("dark"));
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
    w.doubleClickRow(QStringLiteral("operationRow.%1").arg(row));
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
    // The material's name is drawn with its hyphen ("6061-T6"), in both themes: it was once drawn "6061 T6" here (the
    // scale case below says why).
    {
        QQuickItem* material = w.shown(QStringLiteral("operationCombo.material"));
        w.inView(material);
        QQuickItem* label = Window::findItem(material, QStringLiteral("label"));
        REQUIRE(label != nullptr);
        for (const char* theme : {"dark", "light"}) {
            INFO("theme " << theme);
            w.host->theme()->setProperty("mode", QLatin1String(theme));
            QTest::qWait(200);
            CHECK(hyphenInk(w.grab(), label) > 0);
        }
        w.host->theme()->setProperty("mode", QStringLiteral("dark"));
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
            const char* control = field.kind == K::Enum                       ? "schemaCombo."
                                  : field.kind == K::Boolean                  ? "schemaCheck."
                                  : field.kind == K::Unsupported              ? "schemaUnsupported."
                                  : field.format == QLatin1String("pointsXY") ? "schemaPointPick." // a list of points
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

// T-055: the slot and chamfer2d types in the real window from their schemas alone (no QML of their own): the type from
// the panel's list, a library tool from the picker (the types the schema offers first), the geometry picked in the
// view, calculated feeds, the preview generated and drawn, OK; the tree up to date. Screenshots t055_slot and
// t055_chamfer2d (RAY_T045_SHOTS).
TEST_CASE("a slot and a chamfer operation by the UI: picked in the view, previewed, added", "[cam][e2e][t055]")
{
    RAY_REQUIRE_RHI();
    struct Run {
        QString part, type, tool, search, toolType;
        std::function<std::vector<render::PickHit>(const Window&)> picks;
    };
    const auto faceUpAt = [](const Window& w, double z) {
        const kernel::Shape& s = w.host->scene()->body(0)->shape;
        std::vector<render::PickHit> out;
        for (std::uint32_t i = 0; i < s.count(kernel::SubKind::Face); ++i) {
            const kernel::SubId f{kernel::SubKind::Face, i};
            const auto info = kernel::surfaceInfo(s, f);
            const auto sample = kernel::faceSample(s, f);
            if (info && sample && info->type == kernel::SurfaceType::Plane && sample->normal.z > 0.999 &&
                std::abs(info->bounds.min.z - z) < 1e-6) {
                out.push_back(
                    render::PickHit{w.host->scene()->itemForBody(0), kernel::SubKind::Face, i, info->centroid, 0});
            }
        }
        return out;
    };
    const auto holeTopEdges = [](const Window& w) {
        const kernel::Shape& s = w.host->scene()->body(0)->shape;
        const double top = kernel::bounds(s).max.z;
        std::vector<render::PickHit> out;
        for (std::uint32_t i = 0; i < s.count(kernel::SubKind::Edge); ++i) {
            const auto c = kernel::curveInfo(s, kernel::SubId{kernel::SubKind::Edge, i});
            if (c && c->type == kernel::CurveType::Circle && std::abs(c->center.z - top) < 1e-6 && out.size() < 4) {
                out.push_back(render::PickHit{w.host->scene()->itemForBody(0), kernel::SubKind::Edge, i, c->start, 0});
            }
        }
        return out;
    };
    const std::vector<Run> runs = {
        // keyway_tslot_3: a 40 mm shaft along X with a 12 mm keyway, its floor at Z 15 (model), 5 below the top.
        Run{QStringLiteral(RAY_CORPUS_DIR "/raycad-synthetic/keyway_tslot_3.stp"), QStringLiteral("slot"),
            QStringLiteral("82006b3d-f9fe-47c0-b84c-48620d9f23a2"), QStringLiteral("12 mm 4FL"),
            QStringLiteral("flatEndMill"), [&](const Window& w) { return faceUpAt(w, 15); }},
        // The plate's holes, chamfered at their top edges with the 1/4 in 90 degree chamfer mill.
        Run{kPlate, QStringLiteral("chamfer2d"), QStringLiteral("3b8df723-7be6-494f-b0a4-8dc6a3c14e72"),
            QStringLiteral("chamfer mill"), QStringLiteral("chamferMill"), holeTopEdges},
    };
    for (const Run& r : runs) {
        INFO("operation type " << r.type.toStdString());
        Window w;
        w.open(r.part);
        w.toWorkspace(ui::kCamWorkspace);
        addSetup(w);
        w.clickRow(QStringLiteral("setupRow.0"));
        REQUIRE(QTest::qWaitFor([&] { return w.tree()->currentIndex() == 0; }, 5000));
        w.click(w.shown(QStringLiteral("camRowNewOperation.0")));
        REQUIRE(QTest::qWaitFor([&] { return w.op()->active(); }, 5000));
        // The type from the panel's list, as the user steps to it.
        stepDown(w, QStringLiteral("operationCombo.type"), [&] { return w.op()->type() == r.type; });
        CHECK(w.op()->geometryKind() ==
              (r.type == QStringLiteral("slot") ? QStringLiteral("face") : QStringLiteral("edge")));
        // The picker offers the schema's tool types first.
        REQUIRE(QTest::qWaitFor([&] { return pickerList(w)->count() > 0; }, 10000));
        CHECK(pickerTypes(w).count(r.toolType) == 1);
        CHECK(pickerTypes(w).count(QStringLiteral("drill")) == 0);
        chooseTool(w, r.tool, r.search);
        const std::vector<render::PickHit> hits = r.picks(w);
        REQUIRE_FALSE(hits.empty());
        for (std::size_t k = 0; k < hits.size(); ++k) {
            // One click at a time, as a user clicks: each pick becomes a reference before the next.
            w.host->selection()->applyClick(hits[k], Qt::NoModifier);
            QCoreApplication::processEvents();
            REQUIRE(QTest::qWaitFor([&] { return w.op()->picks().size() == static_cast<int>(k + 1); }, kTimeoutMs));
        }
        if (w.op()->calculated().value(QStringLiteral("available")).toBool()) {
            w.click(QStringLiteral("operationUseCalculated"));
            REQUIRE(QTest::qWaitFor([&] { return w.op()->feedsState() == QStringLiteral("calculated"); }, 5000));
        } else {
            w.typeInto(QStringLiteral("operationInput.rpm"), QStringLiteral("6000"));
            w.typeInto(QStringLiteral("operationInput.feedCut"), QStringLiteral("600"));
        }
        w.previewed("ok");
        REQUIRE(w.op()->previewToolpath() != nullptr);
        const cam::Toolpath& preview = *w.op()->previewToolpath();
        CHECK(std::count_if(preview.moves.begin(), preview.moves.end(),
                            [](const cam::Move& m) { return m.role == cam::MoveRole::Cut; }) > 0);
        REQUIRE(w.op()->previewBackplot() != nullptr);
        REQUIRE(QTest::qWaitFor(
            [&] {
                const auto shown = w.display()->shown();
                return std::any_of(shown.begin(), shown.end(),
                                   [](const ToolpathDisplay::Shown& s) { return s.preview; });
            },
            10000));
        CHECK(w.op()->errors().isEmpty());
        REQUIRE(
            QTest::qWaitFor([&] { return statusLine(w) == QStringLiteral("Ready: OK adds the operation"); }, 10000));
        checkPanelLayout(w);
        w.shot(QStringLiteral("t055_") + r.type);
        const QString id = w.op()->operationId();
        w.click(QStringLiteral("operationOk"));
        REQUIRE(QTest::qWaitFor([&] { return !w.op()->active(); }, kTimeoutMs));
        const int row = w.tree()->rowOf(id);
        REQUIRE(row >= 0);
        w.waitStatus(row, "upToDate");
        const doc::DocStatePtr state = w.document()->state();
        REQUIRE(state->cam);
        REQUIRE(state->cam->operations.size() == 1);
        CHECK(state->cam->operations.begin()->second.type == r.type.toStdString());
        CHECK(state->cam->operations.begin()->second.geometry.size() == hits.size());
        w.checkNoWarnings();
    }
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
    // Watchdog (ui_watchdog.h): the worst time the UI thread was busy or blocked between two of its 2 ms ticks. Inside
    // the measured stretch the test waits with ray::test::waitFor, which does not sleep the UI thread.
    ray::test::UiWatchdog dog;
    dog.start();
    std::vector<double> ms;
    for (int i = 0; i < 6; ++i) {
        QElapsedTimer t;
        t.start();
        const int shown = w.op()->previewsShown();
        w.typeInto(QStringLiteral("schemaInput.breakthrough"),
                   QString::number(1 + i % 3 + 0.5 * i) + QStringLiteral(" mm"));
        REQUIRE(
            ray::test::waitFor([&] { return w.op()->previewsShown() > shown && !w.op()->previewBusy(); }, kTimeoutMs));
        ms.push_back(static_cast<double>(t.elapsed()));
    }
    dog.stop();
    std::sort(ms.begin(), ms.end());
    WARN("operation preview latency (six holes, drill): median " << ms[ms.size() / 2] << " ms, worst " << ms.back()
                                                                 << " ms");
    WARN("UI-thread watchdog while operation previews build: worst " << dog.worst().describe() << "; longest gap "
                                                                     << dog.worstWall().describe());
    CHECK(dog.worstStallMs() < ray::test::uiBudgetMs(50));
    CHECK(ms[ms.size() / 2] < ray::test::uiBudgetMs(1000));
    w.op()->cancel();
}

// Native text (Qt Quick's NativeRendering, crisper at 100 % and 200 %) puts each glyph's quad on device pixels by
// rounding in its vertex shader, and samples the glyph cache texel for pixel. At a fractional device pixel ratio
// (125 %, 150 %, 175 %) a text item that lies on half a device pixel (here: at thirds of a logical pixel, in a list
// scrolled by thirds) gets glyph quads a pixel short or long by float rounding, and a stroke one texel thin is dropped
// or doubled: a hyphen vanishes ("6061 T6"), an l thins, an fl thickens. With native text forced here, 51 of some 1,700
// hyphens were lost at 150 % and none at 100 % or 200 %. AppHost uses distance-field text when a screen has a
// fractional ratio (app_host.cpp). This case runs at 100 %, 150 % and 200 % (the [scale] tests) and at the screen's own
// ratio.
TEST_CASE("text keeps its thin strokes at this scale: hyphens in a scrolled list", "[scale]")
{
    RAY_REQUIRE_RHI();
    Window w;
    QQmlComponent component(w.host->engine());
    component.setData(R"(import QtQuick
import RayCad.Ui
Rectangle {
    width: 1300; height: 760; color: Theme.panelBg
    Flickable {
        objectName: "hyphenFlick"
        anchors.fill: parent
        contentHeight: 3000
        clip: true
        Repeater {
            model: 240
            delegate: Text {
                required property int index
                x: 10 + (index % 6) * 210 + (index % 6) / 3
                y: 6 + Math.floor(index / 6) * 18 + (index % 6) / 3
                textFormat: Text.PlainText
                text: "6061-T6 built-in"
                font: index % 2 ? Theme.uiFont : Theme.smallFont
                color: Theme.textPrimary
            }
        }
    }
})",
                      QUrl());
    std::unique_ptr<QObject> made(component.create());
    INFO(component.errorString().toStdString());
    auto* root = qobject_cast<QQuickItem*>(made.get());
    REQUIRE(root != nullptr);
    root->setParentItem(w.window()->contentItem());
    root->setZ(1000);
    QQuickItem* flick = Window::findItem(root, QStringLiteral("hyphenFlick"));
    REQUIRE(flick != nullptr);
    INFO("device pixel ratio " << w.window()->effectiveDevicePixelRatio());
    {
        // Distance-field text where any screen has a fractional ratio, native text elsewhere (app_host.cpp).
        std::string ratios;
        bool fractional = false;
        for (const QScreen* s : QGuiApplication::screens()) {
            ratios += " " + s->name().toStdString() + "=" + std::to_string(s->devicePixelRatio());
            fractional = fractional || std::abs(s->devicePixelRatio() - std::round(s->devicePixelRatio())) > 1e-3;
        }
        INFO("screens" << ratios);
        CHECK(QQuickWindow::textRenderType() ==
              (fractional ? QQuickWindow::QtTextRendering : QQuickWindow::NativeTextRendering));
    }
    int lost = 0;
    for (const double contentY : {0.0, 1.0 / 3.0, 2.0 / 3.0, 1.0, 4.0 / 3.0, 5.0 / 3.0, 7.0}) {
        flick->setProperty("contentY", contentY);
        QTest::qWait(150);
        lost += hyphensLost(w, QStringLiteral("6061-T6 built-in"));
    }
    CHECK(lost == 0);
    root->setParentItem(nullptr);
}

// T-056: the thread milling and engraving types in the real window, through the generic panel and nothing of their
// own: a hole's wall and a bore picked as faces, a line of text typed into the schema's text field, a hole's rim picked
// as an edge, the tool from the library, the preview drawn, OK. Screenshots (RAY_T045_SHOTS): t056_threadmill and
// t056_engrave, the panel with the generated toolpath, in both themes.
namespace {

// The face of body 0 that is the cylinder of radius r about the vertical line through (x, y) (model mm), or the
// circular edge of radius r about it at height z.
std::uint32_t cylinderFace(const Window& w, double x, double y, double r)
{
    const kernel::Shape& s = w.host->scene()->body(0)->shape;
    std::optional<std::uint32_t> found; // (no code after FAIL: MSVC Debug reports it unreachable, C4702)
    for (std::uint32_t i = 0; i < s.count(kernel::SubKind::Face) && !found; ++i) {
        const auto info = kernel::surfaceInfo(s, kernel::SubId{kernel::SubKind::Face, i});
        if (info && info->type == kernel::SurfaceType::Cylinder && std::abs(info->origin.x - x) < 1e-6 &&
            std::abs(info->origin.y - y) < 1e-6 && std::abs(info->radius - r) < 1e-6)
            found = i;
    }
    if (!found)
        FAIL("no cylinder of radius " << r << " at (" << x << ", " << y << ")");
    return found.value_or(0);
}
std::uint32_t circleEdge(const Window& w, double x, double y, double r, double z)
{
    const kernel::Shape& s = w.host->scene()->body(0)->shape;
    std::optional<std::uint32_t> found;
    for (std::uint32_t i = 0; i < s.count(kernel::SubKind::Edge) && !found; ++i) {
        const auto info = kernel::curveInfo(s, kernel::SubId{kernel::SubKind::Edge, i});
        if (info && info->type == kernel::CurveType::Circle && std::abs(info->center.x - x) < 1e-6 &&
            std::abs(info->center.y - y) < 1e-6 && std::abs(info->radius - r) < 1e-6 &&
            std::abs(info->center.z - z) < 1e-6)
            found = i;
    }
    if (!found)
        FAIL("no circle of radius " << r << " at (" << x << ", " << y << ", " << z << ")");
    return found.value_or(0);
}

void pickElement(const Window& w, kernel::SubKind kind, std::uint32_t index)
{
    w.host->selection()->applyClick(render::PickHit{w.host->scene()->itemForBody(0), kind, index, Vec3{}, 0},
                                    Qt::NoModifier);
    QCoreApplication::processEvents();
}

std::size_t helicalArcs(const cam::Toolpath& p)
{
    std::size_t n = 0;
    Nm z = p.startZ;
    for (const cam::Move& m : p.moves) {
        n += (m.kind == cam::MoveKind::ArcCW || m.kind == cam::MoveKind::ArcCCW) && m.z != z ? 1 : 0;
        z = m.z;
    }
    return n;
}

const QString kThreadMillPitch1 = QStringLiteral("d6855e29-d997-4c9a-8e02-d96d6af93a0f"); // multi-form, pitch 1
const QString kSpot10 = QStringLiteral("2bc275c9-613e-4ad5-acc3-1c8db31262c9");           // 10 mm 90 deg spot drill

} // namespace

TEST_CASE("thread milling and engraving by the UI: the generic form, picks, text, the preview, OK", "[cam][e2e][t056]")
{
    RAY_REQUIRE_RHI();
    Window w;
    w.open(kPlate); // plate_thru_holes_1: 6.6 mm through holes at (30, 26.667), (60, 26.667) ..., top at Z 10
    w.toWorkspace(ui::kCamWorkspace);
    addSetup(w);

    // Thread milling: an M8x1 thread in the hole at (30, 26.667), multi-form, with the library's pitch 1 thread mill.
    newOperation(w);
    w.op()->setType(QStringLiteral("threadmill"));
    REQUIRE(w.op()->type() == QStringLiteral("threadmill"));
    CHECK(w.op()->geometryKind() == QStringLiteral("face"));
    CHECK(w.op()->form()->problems().isEmpty());
    for (const char* key : {"location", "form", "pitch", "diameter", "threadLength", "direction", "radialPasses"})
        CHECK(w.find(QStringLiteral("schemaRow.") + QLatin1String(key)) != nullptr);
    CHECK(w.find(QStringLiteral("operationField.feedPlane"))->isVisible());
    CHECK(w.find(QStringLiteral("operationField.top")) == nullptr); // threadmill reads clearance, retract, feed plane
    REQUIRE(QTest::qWaitFor([&] { return pickerList(w)->count() > 0; }, 10000));
    CHECK(pickerTypes(w) == std::set<QString>{QStringLiteral("threadMill")});
    chooseTool(w, kThreadMillPitch1, QStringLiteral("Pitch 1.0"));
    pickElement(w, kernel::SubKind::Face, cylinderFace(w, 30, 26.666666667, 3.3));
    REQUIRE(QTest::qWaitFor([&] { return w.op()->picks().size() == 1; }, kTimeoutMs));
    w.op()->form()->setValue(QStringLiteral("form"), QStringLiteral("multi"));
    w.op()->form()->setValue(QStringLiteral("pitch"), QStringLiteral("1 mm"));
    w.op()->form()->setValue(QStringLiteral("diameter"), QStringLiteral("8 mm"));
    w.op()->form()->setValue(QStringLiteral("radialPasses"), QStringLiteral("2"));
    CHECK(w.find(QStringLiteral("schemaRow.overlap"))->isVisible()); // relevant for multi-form only
    w.op()->setValue(QStringLiteral("rpm"), QStringLiteral("4000"));
    w.op()->setValue(QStringLiteral("feedCut"), QStringLiteral("250"));
    w.previewed("ok");
    REQUIRE(w.op()->previewToolpath() != nullptr);
    CHECK(helicalArcs(*w.op()->previewToolpath()) >= 4); // two radial passes of one turn, in halves
    REQUIRE(QTest::qWaitFor(
        [&] {
            const auto shown = w.display()->shown();
            return std::any_of(shown.begin(), shown.end(), [](const ToolpathDisplay::Shown& s) { return s.preview; });
        },
        10000));
    // The shot: the thread's parameters in the panel, the view framed on the hole and its helix.
    w.inView(w.find(QStringLiteral("schemaRow.radialPasses")));
    w.view()->setCamera(w.view()->framed(w.view()->camera(), Box3{{24, 20.5, 0}, {36, 32.5, 10}}));
    w.shotsBothThemes(QStringLiteral("t056_threadmill"));
    w.host->scene()->setBodyVisible(0, false); // the helix is inside the hole: a shot with the part hidden too
    w.shotsBothThemes(QStringLiteral("t056_threadmill_part_hidden"));
    w.host->scene()->setBodyVisible(0, true);
    w.click(QStringLiteral("operationOk"));
    REQUIRE(QTest::qWaitFor([&] { return !w.op()->active(); }, kTimeoutMs));
    REQUIRE(w.document()->state()->cam->operations.size() == 1);
    CHECK(w.document()->state()->cam->operations.begin()->second.type == "threadmill");

    // Engraving: the rim of the hole at (60, 26.667) and a line of text, with the 10 mm spot drill.
    newOperation(w);
    w.op()->setType(QStringLiteral("engrave"));
    REQUIRE(w.op()->type() == QStringLiteral("engrave"));
    CHECK(w.op()->geometryKind() == QStringLiteral("edge"));
    CHECK(w.op()->form()->problems().isEmpty());
    CHECK(w.find(QStringLiteral("schemaInput.text")) != nullptr); // the text field from "type": "string"
    CHECK(w.find(QStringLiteral("schemaCombo.font")) != nullptr); // the font as a choice
    w.op()->chooseTool(kSpot10);
    REQUIRE(QTest::qWaitFor([&] { return w.op()->toolId() == kSpot10; }, 5000));
    CHECK(w.host->selection()->filterMask() == static_cast<std::uint8_t>(render::PickFilter::Edges));
    pickElement(w, kernel::SubKind::Edge, circleEdge(w, 60, 26.666666667, 3.3, 10));
    REQUIRE(QTest::qWaitFor([&] { return w.op()->picks().size() == 1; }, kTimeoutMs));
    w.typeInto(QStringLiteral("schemaInput.text"), QStringLiteral("RayCAD T-056"));
    w.op()->form()->setValue(QStringLiteral("textHeight"), QStringLiteral("6 mm"));
    w.op()->form()->setValue(QStringLiteral("textX"), QStringLiteral("20 mm"));
    w.op()->form()->setValue(QStringLiteral("textY"), QStringLiteral("8 mm"));
    w.op()->form()->setValue(QStringLiteral("depth"), QStringLiteral("0.3 mm"));
    w.op()->setValue(QStringLiteral("rpm"), QStringLiteral("6000"));
    w.op()->setValue(QStringLiteral("feedCut"), QStringLiteral("300"));
    w.op()->setValue(QStringLiteral("feedPlunge"), QStringLiteral("100"));
    w.previewed("ok");
    REQUIRE(w.op()->previewToolpath() != nullptr);
    CHECK(w.op()->previewToolpath()->moves.size() > 40);
    REQUIRE(QTest::qWaitFor(
        [&] {
            const auto shown = w.display()->shown();
            return std::any_of(shown.begin(), shown.end(), [](const ToolpathDisplay::Shown& s) { return s.preview; });
        },
        10000));
    // The shot: the text field in the panel, the view framed on the text and the rim.
    w.inView(w.find(QStringLiteral("schemaRow.textHeight")));
    w.view()->setCamera(w.view()->framed(w.view()->camera(), Box3{{15, 4, 0}, {68, 32, 10}}));
    w.shotsBothThemes(QStringLiteral("t056_engrave"));
    w.host->scene()->setBodyVisible(0, false); // the strokes are 0.3 mm under the top face: the part hidden too
    w.shotsBothThemes(QStringLiteral("t056_engrave_part_hidden"));
    w.host->scene()->setBodyVisible(0, true);
    w.click(QStringLiteral("operationOk"));
    REQUIRE(QTest::qWaitFor([&] { return !w.op()->active(); }, kTimeoutMs));
    const doc::DocStatePtr state = w.document()->state();
    REQUIRE(state->cam->operations.size() == 2);
    bool engraved = false;
    for (const auto& [id, op] : state->cam->operations) {
        if (op.type == "engrave") {
            engraved = true;
            CHECK(op.paramsJson.find("\"text\":\"RayCAD T-056\"") != std::string::npos);
            CHECK(op.geometry.size() == 1);
        }
    }
    CHECK(engraved);
    w.checkNoWarnings();
}
// T-052: the facing type in the same generic panel (no geometry to pick, a flat end mill, its own parameters from its
// schema), previewed and added. Screenshot with RAY_T045_SHOTS: t052_face_dark / _light.
TEST_CASE("a face operation by the UI: no geometry, a flat end mill, two passes, the preview, OK", "[cam][e2e][t052]")
{
    RAY_REQUIRE_RHI();
    Window w;
    w.open(kPlate); // metric
    w.toWorkspace(ui::kCamWorkspace);
    // A setup whose stock is 3 mm wider on every side and 1 mm above the plate.
    const std::string args = R"({"id":")" + ct::id('a', 1) + R"(","name":"Setup 1","partBodies":[")" +
                             w.bodyId().toStdString() + R"("],"machine":")" + ct::id('9', 1) + R"(","material":")" +
                             kAl6061 +
                             R"(","stock":{"type":"box","xNeg":3,"xPos":3,"yNeg":3,"yPos":3,"zNeg":0,"zPos":1},)"
                             R"("library":{"machines":[)" +
                             ct::machineJson(1) + "],\"materials\":[" + libraryMaterial(w, kAl6061) + "]}}";
    REQUIRE(w.invoke("cam.setup.add", args));
    REQUIRE(QTest::qWaitFor([&] { return w.tree()->setupCount() >= 1; }, kTimeoutMs));
    const std::size_t undoBefore = w.undoCount();

    newOperation(w);
    w.op()->setType(QStringLiteral("face"));
    REQUIRE(w.op()->type() == QStringLiteral("face"));
    CHECK(w.op()->typeTitle() == QStringLiteral("Facing"));
    CHECK(w.op()->geometryKind() == QStringLiteral("none"));
    CHECK_FALSE(w.find(QStringLiteral("operationSection.geometry"))->isVisible());
    CHECK(w.op()->heights().size() == 5);
    for (const char* key : {"pattern", "direction", "stepover", "overhang", "stepdown"})
        CHECK(w.shown(QStringLiteral("schemaRow.%1").arg(QLatin1String(key)))->isVisible());
    CHECK_FALSE(w.find(QStringLiteral("schemaRow.cutMode"))->isVisible()); // one way only (x-relevantWhen)
    // The picker offers the facing tools (x-toolTypes); a 10 mm flat end mill from the library.
    REQUIRE(QTest::qWaitFor([&] { return pickerList(w)->count() > 0; }, 10000));
    CHECK(pickerTypes(w) ==
          std::set<QString>{QStringLiteral("faceMill"), QStringLiteral("flatEndMill"), QStringLiteral("bullEndMill")});
    chooseTool(w, QStringLiteral("3f550375-46eb-47d0-9b8c-4fedbcb8466d"), QStringLiteral("10 mm 4FL"));
    w.click(QStringLiteral("operationUseCalculated"));
    REQUIRE(QTest::qWaitFor([&] { return w.op()->feedsState() == QStringLiteral("calculated"); }, 5000));
    // Two passes of 0.5 mm: the stock top (Z 0 at the origin, its top front left corner) down to the plate's top.
    w.typeInto(QStringLiteral("schemaInput.stepdown"), QStringLiteral("0.5 mm"));
    w.window()->contentItem()->forceActiveFocus();
    QCoreApplication::processEvents();
    w.previewed("ok");
    const auto tp = w.op()->previewToolpath();
    REQUIRE(tp != nullptr);
    std::set<Nm> levels;
    for (const cam::Move& m : tp->moves) {
        if (m.role == cam::MoveRole::Cut)
            levels.insert(m.z);
    }
    CHECK(levels == std::set<Nm>{-1'000'000, -500'000});
    REQUIRE(QTest::qWaitFor(
        [&] {
            const auto shown = w.display()->shown();
            return std::any_of(shown.begin(), shown.end(), [](const ToolpathDisplay::Shown& s) { return s.preview; });
        },
        10000));
    REQUIRE(QTest::qWaitFor([&] { return statusLine(w) == QStringLiteral("Ready: OK adds the operation"); }, 10000));
    w.view()->fitAll();
    w.settle();
    w.shotsBothThemes(QStringLiteral("t052_face"));

    // OK: the operation with the facing parameters, up to date in the tree.
    const QString id = w.op()->operationId();
    w.click(QStringLiteral("operationOk"));
    REQUIRE(QTest::qWaitFor([&] { return !w.op()->active() && w.undoCount() == undoBefore + 1; }, kTimeoutMs));
    const doc::DocStatePtr state = w.document()->state();
    REQUIRE(state->cam);
    REQUIRE(state->cam->operations.size() == 1);
    const cam::Operation& op = state->cam->operations.begin()->second;
    CHECK(op.type == "face");
    CHECK(op.geometry.empty());
    CHECK(op.paramsJson.find("\"stepdown\":0.5") != std::string::npos);
    const int row = w.tree()->rowOf(id);
    REQUIRE(row >= 0);
    w.waitStatus(row, "upToDate");
    w.checkNoWarnings();
}

// After M3: the panel opens on drill, then on the type last added; the type list in its fixed order; each height says
// what it is measured from (x-heightsUsed); a facing bottom above the top (it is measured from the part top) is the
// type's to judge, so 0.2 mm above the part with the top at 0 previews and is added, and a bottom that leaves nothing
// to face shows face's own message. Screenshot with RAY_T045_SHOTS: m3_height_hints_dark / _light.
TEST_CASE("the panel: drill first, then the last type; heights from their references; a facing bottom above the top",
          "[cam][e2e][heights]")
{
    RAY_REQUIRE_RHI();
    Window w;
    w.open(kPlate); // metric
    w.toWorkspace(ui::kCamWorkspace);
    // A setup whose stock is 3 mm wider on every side and 1 mm above the plate (the stock top is Z 0).
    const std::string args = R"({"id":")" + ct::id('a', 1) + R"(","name":"Setup 1","partBodies":[")" +
                             w.bodyId().toStdString() + R"("],"machine":")" + ct::id('9', 1) + R"(","material":")" +
                             kAl6061 +
                             R"(","stock":{"type":"box","xNeg":3,"xPos":3,"yNeg":3,"yPos":3,"zNeg":0,"zPos":1},)"
                             R"("library":{"machines":[)" +
                             ct::machineJson(1) + "],\"materials\":[" + libraryMaterial(w, kAl6061) + "]}}";
    REQUIRE(w.invoke("cam.setup.add", args));
    REQUIRE(QTest::qWaitFor([&] { return w.tree()->setupCount() >= 1; }, kTimeoutMs));

    // Nothing added yet in this session: the panel opens on drill. The types in the panel's order.
    newOperation(w, 0, QString());
    CHECK(w.op()->type() == QStringLiteral("drill"));
    QStringList names;
    for (const QVariant& t : w.op()->types())
        names << t.toMap().value(QStringLiteral("name")).toString();
    REQUIRE(names.size() >= 9);
    CHECK(names.mid(0, 9) ==
          QStringList({QStringLiteral("drill"), QStringLiteral("face"), QStringLiteral("contour2d"),
                       QStringLiteral("pocket2d"), QStringLiteral("adaptive2d"), QStringLiteral("slot"),
                       QStringLiteral("chamfer2d"), QStringLiteral("engrave"), QStringLiteral("threadmill")}));
    QStringList rest = names.mid(9);
    QStringList sorted = rest;
    std::sort(sorted.begin(), sorted.end());
    CHECK(rest == sorted);
    // Drill's heights: from each hole's top, the clearance from the highest.
    CHECK(w.shown(QStringLiteral("operationFieldNote.retract"))->property("text").toString() ==
          QStringLiteral("from each hole's top"));
    CHECK(w.shown(QStringLiteral("operationFieldNote.clearance"))->property("text").toString() ==
          QStringLiteral("from the highest hole top"));

    w.op()->setType(QStringLiteral("face"));
    REQUIRE(w.op()->type() == QStringLiteral("face"));
    QCoreApplication::processEvents();
    for (const char* key : {"clearance", "retract", "feedPlane", "top"})
        CHECK(w.shown(QStringLiteral("operationFieldNote.%1").arg(QLatin1String(key)))->property("text").toString() ==
              QStringLiteral("from stock top"));
    CHECK(w.shown(QStringLiteral("operationFieldNote.bottom"))->property("text").toString() ==
          QStringLiteral("from part top"));
    REQUIRE(QTest::qWaitFor([&] { return pickerList(w)->count() > 0; }, 10000));
    chooseTool(w, QStringLiteral("3f550375-46eb-47d0-9b8c-4fedbcb8466d"), QStringLiteral("10 mm 4FL"));
    w.click(QStringLiteral("operationUseCalculated"));
    REQUIRE(QTest::qWaitFor([&] { return w.op()->feedsState() == QStringLiteral("calculated"); }, 5000));
    w.typeInto(QStringLiteral("schemaInput.stepdown"), QStringLiteral("0.5 mm"));
    // Leave 0.2 mm on the part, the top at 0: faced from Z 0 to the plate's top + 0.2 (Z -0.8) in two passes.
    w.typeInto(QStringLiteral("operationInput.bottom"), QStringLiteral("0.2 mm"));
    w.window()->contentItem()->forceActiveFocus();
    QCoreApplication::processEvents();
    w.previewed("ok");
    CHECK(w.op()->errors().isEmpty());
    const auto tp = w.op()->previewToolpath();
    REQUIRE(tp != nullptr);
    std::set<Nm> levels;
    for (const cam::Move& m : tp->moves) {
        if (m.role == cam::MoveRole::Cut)
            levels.insert(m.z);
    }
    CHECK(levels == std::set<Nm>{-800'000, -400'000});
    REQUIRE(QTest::qWaitFor([&] { return statusLine(w) == QStringLiteral("Ready: OK adds the operation"); }, 10000));
    w.view()->fitAll();
    w.settle();
    w.shotsBothThemes(QStringLiteral("m3_height_hints"));

    // A bottom 1.5 above the part top is above the stock top: nothing to face. The panel does not judge that order
    // itself; face's validation does, and its message is what the panel shows.
    w.typeInto(QStringLiteral("operationInput.bottom"), QStringLiteral("1.5 mm"));
    w.window()->contentItem()->forceActiveFocus();
    REQUIRE(QTest::qWaitFor(
        [&] {
            return !w.op()->previewBusy() && !w.op()->errors().isEmpty() &&
                   w.op()->errors().front().contains(QLatin1String("Nothing to face"));
        },
        kTimeoutMs));
    CHECK_FALSE(w.op()->canCommit());
    REQUIRE(QTest::qWaitFor([&] { return statusLine(w) == QStringLiteral("Fix the error above first"); }, 10000));
    // Two heights from the stock top out of order are the panel's to name, before any preview.
    w.typeInto(QStringLiteral("operationInput.bottom"), QStringLiteral("0.2 mm"));
    w.typeInto(QStringLiteral("operationInput.top"), QStringLiteral("3 mm"));
    REQUIRE(QTest::qWaitFor([&] { return statusLine(w) == QStringLiteral("Feed plane must not be below Top"); }, 5000));
    w.typeInto(QStringLiteral("operationInput.top"), QStringLiteral("0 mm"));
    w.window()->contentItem()->forceActiveFocus();
    REQUIRE(QTest::qWaitFor([&] { return w.op()->canCommit(); }, kTimeoutMs));

    // OK; the next new operation opens on face, the type last added.
    const std::size_t undoBefore = w.undoCount();
    w.click(QStringLiteral("operationOk"));
    REQUIRE(QTest::qWaitFor([&] { return !w.op()->active() && w.undoCount() == undoBefore + 1; }, kTimeoutMs));
    const cam::Operation& op = w.document()->state()->cam->operations.begin()->second;
    CHECK(op.type == "face");
    CHECK(op.heights.bottom == 0.2);
    CHECK(op.heights.top == 0);
    newOperation(w, 0, QString());
    CHECK(w.op()->type() == QStringLiteral("face"));
    w.click(QStringLiteral("operationCancel"));
    REQUIRE(QTest::qWaitFor([&] { return !w.op()->active(); }, 5000));
    w.checkNoWarnings();
}

// T-054: a 2D pocket made in the panel the schema builds (no type-specific QML): a floor face picked in the view, a
// library end mill, calculated feeds, the preview drawn, OK, the tree up to date. With RAY_T054_SHOTS set to a folder,
// the window with the panel and the previewed toolpath, and the committed toolpath, are saved there.
TEST_CASE("a pocket2d operation by the panel: a floor face, a library end mill, the preview, OK", "[cam][e2e][t054]")
{
    RAY_REQUIRE_RHI();
    // closed_pocket_1 (tools/corpus_gen): a block with a closed rectangular pocket with corner radii.
    const QString part = QStringLiteral(RAY_CORPUS_DIR "/raycad-synthetic/closed_pocket_1.stp");
    const QString endMill6 = QStringLiteral("08bc976f-3640-4b04-a389-eda6ba3e659b"); // 6 mm 3FL carbide flat end mill
    Window w;
    w.open(part);
    w.toWorkspace(ui::kCamWorkspace);
    addSetup(w);
    newOperation(w);
    w.op()->setType(QStringLiteral("pocket2d"));
    REQUIRE(w.op()->type() == QStringLiteral("pocket2d"));
    CHECK(w.op()->geometryKind() == QStringLiteral("face"));
    CHECK(w.find(QStringLiteral("operationField.feedPlane"))->isVisible());
    CHECK(w.find(QStringLiteral("operationField.bottom")) == nullptr);
    w.shown(QStringLiteral("schemaCombo.cutMode"));
    w.shown(QStringLiteral("schemaInput.stepover"));
    CHECK_FALSE(w.find(QStringLiteral("schemaRow.wallAllowance"))->isVisible()); // until "Finish the walls"

    // The tool: the picker offers end mills; the 6 mm one.
    REQUIRE(QTest::qWaitFor([&] { return pickerList(w)->count() > 0; }, 10000));
    for (const QString& t : pickerTypes(w))
        CHECK((t == QStringLiteral("flatEndMill") || t == QStringLiteral("bullEndMill")));
    chooseTool(w, endMill6, QStringLiteral("6 mm 3FL"));

    // The floor: the up-facing planar face below the top, picked as a click in the view picks it.
    const kernel::Shape& shape = w.host->scene()->body(0)->shape;
    const Box3 b = kernel::bounds(shape);
    std::optional<std::uint32_t> floor;
    Vec3 at{};
    for (std::uint32_t i = 0; i < shape.count(kernel::SubKind::Face); ++i) {
        const kernel::SubId f{kernel::SubKind::Face, i};
        const auto info = kernel::surfaceInfo(shape, f);
        const auto s = kernel::faceSample(shape, f);
        if (info && s && info->type == kernel::SurfaceType::Plane && s->normal.z > 0.999 &&
            info->bounds.max.z < b.max.z - 1e-6) {
            floor = i;
            at = info->centroid;
        }
    }
    REQUIRE(floor.has_value());
    w.host->selection()->applyClick(
        render::PickHit{w.host->scene()->itemForBody(0), kernel::SubKind::Face, *floor, at, 0}, Qt::NoModifier);
    QCoreApplication::processEvents();
    REQUIRE(QTest::qWaitFor([&] { return w.op()->picks().size() == 1; }, kTimeoutMs));

    // Feeds calculated for the end mill in the setup's 6061; then the preview, generated by the pocket2d type.
    w.click(QStringLiteral("operationUseCalculated"));
    REQUIRE(QTest::qWaitFor([&] { return w.op()->feedsState() == QStringLiteral("calculated"); }, 5000));
    w.previewed("ok");
    REQUIRE(w.op()->previewToolpath() != nullptr);
    const cam::Toolpath& tp = *w.op()->previewToolpath();
    CHECK(tp.moves.size() > 50);
    CHECK(std::none_of(tp.moves.begin(), tp.moves.end(),
                       [](const cam::Move& m) { return m.role == cam::MoveRole::Plunge; }));
    REQUIRE(QTest::qWaitFor(
        [&] {
            const auto shown = w.display()->shown();
            return std::any_of(shown.begin(), shown.end(), [](const ToolpathDisplay::Shown& s) { return s.preview; });
        },
        10000));
    CHECK(w.op()->errors().isEmpty());
    REQUIRE(QTest::qWaitFor([&] { return statusLine(w) == QStringLiteral("Ready: OK adds the operation"); }, 10000));
    const QString dir = qEnvironmentVariable("RAY_T054_SHOTS");
    const auto save = [&](const QString& name) {
        if (dir.isEmpty())
            return;
        QDir().mkpath(dir);
        w.sendMouse(QEvent::MouseMove, QPointF(w.window()->width() / 2.0, w.window()->height() - 4.0), Qt::NoButton,
                    Qt::NoButton);
        QTest::qWait(300);
        w.grab().save(QDir(dir).filePath(name + QStringLiteral(".png")), "PNG");
    };
    w.view()->fitAll();
    w.inView(w.find(QStringLiteral("operationSection.parameters"))); // the type's own parameters in sight
    QTest::qWait(200);
    save(QStringLiteral("t054_pocket2d_panel_preview"));

    // OK: the operation is added and up to date at once (the preview made its toolpath).
    const QString id = w.op()->operationId();
    w.click(QStringLiteral("operationOk"));
    REQUIRE(QTest::qWaitFor([&] { return !w.op()->active(); }, kTimeoutMs));
    const doc::DocStatePtr state = w.document()->state();
    REQUIRE(state->cam);
    REQUIRE(state->cam->operations.size() == 1);
    const cam::Operation& op = state->cam->operations.begin()->second;
    CHECK(op.type == "pocket2d");
    CHECK(op.geometry.size() == 1);
    CHECK(op.paramsJson.find("\"stepover\":45") != std::string::npos); // the defaults are stored
    const int row = w.tree()->rowOf(id);
    REQUIRE(row >= 0);
    w.waitStatus(row, "upToDate");
    save(QStringLiteral("t054_pocket2d_up_to_date"));
    w.checkNoWarnings();
}

// T-061: 2D adaptive roughing made in the panel the schema builds (no type-specific QML), on a pocket with an island
// and on a keyway too narrow for a helix: the floor picked in the view, a library end mill, calculated feeds, the
// preview drawn, OK, the tree up to date, the toolpath shown. Screenshots: build/shots/t061_adaptive2d_<part>.png (the
// panel with the previewed toolpath) and t061_adaptive2d_<part>_up_to_date.png.
TEST_CASE("an adaptive2d operation by the panel: a pocket with an island and a slot, previewed, OK", "[cam][e2e][t061]")
{
    RAY_REQUIRE_RHI();
    struct Part {
        const char* file;
        const char* name;
        QString tool, search;
        bool ramp; // the slot: entered by a ramp, the helix does not fit
    };
    const Part parts[] = {
        // closed_pocket_5 (tools/corpus_gen): a 50 x 30 R3 pocket 12 deep with a round island 10 mm across.
        {"closed_pocket_5.stp", "island", QStringLiteral("08bc976f-3640-4b04-a389-eda6ba3e659b"),
         QStringLiteral("6 mm 3FL"), false},
        // keyway_tslot_3: a 40 mm shaft with a blind keyway 12 mm wide and 5 deep, cut with a 10 mm end mill.
        {"keyway_tslot_3.stp", "slot", QStringLiteral("3f550375-46eb-47d0-9b8c-4fedbcb8466d"),
         QStringLiteral("10 mm 4FL"), true},
    };
    for (const Part& part : parts) {
        INFO("part " << part.file);
        Window w;
        w.open(QStringLiteral(RAY_CORPUS_DIR "/raycad-synthetic/") + QLatin1String(part.file));
        w.toWorkspace(ui::kCamWorkspace);
        addSetup(w);
        newOperation(w, 0, QStringLiteral("adaptive2d"));
        CHECK(w.op()->geometryKind() == QStringLiteral("face"));
        CHECK(w.find(QStringLiteral("operationField.bottom")) == nullptr);
        w.shown(QStringLiteral("schemaInput.stepover"));
        w.shown(QStringLiteral("schemaInput.reducedFeed"));
        w.shown(QStringLiteral("schemaCombo.entry"));
        REQUIRE(QTest::qWaitFor([&] { return pickerList(w)->count() > 0; }, 10000));
        for (const QString& t : pickerTypes(w))
            CHECK((t == QStringLiteral("flatEndMill") || t == QStringLiteral("bullEndMill")));
        chooseTool(w, part.tool, part.search);

        // The floor: the lowest up-facing planar face below the top, picked as a click in the view picks it.
        const kernel::Shape& shape = w.host->scene()->body(0)->shape;
        const Box3 b = kernel::bounds(shape);
        std::optional<std::uint32_t> floor;
        double floorZ = b.max.z;
        Vec3 at{};
        for (std::uint32_t i = 0; i < shape.count(kernel::SubKind::Face); ++i) {
            const kernel::SubId f{kernel::SubKind::Face, i};
            const auto info = kernel::surfaceInfo(shape, f);
            const auto s = kernel::faceSample(shape, f);
            if (info && s && info->type == kernel::SurfaceType::Plane && s->normal.z > 0.999 &&
                info->bounds.max.z < floorZ - 1e-6) {
                floor = i;
                floorZ = info->bounds.max.z;
                at = info->centroid;
            }
        }
        REQUIRE(floor.has_value());
        w.host->selection()->applyClick(
            render::PickHit{w.host->scene()->itemForBody(0), kernel::SubKind::Face, *floor, at, 0}, Qt::NoModifier);
        QCoreApplication::processEvents();
        REQUIRE(QTest::qWaitFor([&] { return w.op()->picks().size() == 1; }, kTimeoutMs));

        w.click(QStringLiteral("operationUseCalculated"));
        REQUIRE(QTest::qWaitFor([&] { return w.op()->feedsState() == QStringLiteral("calculated"); }, 5000));
        w.previewed("ok");
        REQUIRE(w.op()->previewToolpath() != nullptr);
        const cam::Toolpath tp = *w.op()->previewToolpath();
        CHECK(tp.moves.size() > 50);
        CHECK(std::none_of(tp.moves.begin(), tp.moves.end(),
                           [](const cam::Move& m) { return m.role == cam::MoveRole::Plunge; }));
        const auto helixArcs = std::count_if(tp.moves.begin(), tp.moves.end(), [](const cam::Move& m) {
            return m.role == cam::MoveRole::Ramp && m.kind != cam::MoveKind::Linear;
        });
        CHECK((part.ramp ? helixArcs == 0 : helixArcs > 0));
        CHECK(tp.stats.p99EngagementDeg > 0);
        CHECK(w.op()->errors().isEmpty());
        REQUIRE(QTest::qWaitFor(
            [&] {
                const auto shown = w.display()->shown();
                return std::any_of(shown.begin(), shown.end(),
                                   [](const ToolpathDisplay::Shown& s) { return s.preview; });
            },
            10000));
        const QString dir = QStringLiteral(RAY_SHOTS_DIR);
        const auto save = [&](const QString& name) {
            QDir().mkpath(dir);
            w.sendMouse(QEvent::MouseMove, QPointF(w.window()->width() / 2.0, w.window()->height() - 4.0), Qt::NoButton,
                        Qt::NoButton);
            QTest::qWait(300);
            w.grab().save(QDir(dir).filePath(name + QStringLiteral(".png")), "PNG");
        };
        w.view()->fitAll();
        w.inView(w.find(QStringLiteral("operationSection.parameters")));
        QTest::qWait(200);
        save(QStringLiteral("t061_adaptive2d_%1").arg(QLatin1String(part.name)));

        const QString id = w.op()->operationId();
        w.click(QStringLiteral("operationOk"));
        REQUIRE(QTest::qWaitFor([&] { return !w.op()->active(); }, kTimeoutMs));
        const doc::DocStatePtr state = w.document()->state();
        REQUIRE(state->cam);
        REQUIRE(state->cam->operations.size() == 1);
        const cam::Operation& op = state->cam->operations.begin()->second;
        CHECK(op.type == "adaptive2d");
        CHECK(op.paramsJson.find("\"reducedFeed\":50") != std::string::npos); // the defaults are stored
        const int row = w.tree()->rowOf(id);
        REQUIRE(row >= 0);
        w.waitStatus(row, "upToDate");
        w.view()->fitAll();
        w.settle();
        save(QStringLiteral("t061_adaptive2d_%1_up_to_date").arg(QLatin1String(part.name)));
        w.checkNoWarnings();
    }
}

// After M3 (plunge_entry.h): a pocket no helix or ramp fits, the 11 mm counterbore of plate_counterbore_1 cut with a
// 10 mm end mill (the tool centre has a disc 1 mm across: the smallest helix is 3 mm, a ramp along a 3 mm ring is
// shorter than the tool). pocket2d and adaptive2d plunge there with a tool that cuts at its centre, and the warning is
// in the panel's warnings (amber, OK still allowed) and, once added, under the operation's row in the CAM tree with the
// amber mark; adaptive2d's check does not run its engine, so its warning comes from the preview's generation
// (camui::mergedWarnings). The same pocket with a copy of the tool marked as not cutting at its centre is refused, OK
// blocked. Screenshots: build/shots/leftovers_plunge_{warning_panel,warning_tree,refused}_{dark,light}.png.
TEST_CASE("a pocket too small for a helix or a ramp: a plunge warned of in the panel and the tree, or refused",
          "[cam][e2e][plunge]")
{
    RAY_REQUIRE_RHI();
    // plate_counterbore_1 (tools/corpus_gen): an 85 x 55 x 15 plate with four 6.6 mm holes counterbored 11 mm, 6.5
    // deep.
    const QString part = QStringLiteral(RAY_CORPUS_DIR "/raycad-synthetic/plate_counterbore_1.stp");
    const QString endMill10 = QStringLiteral("3f550375-46eb-47d0-9b8c-4fedbcb8466d"); // 10 mm 4FL, centre cutting
    Window w;
    w.open(part);
    w.toWorkspace(ui::kCamWorkspace);
    addSetup(w);

    // A copy of the 10 mm end mill in My library, marked as not cutting at its centre.
    lib::LibraryStack* stack = w.host->app()->libraries();
    const auto roots = stack->roots();
    std::optional<std::size_t> mine;
    for (std::size_t i = 0; i < roots.size(); ++i) {
        if (roots[i].writable)
            mine = i;
    }
    REQUIRE(mine.has_value());
    const lib::Tool* mill = stack->tool(ToolId{Uuid::fromString(endMill10.toStdString())});
    REQUIRE(mill != nullptr);
    REQUIRE(mill->cutsAtCentre());
    lib::Tool noCentre = *mill;
    noCentre.id = ToolId{Uuid::generate()};
    noCentre.name = "Plunge test 10 mm end mill, no centre cutting";
    noCentre.centreCutting = false;
    REQUIRE_FALSE(noCentre.cutsAtCentre());
    const QString noCentreId = QString::fromStdString(noCentre.id.value.toString());
    {
        const QString dir = QString::fromStdString(roots[*mine].dir) + QStringLiteral("/tools");
        REQUIRE(QDir().mkpath(dir));
        QFile f(dir + QStringLiteral("/") + noCentreId + QStringLiteral(".tool.json"));
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(QByteArray::fromStdString(api::cam_tools::toolJson(noCentre)));
    }

    // A counterbore's floor: an up-facing planar face below the plate's top.
    const auto pickFloor = [&] {
        const kernel::Shape& shape = w.host->scene()->body(0)->shape;
        const Box3 b = kernel::bounds(shape);
        std::optional<std::uint32_t> floor;
        Vec3 at{};
        for (std::uint32_t i = 0; i < shape.count(kernel::SubKind::Face) && !floor; ++i) {
            const kernel::SubId f{kernel::SubKind::Face, i};
            const auto info = kernel::surfaceInfo(shape, f);
            const auto s = kernel::faceSample(shape, f);
            if (info && s && info->type == kernel::SurfaceType::Plane && s->normal.z > 0.999 &&
                info->bounds.max.z < b.max.z - 1e-6 && info->bounds.min.z > b.min.z + 1e-6) {
                floor = i;
                at = s->point;
            }
        }
        REQUIRE(floor.has_value());
        w.host->selection()->applyClick(
            render::PickHit{w.host->scene()->itemForBody(0), kernel::SubKind::Face, *floor, at, 0}, Qt::NoModifier);
        QCoreApplication::processEvents();
        REQUIRE(QTest::qWaitFor([&] { return w.op()->picks().size() == 1; }, kTimeoutMs));
    };
    const QString plunged = QStringLiteral("Plunges straight down at ");
    const QString advice = QStringLiteral("no helix or ramp fits there. Check that the tool cuts at its centre");
    const auto plungeWarning = [&](const QStringList& list) {
        for (const QString& s : list) {
            if (s.contains(plunged) && s.contains(advice))
                return s;
        }
        return QString();
    };
    const auto saveBothThemes = [&](const QString& name) {
        const QString dir = QStringLiteral(RAY_SHOTS_DIR);
        QDir().mkpath(dir);
        const QString was = w.host->theme()->property("mode").toString();
        for (const char* mode : {"dark", "light"}) {
            w.host->theme()->setProperty("mode", QLatin1String(mode));
            w.sendMouse(QEvent::MouseMove, QPointF(w.window()->width() / 2.0, w.window()->height() - 4.0), Qt::NoButton,
                        Qt::NoButton);
            QTest::qWait(300);
            w.grab().save(QDir(dir).filePath(name + QStringLiteral("_%1.png").arg(QLatin1String(mode))), "PNG");
        }
        w.host->theme()->setProperty("mode", was);
    };

    for (const QString& type : {QStringLiteral("pocket2d"), QStringLiteral("adaptive2d")}) {
        INFO("type " << type.toStdString());
        newOperation(w, 0, type);
        REQUIRE(QTest::qWaitFor([&] { return pickerList(w)->count() > 0; }, 10000));
        chooseTool(w, endMill10, QStringLiteral("10 mm 4FL"));
        pickFloor();
        w.click(QStringLiteral("operationUseCalculated"));
        REQUIRE(QTest::qWaitFor([&] { return w.op()->feedsState() == QStringLiteral("calculated"); }, 5000));
        w.previewed("ok");
        REQUIRE(w.op()->previewToolpath() != nullptr);
        const cam::Toolpath& tp = *w.op()->previewToolpath();
        CHECK(std::any_of(tp.moves.begin(), tp.moves.end(),
                          [](const cam::Move& m) { return m.role == cam::MoveRole::Plunge; }));
        QStringList generated;
        for (const std::string& s : tp.warnings)
            generated << QString::fromStdString(s);
        REQUIRE_FALSE(plungeWarning(generated).isEmpty());
        // In the panel: listed with the other warnings, amber, and OK is not blocked.
        REQUIRE(QTest::qWaitFor([&] { return !plungeWarning(w.op()->warnings()).isEmpty(); }, kTimeoutMs));
        const QString warning = plungeWarning(w.op()->warnings());
        CHECK(warning == plungeWarning(generated));
        const QStringList shownWarnings = w.op()->warnings();
        CHECK(shownWarnings.count(warning) == 1); // the check's and the generation's, listed once
        const int at = static_cast<int>(shownWarnings.indexOf(warning));
        QQuickItem* label = w.shown(QStringLiteral("operationWarning.%1").arg(at));
        CHECK(label->property("text").toString() == warning);
        CHECK(w.op()->errors().isEmpty());
        REQUIRE(QTest::qWaitFor([&] { return w.op()->canCommit(); }, kTimeoutMs));
        REQUIRE(
            QTest::qWaitFor([&] { return statusLine(w) == QStringLiteral("Ready: OK adds the operation"); }, 10000));
        w.view()->fitAll();
        w.inView(label);
        QTest::qWait(200);
        if (type == QStringLiteral("pocket2d"))
            saveBothThemes(QStringLiteral("leftovers_plunge_warning_panel"));

        // OK: up to date, and the tree has the warning under the row, with the amber mark.
        const QString id = w.op()->operationId();
        w.click(QStringLiteral("operationOk"));
        REQUIRE(QTest::qWaitFor([&] { return !w.op()->active(); }, kTimeoutMs));
        const int row = w.tree()->rowOf(id);
        REQUIRE(row >= 0);
        w.waitStatus(row, "upToDate");
        REQUIRE(QTest::qWaitFor(
            [&] { return w.tree()->get(row).value(QStringLiteral("warnings")).toString().contains(warning); },
            kTimeoutMs));
        CHECK(w.tree()->get(row).value(QStringLiteral("statusText")).toString() == QStringLiteral("Up to date"));
        CHECK(w.tree()->get(row).value(QStringLiteral("message")).toString().isEmpty());
        QQuickItem* details = w.shown(QStringLiteral("camRowDetails.%1").arg(row));
        CHECK(details->property("text").toString().contains(warning));
        CHECK(w.shown(QStringLiteral("camRowWarning.%1").arg(row))->isVisible());
        // The setup's row shows none (its badge is its operations' worst status).
        CHECK(w.tree()->get(0).value(QStringLiteral("warnings")).toString().isEmpty());
    }
    w.view()->fitAll();
    w.settle();
    saveBothThemes(QStringLiteral("leftovers_plunge_warning_tree"));

    // The tool that does not cut at its centre: refused, with the reason, and OK blocked.
    for (const QString& type : {QStringLiteral("pocket2d"), QStringLiteral("adaptive2d")}) {
        INFO("type " << type.toStdString() << ", a tool that does not cut at its centre");
        newOperation(w, 0, type);
        REQUIRE(QTest::qWaitFor([&] { return pickerList(w)->count() > 0; }, 10000));
        chooseTool(w, noCentreId, QStringLiteral("plunge test"));
        pickFloor();
        w.click(QStringLiteral("operationUseCalculated"));
        REQUIRE(QTest::qWaitFor([&] { return w.op()->feedsState() == QStringLiteral("calculated"); }, 5000));
        // pocket2d names the cause (linking's "the tool does not cut at its centre, so it cannot plunge"); adaptive2d's
        // refusal lists the entries it tried, and says nothing of the plunge it did not (src/cam, not changed here).
        const QLatin1String cause = type == QStringLiteral("pocket2d") ? QLatin1String("does not cut at its centre")
                                                                       : QLatin1String("no entry into the area");
        const bool refused = QTest::qWaitFor(
            [&] {
                return !w.op()->previewBusy() && !w.op()->errors().isEmpty() &&
                       w.op()->errors().join(QLatin1Char(' ')).contains(cause);
            },
            kTimeoutMs);
        INFO("errors: " << w.op()->errors().join(QStringLiteral(" | ")).toStdString());
        REQUIRE(refused);
        CHECK(plungeWarning(w.op()->warnings()).isEmpty());
        CHECK_FALSE(w.op()->canCommit());
        REQUIRE(QTest::qWaitFor([&] { return statusLine(w) == QStringLiteral("Fix the error above first"); }, 10000));
        if (type == QStringLiteral("pocket2d")) {
            w.inView(w.shown(QStringLiteral("operationOk")));
            saveBothThemes(QStringLiteral("leftovers_plunge_refused"));
        }
        w.click(QStringLiteral("operationCancel"));
        REQUIRE(QTest::qWaitFor([&] { return !w.op()->active(); }, 5000));
    }
    w.checkNoWarnings();
}

// ---------- After M3: the gaps left in the panel, closed ----------
// Screenshots with RAY_T045_SHOTS set: gaps_<n>_<what>_dark, the window in the dark theme.
namespace {

// The up-facing planar face of body 0 at the top of its box.
std::uint32_t topFace(const Window& w)
{
    const kernel::Shape& s = w.host->scene()->body(0)->shape;
    const Box3 b = kernel::bounds(s);
    std::optional<std::uint32_t> found;
    for (std::uint32_t i = 0; i < s.count(kernel::SubKind::Face) && !found; ++i) {
        const kernel::SubId f{kernel::SubKind::Face, i};
        const auto info = kernel::surfaceInfo(s, f);
        const auto sample = kernel::faceSample(s, f);
        if (info && sample && info->type == kernel::SurfaceType::Plane && sample->normal.z > 0.999 &&
            std::abs(info->bounds.max.z - b.max.z) < 1e-6)
            found = i;
    }
    if (!found)
        FAIL("no top face");
    return found.value_or(0);
}

const QString kMill3175 = QStringLiteral("bd50ea0a-cb02-4e9c-8223-3a18c9d59197"); // 1/8 in 2FL carbide flat end mill

} // namespace

// Gap 1: a type that takes several kinds of geometry (contour2d: edges and faces). The selection filter takes both,
// the list shows the picks by kind under a heading each, the count names both; the plate's top face (its outer loop)
// and a hole's rim edge make one operation with two contours.
TEST_CASE("a contour from a face and an edge: picks of two kinds, listed by kind", "[cam][e2e][gaps]")
{
    RAY_REQUIRE_RHI();
    Window w;
    w.open(kPlate); // metric; 6.6 mm through holes, the first at (30, 26.667), top at Z 10
    w.toWorkspace(ui::kCamWorkspace);
    addSetup(w);
    newOperation(w, 0, QStringLiteral("contour2d"));
    REQUIRE(w.op()->geometryKinds() == QStringList({QStringLiteral("edge"), QStringLiteral("face")}));
    CHECK(w.op()->pickCountText() == QStringLiteral("No edges or faces picked"));
    CHECK(w.shown(QStringLiteral("operationGeometryPrompt"))
              ->property("text")
              .toString()
              .contains(QLatin1String("faces")));
    CHECK(w.shown(QStringLiteral("operationFieldNote.bottom"))->property("text").toString() ==
          QStringLiteral("from the picked edges and faces"));
    REQUIRE(QTest::qWaitFor([&] { return pickerList(w)->count() > 0; }, 10000));
    chooseTool(w, kMill3175, QStringLiteral("1/8 in 2FL"));

    // A face first, then an edge: both are taken; the list puts the edge under "Edges" and the face under "Faces".
    pickElement(w, kernel::SubKind::Face, topFace(w));
    REQUIRE(QTest::qWaitFor([&] { return w.op()->picks().size() == 1; }, kTimeoutMs));
    CHECK(w.op()->pickCountText() == QStringLiteral("1 face"));
    pickElement(w, kernel::SubKind::Edge, circleEdge(w, 30, 26.666666667, 3.3, 10));
    REQUIRE(QTest::qWaitFor([&] { return w.op()->picks().size() == 2; }, kTimeoutMs));
    CHECK(w.op()->pickCountText() == QStringLiteral("1 edge, 1 face"));
    const QVariantList picks = w.op()->picks();
    CHECK(picks[0].toMap().value(QStringLiteral("kind")).toString() == QStringLiteral("edge"));
    CHECK(picks[0].toMap().value(QStringLiteral("index")).toInt() == 1); // the second pick, listed first
    CHECK(picks[0].toMap().value(QStringLiteral("heading")).toString() == QStringLiteral("Edges"));
    CHECK(picks[1].toMap().value(QStringLiteral("kind")).toString() == QStringLiteral("face"));
    CHECK(picks[1].toMap().value(QStringLiteral("heading")).toString() == QStringLiteral("Faces"));
    CHECK(w.shown(QStringLiteral("operationPickHeading.edge"))->property("text").toString() == QStringLiteral("Edges"));
    CHECK(w.shown(QStringLiteral("operationPickHeading.face"))->property("text").toString() == QStringLiteral("Faces"));
    // The same face again is refused with the reason.
    pickElement(w, kernel::SubKind::Face, topFace(w));
    REQUIRE(QTest::qWaitFor([&] { return statusLine(w) == QStringLiteral("That face is picked already"); }, 5000));
    CHECK(w.op()->picks().size() == 2);

    // 3 mm down from each pick; no leads.
    w.op()->form()->setValue(QStringLiteral("leadType"), QStringLiteral("none"));
    w.typeInto(QStringLiteral("operationInput.bottom"), QStringLiteral("-3"));
    w.typeInto(QStringLiteral("operationInput.rpm"), QStringLiteral("12000"));
    w.typeInto(QStringLiteral("operationInput.feedCut"), QStringLiteral("600"));
    w.window()->contentItem()->forceActiveFocus();
    QCoreApplication::processEvents();
    w.previewed("ok");
    const std::shared_ptr<const cam::Toolpath> tp = w.op()->previewToolpath();
    REQUIRE(tp != nullptr);
    // Around the plate (the face's outer loop, the tool centre 1.5875 mm outside X 0) and inside the hole (its rim).
    bool outside = false, inHole = false;
    for (const cam::Move& m : tp->moves) {
        if (m.role != cam::MoveRole::Cut)
            continue;
        outside = outside || m.x == -1'587'500;
        const double dx = m.x / 1e6 - 30, dy = m.y / 1e6 - 26.666666667;
        inHole = inHole || std::hypot(dx, dy) < 3.3 - 1.5;
    }
    CHECK(outside);
    CHECK(inHole);
    REQUIRE(QTest::qWaitFor(
        [&] {
            const auto shown = w.display()->shown();
            return std::any_of(shown.begin(), shown.end(), [](const ToolpathDisplay::Shown& s) { return s.preview; });
        },
        10000));
    w.inView(w.shown(QStringLiteral("operationSection.geometry")));
    w.view()->fitAll();
    w.settle();
    w.shot(QStringLiteral("gaps_1_picks_by_kind_dark"));
    w.click(QStringLiteral("operationOk"));
    REQUIRE(QTest::qWaitFor([&] { return !w.op()->active(); }, kTimeoutMs));
    const cam::Operation& op = w.document()->state()->cam->operations.begin()->second;
    REQUIRE(op.geometry.size() == 2);
    CHECK(op.geometry[0].kind == kernel::SubKind::Face); // stored in the order picked
    CHECK(op.geometry[1].kind == kernel::SubKind::Edge);

    // Edited again: the picks come back with their kinds.
    w.op()->edit(QString::fromStdString(op.id.value.toString()));
    REQUIRE(QTest::qWaitFor([&] { return w.op()->active() && w.op()->picks().size() == 2; }, kTimeoutMs));
    CHECK(w.op()->pickCountText() == QStringLiteral("1 edge, 1 face"));
    w.click(QStringLiteral("operationCancel"));
    REQUIRE(QTest::qWaitFor([&] { return !w.op()->active(); }, 5000));
    w.checkNoWarnings();
}

// Gap 2: a list of points on the setup's XY plane (pocket2d's pre-drilled holes), in an inch document: clicks in the
// view add points (a hole's face snaps to its centre, a vertex to itself), the list shows them in inches, a point is
// added and typed, a bad coordinate is named, one is removed, the points are drawn in the view; stored in mm.
TEST_CASE("a list of points: picked in the view, typed in inches, drawn", "[cam][e2e][gaps]")
{
    RAY_REQUIRE_RHI();
    Window w;
    w.openInNewDocument(kPlate); // an inch document; the plate's holes at (30, 26.667) ... mm
    w.toWorkspace(ui::kCamWorkspace);
    addSetup(w);
    newOperation(w, 0, QStringLiteral("pocket2d"));
    const QString key = QStringLiteral("preDrillPoints");
    w.inView(w.shown(QStringLiteral("schemaPointPick.") + key));
    CHECK(w.shown(QStringLiteral("schemaPointCount.") + key)->property("text").toString() == QStringLiteral("None"));
    CHECK(w.find(QStringLiteral("schemaInput.") + key) == nullptr); // no text field: the list
    // Picking points: faces and vertices, and the geometry picker stops.
    w.click(QStringLiteral("schemaPointPick.") + key);
    REQUIRE(w.op()->pointPickKey() == key);
    CHECK_FALSE(w.op()->picking());
    CHECK(w.host->selection()->filterMask() ==
          static_cast<std::uint8_t>(static_cast<std::uint8_t>(render::PickFilter::Faces) |
                                    static_cast<std::uint8_t>(render::PickFilter::Vertices)));
    REQUIRE(QTest::qWaitFor([&] { return w.op()->pointFrameReady() && w.op()->holeCount() == 6; }, kTimeoutMs));

    // A face of a hole: its centre.
    const api::cam_operation::Hole hole = w.op()->holes()[0];
    w.host->selection()->applyClick(
        render::PickHit{w.host->scene()->itemForBody(0), kernel::SubKind::Face, hole.boreFace, hole.top, 0},
        Qt::NoModifier);
    QCoreApplication::processEvents();
    REQUIRE(QTest::qWaitFor([&] { return w.op()->form()->pointsOf(key).size() == 1; }, kTimeoutMs));
    CHECK(w.op()->form()->pointsOf(key)[0].first == Approx(hole.x).margin(1e-6));
    CHECK(w.op()->form()->pointsOf(key)[0].second == Approx(hole.y).margin(1e-6));
    // Shown in the document's unit.
    QQuickItem* x0 = w.shown(QStringLiteral("schemaPointInput.%1.0.x").arg(key));
    CHECK(x0->property("text").toString().endsWith(QLatin1String(" in")));
    // A vertex, clicked in the view as a user clicks it: a top corner of the plate not under the panel. The frame is
    // the setup's (from the hole: WCS = model + its offset, the setup being axis-aligned).
    const double dx = hole.x - hole.top.x, dy = hole.y - hole.top.y;
    const kernel::Shape& shape = w.host->scene()->body(0)->shape;
    const Box3 b = kernel::bounds(shape);
    std::optional<Vec3> corner;
    for (const Vec3 c : {Vec3{b.max.x, b.max.y, b.max.z}, Vec3{b.max.x, b.min.y, b.max.z},
                         Vec3{b.min.x, b.max.y, b.max.z}, Vec3{b.min.x, b.min.y, b.max.z}}) {
        if (!corner && w.freeOfPanel(w.sceneOf(c)))
            corner = c;
    }
    REQUIRE(corner.has_value());
    w.grab();
    w.clickScene(w.sceneOf(*corner));
    REQUIRE(QTest::qWaitFor([&] { return w.op()->form()->pointsOf(key).size() == 2; }, kTimeoutMs));
    CHECK(w.op()->form()->pointsOf(key)[1].first == Approx(corner->x + dx).margin(1e-6));
    CHECK(w.op()->form()->pointsOf(key)[1].second == Approx(corner->y + dy).margin(1e-6));
    // Drawn: a cross per point.
    REQUIRE(QTest::qWaitFor([&] { return w.op()->pointMarks().size() == 8; }, 5000));
    w.click(QStringLiteral("schemaPointPick.") + key); // stop
    CHECK(w.op()->pointPickKey().isEmpty());

    // Added with the button and typed in inches; a coordinate that does not read is named; one removed.
    w.click(QStringLiteral("schemaPointAdd.") + key);
    REQUIRE(QTest::qWaitFor([&] { return w.find(QStringLiteral("schemaPoint.%1.2").arg(key)) != nullptr; }, 5000));
    w.typeInto(QStringLiteral("schemaPointInput.%1.2.x").arg(key), QStringLiteral("1"));
    w.typeInto(QStringLiteral("schemaPointInput.%1.2.y").arg(key), QStringLiteral("0.5"));
    CHECK(w.op()->form()->pointsOf(key)[2] == std::pair<double, double>(25.4, 12.7));
    w.typeInto(QStringLiteral("schemaPointInput.%1.1.y").arg(key), QStringLiteral("a"));
    REQUIRE(QTest::qWaitFor([&] { return w.shown(QStringLiteral("schemaError.") + key)->isVisible(); }, 5000));
    CHECK_THAT(w.find(QStringLiteral("schemaError.") + key)->property("text").toString().toStdString(),
               ContainsSubstring("Point 2, Y"));
    CHECK_FALSE(w.op()->canCommit());
    w.inView(w.shown(QStringLiteral("schemaPointInput.%1.1.y").arg(key)));
    w.shot(QStringLiteral("gaps_2_point_list_dark"));
    w.click(QStringLiteral("schemaPointRemove.%1.1").arg(key));
    REQUIRE(QTest::qWaitFor([&] { return w.op()->form()->pointsOf(key).size() == 2; }, 5000));
    CHECK_FALSE(w.find(QStringLiteral("schemaError.") + key)->isVisible());
    const QString stored = w.op()->form()->params().value(key).toString();
    CHECK(stored.endsWith(QLatin1String("; 25.4 12.7"))); // mm, whatever the display unit
    REQUIRE(QTest::qWaitFor([&] { return w.op()->pointMarks().size() == 8; }, 5000));
    w.view()->fitAll();
    w.settle();
    w.shot(QStringLiteral("gaps_2_points_drawn_dark"));
    w.click(QStringLiteral("operationCancel"));
    REQUIRE(QTest::qWaitFor([&] { return !w.op()->active(); }, 5000));
    CHECK(w.op()->pointMarks().empty()); // the marks go with the panel
    w.checkNoWarnings();
}

// Gap 2, through to the toolpath: a pocket entered at a pre-drilled point clicked on its floor; the operation stores
// the point in mm as pocket2d reads it, and shows it again when edited.
TEST_CASE("a pocket entered at a point clicked in the view", "[cam][e2e][gaps]")
{
    RAY_REQUIRE_RHI();
    const QString part = QStringLiteral(RAY_CORPUS_DIR "/raycad-synthetic/closed_pocket_1.stp");
    const QString endMill6 = QStringLiteral("08bc976f-3640-4b04-a389-eda6ba3e659b");
    Window w;
    w.open(part);
    w.toWorkspace(ui::kCamWorkspace);
    addSetup(w);
    newOperation(w, 0, QStringLiteral("pocket2d"));
    REQUIRE(QTest::qWaitFor([&] { return pickerList(w)->count() > 0; }, 10000));
    chooseTool(w, endMill6, QStringLiteral("6 mm 3FL"));
    const kernel::Shape& shape = w.host->scene()->body(0)->shape;
    const Box3 b = kernel::bounds(shape);
    std::optional<std::uint32_t> floor;
    Vec3 at{};
    for (std::uint32_t i = 0; i < shape.count(kernel::SubKind::Face); ++i) {
        const kernel::SubId f{kernel::SubKind::Face, i};
        const auto info = kernel::surfaceInfo(shape, f);
        const auto s = kernel::faceSample(shape, f);
        if (info && s && info->type == kernel::SurfaceType::Plane && s->normal.z > 0.999 &&
            info->bounds.max.z < b.max.z - 1e-6) {
            floor = i;
            at = info->centroid;
        }
    }
    REQUIRE(floor.has_value());
    const auto clickFloor = [&] {
        w.host->selection()->applyClick(
            render::PickHit{w.host->scene()->itemForBody(0), kernel::SubKind::Face, *floor, at, 0}, Qt::NoModifier);
        QCoreApplication::processEvents();
    };
    clickFloor(); // the floor, as geometry
    REQUIRE(QTest::qWaitFor([&] { return w.op()->picks().size() == 1; }, kTimeoutMs));
    const QString key = QStringLiteral("preDrillPoints");
    w.op()->setPointPicking(key);
    REQUIRE(QTest::qWaitFor([&] { return w.op()->pointFrameReady(); }, kTimeoutMs));
    clickFloor(); // the same face, as a point: where it was clicked
    REQUIRE(QTest::qWaitFor([&] { return w.op()->form()->pointsOf(key).size() == 1; }, kTimeoutMs));
    CHECK(w.op()->picks().size() == 1); // not a second floor
    const auto [px, py] = w.op()->form()->pointsOf(key)[0];
    w.op()->setPointPicking(QString());
    w.op()->form()->setValue(QStringLiteral("entry"), QStringLiteral("preDrill"));
    w.op()->form()->setValue(QStringLiteral("preDrillDiameter"), QStringLiteral("8 mm"));
    w.click(QStringLiteral("operationUseCalculated"));
    REQUIRE(QTest::qWaitFor([&] { return w.op()->feedsState() == QStringLiteral("calculated"); }, 5000));
    w.previewed("ok");
    REQUIRE(w.op()->previewToolpath() != nullptr);
    // The pocket starts at the pre-drilled point (pocket2d.h ENTRY): a move there.
    const Nm nx = toNm(px), ny = toNm(py);
    const auto& moves = w.op()->previewToolpath()->moves;
    CHECK(std::any_of(moves.begin(), moves.end(),
                      [&](const cam::Move& m) { return std::llabs(m.x - nx) <= 1 && std::llabs(m.y - ny) <= 1; }));
    w.inView(w.shown(QStringLiteral("schemaPoint.%1.0").arg(key)));
    w.shot(QStringLiteral("gaps_2_pocket_predrill_dark"));
    w.click(QStringLiteral("operationOk"));
    REQUIRE(QTest::qWaitFor([&] { return !w.op()->active(); }, kTimeoutMs));
    const cam::Operation& op = w.document()->state()->cam->operations.begin()->second;
    const QJsonObject params = QJsonDocument::fromJson(QByteArray::fromStdString(op.paramsJson)).object();
    QString unused;
    const auto stored = ui::camui::parsePoints(params.value(key).toString(), &unused);
    REQUIRE(stored.has_value());
    REQUIRE(stored->size() == 1);
    CHECK(stored->front().x.toDouble() == Approx(px).margin(1e-6));
    // Edited again: the point is listed.
    w.op()->edit(QString::fromStdString(op.id.value.toString()));
    REQUIRE(QTest::qWaitFor([&] { return w.op()->active(); }, kTimeoutMs));
    CHECK(w.op()->form()->pointsOf(key).size() == 1);
    REQUIRE(QTest::qWaitFor([&] { return w.op()->pointMarks().size() == 4; }, kTimeoutMs));
    w.click(QStringLiteral("operationCancel"));
    REQUIRE(QTest::qWaitFor([&] { return !w.op()->active(); }, 5000));
    w.checkNoWarnings();
}

// Gap 3: a percentage (x-unit "percent"): pocket2d's stepover shows "45 %" under the title "Stepover", takes "50%",
// and says its limits in percent.
TEST_CASE("a percentage in the form: shown, typed, out of range", "[cam][e2e][gaps]")
{
    RAY_REQUIRE_RHI();
    Window w;
    w.openInNewDocument(kPlate); // inches: a percentage does not follow the length unit
    w.toWorkspace(ui::kCamWorkspace);
    addSetup(w);
    newOperation(w, 0, QStringLiteral("pocket2d"));
    QQuickItem* input = w.shown(QStringLiteral("schemaInput.stepover"));
    w.inView(input);
    CHECK(input->property("text").toString() == QStringLiteral("45 %"));
    CHECK(w.find(QStringLiteral("schemaLabel.stepover"))->property("text").toString() == QStringLiteral("Stepover"));
    w.typeInto(QStringLiteral("schemaInput.stepover"), QStringLiteral("50%"));
    CHECK(w.op()->form()->params().value(QStringLiteral("stepover")).toDouble() == 50.0);
    w.typeInto(QStringLiteral("schemaInput.helixDiameter"), QStringLiteral("500"));
    QQuickItem* error = w.shown(QStringLiteral("schemaError.helixDiameter"));
    CHECK(error->property("text").toString() == QStringLiteral("Must be at most 400 %"));
    CHECK_FALSE(w.op()->canCommit());
    w.inView(w.shown(QStringLiteral("schemaInput.helixDiameter")));
    w.shot(QStringLiteral("gaps_3_percent_dark"));
    w.click(QStringLiteral("schemaReset.helixDiameter"));
    REQUIRE(QTest::qWaitFor([&] { return !w.find(QStringLiteral("schemaError.helixDiameter"))->isVisible(); }, 5000));
    CHECK(w.find(QStringLiteral("schemaInput.helixDiameter"))->property("text").toString() == QStringLiteral("90 %"));
    w.click(QStringLiteral("operationCancel"));
    REQUIRE(QTest::qWaitFor([&] { return !w.op()->active(); }, 5000));
    w.checkNoWarnings();
}

// Gap 4: the starter single-profile thread mill has its neck (4.2 mm to 20 mm from the tip), so it threads an M8x1.25
// through the whole 10 mm plate in the real panel (before, the 6 mm "shank" 1.5 mm up refused anything past the
// flutes).
TEST_CASE("the starter single-profile thread mill threads M8 the plate's full depth", "[cam][e2e][gaps]")
{
    RAY_REQUIRE_RHI();
    Window w;
    w.open(kPlate); // 6.6 mm through holes, 10 mm deep
    w.toWorkspace(ui::kCamWorkspace);
    addSetup(w);
    newOperation(w, 0, QStringLiteral("threadmill"));
    const QString single = QStringLiteral("15348ad9-df0b-4ded-9fbb-5383bb22e0b7"); // tool.threadmill.single60.6mm
    REQUIRE(QTest::qWaitFor([&] { return pickerList(w)->count() > 0; }, 10000));
    chooseTool(w, single, QStringLiteral("single-profile"));
    pickElement(w, kernel::SubKind::Face, cylinderFace(w, 30, 26.666666667, 3.3));
    REQUIRE(QTest::qWaitFor([&] { return w.op()->picks().size() == 1; }, kTimeoutMs));
    w.op()->form()->setValue(QStringLiteral("form"), QStringLiteral("single"));
    w.op()->form()->setValue(QStringLiteral("pitch"), QStringLiteral("1.25 mm"));
    w.op()->form()->setValue(QStringLiteral("diameter"), QStringLiteral("8 mm"));
    w.op()->setValue(QStringLiteral("rpm"), QStringLiteral("6000"));
    w.op()->setValue(QStringLiteral("feedCut"), QStringLiteral("300"));
    w.previewed("ok");
    CHECK(w.op()->errors().isEmpty());
    REQUIRE(w.op()->previewToolpath() != nullptr);
    // The helix from the bottom of the through hole (Z 0) up through its 10 mm: 8 turns of 1.25 mm, in halves.
    Nm lowest = std::numeric_limits<Nm>::max(), highest = std::numeric_limits<Nm>::min();
    for (const cam::Move& m : w.op()->previewToolpath()->moves) {
        if (m.role == cam::MoveRole::Cut && (m.kind == cam::MoveKind::ArcCW || m.kind == cam::MoveKind::ArcCCW)) {
            lowest = std::min(lowest, m.z);
            highest = std::max(highest, m.z);
        }
    }
    CHECK(highest - lowest >= 9'000'000);
    CHECK(helicalArcs(*w.op()->previewToolpath()) >= 16);
    w.click(QStringLiteral("operationCancel"));
    REQUIRE(QTest::qWaitFor([&] { return !w.op()->active(); }, 5000));
    w.checkNoWarnings();
}

// Gap 5: engraving text of two lines: the text area (x-multiline) takes Enter as a line break, the line spacing is a
// percentage, each line is centred on its own; the preview has both lines and OK stores the line break.
TEST_CASE("engraving two lines of text typed in the panel's text area", "[cam][e2e][gaps]")
{
    RAY_REQUIRE_RHI();
    Window w;
    w.open(kPlate); // top at Z 10, holes at Y 26.667 and 53.333
    w.toWorkspace(ui::kCamWorkspace);
    addSetup(w);
    newOperation(w, 0, QStringLiteral("engrave"));
    w.op()->chooseTool(kSpot10);
    REQUIRE(QTest::qWaitFor([&] { return w.op()->toolId() == kSpot10; }, 5000));
    QQuickItem* area = w.shown(QStringLiteral("schemaInput.text"));
    CHECK(area->inherits("QQuickTextEdit")); // several lines, not a line field
    // Typed as a user types it: a line, Enter, the next line (QTest has no key for the character '\n' itself).
    w.typeInto(QStringLiteral("schemaInput.text"), QStringLiteral("RayCAD"));
    QTest::keyClick(w.window(), Qt::Key_Return);
    for (const QChar c : QStringLiteral("T-056"))
        QTest::keyClick(w.window(), c.toLatin1());
    QCoreApplication::processEvents();
    CHECK(w.op()->form()->params().value(QStringLiteral("text")).toString() == QStringLiteral("RayCAD\nT-056"));
    CHECK(area->property("text").toString() == QStringLiteral("RayCAD\nT-056"));
    CHECK(w.op()->active()); // Enter was a line break, not OK
    QQuickItem* spacing = w.shown(QStringLiteral("schemaInput.lineSpacing"));
    CHECK(spacing->property("text").toString() == QStringLiteral("160 %"));
    w.typeInto(QStringLiteral("schemaInput.lineSpacing"), QStringLiteral("150"));
    w.op()->form()->setValue(QStringLiteral("textAlign"), QStringLiteral("centre"));
    w.op()->form()->setValue(QStringLiteral("textHeight"), QStringLiteral("5 mm"));
    w.op()->form()->setValue(QStringLiteral("textX"), QStringLiteral("60 mm"));
    w.op()->form()->setValue(QStringLiteral("textY"), QStringLiteral("42 mm"));
    w.op()->form()->setValue(QStringLiteral("depth"), QStringLiteral("0.3 mm"));
    w.op()->setValue(QStringLiteral("rpm"), QStringLiteral("6000"));
    w.op()->setValue(QStringLiteral("feedCut"), QStringLiteral("300"));
    w.op()->setValue(QStringLiteral("feedPlunge"), QStringLiteral("100"));
    w.window()->contentItem()->forceActiveFocus();
    w.previewed("ok");
    REQUIRE(w.op()->previewToolpath() != nullptr);
    // Cuts on both baselines: Y 42 and 42 - 1.5 x 5 = Y 34.5 (WCS from the plate's corner).
    bool first = false, second = false;
    for (const cam::Move& m : w.op()->previewToolpath()->moves) {
        if (m.role == cam::MoveRole::Cut) {
            first = first || m.y == 42'000'000;
            second = second || m.y == 34'500'000;
        }
    }
    CHECK(first);
    CHECK(second);
    REQUIRE(QTest::qWaitFor(
        [&] {
            const auto shown = w.display()->shown();
            return std::any_of(shown.begin(), shown.end(), [](const ToolpathDisplay::Shown& s) { return s.preview; });
        },
        10000));
    w.inView(w.find(QStringLiteral("schemaRow.text")));
    w.view()->setCamera(w.view()->framed(w.view()->camera(), Box3{{38, 28, 0}, {82, 50, 10}}));
    w.shot(QStringLiteral("gaps_5_multiline_text_dark"));
    w.click(QStringLiteral("operationOk"));
    REQUIRE(QTest::qWaitFor([&] { return !w.op()->active(); }, kTimeoutMs));
    const cam::Operation& op = w.document()->state()->cam->operations.begin()->second;
    CHECK(op.paramsJson.find("\"text\":\"RayCAD\\nT-056\"") != std::string::npos);
    CHECK(op.paramsJson.find("\"lineSpacing\":150") != std::string::npos);
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
