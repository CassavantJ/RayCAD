// The whole of M2 in one window, as a user does it, on the real Qt Quick window with the real application session:
// import the plate with holes, the CAM tab, a new setup (top face, origin, machine, material), a drill operation (holes
// picked by real clicks on their bores in the view, a library tool from the picker, the calculated feeds), up to date,
// posted with haas-ngc into a folder, the G-code viewer both ways (a line highlights its move, a move picks its line);
// then the drill's depth edited, the post blocked with the named reason until the operation is generated again, a
// second post that differs only where the depth is written; saved, closed and opened again with the setup, the
// operation, its embedded tool and the remembered post settings back and the same toolpath hash.
//
// Screenshots of every step, light theme: build/shots/m2_flow_<n>_<step>.png (RAY_M2_FLOW_SHOTS names another folder).
#include "../scene_graph_probe.h"
#include "../window_focus.h"
#include "app_context.h"
#include "app_host.h"
#include "app_options.h"
#include "cam/operation_tool.h"
#include "cam/operation_view.h"
#include "cam/post_tool.h"
#include "cam/post_view.h"
#include "cam/setup_tool.h"
#include "cam/setup_tree.h"
#include "cam/tools_model.h"
#include "files.h"
#include "palette_commands.h"
#include "scene_controller.h"
#include "selection_model.h"
#include "session.h"
#include "theme_object.h"
#include "viewport_item.h"
#include "viewport_navigation.h"

#include <ray/sim.h>
#include <ray_api/cam_operation.h>
#include <ray_api/cam_post.h>
#include <ray_api/viewer.h>

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
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

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace ray;
using ray::ui::AppHost;
using ray::ui::AppOptions;
using ray::ui::GcodeModel;
using ray::ui::OperationToolModel;
using ray::ui::PostToolModel;
using ray::ui::SetupToolModel;
using ray::ui::SetupTreeModel;
using ray::ui::ToolListModel;
using ray::ui::ToolpathDisplay;
namespace camui = ray::ui::camui;

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
// resources/library: the 6 mm HSS-Co jobber drill, 6061-T6, the Generic VMC (its default post is haas-ngc).
const QString kDrill6 = QStringLiteral("3d2ce280-3863-4869-9ef4-b75ed93ce20e");
const QString kAl6061 = QStringLiteral("b72c5171-c258-44d2-91ae-ee049b636621");
const QString kVmc = QStringLiteral("b903ede7-5123-4c26-bc15-466e92fba4c6");

QRectF sceneRect(QQuickItem* i)
{
    return i->mapRectToScene(QRectF(0, 0, i->width(), i->height()));
}

QByteArray readFile(const QString& path)
{
    QFile f(path);
    REQUIRE(f.open(QIODevice::ReadOnly));
    return f.readAll();
}

struct Window {
    QTemporaryDir cache;
    QTemporaryDir userData;
    QTemporaryDir out; // the post's folder and the saved document
    std::unique_ptr<AppHost> host;
    int confirms = 0;
    int shots = 0;

