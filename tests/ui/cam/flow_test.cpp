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
#include "units.h"
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
#include <QtGui/QAccessible>
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

    explicit Window(QSize size = QSize(1440, 900))
    {
        AppOptions options;
        options.theme = QStringLiteral("light");
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

// Every list (ComboBox) and check box under `root` that is shown: a check box's label is never elided (it wraps); a
// list's text is elided only where its layout cannot give it more room (at its widthLimit), and its open list is as
// wide as its longest choice, within the window. Returns how many were checked.
int checkNoCutText(const Window& w, QQuickItem* root, const QString& where)
{
    REQUIRE(root != nullptr);
    std::vector<QQuickItem*> items{root};
    for (std::size_t i = 0; i < items.size(); ++i) {
        for (QQuickItem* c : items[i]->childItems())
            items.push_back(c);
    }
    int checked = 0;
    const double margin = w.host->theme()->property("spacingS").toDouble();
    for (QQuickItem* item : items) {
        const bool combo = item->inherits("QQuickComboBox");
        const bool check = item->inherits("QQuickCheckBox");
        if ((!combo && !check) || !item->isVisible())
            continue;
        auto* label = qvariant_cast<QQuickItem*>(item->property("contentItem"));
        REQUIRE(label != nullptr);
        ++checked;
        const QString text = combo ? item->property("displayText").toString() : item->property("text").toString();
        INFO(where.toStdString() << ": " << (combo ? "list " : "check box ") << item->objectName().toStdString() << " '"
                                 << text.toStdString() << "', " << item->width() << " wide");
        const bool cut = label->property("truncated").toBool();
        if (check) {
            CHECK_FALSE(cut);
            continue;
        }
        if (cut) {
            INFO("its limit " << item->property("widthLimit").toDouble());
            CHECK(item->width() >= item->property("widthLimit").toDouble() - 0.5);
        }
        auto* popup = qvariant_cast<QObject*>(item->property("popup"));
        REQUIRE(popup != nullptr);
        const double open = popup->property("width").toDouble();
        const double widest = item->property("widestText").toDouble();
        INFO("open list " << open << " wide, longest choice " << widest);
        CHECK(open <= w.window()->width() + 0.5);
        CHECK((open >= widest || open >= w.window()->width() - 2 * margin - 1));
    }
    return checked;
}

// Every button under `root` that shows no text (an icon alone) says what it does: a tooltip and an accessible name.
// Returns how many such buttons there were.
int checkIconButtons(QQuickItem* root, const QString& where)
{
    REQUIRE(root != nullptr);
    std::vector<QQuickItem*> items{root};
    for (std::size_t i = 0; i < items.size(); ++i) {
        for (QQuickItem* c : items[i]->childItems())
            items.push_back(c);
    }
    int icons = 0;
    for (QQuickItem* item : items) {
        if (!item->inherits("QQuickAbstractButton") || !item->isVisible() || item->inherits("QQuickCheckBox") ||
            item->inherits("QQuickItemDelegate") || !item->property("text").toString().isEmpty())
            continue;
        ++icons;
        QAccessibleInterface* a = QAccessible::queryAccessibleInterface(item);
        const QString name = a != nullptr ? a->text(QAccessible::Name) : QString();
        INFO(where.toStdString() << ": icon button " << item->objectName().toStdString() << " (icon '"
                                 << item->property("iconName").toString().toStdString() << "')");
        CHECK_FALSE(item->property("toolTipText").toString().isEmpty());
        CHECK_FALSE(name.isEmpty());
    }
    return icons;
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
    // A metric file opens as a metric document (new documents default to inches here), and the status line says so.
    CHECK(w.document()->state()->settings.units == UnitSystem::Metric);
    CHECK(w.host->context()->statusText().contains(
        QStringLiteral("millimetres, the unit plate_thru_holes_1.stp declares")));
    w.view()->fitAll();
    w.shot("imported");

    // ---- 2. The CAM tab ----
    w.click(QStringLiteral("workspaceTab.%1").arg(ui::kCamWorkspace));
    REQUIRE(QTest::qWaitFor([&] { return w.host->context()->workspace() == ui::kCamWorkspace; }, 5000));
    w.shown(QStringLiteral("camTreeEmpty"));
    w.shot("cam_empty");

    // ---- 3. A new setup: the top face, an origin, the machine, the material ----
    const render::Camera beforeSetupPanel = w.view()->camera();
    w.click(QStringLiteral("camTreeEmptyNewSetup"));
    REQUIRE(QTest::qWaitFor([&] { return w.setupPanel()->active() && !w.setupPanel()->previewBusy(); }, kTimeoutMs));
    // The tree's own New setup button goes while the panel makes one (the panel is where it goes on).
    CHECK_FALSE(w.find(QStringLiteral("camTreeEmptyNewSetup"))->isVisible());
    CHECK(w.shown(QStringLiteral("camTreeEmptyText"))->property("text").toString().contains(QStringLiteral("panel")));
    // The stock and the WCS triad with its labels lie in the part of the view no panel or bar covers: framed there
    // when they were not (the panel covers the left of the view; the Z arrow ran into the selection filter bar).
    const auto frameOf = [&] {
        const QRectF f = w.view()->frameRect();
        return f.isEmpty() ? QRectF(0, 0, w.view()->width(), w.view()->height()) : f;
    };
    const auto inFrame = [&](Vec3 p, double margin) {
        const auto v = ui::nav::project(w.view()->camera(), p, w.view()->width(), w.view()->height());
        return v.has_value() && frameOf().adjusted(margin, margin, -margin, -margin).contains(*v);
    };
    const auto setupInView = [&] {
        const auto& p = w.setupPanel()->lastPreview();
        if (!p)
            return false;
        for (const Vec3& m : p->markers) {
            if (!inFrame(m, 0))
                return false;
        }
        for (QQuickItem* label :
             {w.find(QStringLiteral("setupAxisLabel.X")), w.find(QStringLiteral("setupAxisLabel.Y")),
              w.find(QStringLiteral("setupAxisLabel.Z"))}) {
            if (label == nullptr || !frameOf().contains(label->mapRectToItem(w.view(), label->boundingRect())))
                return false;
        }
        return true;
    };
    {
        // The frame excludes the panel and the bar.
        QQuickItem* panel = w.shown(QStringLiteral("setupPanel"));
        CHECK(frameOf().left() >= panel->mapRectToItem(w.view(), panel->boundingRect()).right());
        QQuickItem* bar = w.shown(QStringLiteral("selectionFilter"));
        CHECK(frameOf().top() >= bar->mapRectToItem(w.view(), bar->boundingRect()).bottom() - 0.5);
    }
    REQUIRE(QTest::qWaitFor([&] { return w.settled() && setupInView(); }, kTimeoutMs));
    CHECK(w.view()->camera().target.x != beforeSetupPanel.target.x); // it was not in view: framed anew
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
    CHECK(checkIconButtons(w.shown(QStringLiteral("camWorkspace")), QStringLiteral("setup panel")) >= 4);
    CHECK(setupInView());
    w.shot("setup_panel");
    w.click(QStringLiteral("setupOk"));
    REQUIRE(QTest::qWaitFor([&] { return !w.setupPanel()->active(); }, kTimeoutMs));
    w.settle();
    REQUIRE(QTest::qWaitFor([&] { return w.tree()->setupCount() == 1; }, 5000));
    const cam::Setup setup = w.document()->state()->cam->setups.front();
    CHECK(setup.machine == MachineId{Uuid::fromString(kVmc.toStdString())});
    CHECK(setup.material == MaterialId{Uuid::fromString(kAl6061.toStdString())});
    CHECK(setup.postId == "haas-ngc");
    // The new setup is current, and its WCS stays in the view after OK: a small triad, labelled with its work offset.
    {
        const bool shown = QTest::qWaitFor(
            [&] {
                return w.setupPanel()->shownWcs().has_value() && w.setupPanel()->wcsLines().count() == 3 &&
                       w.setupPanel()->wcsLabel().value(QStringLiteral("text")).toString() == QStringLiteral("G54");
            },
            kTimeoutMs);
        INFO("current row " << w.tree()->currentIndex() << ", WCS resolved " << w.setupPanel()->shownWcs().has_value()
                            << ", lines " << w.setupPanel()->wcsLines().count() << ", label '"
                            << w.setupPanel()->wcsLabel().value(QStringLiteral("text")).toString().toStdString()
                            << "'");
        REQUIRE(shown);
    }
    CHECK(w.shown(QStringLiteral("setupWcsLabel"))->property("text").toString() == QStringLiteral("G54"));
    w.shot("setup_added");

    // ---- 4. A drill operation: holes clicked in the view, a library tool from the picker, calculated feeds ----
    w.clickRow(QStringLiteral("setupRow.0"));
    const render::Camera beforeOperationPanel = w.view()->camera();
    w.click(QStringLiteral("camRowNewOperation.0"));
    REQUIRE(QTest::qWaitFor([&] { return w.op()->active(); }, 5000));
    // T-055: drill is no longer the only built-in type (the panel starts with the first registered, by name).
    if (w.op()->type() != QStringLiteral("drill"))
        w.op()->setType(QStringLiteral("drill"));
    CHECK(w.op()->type() == QStringLiteral("drill"));
    CHECK(w.op()->materialSource() == QStringLiteral("setup"));
    // Opening the panel moves no camera; Fit then frames the part beside it, in the part of the view nothing covers.
    QTest::qWait(100);
    CHECK(w.view()->camera().eye.x == beforeOperationPanel.eye.x);
    CHECK(w.view()->camera().target.y == beforeOperationPanel.target.y);
    const auto partInFrame = [&](const char* beside) {
        QQuickItem* panel = w.shown(QLatin1String(beside));
        const QRectF covered = panel->mapRectToItem(w.view(), panel->boundingRect());
        const QRectF frame = w.view()->frameRect();
        INFO(beside << " covers " << covered.left() << ".." << covered.right() << "; frame " << frame.left() << ".."
                    << frame.right() << " x " << frame.top() << ".." << frame.bottom());
        CHECK_FALSE(frame.intersects(covered));
        const Box3 b = kernel::bounds(w.host->scene()->body(0)->shape);
        for (int k = 0; k < 8; ++k) {
            const Vec3 c{(k & 1) ? b.max.x : b.min.x, (k & 2) ? b.max.y : b.min.y, (k & 4) ? b.max.z : b.min.z};
            const auto v = ui::nav::project(w.view()->camera(), c, w.view()->width(), w.view()->height());
            REQUIRE(v.has_value());
            INFO("corner " << k << " at " << v->x() << ", " << v->y());
            CHECK(frame.adjusted(-1, -1, 1, 1).contains(*v));
        }
    };
    w.view()->fitAll();
    w.settle();
    partInFrame("operationPanel");
    w.shot("operation_panel");
    // The picker's Library button (labelled): the library as a large dialog inset over the shell; Esc closes it and the
    // picker has the focus again; "Use this tool" there picks the selected tool here.
    {
        auto* tools = w.singleton<ui::ToolLibraryModel>("CamTools");
        QQuickItem* library = w.shown(QStringLiteral("toolPickerOpenLibrary"));
        CHECK(library->property("text").toString() == QStringLiteral("Library"));
        QQuickItem* search = w.shown(QStringLiteral("toolPickerSearch"));
        w.click(library);
        REQUIRE(QTest::qWaitFor([&] { return tools->shown() && tools->choosing(); }, 5000));
        {
            const QRectF r = sceneRect(w.shown(QStringLiteral("toolLibraryBackground")));
            const double inset = w.host->theme()->property("controlHeight").toDouble() * 2;
            INFO("library " << r.left() << "," << r.top() << " " << r.width() << " x " << r.height() << " in "
                            << w.window()->width() << " x " << w.window()->height());
            CHECK(r.left() >= inset - 1);
            CHECK(r.top() >= inset - 1);
            CHECK(r.right() <= w.window()->width() - inset + 1);
            CHECK(r.bottom() <= w.window()->height() - inset + 1);
        }
        w.shot("tool_library");
        CHECK(checkIconButtons(w.shown(QStringLiteral("toolLibraryBackground"))->parentItem(),
                               QStringLiteral("tool library")) >= 2);
        if (QGuiApplication::focusWindow() != w.window())
            ray::test::focusWindow(w.window());
        QTest::keyClick(w.window(), Qt::Key_Escape);
        REQUIRE(QTest::qWaitFor([&] { return !tools->shown() && !tools->choosing(); }, 5000));
        CHECK(w.op()->active()); // Esc closed the library, not the panel under it
        REQUIRE(QTest::qWaitFor([&] { return search->hasActiveFocus(); }, 5000));
        w.click(library);
        REQUIRE(QTest::qWaitFor([&] { return tools->shown(); }, 5000));
        REQUIRE(tools->select(QStringLiteral("library"), QStringLiteral("tool"), kDrill6));
        w.click(QStringLiteral("toolUse"));
        REQUIRE(QTest::qWaitFor([&] { return w.op()->toolId() == kDrill6 && !tools->shown(); }, 5000));
        CHECK(search->hasActiveFocus());
    }
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
    // Four holes, each by a real click on its bore wall where the view shows it; the fourth is removed again below.
    REQUIRE(QTest::qWaitFor([&] { return w.op()->holeCount() == 6; }, kTimeoutMs));
    CHECK(w.shown(QStringLiteral("operationPickCount"))->property("text").toString() ==
          QStringLiteral("No holes picked"));
    CHECK(w.host->selection()->toolHighlight().empty());
    std::vector<QPointF> pickedAt; // where each picked hole's bore was clicked
    {
        std::vector<std::string> refused;
        int picked = 0;
        for (std::size_t i = 0; i < w.op()->holeCount() && picked < 4; ++i) {
            const api::cam_operation::Hole h = w.op()->holes()[i];
            const QPointF p = w.sceneOf(farWall(w, h));
            if (!w.clear(p))
                continue;
            const int before = static_cast<int>(w.op()->picks().size());
            w.clickScene(p);
            const bool took = QTest::qWaitFor([&] { return static_cast<int>(w.op()->picks().size()) > before; }, 10000);
            if (took) {
                ++picked;
                pickedAt.push_back(p);
            } else {
                refused.push_back("hole " + std::to_string(i) + " at " + std::to_string(p.x()) + "," +
                                  std::to_string(p.y()) + ": " + w.op()->message().toStdString());
            }
        }
        for (const std::string& r : refused)
            UNSCOPED_INFO("a click on a bore picked no hole: " << r);
        INFO("holes picked by a click in the view: " << picked);
        REQUIRE(picked == 4);
    }
    // Every picked hole is marked in the view by all its faces, in the selection's highlight, and the count stays in
    // the panel's fixed header whatever the list's scroll.
    const auto holeFacesMarked = [&](int expectedPicks) {
        std::vector<render::PickHit> want;
        for (int i = 0; i < expectedPicks; ++i) {
            const std::vector<render::PickHit> h = w.op()->pickHits(i);
            INFO("pick " << i);
            REQUIRE_FALSE(h.empty());
            const auto& holes = w.op()->holes();
            const auto hole = std::find_if(holes.begin(), holes.end(), [&](const api::cam_operation::Hole& x) {
                return x.boreFace == h.front().subIndex;
            });
            REQUIRE(hole != holes.end());
            CHECK(h.size() == hole->faces.size());
            for (const render::PickHit& f : h) {
                CHECK(f.kind == kernel::SubKind::Face);
                CHECK(f.item == w.host->scene()->itemForBody(0));
                CHECK(std::binary_search(hole->faces.begin(), hole->faces.end(), f.subIndex));
            }
            want.insert(want.end(), h.begin(), h.end());
        }
        const std::vector<render::PickHit>& shown = w.host->selection()->toolHighlight();
        REQUIRE(shown.size() == want.size());
        for (std::size_t i = 0; i < want.size(); ++i)
            CHECK((shown[i].item == want[i].item && shown[i].subIndex == want[i].subIndex));
        // What the viewport is asked to draw includes them.
        const std::vector<render::PickHit> drawn = w.host->selection()->hits();
        for (const render::PickHit& f : want)
            CHECK(std::any_of(drawn.begin(), drawn.end(),
                              [&](const render::PickHit& d) { return d.item == f.item && d.subIndex == f.subIndex; }));
    };
    REQUIRE(QTest::qWaitFor([&] { return w.op()->picks().size() == 4 && !w.op()->previewBusy(); }, kTimeoutMs));
    holeFacesMarked(4);
    {
        QQuickItem* count = w.shown(QStringLiteral("operationPickCount"));
        CHECK(count->property("text").toString() == QStringLiteral("4 holes"));
        CHECK(w.shown(QStringLiteral("operationHeader"))->isAncestorOf(count));
        // A picked hole looks marked: its bore wall's pixel differs from what it was before the pick, the pointer away.
        w.sendMouse(QEvent::MouseMove, QPointF(w.window()->width() / 2.0, w.window()->height() - 4.0), Qt::NoButton,
                    Qt::NoButton);
        QTest::qWait(300);
        w.host->selection()->setToolHighlight({});
        const QImage unmarked = w.grab();
        w.op()->setHoveredPick(-1);
        emit w.op()->picksChanged(); // the panel marks them again
        REQUIRE(!w.host->selection()->toolHighlight().empty());
        const QImage marked = w.grab();
        const double dpr = w.window()->effectiveDevicePixelRatio();
        const QPoint at = (pickedAt.front() * dpr).toPoint();
        INFO("pixel at the first picked bore: " << unmarked.pixelColor(at).name().toStdString() << " -> "
                                                << marked.pixelColor(at).name().toStdString());
        CHECK(unmarked.pixel(at) != marked.pixel(at));
    }
    // Removing a pick from the list unmarks its hole at once.
    w.click(QStringLiteral("operationPickRemove.3"));
    REQUIRE(QTest::qWaitFor([&] { return w.op()->picks().size() == 3; }, 5000));
    holeFacesMarked(3);
    CHECK(w.shown(QStringLiteral("operationPickCount"))->property("text").toString() == QStringLiteral("3 holes"));
    // The list row under the pointer marks its hole more strongly (the hover highlight on its bore).
    {
        QQuickItem* pickRow = w.shown(QStringLiteral("operationPick.1"));
        w.inView(pickRow);
        w.sendMouse(QEvent::MouseMove, w.centreOf(pickRow), Qt::NoButton, Qt::NoButton);
        REQUIRE(QTest::qWaitFor([&] { return w.op()->hoveredPick() == 1; }, 5000));
        const auto emphasis = w.host->selection()->toolEmphasis();
        REQUIRE(emphasis.has_value());
        CHECK(emphasis->subIndex == w.op()->pickHits(1).front().subIndex);
        CHECK(w.host->selection()->hoverHighlight().has_value());
        w.shot("hole_row_hovered", true);
        w.sendMouse(QEvent::MouseMove, QPointF(w.window()->width() / 2.0, w.window()->height() - 4.0), Qt::NoButton,
                    Qt::NoButton);
        REQUIRE(QTest::qWaitFor([&] { return w.op()->hoveredPick() == -1; }, 5000));
        CHECK_FALSE(w.host->selection()->toolEmphasis().has_value());
    }
    w.shot("holes_picked");
    w.click(QStringLiteral("operationUseCalculated"));
    REQUIRE(QTest::qWaitFor([&] { return w.op()->feedsState() == QStringLiteral("calculated"); }, 5000));
    REQUIRE(QTest::qWaitFor([&] { return !w.op()->previewBusy() && w.op()->previewStatus() == QStringLiteral("ok"); },
                            kTimeoutMs));
    REQUIRE(w.op()->canCommit());
    // Every icon-only button of the CAM workspace says what it does (here: the operation panel's and the tree's).
    CHECK(checkIconButtons(w.shown(QStringLiteral("camWorkspace")), QStringLiteral("operation panel")) >= 5);
    CHECK(checkIconButtons(w.shown(QStringLiteral("camTree")), QStringLiteral("CAM tree")) >= 3);
    w.shot("drill_ready");
    w.click(QStringLiteral("operationOk"));
    REQUIRE(QTest::qWaitFor([&] { return !w.op()->active(); }, kTimeoutMs));
    REQUIRE(QTest::qWaitFor([&] { return w.tree()->count() == 2; }, 10000));
    w.waitStatus(1, "upToDate");
    const OperationId opId = w.document()->state()->cam->setups.front().operations.front();
    CHECK(w.document()->state()->cam->tools.count(ToolId{Uuid::fromString(kDrill6.toStdString())}) == 1);
    // An operation of the setup current: its setup's WCS is shown.
    REQUIRE(QTest::qWaitFor([&] { return w.tree()->currentOperation().has_value(); }, 5000));
    REQUIRE(QTest::qWaitFor([&] { return w.setupPanel()->wcsLines().count() == 3; }, kTimeoutMs));
    CHECK(w.shown(QStringLiteral("setupWcsLabel"))->property("text").toString() == QStringLiteral("G54"));
    w.shot("tree_up_to_date");

    // ---- 5. Post with haas-ngc into a folder ----
    w.clickRow(QStringLiteral("setupRow.0"));
    w.click(QStringLiteral("camRowPost.0"));
    REQUIRE(QTest::qWaitFor([&] { return w.post()->active(); }, 5000));
    w.postRead();
    CHECK(w.post()->postId() == QStringLiteral("haas-ngc"));
    // The plate is a metric file, so the document is metric and so is the program by default; the dialog says so
    // beside the post's name.
    CHECK(w.post()->values().value(QStringLiteral("units")).toString() == QStringLiteral("metric"));
    CHECK(w.shown(QStringLiteral("postDialogOutput"))->property("text").toString() ==
          QStringLiteral("haas-ngc, writing millimetres (G21)"));
    const QString outDir = QDir::toNativeSeparators(w.out.path() + QStringLiteral("/nc"));
    REQUIRE(QDir().mkpath(outDir));
    w.typeInto(QStringLiteral("postInput.directory"), outDir);
    REQUIRE(QTest::qWaitFor([&] { return w.post()->canPost(); }, 5000));
    checkIconButtons(w.shown(QStringLiteral("camWorkspace")), QStringLiteral("post dialog"));
    {
        // At rest the folder shows its end with "…" in front, the home folder as "~", the whole path in the tooltip;
        // what is written to is the folder as typed.
        QQuickItem* folder = w.shown(QStringLiteral("postInput.directory"));
        w.shown(QStringLiteral("postInput.pattern"))->forceActiveFocus();
        QCoreApplication::processEvents();
        const QString atRest = folder->property("text").toString();
        const QString home = QDir::toNativeSeparators(QDir::homePath());
        INFO("folder at rest '" << atRest.toStdString() << "', shown '" << w.post()->directoryShown().toStdString()
                                << "'");
        CHECK(w.post()->values().value(QStringLiteral("directory")).toString() == outDir);
        CHECK(folder->property("full").toString() == outDir);
        if (outDir.startsWith(home + QDir::separator(), Qt::CaseInsensitive)) {
            CHECK(w.post()->directoryShown() == QStringLiteral("~") + outDir.mid(home.size()));
            CHECK_FALSE(atRest.contains(home, Qt::CaseInsensitive));
        }
        CHECK((atRest.startsWith(QChar(0x2026)) || atRest == w.post()->directoryShown()));
        CHECK(atRest.endsWith(QStringLiteral("\\nc")));
    }
    {
        // Scrolled to its end, the post's properties: the heading with its rows, never alone.
        QQuickItem* flick = w.shown(QStringLiteral("postDialogScroll"));
        const double maxY = std::max(0.0, flick->property("contentHeight").toDouble() - flick->height());
        flick->setProperty("contentY", maxY);
        QCoreApplication::processEvents();
        QQuickItem* heading = w.find(QStringLiteral("postSection.properties"));
        QQuickItem* form = w.find(QStringLiteral("postProperties"));
        REQUIRE((heading != nullptr && form != nullptr));
        const QRectF view = sceneRect(flick);
        if (heading->isVisible() && view.intersects(sceneRect(heading))) {
            CHECK(form->isVisible());
            CHECK(form->height() > 0);
            CHECK(view.intersects(sceneRect(form)));
        }
        // The edges say the body scrolls: more above now, nothing more below.
        CHECK(w.find(QStringLiteral("postDialogScrollEdges")) != nullptr);
        w.shot("post_dialog_end");
        flick->setProperty("contentY", 0);
        QCoreApplication::processEvents();
    }
    w.shot("post_dialog");
    w.click(QStringLiteral("postPost"));
    w.posted("done");
    const QString file = w.post()->resultPath();
    const QByteArray first = readFile(file);
    CHECK((first.contains("G83") || first.contains("G81")));
    CHECK(first.contains("G21"));
    CHECK_FALSE(first.contains("G20"));
    CHECK(QFileInfo(file).fileName() == QStringLiteral("plate_thru_holes_1_Setup_1.nc"));
    w.shot("posted");

    // ---- 6. The viewer: a line highlights its move; a move clicked in the view selects its line ----
    w.click(QStringLiteral("postShowGcode"));
    GcodeModel* g = w.gcode();
    REQUIRE(QTest::qWaitFor([&] { return g->isOpen(); }, 5000));
    w.shown(QStringLiteral("gcodeView"));
    // Fit with the viewer open on the right: the part beside it.
    w.view()->fitAll();
    w.settle();
    partInFrame("gcodeView");
    CHECK(checkIconButtons(w.shown(QStringLiteral("camWorkspace")), QStringLiteral("G-code viewer")) >= 1);
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
    // The view was last fitted beside the G-code viewer, so the panel now covers the part: it is framed beside it.
    w.settle();
    partInFrame("operationPanel");
    w.chooseInCombo(QStringLiteral("schemaCombo.depthFrom"), [&] {
        return w.op()->form()->valueOf(QStringLiteral("depthFrom")).toString() == QStringLiteral("value");
    });
    w.typeInto(QStringLiteral("schemaInput.depth"), QStringLiteral("5 mm"));
    REQUIRE(QTest::qWaitFor([&] { return !w.op()->previewBusy() && w.op()->previewStatus() == QStringLiteral("ok"); },
                            kTimeoutMs));
    {
        // Scrolled: the area's edges under the header and over the footer are drawn (a rule and a fade) while there is
        // more beyond them, so a row cut there reads as scrolled; at the top, none above.
        QQuickItem* flick = w.shown(QStringLiteral("operationScroll"));
        QQuickItem* edges = w.shown(QStringLiteral("operationScrollEdges"));
        QQuickItem* top = Window::findItem(edges, QStringLiteral("scrollEdgeTop"));
        QQuickItem* bottom = Window::findItem(edges, QStringLiteral("scrollEdgeBottom"));
        REQUIRE((top != nullptr && bottom != nullptr));
        REQUIRE(flick->property("contentY").toDouble() > 0);
        CHECK(top->isVisible());
        CHECK(std::abs(sceneRect(top).top() - sceneRect(flick).top()) < 1);
        CHECK(sceneRect(top).top() >= sceneRect(w.shown(QStringLiteral("operationHeader"))).bottom());
        const double more = flick->property("contentHeight").toDouble() - flick->height();
        CHECK(bottom->isVisible() == (flick->property("contentY").toDouble() < more - 0.5));
        CHECK(sceneRect(bottom).bottom() <= sceneRect(w.shown(QStringLiteral("operationFooter"))).top() + 0.5);
        const double y = flick->property("contentY").toDouble();
        flick->setProperty("contentY", 0);
        QCoreApplication::processEvents();
        CHECK_FALSE(top->isVisible());
        flick->setProperty("contentY", y);
        QCoreApplication::processEvents();
    }
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
    // What the tree says is true: toolpaths are not saved with the document, so an operation is "Up to date" only
    // while this session still has its toolpath (the engine's cache by input hash, kept after a close for exactly
    // this), and then it is drawn: the document opens with its first setup current. In a new session it would say
    // "Not generated" (cam_test, "badges"). No toolpath hash is stored with the operation, so nothing is regenerated
    // on open.
    REQUIRE(QTest::qWaitFor([&] { return w.tree()->currentIndex() == 0; }, 10000));
    w.waitStatus(1, "upToDate");
    REQUIRE(QTest::qWaitFor(
        [&] {
            for (const ToolpathDisplay::Shown& s : w.display()->shown())
                if (s.op == opId && s.backplot && s.toolpath && !s.stale)
                    return true;
            return false;
        },
        kTimeoutMs));
    for (const ToolpathDisplay::Shown& s : w.display()->shown()) {
        if (s.op == opId)
            CHECK(s.toolpath->hash().hex() == toolpathHash);
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
    // Saved as "flow": the remembered {doc}_{setup} pattern names the file after the new document name, and the dialog
    // says so before posting (no spaces: whitespace in a field's value becomes '_').
    CHECK(w.post()->values().value(QStringLiteral("pattern")).toString() == QStringLiteral("{doc}_{setup}"));
    REQUIRE(QTest::qWaitFor([&] { return w.post()->fileName() == QStringLiteral("flow_Setup_1.nc"); }, 10000));
    CHECK(w.shown(QStringLiteral("postFileName"))->property("text").toString() == QStringLiteral("flow_Setup_1.nc"));
    // It posts: the toolpath this session has is a real one.
    REQUIRE(QTest::qWaitFor([&] { return w.post()->canPost(); }, kTimeoutMs));
    w.click(QStringLiteral("postPost"));
    REQUIRE(QTest::qWaitFor(
        [&] {
            const QString s = w.post()->state();
            return s == QLatin1String("done") || s == QLatin1String("asking") || s == QLatin1String("failed");
        },
        kTimeoutMs * 2));
    if (w.post()->state() == QLatin1String("asking"))
        w.click(QStringLiteral("postReplace"));
    w.posted("done");
    CHECK(QFileInfo(w.post()->resultPath()).fileName() == QStringLiteral("flow_Setup_1.nc"));
    {
        const QByteArray reposted = readFile(w.post()->resultPath());
        CHECK((reposted.contains("G83") || reposted.contains("G81")));
    }
    w.post()->close();
    // Generated again: the same toolpath.
    CHECK(w.toolpath(opId)->hash().hex() == toolpathHash);
    w.waitStatus(1, "upToDate");
    w.shot("regenerated");
    CHECK(w.confirms == 0);
    w.checkNoWarnings();
}

// Run at 100 %, 150 % and 200 % (ray_ui_cam_flow_scale_*: QT_SCALE_FACTOR with the platform's own scaling off).
TEST_CASE("no list or check box text is cut where it has room, in the CAM panels, at this scale", "[scale]")
{
    RAY_REQUIRE_RHI();
    const QString scale =
        qEnvironmentVariableIsSet("RAY_TEST_SCALE") ? qEnvironmentVariable("RAY_TEST_SCALE") : QStringLiteral("native");
    INFO("scale " << scale.toStdString());
    Window w(QSize(1280, 860));
    REQUIRE(w.host->files()->open(kPlate));
    REQUIRE(QTest::qWaitFor([&] { return w.host->session()->hasDocument() && w.settled(); }, kTimeoutMs));
    w.settle();
    w.click(QStringLiteral("workspaceTab.%1").arg(ui::kCamWorkspace));
    REQUIRE(QTest::qWaitFor([&] { return w.host->context()->workspace() == ui::kCamWorkspace; }, 5000));

    // The setup panel, with the machine of the long name and a material chosen.
    w.click(QStringLiteral("camTreeEmptyNewSetup"));
    REQUIRE(QTest::qWaitFor([&] { return w.setupPanel()->active(); }, kTimeoutMs));
    w.setupPanel()->setValue(QStringLiteral("machine"), kVmc);
    w.setupPanel()->setValue(QStringLiteral("material"), kAl6061);
    {
        // The top face, as the user clicks it (OK needs it).
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
    REQUIRE(QTest::qWaitFor([&] { return !w.setupPanel()->previewBusy() && w.setupPanel()->canCommit(); }, kTimeoutMs));
    QTest::qWait(100);
    CHECK(checkNoCutText(w, w.shown(QStringLiteral("setupPanel")), QStringLiteral("setup panel")) >= 8);
    CHECK(w.shown(QStringLiteral("setupCombo.machine"))
              ->property("displayText")
              .toString()
              .contains(QStringLiteral("VF-2")));
    w.shot(
        QStringLiteral("scale_%1_setup").arg(scale).replace(QLatin1Char('.'), QLatin1Char('_')).toLatin1().constData());
    w.click(QStringLiteral("setupOk"));
    REQUIRE(QTest::qWaitFor([&] { return !w.setupPanel()->active() && w.tree()->setupCount() == 1; }, kTimeoutMs));
    w.settle();

    // The operation panel (the drill: its cycle list, its check boxes).
    w.clickRow(QStringLiteral("setupRow.0"));
    w.click(QStringLiteral("camRowNewOperation.0"));
    REQUIRE(QTest::qWaitFor([&] { return w.op()->active(); }, kTimeoutMs));
    // T-055: drill is no longer the only built-in type (the panel starts with the first registered, by name).
    if (w.op()->type() != QStringLiteral("drill"))
        w.op()->setType(QStringLiteral("drill"));
    REQUIRE(QTest::qWaitFor([&] { return !w.op()->previewBusy(); }, kTimeoutMs));
    QTest::qWait(100);
    CHECK(checkNoCutText(w, w.shown(QStringLiteral("operationPanel")), QStringLiteral("operation panel")) >= 6);
    {
        // The cycle's list says the cycle; the G codes are its description (the open list's second line, the tooltip).
        QQuickItem* cycle = w.shown(QStringLiteral("schemaCombo.cycle"));
        CHECK(cycle->property("displayText").toString() == QStringLiteral("Drill"));
        CHECK(cycle->property("currentDescription").toString().contains(QStringLiteral("G81")));
    }
    w.shot(QStringLiteral("scale_%1_operation")
               .arg(scale)
               .replace(QLatin1Char('.'), QLatin1Char('_'))
               .toLatin1()
               .constData());
    w.op()->cancel();
    REQUIRE(QTest::qWaitFor([&] { return !w.op()->active(); }, 5000));

    // The post dialog.
    w.clickRow(QStringLiteral("setupRow.0"));
    w.click(QStringLiteral("camRowPost.0"));
    REQUIRE(QTest::qWaitFor([&] { return w.post()->active(); }, 5000));
    w.postRead();
    QTest::qWait(100);
    CHECK(checkNoCutText(w, w.shown(QStringLiteral("postDialog")), QStringLiteral("post dialog")) >= 3);
    w.shot(
        QStringLiteral("scale_%1_post").arg(scale).replace(QLatin1Char('.'), QLatin1Char('_')).toLatin1().constData());
    w.post()->close();
    REQUIRE(QTest::qWaitFor([&] { return !w.post()->active(); }, 5000));

    // The tool library, a drill shown in its editor.
    auto* tools = w.singleton<ui::ToolLibraryModel>("CamTools");
    tools->show();
    QQuickItem* background = w.shown(QStringLiteral("toolLibraryBackground"));
    REQUIRE(tools->select(QStringLiteral("library"), QStringLiteral("tool"), kDrill6));
    QTest::qWait(200);
    CHECK(checkNoCutText(w, background->parentItem(), QStringLiteral("tool library")) >= 3);
    w.shot(QStringLiteral("scale_%1_library")
               .arg(scale)
               .replace(QLatin1Char('.'), QLatin1Char('_'))
               .toLatin1()
               .constData());
    tools->hide();
    w.checkNoWarnings();
}

TEST_CASE("A STEP file opened as a new document takes the length unit it declares; an import does not change it",
          "[cam][flow][units]")
{
    RAY_REQUIRE_RHI();
    Window w;
    // New documents are in inches unless the settings say otherwise (here they do not).
    const QString inchPlate = QStringLiteral(RAY_CORPUS_DIR "/raycad-synthetic/plate_thru_holes_4.stp");
    REQUIRE(w.host->files()->open(inchPlate));
    REQUIRE(QTest::qWaitFor([&] { return w.host->session()->hasDocument() && w.settled(); }, kTimeoutMs));
    w.settle();
    CHECK(w.document()->state()->settings.units == UnitSystem::Inch);
    CHECK(w.host->context()->statusText().contains(QStringLiteral("inches, the unit plate_thru_holes_4.stp declares")));
    // A metric file opened anew: a metric document, with the user's metric decimals, said in the status line.
    REQUIRE(w.host->files()->open(kPlate));
    if (w.host->files()->promptVisible())
        w.host->files()->answerPrompt(QStringLiteral("discard")); // the first one's import is an unsaved change
    REQUIRE(QTest::qWaitFor(
        [&] {
            return w.host->session()->hasDocument() && w.settled() &&
                   w.host->session()->documentTitle() == QStringLiteral("plate_thru_holes_1");
        },
        kTimeoutMs));
    w.settle();
    CHECK(w.document()->state()->settings.units == UnitSystem::Metric);
    CHECK(w.document()->state()->settings.decimals == 3);
    CHECK(w.host->context()->statusText().contains(
        QStringLiteral("millimetres, the unit plate_thru_holes_1.stp declares")));
    CHECK(w.singleton<ui::UnitsObject>("Units")->unitLabel() == QStringLiteral("mm"));
    // The inch file imported into that document: the document keeps its unit (it is not new any more).
    w.host->files()->importFile(inchPlate);
    REQUIRE(QTest::qWaitFor([&] { return w.host->scene()->bodyCount() == 2 && w.settled(); }, kTimeoutMs));
    CHECK(w.document()->state()->settings.units == UnitSystem::Metric);
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