    Window()
    {
        AppOptions options;
        options.theme = QStringLiteral("light");
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
    SetupToolModel* setupPanel() const { return singleton<SetupToolModel>("CamSetup"); }
    OperationToolModel* op() const { return singleton<OperationToolModel>("CamOperation"); }
    PostToolModel* post() const { return singleton<PostToolModel>("CamPost"); }
    GcodeModel* gcode() const { return singleton<GcodeModel>("CamGcode"); }
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
    void sendMouse(QEvent::Type type, QPointF p, Qt::MouseButton button, Qt::MouseButtons buttons) const
    {
        QMouseEvent e(type, p, window()->mapToGlobal(p), button, buttons, Qt::NoModifier);
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
    QPointF centreOf(QQuickItem* item) const { return sceneRect(item).center(); }
    // Scrolls the panel or dialog that holds `item` so it is in view.
    void inView(QQuickItem* item) const
    {
        for (const char* name : {"operationScroll", "postDialogScroll", "setupScroll"}) {
            QQuickItem* flick = find(QLatin1String(name));
            if (flick == nullptr || !flick->isAncestorOf(item))
                continue;
            const double contentY = flick->property("contentY").toDouble();
            const double h = flick->height();
            const double y = item->mapToItem(flick, QPointF(0, 0)).y();
            const double maxY = std::max(0.0, flick->property("contentHeight").toDouble() - h);
            if (y < 0 || y + item->height() > h) {
                const double dpr = window()->effectiveDevicePixelRatio();
                flick->setProperty("contentY", std::round(std::clamp(contentY + y - h / 3, 0.0, maxY) * dpr) / dpr);
            }
            QCoreApplication::processEvents();
        }
    }
    void click(QQuickItem* item) const
    {
        REQUIRE(item != nullptr);
        inView(item);
        grab();
        clickScene(centreOf(item));
    }
    void click(const QString& objectName) const { click(shown(objectName)); }
    // A CAM tree row, clicked on its name (its tool buttons are at its right end).
    QPointF rowNameAt(const QString& row) const
    {
        QQuickItem* name = findItem(shown(row), QStringLiteral("camRowName"));
        REQUIRE(name != nullptr);
        return name->mapToScene(
            QPointF(std::min(name->width(), name->property("contentWidth").toDouble()) / 2, name->height() / 2));
    }
    void clickRow(const QString& row) const
    {
        grab();
        clickScene(rowNameAt(row));
    }
    void doubleClickRow(const QString& row) const
    {
        grab();
        const QPointF c = rowNameAt(row);
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
        QTest::keyClick(window(), Qt::Key_Delete);
        for (const QChar c : text)
            QTest::keyClick(window(), c.toLatin1());
        QCoreApplication::processEvents();
        INFO("typed into " << objectName.toStdString());
        CHECK(field->property("text").toString() == text);
    }
    // Steps a combo box with the arrow keys (down first, then up) until `done` holds, as the user does.
    void chooseInCombo(const QString& objectName, const std::function<bool()>& done) const
    {
        QQuickItem* combo = shown(objectName);
        inView(combo);
        if (QGuiApplication::focusWindow() != window())
            ray::test::focusWindow(window());
        combo->forceActiveFocus();
        for (const Qt::Key key : {Qt::Key_Down, Qt::Key_Up}) {
            for (int i = 0; i < 40 && !done(); ++i) {
                QTest::keyClick(window(), key);
                QTest::qWait(20);
            }
        }
        INFO("combo " << objectName.toStdString());
        REQUIRE(QTest::qWaitFor(done, 5000));
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
    QPointF sceneOf(Vec3 world) const
    {
        const auto v = ui::nav::project(view()->camera(), world, view()->width(), view()->height());
        REQUIRE(v.has_value());
        return view()->mapToScene(*v);
    }
    // Whether a window point is in the view and clear of the panels over it.
    bool clear(QPointF p) const
    {
        for (const char* name : {"operationPanel", "setupPanel", "gcodeView", "postDialog"}) {
            QQuickItem* panel = find(QLatin1String(name));
            if (panel != nullptr && panel->isVisible() && sceneRect(panel).adjusted(-8, -8, 8, 8).contains(p))
                return false;
        }
        return sceneRect(view()).adjusted(40, 80, -200, -40).contains(p);
    }
    QString rowStatus(int row) const { return tree()->get(row).value(QStringLiteral("status")).toString(); }
    bool waitStatus(int row, const char* status) const
    {
        const bool ok = QTest::qWaitFor([&] { return rowStatus(row) == QLatin1String(status); }, kTimeoutMs * 2);
        INFO("row " << row << ": '" << rowStatus(row).toStdString() << "' ("
                    << tree()->get(row).value(QStringLiteral("statusText")).toString().toStdString() << "), wanted '"
                    << status << "'");
        CHECK(ok);
        return ok;
    }
    // A screenshot of this step (light theme), with the pointer resting at the bottom edge unless `keepPointer`.
    void shot(const char* step, bool keepPointer = false)
    {
        QString dir = qEnvironmentVariable("RAY_M2_FLOW_SHOTS");
        if (dir.isEmpty())
            dir = QStringLiteral(RAY_SHOTS_DIR);
        QDir().mkpath(dir);
        if (!keepPointer)
            sendMouse(QEvent::MouseMove, QPointF(window()->width() / 2.0, window()->height() - 4.0), Qt::NoButton,
                      Qt::NoButton);
        QTest::qWait(300);
        const QString name =
            QStringLiteral("m2_flow_%1_%2.png").arg(++shots, 2, 10, QLatin1Char('0')).arg(QLatin1String(step));
        grab().save(QDir(dir).filePath(name), "PNG");
    }
    void checkNoWarnings() const
    {
        for (const QString& warning : host->warnings())
            UNSCOPED_INFO(warning.toStdString());
        CHECK(host->warnings().isEmpty());
    }
    void posted(const char* state) const
    {
        const bool ok = QTest::qWaitFor([&] { return post()->state() == QLatin1String(state); }, kTimeoutMs * 2);
        INFO("post state '" << post()->state().toStdString() << "', status '" << post()->statusText().toStdString()
                            << "'");
        REQUIRE(ok);
    }
    void postRead() const
    {
        REQUIRE(QTest::qWaitFor([&] { return post()->active() && !post()->checking(); }, kTimeoutMs));
        REQUIRE(QTest::qWaitFor([&] { return post()->postState() != QLatin1String("loading"); }, kTimeoutMs));
        INFO("post '" << post()->postId().toStdString() << "': " << post()->postMessage().toStdString());
        REQUIRE(post()->postState() == QLatin1String("ok"));
    }
    std::shared_ptr<const cam::Toolpath> toolpath(const OperationId& id) const
    {
        auto job = host->app()->cam()->generate(document()->state(), document()->build(), id);
        REQUIRE(QTest::qWaitFor([&] { return job.done(); }, kTimeoutMs * 3));
        auto r = job.wait();
        REQUIRE(r);
        return *r;
    }
};

// The picker's list in the operation panel.
ToolListModel* pickerList(const Window& w)
{
    auto* list =
        qobject_cast<ToolListModel*>(w.shown(QStringLiteral("toolPickerView"))->property("model").value<QObject*>());
    REQUIRE(list != nullptr);
    return list;
}

// A point of the hole's bore wall that the view shows: on the far side of the hole from the eye, a little below its
// top (looking down into it, the near wall hides itself and the far one faces the eye).
Vec3 farWall(const Window& w, const api::cam_operation::Hole& h)
{
    const render::Camera c = w.view()->camera();
    Vec3 d{c.target.x - c.eye.x, c.target.y - c.eye.y, 0};
    const double n = std::hypot(d.x, d.y);
    REQUIRE(n > 1e-9);
    d = Vec3{d.x / n, d.y / n, 0};
    const double r = h.diameter / 2;
    return Vec3{h.top.x + 0.85 * r * d.x, h.top.y + 0.85 * r * d.y, h.top.z - 0.25 * h.depth};
}

} // namespace

TEST_CASE("M2 in one window: setup, drill, post, viewer, edit, post again, save and reopen", "[cam][e2e][flow]")
{
    RAY_REQUIRE_RHI();
    Window w;

    // ---- 1. Import the plate ----
    REQUIRE(w.host->files()->open(kPlate));
    REQUIRE(QTest::qWaitFor([&] { return w.host->session()->hasDocument() && w.settled(); }, kTimeoutMs));
    w.settle();
    REQUIRE(w.host->scene()->bodyCount() == 1);
    w.view()->fitAll();
    w.shot("imported");

    // ---- 2. The CAM tab ----
    w.click(QStringLiteral("workspaceTab.%1").arg(ui::kCamWorkspace));
    REQUIRE(QTest::qWaitFor([&] { return w.host->context()->workspace() == ui::kCamWorkspace; }, 5000));
    w.shown(QStringLiteral("camTreeEmpty"));
    w.shot("cam_empty");

    // ---- 3. A new setup: the top face, an origin, the machine, the material ----
    w.click(QStringLiteral("camTreeEmptyNewSetup"));
    REQUIRE(QTest::qWaitFor([&] { return w.setupPanel()->active() && !w.setupPanel()->previewBusy(); }, kTimeoutMs));
    {
        // The top face: a click on the plate's top between the rows of holes, clear of the panel.
        const Box3 b = kernel::bounds(w.host->scene()->body(0)->shape);
        std::optional<QPointF> at;
        for (const double fx : {0.92, 0.85, 0.75, 0.65}) {
            const QPointF s = w.sceneOf(Vec3{b.min.x + fx * (b.max.x - b.min.x), (b.min.y + b.max.y) / 2, b.max.z});
            if (!at && w.clear(s))
                at = s;
        }
        REQUIRE(at.has_value());
        w.clickScene(*at);
        REQUIRE(QTest::qWaitFor([&] { return w.setupPanel()->hasZFace(); }, kTimeoutMs));
    }
    REQUIRE(QTest::qWaitFor([&] { return !w.setupPanel()->previewBusy(); }, kTimeoutMs));
    {
        // The origin: a marker on the previewed stock (top front left, else any in sight).
        QString key;
        for (const QVariant& m : w.setupPanel()->markers()) {
            const QString k = m.toMap().value(QStringLiteral("key")).toString();
            QQuickItem* item = w.find(QStringLiteral("setupMarker.") + k);
            if (item == nullptr || !item->isVisible() || !w.clear(w.centreOf(item)))
                continue;
            if (key.isEmpty() || k == QLatin1String("corner3"))
                key = k;
        }
        REQUIRE_FALSE(key.isEmpty());
        w.click(w.shown(QStringLiteral("setupMarker.") + key));
        REQUIRE(QTest::qWaitFor(
            [&] { return w.setupPanel()->values().value(QStringLiteral("origin")).toString() == key; }, 5000));
    }
    w.chooseInCombo(QStringLiteral("setupCombo.machine"),
                    [&] { return w.setupPanel()->values().value(QStringLiteral("machine")).toString() == kVmc; });
    CHECK(w.setupPanel()->values().value(QStringLiteral("post")).toString() == QStringLiteral("haas-ngc"));
    w.chooseInCombo(QStringLiteral("setupCombo.material"),
                    [&] { return w.setupPanel()->values().value(QStringLiteral("material")).toString() == kAl6061; });
    REQUIRE(QTest::qWaitFor([&] { return !w.setupPanel()->previewBusy() && w.setupPanel()->canCommit(); }, kTimeoutMs));
    w.shot("setup_panel");
    w.click(QStringLiteral("setupOk"));
    REQUIRE(QTest::qWaitFor([&] { return !w.setupPanel()->active(); }, kTimeoutMs));
    w.settle();
    REQUIRE(QTest::qWaitFor([&] { return w.tree()->setupCount() == 1; }, 5000));
    const cam::Setup setup = w.document()->state()->cam->setups.front();
    CHECK(setup.machine == MachineId{Uuid::fromString(kVmc.toStdString())});
    CHECK(setup.material == MaterialId{Uuid::fromString(kAl6061.toStdString())});
    CHECK(setup.postId == "haas-ngc");
    w.shot("setup_added");

    // ---- 4. A drill operation: holes clicked in the view, a library tool from the picker, calculated feeds ----
    w.clickRow(QStringLiteral("setupRow.0"));
    w.click(QStringLiteral("camRowNewOperation.0"));
    REQUIRE(QTest::qWaitFor([&] { return w.op()->active(); }, 5000));
    CHECK(w.op()->type() == QStringLiteral("drill"));
    CHECK(w.op()->materialSource() == QStringLiteral("setup"));
    w.shot("operation_panel");
    // The tool, from the tool library's picker.
    w.typeInto(QStringLiteral("toolPickerSearch"), QStringLiteral("6 mm HSS"));
    int row = -1;
    REQUIRE(
        QTest::qWaitFor([&] { return (row = pickerList(w)->rowOf(QStringLiteral("library"), kDrill6)) >= 0; }, 5000));
    {
        QQuickItem* view = w.shown(QStringLiteral("toolPickerView"));
        QQuickItem* item = nullptr;
        REQUIRE(QTest::qWaitFor(
            [&] {
                QMetaObject::invokeMethod(view, "positionViewAtIndex", Q_ARG(int, row), Q_ARG(int, 4 /* Contain */));
                item = w.find(QStringLiteral("toolPickerRow.%1").arg(row));
                return item != nullptr && item->isVisible() && item->property("itemId").toString() == kDrill6;
            },
            5000));
        w.click(item);
        REQUIRE(QTest::qWaitFor([&] { return w.op()->toolId() == kDrill6; }, 5000));
    }
    // Three holes, each by a real click on its bore wall where the view shows it.
    REQUIRE(QTest::qWaitFor([&] { return w.op()->holeCount() == 6; }, kTimeoutMs));
    {
        std::vector<std::string> refused;
        int picked = 0;
        for (std::size_t i = 0; i < w.op()->holeCount() && picked < 3; ++i) {
            const api::cam_operation::Hole h = w.op()->holes()[i];
            const QPointF p = w.sceneOf(farWall(w, h));
            if (!w.clear(p))
                continue;
            const int before = static_cast<int>(w.op()->picks().size());
            w.clickScene(p);
            const bool took = QTest::qWaitFor([&] { return static_cast<int>(w.op()->picks().size()) > before; }, 10000);
            if (took) {
                ++picked;
            } else {
                refused.push_back("hole " + std::to_string(i) + " at " + std::to_string(p.x()) + "," +
                                  std::to_string(p.y()) + ": " + w.op()->message().toStdString());
            }
        }
        for (const std::string& r : refused)
            UNSCOPED_INFO("a click on a bore picked no hole: " << r);
        INFO("holes picked by a click in the view: " << picked);
        REQUIRE(picked == 3);
    }
    w.shot("holes_picked");
    w.click(QStringLiteral("operationUseCalculated"));
    REQUIRE(QTest::qWaitFor([&] { return w.op()->feedsState() == QStringLiteral("calculated"); }, 5000));
    REQUIRE(QTest::qWaitFor([&] { return !w.op()->previewBusy() && w.op()->previewStatus() == QStringLiteral("ok"); },
                            kTimeoutMs));
    REQUIRE(w.op()->canCommit());
    w.shot("drill_ready");
    w.click(QStringLiteral("operationOk"));
    REQUIRE(QTest::qWaitFor([&] { return !w.op()->active(); }, kTimeoutMs));
    REQUIRE(QTest::qWaitFor([&] { return w.tree()->count() == 2; }, 10000));
    w.waitStatus(1, "upToDate");
    const OperationId opId = w.document()->state()->cam->setups.front().operations.front();
    CHECK(w.document()->state()->cam->tools.count(ToolId{Uuid::fromString(kDrill6.toStdString())}) == 1);
    w.shot("tree_up_to_date");

    // ---- 5. Post with haas-ngc into a folder ----
    w.clickRow(QStringLiteral("setupRow.0"));
    w.click(QStringLiteral("camRowPost.0"));
    REQUIRE(QTest::qWaitFor([&] { return w.post()->active(); }, 5000));
    w.postRead();
    CHECK(w.post()->postId() == QStringLiteral("haas-ngc"));
    const QString outDir = QDir::toNativeSeparators(w.out.path() + QStringLiteral("/nc"));
    REQUIRE(QDir().mkpath(outDir));
    w.typeInto(QStringLiteral("postInput.directory"), outDir);
    REQUIRE(QTest::qWaitFor([&] { return w.post()->canPost(); }, 5000));
    w.shot("post_dialog");
    w.click(QStringLiteral("postPost"));
    w.posted("done");
    const QString file = w.post()->resultPath();
    const QByteArray first = readFile(file);
    CHECK((first.contains("G83") || first.contains("G81")));
    w.shot("posted");

    // ---- 6. The viewer: a line highlights its move; a move clicked in the view selects its line ----
    w.click(QStringLiteral("postShowGcode"));
    GcodeModel* g = w.gcode();
    REQUIRE(QTest::qWaitFor([&] { return g->isOpen(); }, 5000));
    w.shown(QStringLiteral("gcodeView"));
    const camui::GcodeIndex& index = *g->program()->index;
    int moveLine = -1;
    for (int i = 0; i < g->lineCount() && moveLine < 0; ++i) {
        const std::string_view l = index.line(static_cast<std::size_t>(i));
        if (index.moveOf(static_cast<std::size_t>(i)) && (l.find("G83") != l.npos || l.find("G81") != l.npos))
            moveLine = i;
    }
    REQUIRE(moveLine >= 0);
    {
        QQuickItem* list = w.shown(QStringLiteral("gcodeList"));
        QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, moveLine), Q_ARG(int, 4 /* Contain */));
        QQuickItem* line = nullptr;
        REQUIRE(QTest::qWaitFor(
            [&] { return (line = w.find(QStringLiteral("gcodeLine.%1").arg(moveLine))) && line->isVisible(); }, 5000));
        w.clickScene(w.centreOf(line));
    }
    REQUIRE(QTest::qWaitFor([&] { return g->currentLine() == moveLine && g->highlighted(); }, kTimeoutMs));
    const auto hit = g->highlightedHit();
    REQUIRE(hit);
    CHECK(hit->moveIndex == index.moveOf(static_cast<std::size_t>(moveLine))->move);
    w.shot("viewer_line");
    {
        // A real click on a move of the drawn toolpath: a rapid well above the plate, clear of the panels.
        std::shared_ptr<const sim::Backplot> bp;
        for (const ToolpathDisplay::Shown& s : w.display()->shown())
            if (s.item == hit->item)
                bp = s.backplot;
        REQUIRE(bp != nullptr);
        const camui::MoveRef move = *index.moveOf(static_cast<std::size_t>(moveLine));
        std::optional<std::pair<QPointF, std::uint32_t>> spot;
        const auto& v = bp->lineVertices;
        for (std::size_t s = 1; s < v.size() && !spot; ++s) {
            if (v[s].moveIndex != v[s - 1].moveIndex || v[s].moveIndex == 0xFFFFFFFFu)
                continue;
            const Vec3 mid{(v[s].x + v[s - 1].x) / 2.0, (v[s].y + v[s - 1].y) / 2.0, (v[s].z + v[s - 1].z) / 2.0};
            if (mid.z < 7.0)
                continue;
            const auto p = ui::nav::project(w.view()->camera(), mid, w.view()->width(), w.view()->height());
            if (!p)
                continue;
            const QPointF sp = w.view()->mapToScene(*p);
            if (w.clear(sp) && index.linesOf(camui::MoveRef{move.section, v[s].moveIndex}))
                spot = std::make_pair(sp, v[s].moveIndex);
        }
        REQUIRE(spot.has_value());
        g->selectLine(0);
        w.clickScene(spot->first);
        INFO("clicked move " << spot->second);
        REQUIRE(QTest::qWaitFor([&] { return g->pickedFirst() >= 0; }, 10000));
        const auto m = index.moveOf(static_cast<std::size_t>(g->currentLine()));
        REQUIRE(m);
        CHECK(std::abs(static_cast<long>(m->move) - static_cast<long>(spot->second)) <= 1);
        w.shot("viewer_move", true);
    }
    w.click(QStringLiteral("gcodeClose"));
    REQUIRE(QTest::qWaitFor([&] { return !g->isOpen(); }, 5000));

    // ---- 7. The drill's depth edited: stale, the post blocked with the reason, generated again ----
    w.doubleClickRow(QStringLiteral("operationRow.1"));
    REQUIRE(QTest::qWaitFor([&] { return w.op()->active() && w.op()->editing(); }, 5000));
    REQUIRE(QTest::qWaitFor([&] { return !w.op()->previewBusy(); }, kTimeoutMs));
    w.chooseInCombo(QStringLiteral("schemaCombo.depthFrom"), [&] {
        return w.op()->form()->valueOf(QStringLiteral("depthFrom")).toString() == QStringLiteral("value");
    });
    w.typeInto(QStringLiteral("schemaInput.depth"), QStringLiteral("5 mm"));
    REQUIRE(QTest::qWaitFor([&] { return !w.op()->previewBusy() && w.op()->previewStatus() == QStringLiteral("ok"); },
                            kTimeoutMs));
    w.shot("depth_edited");
    w.click(QStringLiteral("operationOk"));
    REQUIRE(QTest::qWaitFor([&] { return !w.op()->active(); }, kTimeoutMs));
    // What the tree shows right after Apply (the panel asks the engine for the toolpath its preview made).
    const QString afterApply = w.rowStatus(1);
    w.shot("after_apply");
    WARN("the operation's status right after Apply: " << afterApply.toStdString());
    w.waitStatus(1, "upToDate");
    // Stale, as a change outside the panel leaves it: the same edit made again with another depth, by the action
    // (an automation, or an undo and redo, does not generate).
    {
        const cam::Operation& o = w.document()->state()->cam->operations.at(opId);
        QJsonObject params = QJsonDocument::fromJson(QByteArray::fromStdString(o.paramsJson)).object();
        params[QStringLiteral("depth")] = 6;
        const QJsonObject args{{QStringLiteral("id"), QString::fromStdString(opId.value.toString())},
                               {QStringLiteral("params"), params}};
        JobHandle<api::ActionResult> job = w.host->app()->actions().invoke(
            "cam.operation.edit", QJsonDocument(args).toJson(QJsonDocument::Compact).toStdString(), api::Caller::User);
        REQUIRE(QTest::qWaitFor([&] { return job.done(); }, kTimeoutMs));
        REQUIRE(job.wait());
    }
    w.waitStatus(1, "stale");
    w.waitStatus(0, "stale"); // the setup shows its worst operation
    w.shot("stale");
    w.clickRow(QStringLiteral("setupRow.0"));
    w.click(QStringLiteral("camRowPost.0"));
    REQUIRE(QTest::qWaitFor([&] { return w.post()->active(); }, 5000));
    w.postRead();
    REQUIRE(QTest::qWaitFor([&] { return w.post()->blockerList().size() == 1; }, kTimeoutMs));
    CHECK(w.post()->blockerList().front().name == w.document()->state()->cam->operations.at(opId).name);
    CHECK(api::cam_post::reasonKey(w.post()->blockerList().front().reason) == "stale");
    CHECK_FALSE(w.post()->canPost());
    CHECK_FALSE(w.shown(QStringLiteral("postPost"))->isEnabled());
    CHECK(w.shown(QStringLiteral("postBlockerName.0"))
              ->property("text")
              .toString()
              .startsWith(QStringLiteral("Drilling")));
    const QString blockedText = w.shown(QStringLiteral("postBlockerMessage.0"))->property("text").toString();
    CHECK_FALSE(blockedText.isEmpty());
    w.shot("post_blocked");
    w.click(QStringLiteral("postGenerate.0"));
    REQUIRE(QTest::qWaitFor([&] { return w.post()->canPost(); }, kTimeoutMs * 2));
    w.waitStatus(1, "upToDate");
    w.click(QStringLiteral("postPost"));
    // The same file name again: the rule (replace, ask) decides; answer Replace when asked.
    REQUIRE(QTest::qWaitFor(
        [&] {
            const QString s = w.post()->state();
            return s == QLatin1String("done") || s == QLatin1String("asking") || s == QLatin1String("failed");
        },
        kTimeoutMs * 2));
    if (w.post()->state() == QLatin1String("asking")) {
        w.shot("post_ask");
        w.click(QStringLiteral("postReplace"));
    }
    w.posted("done");
    CHECK(w.post()->resultPath() == file);
    const QByteArray second = readFile(file);
    w.shot("posted_again");
    // Only the lines that write the depth changed.
    {
        const QList<QByteArray> a = first.split('\n'), b = second.split('\n');
        REQUIRE(a.size() == b.size());
        int changed = 0;
        for (qsizetype i = 0; i < a.size(); ++i) {
            if (a[i] == b[i])
                continue;
            ++changed;
            INFO("line " << i + 1 << ": '" << a[i].trimmed().toStdString() << "' -> '" << b[i].trimmed().toStdString()
                         << "'");
            CHECK(a[i].contains(" Z"));
            CHECK((a[i].startsWith("G83") || a[i].startsWith("G81") || a[i].startsWith("G73") || a[i].contains(" G8")));
        }
        CHECK(changed >= 1);
    }
    w.post()->close();

    // ---- 8. Save, close, open again ----
    const std::shared_ptr<const cam::Toolpath> before = w.toolpath(opId);
    const std::string toolpathHash = before->hash().hex();
    const cam::Operation savedOp = w.document()->state()->cam->operations.at(opId);
    const Hash256 toolHash =
        w.document()->state()->cam->tools.at(ToolId{Uuid::fromString(kDrill6.toStdString())}).item.contentHash();
    const QString saved = QDir(w.out.path()).filePath(QStringLiteral("flow.raycad"));
    w.host->files()->saveAs(saved);
    REQUIRE(QTest::qWaitFor([&] { return !w.host->files()->busy() && QFileInfo::exists(saved); }, kTimeoutMs));
    w.settle();
    w.host->files()->close();
    REQUIRE(QTest::qWaitFor([&] { return !w.host->session()->hasDocument(); }, kTimeoutMs));
    w.shot("closed");
    REQUIRE(w.host->files()->open(saved));
    REQUIRE(QTest::qWaitFor([&] { return w.host->session()->hasDocument() && w.settled(); }, kTimeoutMs));
    w.settle();
    if (w.host->context()->workspace() != ui::kCamWorkspace)
        w.click(QStringLiteral("workspaceTab.%1").arg(ui::kCamWorkspace));
    REQUIRE(QTest::qWaitFor([&] { return w.tree()->count() == 2; }, 10000));
    {
        const doc::DocStatePtr s = w.document()->state();
        REQUIRE(s->cam);
        REQUIRE(s->cam->setups.size() == 1);
        const cam::Setup& back = s->cam->setups.front();
        CHECK(back.id == setup.id);
        CHECK(back.name == setup.name);
        CHECK(back.machine == setup.machine);
        CHECK(back.material == setup.material);
        CHECK(back.postId == "haas-ngc");
        REQUIRE(s->cam->operations.count(opId) == 1);
        const cam::Operation& o = s->cam->operations.at(opId);
        CHECK(o.name == savedOp.name);
        CHECK(o.paramsJson == savedOp.paramsJson);
        CHECK(o.geometry.size() == 3);
        REQUIRE(s->cam->tools.count(ToolId{Uuid::fromString(kDrill6.toStdString())}) == 1);
        CHECK(s->cam->tools.at(ToolId{Uuid::fromString(kDrill6.toStdString())}).item.contentHash() == toolHash);
    }
    w.shot("reopened");
    // The post's settings for the setup are remembered.
    w.clickRow(QStringLiteral("setupRow.0"));
    w.click(QStringLiteral("camRowPost.0"));
    REQUIRE(QTest::qWaitFor([&] { return w.post()->active(); }, 5000));
    w.postRead();
    CHECK(w.post()->postId() == QStringLiteral("haas-ngc"));
    CHECK(w.post()->values().value(QStringLiteral("directory")).toString() == outDir);
    w.shot("post_remembered");
    w.post()->close();
    // Generated again: the same toolpath.
    CHECK(w.toolpath(opId)->hash().hex() == toolpathHash);
    w.waitStatus(1, "upToDate");
    w.shot("regenerated");
    CHECK(w.confirms == 0);
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
