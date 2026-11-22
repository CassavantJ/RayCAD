// The tool library manager (task T-046) in the desktop app, on the real Qt Quick window with the real application
// session and its default library stack (the built-in library, read-only; a shared folder from the settings; the
// user's own library):
//
//   - browse: every root listed with which is read-only; each row says where it comes from, a lock on read-only rows;
//     [wallclock] 5,000 tools in a shared folder: the catalog, every filter keystroke, scrolling to the end and a
//     rescan of all 5,000 files stay within the UI-thread budget of tests/ui (ui_budget.h), and only the visible rows
//     exist as items
//   - filter by words, type, diameter range and material: the rows are exactly those an independent filter over the
//     stack finds, in (diameter, name, id) order, the same every time
//   - create, duplicate, edit and delete tools, holders and cutting presets through the editor; problems beside their
//     fields in the schema's words; lengths in the display unit; built-in items read-only with "Duplicate to my
//     library"
//   - the side view: to scale, in a holder when one is chosen, remade when and only when the geometry changes
//   - a shared library changed underneath (a second LibraryStack on the same folder) or with its file held open by
//     another program: the conflict message, and retry, a copy to my library and discard, with no data lost
//   - tools of the open document: picking a library tool for an operation embeds a copy in the same undo step; the
//     document's group; the mark when the library's original changed shape; update from library
//   - the keyboard alone: Shift+T opens it, list navigation, Enter opens the fields, Ctrl+S saves, Esc reverts and
//     closes; the Tab order; and the command with no document open
//
// Screenshots: with RAY_T046_SHOTS set to a folder, the manager is saved there in both themes (list, editor, conflict).
#include "../scene_graph_probe.h"
#include "../ui_budget.h"
#include "../window_focus.h"
#include "app_context.h"
#include "app_host.h"
#include "app_options.h"
#include "cam/tools_model.h"
#include "engine_test_util.h" // the CAM engine tests' test-only operation type and library JSON (tests/unit/cam)
#include "files.h"
#include "palette_commands.h"
#include "scene_controller.h"
#include "session.h"
#include "theme_object.h"
#include "ui_commands.h"
#include "units.h"
#include "viewport_item.h"

#include <ray_api/cam_tools.h>
#include <ray_api/settings.h>

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
#include <QtQml/QQmlComponent>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickItemGrabResult>
#include <QtQuick/QQuickWindow>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <vector>

template <>
struct Catch::StringMaker<QString> {
    static std::string convert(const QString& s) { return '"' + s.toStdString() + '"'; }
};

using namespace ray;
using ray::ui::AppHost;
using ray::ui::AppOptions;
using ray::ui::ToolLibraryModel;
using ray::ui::ToolListModel;
namespace ct = ray::cam::test;

namespace {

constexpr int kTimeoutMs = 60000;
const QString kPlate = QStringLiteral(RAY_CORPUS_DIR "/raycad-synthetic/plate_thru_holes_1.stp");

QString qs(const std::string& s)
{
    return QString::fromStdString(s);
}

// Measures the longest gap between UI-thread timer ticks while it runs.
struct Watchdog {
    QElapsedTimer clock;
    QTimer tick;
    qint64 last = 0, worst = 0;
    Watchdog()
    {
        tick.setTimerType(Qt::PreciseTimer);
        tick.setInterval(2);
        QObject::connect(&tick, &QTimer::timeout, [this] {
            const qint64 now = clock.elapsed();
            worst = std::max(worst, now - last);
            last = now;
        });
        clock.start();
        tick.start();
    }
    void reset()
    {
        last = clock.elapsed();
        worst = 0;
    }
};

struct Window {
    QTemporaryDir cache;
    QTemporaryDir userData;
    std::unique_ptr<AppHost> host;

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
        ray::test::focusWindow(host->window()); // keys and the panel's shortcuts go to this window
        REQUIRE(host->startSession(userData.path().toStdString()));
        if (testOperation)
            host->app()->operationTypes()->add(std::make_unique<ct::TestOperation>());
        host->app()->setConfirmHook([](const api::ActionInfo&, std::string_view, std::string_view) { return true; });
        host->files()->setUseNativeDialogs(false);
        if (host->viewport() != nullptr)
            host->viewport()->setAnimationsEnabled(false);
        // The tests type metric values; the display follows the unit system of new documents.
        REQUIRE(settings()->set("units.newDocument", "\"metric\""));
        QTest::qWait(50);
        grab();
    }
    ~Window()
    {
        if (tools() && tools()->shown()) {
            tools()->revert();
            tools()->hide();
        }
    }

    std::shared_ptr<api::SettingsStore> settings() const
    {
        return api::SettingsStore::open(userData.path().toStdString());
    }
    QQuickWindow* window() const { return host->window(); }
    ToolLibraryModel* tools() const
    {
        return host->engine()->singletonInstance<ToolLibraryModel*>(QStringLiteral("RayCad.Ui"),
                                                                    QStringLiteral("CamTools"));
    }
    lib::LibraryStack* stack() const { return host->app()->libraries(); }
    QImage grab() const { return host->window()->grabWindow(); }
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
    QQuickItem* find(const QString& objectName) const { return findItem(window()->contentItem(), objectName); }
    QQuickItem* shown(const QString& objectName) const
    {
        QQuickItem* item = nullptr;
        const bool ok = QTest::qWaitFor([&] { return (item = find(objectName)) && item->isVisible(); }, 10000);
        INFO("item " << objectName.toStdString());
        REQUIRE(ok);
        return item;
    }
    bool visible(const QString& objectName) const
    {
        QQuickItem* item = find(objectName);
        return item != nullptr && item->isVisible();
    }
    // The manager's list model (a ToolList inside the panel).
    ToolListModel* list() const
    {
        std::function<ToolListModel*(QQuickItem*)> walk = [&](QQuickItem* item) -> ToolListModel* {
            for (QObject* child : item->children()) {
                if (auto* l = qobject_cast<ToolListModel*>(child);
                    l && l->objectName() == QLatin1String("toolLibraryList"))
                    return l;
            }
            for (QQuickItem* child : item->childItems()) {
                if (ToolListModel* l = walk(child))
                    return l;
            }
            return nullptr;
        };
        ToolListModel* l = walk(window()->contentItem());
        REQUIRE(l != nullptr);
        return l;
    }
    void sendMouse(QEvent::Type type, QPointF scenePos, Qt::MouseButton button, Qt::MouseButtons buttons) const
    {
        QMouseEvent e(type, scenePos, window()->mapToGlobal(scenePos), button, buttons, Qt::NoModifier);
        QGuiApplication::sendEvent(window(), &e);
    }
    void click(QQuickItem* item) const
    {
        REQUIRE(item != nullptr);
        grab();
        const QPointF p = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
        sendMouse(QEvent::MouseMove, p, Qt::NoButton, Qt::NoButton);
        QCoreApplication::processEvents();
        sendMouse(QEvent::MouseButtonPress, p, Qt::LeftButton, Qt::LeftButton);
        sendMouse(QEvent::MouseButtonRelease, p, Qt::LeftButton, Qt::NoButton);
        QCoreApplication::processEvents();
    }
    void click(const char* objectName) const { click(shown(QString::fromLatin1(objectName))); }
    // Keys reach the window even when the desktop gave the activation to another program (window_focus.h).
    void ensureFocus() const
    {
        if (QGuiApplication::focusWindow() != window())
            ray::test::focusWindow(window());
    }
    void key(int k, Qt::KeyboardModifiers mods = Qt::NoModifier) const
    {
        ensureFocus();
        QTest::keyClick(window(), static_cast<Qt::Key>(k), mods);
        QCoreApplication::processEvents();
    }
    void type(const QString& text) const
    {
        ensureFocus();
        for (const QChar c : text)
            QTest::keyClick(window(), c.toLatin1());
        QCoreApplication::processEvents();
    }
    // Types into a field of the editor (replacing its text).
    void typeInto(const QString& objectName, const QString& text) const
    {
        QQuickItem* field = shown(objectName);
        field->forceActiveFocus();
        QMetaObject::invokeMethod(field, "selectAll");
        if (text.isEmpty())
            key(Qt::Key_Delete);
        type(text);
        // Every key arrived (a person using the desktop can take the focus from the test window).
        INFO("typed into " << objectName.toStdString());
        CHECK(field->property("text").toString() == text);
    }
    void open() const
    {
        if (!tools()->shown())
            REQUIRE(host->commands()->trigger(QStringLiteral("cam.toolLibrary.open")));
        shown(QStringLiteral("toolLibraryBrowser"));
        idle();
    }
    // Nothing running in the manager (saves, rescans, the catalog) and the window drawn.
    void idle() const
    {
        REQUIRE(QTest::qWaitFor([&] { return !tools()->busy(); }, kTimeoutMs));
        QCoreApplication::processEvents();
        grab();
    }
    // The stack with the shared folder from the settings (between the built-in library and the user's own).
    void shareFolder(const QString& dir) const
    {
        const QJsonArray folders{dir};
        REQUIRE(settings()->set("library.sharedFolders",
                                QJsonDocument(folders).toJson(QJsonDocument::Compact).toStdString()));
        const bool ok = QTest::qWaitFor(
            [&] {
                const auto roots = stack()->roots();
                return roots.size() == 3 && roots[1].open && !roots[1].loading && roots[2].open;
            },
            kTimeoutMs);
        REQUIRE(ok);
    }
    QString userLibrary() const { return userData.path() + QStringLiteral("/library"); }
    void toCam() const
    {
        click(shown(QStringLiteral("workspaceTab.%1").arg(ui::kCamWorkspace)));
        REQUIRE(QTest::qWaitFor([&] { return host->context()->workspace() == ui::kCamWorkspace; }, 5000));
    }
    void openPlate() const
    {
        REQUIRE(QFileInfo::exists(kPlate));
        REQUIRE(host->files()->open(kPlate));
        const auto ready = [&] {
            api::OpenDocument* od = host->app()->active();
            return od && od->document && !od->document->rebuildPending() && host->scene()->ready() &&
                   host->scene()->bodyCount() > 0 && !host->files()->busy();
        };
        const bool ok = QTest::qWaitFor(ready, kTimeoutMs);
        api::OpenDocument* od = host->app()->active();
        INFO("document " << (od != nullptr) << ", rebuild pending "
                         << (od && od->document && od->document->rebuildPending()) << ", scene ready "
                         << host->scene()->ready() << ", bodies " << host->scene()->bodyCount() << ", files busy "
                         << host->files()->busy() << ", message " << host->context()->message().toStdString());
        REQUIRE(ok);
    }
    doc::DocStatePtr state() const
    {
        api::OpenDocument* od = host->app()->active();
        REQUIRE(od != nullptr);
        return od->document->state();
    }
    bool invoke(const std::string& action, const std::string& args) const
    {
        JobHandle<api::ActionResult> job = host->app()->actions().invoke(action, args, api::Caller::User);
        REQUIRE(QTest::qWaitFor([&] { return job.done(); }, kTimeoutMs));
        auto r = job.wait();
        if (!r)
            UNSCOPED_INFO(action << ": " << r.error().message);
        return r.has_value();
    }
    // Selects the list row of an item by id (through the list, as a click would).
    void selectRow(const QString& origin, const QString& id) const
    {
        ToolListModel* l = list();
        int row = -1;
        REQUIRE(QTest::qWaitFor([&] { return (row = l->rowOf(origin, id)) >= 0; }, 10000));
        QQuickItem* view = shown(QStringLiteral("toolLibraryView"));
        QMetaObject::invokeMethod(view, "positionViewAtIndex", Q_ARG(int, row), Q_ARG(int, 1 /* ListView.Center */));
        QCoreApplication::processEvents();
        QQuickItem* item = nullptr;
        REQUIRE(QTest::qWaitFor(
            [&] { return (item = find(QStringLiteral("toolRow.%1").arg(row))) && item->isVisible(); }, 10000));
        click(item);
        REQUIRE(QTest::qWaitFor([&] { return tools()->selectedId() == id; }, 5000));
        idle();
    }
    QString field(const char* key) const { return tools()->values().value(QLatin1String(key)).toString(); }
    QString problem(const QString& key) const { return tools()->problems().value(key).toString(); }
    void shot(const char* name) const
    {
        const QString dir = qEnvironmentVariable("RAY_T046_SHOTS");
        if (dir.isEmpty())
            return;
        QDir().mkpath(dir);
        sendMouse(QEvent::MouseMove, QPointF(4, window()->height() - 4.0), Qt::NoButton, Qt::NoButton);
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

// Writes tools straight into a library folder (no atomic save per file: thousands of them).
std::vector<lib::Tool> writeTools(const QString& root, int count, const std::vector<lib::Material>& materials,
                                  const QString& prefix = QStringLiteral("Shop"))
{
    QDir().mkpath(root + QStringLiteral("/tools"));
    std::vector<lib::Tool> out;
    const lib::ToolType types[] = {lib::ToolType::FlatEndMill, lib::ToolType::BallEndMill, lib::ToolType::Drill,
                                   lib::ToolType::Tap,         lib::ToolType::SpotDrill,   lib::ToolType::ChamferMill};
    for (int i = 0; i < count; ++i) {
        lib::Tool t;
        t.id = ToolId{Uuid::generate()};
        t.type = types[i % 6];
        t.name = (prefix + QStringLiteral(" tool %1").arg(i)).toStdString();
        t.vendor = i % 2 == 0 ? "Acme" : "Generic";
        t.diameter = 1 + (i % 200) * 0.1;
        t.shankDiameter = std::max(3.0, t.diameter);
        t.fluteLength = 3 * t.diameter;
        t.shoulderLength = t.fluteLength;
        t.overallLength = 3 * t.diameter + 40;
        t.flutes = 2 + i % 3;
        if (t.type == lib::ToolType::BallEndMill)
            t.cornerRadius = t.diameter / 2;
        if (t.type == lib::ToolType::Drill || t.type == lib::ToolType::SpotDrill ||
            t.type == lib::ToolType::ChamferMill)
            t.tipAngleDeg = t.type == lib::ToolType::Drill ? 135 : 90;
        if (t.type == lib::ToolType::Tap)
            t.threadPitch = 0.5;
        if (!materials.empty()) {
            lib::ToolPreset p;
            p.material = materials[static_cast<std::size_t>(i) % materials.size()].id;
            p.opClass = "rough";
            p.surfaceSpeedMMin = 100;
            p.chipLoadMm = 0.02;
            p.maxRadialFraction = 0.2;
            p.maxAxialFraction = 1;
            t.presets.push_back(p);
        }
        QFile f(root + QStringLiteral("/tools/") + qs(t.id.value.toString()) + QStringLiteral(".tool.json"));
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(QByteArray::fromStdString(api::cam_tools::toolJson(t)));
        out.push_back(std::move(t));
    }
    return out;
}

lib::Tool fileTool(const QString& path)
{
    QFile f(path);
    REQUIRE(f.open(QIODevice::ReadOnly));
    auto t = api::cam_tools::toolFromJson(f.readAll().toStdString());
    REQUIRE(t);
    return *t;
}

QString toolPath(const QString& root, const lib::Tool& t)
{
    return root + QStringLiteral("/tools/") + qs(t.id.value.toString()) + QStringLiteral(".tool.json");
}

// The bounding box of what is drawn in an item's grab (pixels that differ from its top-left pixel).
QRect drawnBox(const QImage& image)
{
    const QRgb bg = image.pixel(1, 1);
    int x0 = image.width(), y0 = image.height(), x1 = -1, y1 = -1;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QRgb p = image.pixel(x, y);
            if (std::abs(qRed(p) - qRed(bg)) + std::abs(qGreen(p) - qGreen(bg)) + std::abs(qBlue(p) - qBlue(bg)) > 60) {
                x0 = std::min(x0, x);
                y0 = std::min(y0, y);
                x1 = std::max(x1, x);
                y1 = std::max(y1, y);
            }
        }
    }
    return x1 < 0 ? QRect() : QRect(QPoint(x0, y0), QPoint(x1, y1));
}

QImage grabItem(QQuickItem* item)
{
    auto result = item->grabToImage();
    REQUIRE(result);
    bool ready = false;
    QObject::connect(result.data(), &QQuickItemGrabResult::ready, [&ready] { ready = true; });
    REQUIRE(QTest::qWaitFor([&] { return ready; }, 10000));
    return result->image();
}

} // namespace

TEST_CASE("the manager lists every library root and which items are read-only", "[cam][tools]")
{
    Window w;
    QTemporaryDir share;
    const auto materials = w.stack()->materials();
    const auto shopTools = writeTools(share.path(), 6, materials);
    w.shareFolder(share.path());
    w.open();
    ToolLibraryModel* m = w.tools();
    // Three roots, lowest first: built-in (read-only), the shared folder, my library.
    const QVariantList roots = m->roots();
    REQUIRE(roots.size() == 3);
    CHECK(roots[0].toMap().value(QStringLiteral("kind")).toString() == QStringLiteral("builtin"));
    CHECK_FALSE(roots[0].toMap().value(QStringLiteral("writable")).toBool());
    CHECK(roots[1].toMap().value(QStringLiteral("kind")).toString() == QStringLiteral("shared"));
    CHECK(roots[1].toMap().value(QStringLiteral("writable")).toBool());
    CHECK(roots[2].toMap().value(QStringLiteral("kind")).toString() == QStringLiteral("user"));
    for (int i = 0; i < 3; ++i)
        CHECK(w.visible(QStringLiteral("toolLibraryRoot.%1").arg(i)));
    // Every row names its root; read-only rows (the built-in library) carry a lock, the shared ones none.
    ToolListModel* l = w.list();
    const int total = static_cast<int>(w.stack()->tools().size());
    CHECK(l->count() == total);
    int builtin = 0, shared = 0;
    for (int row = 0; row < l->count(); ++row) {
        const QVariantMap r = l->get(row);
        const auto root = w.stack()->rootOf(
            lib::ItemKind::Tool, Uuid::fromString(r.value(QStringLiteral("itemId")).toString().toStdString()));
        REQUIRE(root);
        CHECK(r.value(QStringLiteral("rootName")).toString() == m->rootLabel(static_cast<int>(*root)));
        CHECK(r.value(QStringLiteral("readOnly")).toBool() == (*root == 0));
        builtin += *root == 0 ? 1 : 0;
        shared += *root == 1 ? 1 : 0;
    }
    CHECK(builtin >= 50);
    CHECK(shared == 6);
    // Shown in the rows: a built-in row's lock and root name, a shared row's root name without a lock.
    w.selectRow(QStringLiteral("library"), qs(shopTools[0].id.value.toString()));
    const int sharedRow = l->rowOf(QStringLiteral("library"), qs(shopTools[0].id.value.toString()));
    CHECK(w.visible(QStringLiteral("toolRowRoot.%1").arg(sharedRow)));
    CHECK_FALSE(w.visible(QStringLiteral("toolRowLock.%1").arg(sharedRow)));
    CHECK_FALSE(m->readOnly());
    lib::Tool builtinTool;
    for (const lib::Tool& t : w.stack()->tools()) {
        if (w.stack()->rootOf(lib::ItemKind::Tool, t.id.value) == std::optional<std::size_t>(0)) {
            builtinTool = t;
            break;
        }
    }
    w.selectRow(QStringLiteral("library"), qs(builtinTool.id.value.toString()));
    const int builtinRow = l->rowOf(QStringLiteral("library"), qs(builtinTool.id.value.toString()));
    CHECK(w.visible(QStringLiteral("toolRowLock.%1").arg(builtinRow)));
    CHECK(m->readOnly());
    CHECK(m->whereText().contains(QStringLiteral("read-only")));
    w.checkNoWarnings();
}

TEST_CASE("filters give exactly the matching tools in a stable order", "[cam][tools]")
{
    Window w;
    w.open();
    ToolLibraryModel* m = w.tools();
    ToolListModel* l = w.list();
    const std::vector<lib::Tool> all = w.stack()->tools();
    REQUIRE(l->count() == static_cast<int>(all.size()));
    // The material filter: a material of the built-in library that some, not all, tools have presets for.
    const QVariantList materials = m->materials();
    REQUIRE(!materials.isEmpty());
    const QString material = materials.front().toMap().value(QStringLiteral("id")).toString();

    // Typed and chosen in the panel's own controls.
    QQuickItem* typeFilter = w.shown(QStringLiteral("toolLibraryTypeFilter"));
    const int drillIndex = [&] {
        const QVariantList types = m->toolTypes();
        for (int i = 0; i < types.size(); ++i) {
            if (types[i].toMap().value(QStringLiteral("value")).toString() == QStringLiteral("drill"))
                return i + 1; // after "Any type"
        }
        return -1;
    }();
    REQUIRE(drillIndex > 0);
    typeFilter->setProperty("currentIndex", drillIndex);
    QQuickItem* materialFilter = w.shown(QStringLiteral("toolLibraryMaterialFilter"));
    materialFilter->setProperty("currentIndex", 1);
    CHECK(l->material() == material);
    w.typeInto(QStringLiteral("toolLibraryMinDiameter"), QStringLiteral("3 mm"));
    w.typeInto(QStringLiteral("toolLibraryMaxDiameter"), QStringLiteral("8.5mm"));
    w.typeInto(QStringLiteral("toolLibrarySearch"), QStringLiteral("JOBBER"));
    w.idle();

    // An independent filter over the stack, in (diameter, name, id) order.
    std::vector<const lib::Tool*> expected;
    for (const lib::Tool& t : all) {
        if (t.type != lib::ToolType::Drill || t.diameter < 3 - 1e-9 || t.diameter > 8.5 + 1e-9)
            continue;
        const QString text = (qs(t.name) + QLatin1Char(' ') + qs(t.comment)).toLower();
        if (!text.contains(QStringLiteral("jobber")))
            continue;
        const bool hasMaterial = std::any_of(t.presets.begin(), t.presets.end(), [&](const lib::ToolPreset& p) {
            return qs(p.material.value.toString()) == material;
        });
        if (hasMaterial)
            expected.push_back(&t);
    }
    std::sort(expected.begin(), expected.end(), [](const lib::Tool* a, const lib::Tool* b) {
        if (a->diameter != b->diameter)
            return a->diameter < b->diameter;
        const int c = qs(a->name).compare(qs(b->name), Qt::CaseInsensitive);
        if (c != 0)
            return c < 0;
        const int d = qs(a->name).compare(qs(b->name), Qt::CaseSensitive);
        if (d != 0)
            return d < 0;
        return a->id.value.toString() < b->id.value.toString();
    });
    REQUIRE(!expected.empty());
    REQUIRE(l->count() == static_cast<int>(expected.size()));
    std::vector<QString> first;
    for (int row = 0; row < l->count(); ++row) {
        first.push_back(l->get(row).value(QStringLiteral("itemId")).toString());
        CHECK(first.back() == qs(expected[static_cast<std::size_t>(row)]->id.value.toString()));
    }
    CHECK(
        w.shown(QStringLiteral("toolLibraryCount"))->property("text").toString().contains(QString::number(l->count())));
    // The same again after the stack is merged anew (and the catalog made again).
    const int builds = m->catalogBuilds();
    w.stack()->remerge();
    REQUIRE(QTest::qWaitFor([&] { return m->catalogBuilds() > builds; }, 10000));
    w.idle();
    for (int row = 0; row < l->count(); ++row)
        CHECK(l->get(row).value(QStringLiteral("itemId")).toString() == first[static_cast<std::size_t>(row)]);

    // By name: names ascending (case-insensitive), ties by id.
    w.typeInto(QStringLiteral("toolLibrarySearch"), QString());
    w.shown(QStringLiteral("toolLibrarySort"))->setProperty("currentIndex", 1);
    w.idle();
    CHECK(l->sort() == QStringLiteral("name"));
    for (int row = 1; row < l->count(); ++row) {
        const QString a = l->get(row - 1).value(QStringLiteral("name")).toString();
        const QString b = l->get(row).value(QStringLiteral("name")).toString();
        CHECK(a.compare(b, Qt::CaseInsensitive) <= 0);
    }
    // A diameter that does not read is named, and ignored.
    w.typeInto(QStringLiteral("toolLibraryMaxDiameter"), QStringLiteral("wide"));
    w.idle();
    CHECK(l->diameterProblem().contains(QStringLiteral("wide")));
    w.checkNoWarnings();
}

TEST_CASE("tools, holders and presets are made, copied, edited and deleted through the editor", "[cam][tools]")
{
    Window w;
    w.open();
    ToolLibraryModel* m = w.tools();
    const QString user = w.userLibrary();

    // A built-in tool is read-only; "Duplicate to my library" is offered.
    lib::Tool builtin;
    for (const lib::Tool& t : w.stack()->tools()) {
        if (t.type == lib::ToolType::FlatEndMill && !t.presets.empty()) {
            builtin = t;
            break;
        }
    }
    REQUIRE(builtin.id.value != Uuid{});
    w.selectRow(QStringLiteral("library"), qs(builtin.id.value.toString()));
    CHECK(m->readOnly());
    CHECK_FALSE(w.visible(QStringLiteral("toolSave")));
    QQuickItem* dup = w.shown(QStringLiteral("toolDuplicate"));
    CHECK(dup->property("text").toString() == QStringLiteral("Duplicate to my library"));
    // Lengths as the display unit shows them (metric here).
    CHECK(w.field("diameter").endsWith(QStringLiteral(" mm")));
    w.click(dup);
    REQUIRE(QTest::qWaitFor([&] { return m->selectedOrigin() == QStringLiteral("new"); }, 5000));
    CHECK_FALSE(m->readOnly());
    CHECK(m->whereText().contains(QStringLiteral("My library")));
    CHECK(w.field("name").endsWith(QStringLiteral("(copy)")));

    // Problems beside their fields, in the schema's words.
    w.typeInto(QStringLiteral("toolInput.diameter"), QStringLiteral("0"));
    CHECK(w.problem(QStringLiteral("diameter")) == QStringLiteral("must be greater than 0 mm"));
    QQuickItem* problemLabel = w.shown(QStringLiteral("toolProblem.diameter"));
    CHECK(problemLabel->property("text").toString() == QStringLiteral("must be greater than 0 mm"));
    CHECK_FALSE(m->canSave());
    w.typeInto(QStringLiteral("toolInput.diameter"), QStringLiteral("abc"));
    CHECK(w.problem(QStringLiteral("diameter")).startsWith(QStringLiteral("Not a length")));
    w.typeInto(QStringLiteral("toolInput.diameter"), QStringLiteral("-2"));
    CHECK(w.field("diameter") == QStringLiteral("-2"));
    CHECK(w.shown(QStringLiteral("toolInput.diameter"))->property("text").toString() == QStringLiteral("-2"));
    w.typeInto(QStringLiteral("toolInput.diameter"), QStringLiteral("0.5 in"));
    CHECK(w.problem(QStringLiteral("diameter")).isEmpty());
    CHECK_FALSE(w.visible(QStringLiteral("toolProblem.diameter")));
    w.typeInto(QStringLiteral("toolInput.name"), QStringLiteral("My half inch"));
    // A preset: one more, its chip load wrong, then right.
    const int presets = static_cast<int>(m->rows().size());
    w.click("toolAddRow");
    REQUIRE(static_cast<int>(m->rows().size()) == presets + 1);
    const QString chip = QStringLiteral("presets.%1.chipLoad").arg(presets);
    w.typeInto(QStringLiteral("toolInput.") + chip, QStringLiteral("11 mm"));
    CHECK(w.problem(chip) == QStringLiteral("must be at most 10 mm"));
    CHECK(w.visible(QStringLiteral("toolProblem.") + chip));
    w.typeInto(QStringLiteral("toolInput.") + chip, QStringLiteral("0.0317 mm"));
    CHECK(w.problem(chip).isEmpty());
    INFO("problems: " << QJsonDocument(QJsonObject::fromVariantMap(m->problems())).toJson().toStdString());
    REQUIRE(m->canSave());
    w.click("toolSave");
    w.idle();
    CHECK(m->selectedOrigin() == QStringLiteral("library"));
    CHECK(m->status().contains(QStringLiteral("My library")));
    // In my library's folder, as typed.
    const QString id = m->selectedId();
    const lib::Tool saved = fileTool(user + QStringLiteral("/tools/") + id + QStringLiteral(".tool.json"));
    CHECK(saved.name == "My half inch");
    CHECK(saved.diameter == 12.7);
    REQUIRE(saved.presets.size() == builtin.presets.size() + 1);
    CHECK(std::count_if(saved.presets.begin(), saved.presets.end(),
                        [](const lib::ToolPreset& p) { return p.chipLoadMm == 0.0317; }) == 1);
    // The rest as the built-in tool had it.
    CHECK(saved.overallLength == builtin.overallLength);
    CHECK(saved.presets.front().surfaceSpeedMMin == builtin.presets.front().surfaceSpeedMMin);
    CHECK(w.list()->rowOf(QStringLiteral("library"), id) >= 0);

    // Edited in place: duplicate a preset, remove the first, save.
    w.click("toolDuplicateRow.0");
    w.click("toolRemoveRow.0");
    CHECK(m->dirty());
    w.click("toolSave");
    w.idle();
    CHECK(fileTool(user + QStringLiteral("/tools/") + id + QStringLiteral(".tool.json")).presets.size() ==
          builtin.presets.size() + 1);
    // Revert drops unsaved edits.
    w.typeInto(QStringLiteral("toolInput.name"), QStringLiteral("Not kept"));
    CHECK(m->dirty());
    w.click("toolRevert");
    CHECK(w.field("name") == QStringLiteral("My half inch"));
    CHECK_FALSE(m->dirty());

    // A new tool of a type.
    REQUIRE(m->newItem(QStringLiteral("drill")));
    CHECK(w.field("type") == QStringLiteral("drill"));
    CHECK(w.visible(QStringLiteral("toolInput.tipAngle"))); // the drill's own field
    CHECK_FALSE(w.find(QStringLiteral("toolInput.cornerRadius")));
    w.typeInto(QStringLiteral("toolInput.name"), QStringLiteral("Drill 5"));
    w.typeInto(QStringLiteral("toolInput.diameter"), QStringLiteral("5 mm"));
    w.key(Qt::Key_S, Qt::ControlModifier); // the panel's Save key, from a field
    w.idle();
    INFO("after Ctrl+S: origin " << m->selectedOrigin().toStdString() << ", status " << m->status().toStdString()
                                 << ", conflict "
                                 << m->conflict().value(QStringLiteral("detail")).toString().toStdString());
    CHECK(m->selectedOrigin() == QStringLiteral("library"));
    const QString drillId = m->selectedId();
    CHECK(fileTool(user + QStringLiteral("/tools/") + drillId + QStringLiteral(".tool.json")).diameter == 5);

    // Deleted after the question.
    w.click("toolDelete");
    w.shown(QStringLiteral("toolDeleteQuestion"));
    w.click("toolDeleteConfirm");
    w.idle();
    CHECK_FALSE(QFileInfo::exists(user + QStringLiteral("/tools/") + drillId + QStringLiteral(".tool.json")));
    CHECK(m->selectedId().isEmpty());
    CHECK(w.list()->rowOf(QStringLiteral("library"), drillId) < 0);

    // A holder: new, a segment edited, saved; then copied.
    w.click("toolLibraryTab.holders");
    REQUIRE(QTest::qWaitFor([&] { return w.list()->kind() == QStringLiteral("holders"); }, 5000));
    w.click("toolLibraryNew");
    REQUIRE(m->editorKind() == QStringLiteral("holder"));
    w.typeInto(QStringLiteral("toolInput.name"), QStringLiteral("ER32 short"));
    CHECK_FALSE(w.visible(QStringLiteral("toolInput.segments.1.height"))); // the cards are closed: a summary each
    CHECK(
        w.shown(QStringLiteral("toolRowSummary.1"))->property("text").toString().startsWith(QStringLiteral("Height")));
    w.click("toolRowToggle.1");
    w.typeInto(QStringLiteral("toolInput.segments.1.height"), QStringLiteral("0"));
    CHECK(w.problem(QStringLiteral("segments.1.height")) == QStringLiteral("must be greater than 0 mm"));
    w.typeInto(QStringLiteral("toolInput.segments.1.height"), QStringLiteral("30 mm"));
    w.typeInto(QStringLiteral("toolInput.gaugeLength"), QStringLiteral("45 mm"));
    REQUIRE(m->canSave());
    w.click("toolSave");
    w.idle();
    const QString holderId = m->selectedId();
    QFile hf(user + QStringLiteral("/holders/") + holderId + QStringLiteral(".holder.json"));
    REQUIRE(hf.open(QIODevice::ReadOnly));
    const auto holder = api::cam_tools::holderFromJson(hf.readAll().toStdString());
    REQUIRE(holder);
    CHECK(holder->name == "ER32 short");
    CHECK(holder->segments.at(1).height == 30);
    CHECK(holder->gaugeLength == 45);
    CHECK(w.list()->rowOf(QStringLiteral("library"), holderId) >= 0);
    w.checkNoWarnings();
}

TEST_CASE("the side view is to scale and is remade when and only when the geometry changes", "[cam][tools]")
{
    RAY_REQUIRE_RHI();
    Window w;
    w.open();
    ToolLibraryModel* m = w.tools();
    // A tool of my library to edit.
    REQUIRE(m->newItem(QStringLiteral("flatEndMill")));
    w.typeInto(QStringLiteral("toolInput.diameter"), QStringLiteral("10 mm"));
    w.typeInto(QStringLiteral("toolInput.shankDiameter"), QStringLiteral("10 mm"));
    w.typeInto(QStringLiteral("toolInput.overallLength"), QStringLiteral("60 mm"));
    w.click("toolSave");
    w.idle();
    QQuickItem* view = w.shown(QStringLiteral("toolProfile"));
    // To scale: the drawing's box has the tool's proportions (10 x 60 mm), whichever theme.
    for (const char* mode : {"dark", "light"}) {
        w.host->theme()->setProperty("mode", QString::fromLatin1(mode));
        QTest::qWait(100);
        const QRect box = drawnBox(grabItem(w.find(QStringLiteral("toolProfileShape"))));
        REQUIRE(box.isValid());
        INFO(mode << ": " << box.width() << " x " << box.height() << " px");
        const double ratio = static_cast<double>(box.height()) / box.width();
        CHECK(std::abs(ratio - 6.0) < 6.0 * 0.06);
    }
    const int rev = m->profileRevision();
    const QImage before = grabItem(view);
    // Not geometry: the same drawing, not remade.
    w.typeInto(QStringLiteral("toolInput.name"), QStringLiteral("Renamed end mill"));
    w.typeInto(QStringLiteral("toolInput.vendor"), QStringLiteral("Acme"));
    REQUIRE(!m->rows().isEmpty()); // a new tool starts with cutting data for the first material
    w.click("toolRowToggle.0");    // its card opens
    w.typeInto(QStringLiteral("toolInput.presets.0.surfaceSpeed"), QStringLiteral("150 m/min"));
    CHECK(m->profileRevision() == rev);
    CHECK(grabItem(view) == before);
    // Geometry: remade, and drawn differently.
    // (Set whole, as a paste would: typed key by key, "3" is already a valid flute length.)
    m->setValue(QStringLiteral("fluteLength"), QStringLiteral("35 mm"));
    CHECK(m->profileRevision() == rev + 1);
    QTest::qWait(50);
    CHECK(grabItem(view) != before);
    // While a length does not read, the last drawing stays.
    w.typeInto(QStringLiteral("toolInput.overallLength"), QStringLiteral("x"));
    CHECK(m->profileRevision() == rev + 1);
    m->setValue(QStringLiteral("overallLength"), QStringLiteral("60 mm"));
    CHECK(m->profileRevision() == rev + 1);
    // In a holder: remade with the holder, taller than the tool.
    const QVariantList holders = m->holderChoices();
    REQUIRE(!holders.isEmpty());
    m->setPreviewHolder(holders.front().toMap().value(QStringLiteral("id")).toString());
    CHECK(m->profileRevision() == rev + 2);
    CHECK(m->profile().value(QStringLiteral("holder")).toList().size() > 3);
    CHECK(m->profile().value(QStringLiteral("height")).toDouble() > 60);
    m->setPreviewHolder(QString());
    m->revert();
    w.checkNoWarnings();
}

TEST_CASE("a shared library changed underneath or held open gives a clear conflict, and nothing is lost",
          "[cam][tools][conflict]")
{
    Window w;
    QTemporaryDir share;
    const auto shop = writeTools(share.path(), 3, w.stack()->materials());
    w.shareFolder(share.path());
    w.open();
    ToolLibraryModel* m = w.tools();
    const QString file = toolPath(share.path(), shop[0]);
    // Another user's stack over the same folder.
    lib::LibraryStack other;
    other.setRoots({share.path().toStdString()});
    const auto theirEdit = [&](double diameter) {
        lib::Tool t = *other.tool(shop[0].id);
        t.diameter = diameter;
        REQUIRE(other.library(0)->save(t));
        other.remerge();
    };

    // 1. Changed underneath since I opened it; Retry writes mine over theirs.
    w.selectRow(QStringLiteral("library"), qs(shop[0].id.value.toString()));
    w.typeInto(QStringLiteral("toolInput.diameter"), QStringLiteral("7 mm"));
    theirEdit(8);
    w.click("toolSave");
    w.idle();
    QVariantMap c = m->conflict();
    REQUIRE(!c.isEmpty());
    CHECK(c.value(QStringLiteral("kind")).toString() == QStringLiteral("changed"));
    CHECK(c.value(QStringLiteral("title")).toString().contains(QStringLiteral("Someone else changed")));
    CHECK(c.value(QStringLiteral("library")).toString() == m->rootLabel(1));
    CHECK(QDir::fromNativeSeparators(c.value(QStringLiteral("file")).toString()) == file);
    CHECK(w.visible(QStringLiteral("toolConflict")));
    CHECK(w.visible(QStringLiteral("toolConflictRetry")));
    CHECK(w.visible(QStringLiteral("toolConflictCopy")));
    CHECK(w.visible(QStringLiteral("toolConflictDiscard")));
    CHECK(
        w.find(QStringLiteral("toolConflictFile"))->property("text").toString().contains(QStringLiteral(".tool.json")));
    // Nothing lost: their version on disk, mine still in the editor.
    CHECK(fileTool(file).diameter == 8);
    CHECK(w.field("diameter") == QStringLiteral("7 mm"));
    CHECK(m->dirty());
    w.shotsBothThemes("t046_conflict");
    w.click("toolConflictRetry");
    w.idle();
    CHECK(m->conflict().isEmpty());
    CHECK(fileTool(file).diameter == 7);

    // 2. The same through the library's own check (the change after the library last read the file); a copy to my
    // library keeps both.
    w.typeInto(QStringLiteral("toolInput.diameter"), QStringLiteral("6.5 mm"));
    other.refresh();
    theirEdit(9);
    w.click("toolSave");
    w.idle();
    c = m->conflict();
    REQUIRE(!c.isEmpty());
    CHECK(c.value(QStringLiteral("kind")).toString() == QStringLiteral("changed"));
    w.click("toolConflictCopy");
    w.idle();
    CHECK(m->conflict().isEmpty());
    CHECK(fileTool(file).diameter == 9); // theirs, untouched
    const QString copyId = m->selectedId();
    CHECK(copyId != qs(shop[0].id.value.toString()));
    const lib::Tool copy =
        fileTool(w.userLibrary() + QStringLiteral("/tools/") + copyId + QStringLiteral(".tool.json"));
    CHECK(copy.diameter == 6.5); // mine, in my library
    CHECK(m->whereText() == QStringLiteral("My library"));

    // 3. Discard: my edits go and theirs is shown.
    w.selectRow(QStringLiteral("library"), qs(shop[0].id.value.toString()));
    w.typeInto(QStringLiteral("toolInput.diameter"), QStringLiteral("5.5 mm"));
    other.refresh();
    theirEdit(10);
    w.click("toolSave");
    w.idle();
    REQUIRE(!m->conflict().isEmpty());
    w.click("toolConflictDiscard");
    w.idle();
    CHECK(m->conflict().isEmpty());
    CHECK_FALSE(m->dirty());
    CHECK(w.field("diameter") == QStringLiteral("10 mm"));
    CHECK(fileTool(file).diameter == 10);

#ifdef _WIN32
    // 4. Another program has the file open (reading, without letting it be replaced): Locked, "in use"; retry once
    // it lets go.
    w.typeInto(QStringLiteral("toolInput.diameter"), QStringLiteral("4.5 mm"));
    HANDLE h = CreateFileW(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(file).utf16()), GENERIC_READ,
                           FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(h != INVALID_HANDLE_VALUE);
    w.click("toolSave");
    w.idle();
    c = m->conflict();
    REQUIRE(!c.isEmpty());
    INFO(c.value(QStringLiteral("detail")).toString().toStdString());
    CHECK(c.value(QStringLiteral("kind")).toString() == QStringLiteral("locked"));
    CHECK(c.value(QStringLiteral("title")).toString() == QStringLiteral("The library file is in use"));
    CHECK(fileTool(file).diameter == 10);
    CHECK(w.field("diameter") == QStringLiteral("4.5 mm"));
    CloseHandle(h);
    w.click("toolConflictRetry");
    w.idle();
    CHECK(m->conflict().isEmpty());
    CHECK(fileTool(file).diameter == 4.5);
#endif

    // 5. Deleted underneath while open: the save names it; a copy keeps my edits.
    w.typeInto(QStringLiteral("toolInput.diameter"), QStringLiteral("3.5 mm"));
    other.refresh();
    REQUIRE(other.library(0)->remove(lib::ItemKind::Tool, shop[0].id.value));
    w.click("toolSave");
    w.idle();
    c = m->conflict();
    REQUIRE(!c.isEmpty());
    CHECK(c.value(QStringLiteral("message")).toString().contains(m->rootLabel(1)));
    w.click("toolConflictCopy");
    w.idle();
    CHECK(fileTool(w.userLibrary() + QStringLiteral("/tools/") + m->selectedId() + QStringLiteral(".tool.json"))
              .diameter == 3.5);
    w.checkNoWarnings();
}

TEST_CASE("picking a library tool for an operation embeds it; the document's tools are their own group",
          "[cam][tools][document]")
{
    Window w(true);
    w.openPlate();
    ToolLibraryModel* m = w.tools();
    // My library has a tool; an operation of the document uses its copy.
    const lib::Tool mine = [&] {
        auto t = api::cam_tools::toolFromJson(ct::toolJson(1));
        REQUIRE(t);
        return *t;
    }();
    REQUIRE(w.stack()->library(1) != nullptr);
    REQUIRE(w.stack()->library(1)->save(mine));
    w.stack()->remerge();
    const QString body = qs(w.host->scene()->body(0)->id.value.toString());
    REQUIRE(w.invoke("cam.setup.add", R"({"id":")" + ct::id('a', 1) + R"(","name":"Setup 1","partBodies":[")" +
                                          body.toStdString() + R"("],"machine":")" + ct::id('9', 1) +
                                          R"(","library":{"machines":[)" + ct::machineJson(1) + "]}}"));
    {
        doc::Command add = ct::opAdd(1, 1, 1);
        QJsonObject args = QJsonDocument::fromJson(QByteArray::fromStdString(add.argsJson)).object();
        args[QStringLiteral("library")] =
            QJsonDocument::fromJson(QByteArray::fromStdString(ct::library(ct::toolJson(1)))).object();
        REQUIRE(w.invoke("cam.operation.add", QJsonDocument(args).toJson(QJsonDocument::Compact).toStdString()));
    }
    w.toCam();
    w.open();
    // The document's copy first, in its own group, read-only.
    ToolListModel* l = w.list();
    REQUIRE(QTest::qWaitFor(
        [&] { return l->count() > 0 && l->get(0).value(QStringLiteral("origin")) == QStringLiteral("document"); },
        10000));
    CHECK(l->get(0).value(QStringLiteral("group")).toString() == QStringLiteral("In this document"));
    CHECK(l->get(0).value(QStringLiteral("itemId")).toString() == qs(ct::id('d', 1)));
    CHECK_FALSE(l->get(0).value(QStringLiteral("libraryChanged")).toBool());
    w.selectRow(QStringLiteral("document"), qs(ct::id('d', 1)));
    CHECK(m->readOnly());
    CHECK_FALSE(m->canUpdateFromLibrary());

    // The picker (qml/cam/ToolPicker.qml, the component the operation panel can use): drills only, a search, Enter.
    lib::Tool drill;
    m->hide(); // the picker lives in another panel
    REQUIRE_FALSE(m->shown());
    {
        QQmlComponent component(w.host->engine());
        component.setData("import QtQuick\nimport RayCad.Ui\nToolPicker { width: 400; toolType: \"drill\" }", QUrl());
        std::unique_ptr<QObject> made(component.create());
        INFO(component.errorString().toStdString());
        auto* picker = qobject_cast<QQuickItem*>(made.get());
        REQUIRE(picker != nullptr);
        picker->setParentItem(w.window()->contentItem());
        picker->setZ(100);
        QSignalSpy picked(picker, SIGNAL(picked(QString, QString)));
        w.typeInto(QStringLiteral("toolPickerSearch"), QStringLiteral("jobber 135"));
        w.key(Qt::Key_Down);
        w.key(Qt::Key_Return);
        REQUIRE(picked.count() == 1);
        const QString id = picked.at(0).at(0).toString();
        CHECK(picked.at(0).at(1).toString() == QStringLiteral("library"));
        const lib::Tool* t = w.stack()->tool(ToolId{Uuid::fromString(id.toStdString())});
        REQUIRE(t != nullptr);
        CHECK(t->type == lib::ToolType::Drill);
        drill = *t;
        picker->setParentItem(nullptr);
    }
    w.open();
    // The pick for the operation: embedded in the same step, undone in one.
    const QString drillId = qs(drill.id.value.toString());
    const QVariantMap library = m->libraryArgsFor(drillId);
    REQUIRE(library.value(QStringLiteral("tools")).toList().size() == 1);
    const std::size_t undo = w.host->app()->active()->document->undoCount();
    REQUIRE(m->assignTool(qs(ct::id('c', 1)), drillId));
    REQUIRE(QTest::qWaitFor([&] { return w.host->app()->active()->document->undoCount() == undo + 1; }, kTimeoutMs));
    w.idle();
    doc::DocStatePtr s = w.state();
    REQUIRE(s->cam->tools.count(drill.id) == 1);
    CHECK(s->cam->tools.at(drill.id).item.contentHash() == drill.contentHash());
    CHECK(s->cam->operations.at(ct::opId(1)).tool.tool == drill.id);
    CHECK(m->libraryArgsFor(drillId).isEmpty()); // embedded now
    REQUIRE(QTest::qWaitFor([&] { return l->rowOf(QStringLiteral("document"), drillId) >= 0; }, 10000));

    // The library's original of the document's tool changes shape: marked, and updated on request.
    lib::Tool changed = mine;
    changed.diameter = 6.5;
    REQUIRE(w.stack()->library(1)->save(changed));
    w.stack()->remerge();
    w.idle();
    const int row = l->rowOf(QStringLiteral("document"), qs(ct::id('d', 1)));
    REQUIRE(row >= 0);
    CHECK(l->get(row).value(QStringLiteral("libraryChanged")).toBool());
    w.selectRow(QStringLiteral("document"), qs(ct::id('d', 1)));
    CHECK(m->libraryChanged());
    CHECK(w.visible(QStringLiteral("toolLibraryChangedNote")));
    QQuickItem* update = w.shown(QStringLiteral("toolUpdateFromLibrary"));
    CHECK(update->isEnabled());
    w.click(update);
    REQUIRE(QTest::qWaitFor([&] { return w.state()->cam->tools.at(mine.id).item.diameter == 6.5; }, kTimeoutMs));
    w.idle();
    CHECK_FALSE(l->get(l->rowOf(QStringLiteral("document"), qs(ct::id('d', 1))))
                    .value(QStringLiteral("libraryChanged"))
                    .toBool());
    CHECK_FALSE(m->libraryChanged());
    // A name-only change of the original is no shape change: no mark, but the copy can still be updated.
    lib::Tool renamed = changed;
    renamed.name = "Renamed in the library";
    REQUIRE(w.stack()->library(1)->save(renamed));
    w.stack()->remerge();
    w.idle();
    CHECK_FALSE(l->get(l->rowOf(QStringLiteral("document"), qs(ct::id('d', 1))))
                    .value(QStringLiteral("libraryChanged"))
                    .toBool());
    w.selectRow(QStringLiteral("library"), drillId);
    w.selectRow(QStringLiteral("document"), qs(ct::id('d', 1)));
    CHECK(m->canUpdateFromLibrary());
    w.checkNoWarnings();
}

TEST_CASE("the manager works from the keyboard alone, and with no document open", "[cam][tools][keyboard]")
{
    Window w;
    ToolLibraryModel* m = w.tools();
    // No document: the command runs.
    CHECK_FALSE(w.host->session()->hasDocument());
    CHECK(w.host->commands()
              ->disabledReason(*w.host->commands()->find(QStringLiteral("cam.toolLibrary.open")))
              .isEmpty());
    // Shift+T in the CAM workspace.
    w.toCam();
    w.window()->contentItem()->forceActiveFocus();
    w.key(Qt::Key_T, Qt::ShiftModifier);
    REQUIRE(QTest::qWaitFor([&] { return m->shown(); }, 5000));
    w.shown(QStringLiteral("toolLibraryBrowser"));
    w.idle();
    // The search field has the focus; the Tab order runs through the filters to the list.
    QQuickItem* search = w.shown(QStringLiteral("toolLibrarySearch"));
    REQUIRE(QTest::qWaitFor([&] { return search->hasActiveFocus(); }, 5000));
    std::vector<QString> order;
    for (int i = 0; i < 6; ++i) {
        w.key(Qt::Key_Tab);
        QQuickItem* f = w.window()->activeFocusItem();
        order.push_back(f ? f->objectName() : QString());
    }
    INFO("tab order: " << order[0].toStdString() << ", " << order[1].toStdString() << ", " << order[2].toStdString()
                       << ", " << order[3].toStdString() << ", " << order[4].toStdString() << ", "
                       << order[5].toStdString());
    const std::vector<QString> wanted = {
        QStringLiteral("toolLibraryTypeFilter"),  QStringLiteral("toolLibraryMaterialFilter"),
        QStringLiteral("toolLibraryMinDiameter"), QStringLiteral("toolLibraryMaxDiameter"),
        QStringLiteral("toolLibrarySort"),        QStringLiteral("toolLibraryView")};
    CHECK(order == wanted);
    // Back to the search; type, Down into the list: the first match is selected; Down again moves.
    search->forceActiveFocus();
    w.type(QStringLiteral("end mill"));
    w.idle();
    ToolListModel* l = w.list();
    REQUIRE(l->count() >= 3);
    w.key(Qt::Key_Down);
    REQUIRE(
        QTest::qWaitFor([&] { return m->selectedId() == l->get(0).value(QStringLiteral("itemId")).toString(); }, 5000));
    QQuickItem* focused = w.window()->activeFocusItem();
    INFO("focus after Down: " << (focused
                                      ? focused->objectName().toStdString() + " " + focused->metaObject()->className()
                                      : std::string("none")));
    CHECK(w.find(QStringLiteral("toolLibraryView"))->hasActiveFocus());
    w.key(Qt::Key_Down);
    CHECK(m->selectedId() == l->get(1).value(QStringLiteral("itemId")).toString());
    w.key(Qt::Key_End);
    CHECK(m->selectedId() == l->get(l->count() - 1).value(QStringLiteral("itemId")).toString());
    w.key(Qt::Key_Home);
    CHECK(m->selectedId() == l->get(0).value(QStringLiteral("itemId")).toString());
    // A built-in tool: Ctrl+D copies it to my library; Enter puts the focus in its first field.
    w.key(Qt::Key_D, Qt::ControlModifier);
    REQUIRE(m->selectedOrigin() == QStringLiteral("new"));
    QQuickItem* name = w.shown(QStringLiteral("toolInput.name"));
    REQUIRE(QTest::qWaitFor([&] { return name->hasActiveFocus(); }, 5000));
    QMetaObject::invokeMethod(name, "selectAll");
    w.type(QStringLiteral("Keyboard tool"));
    w.key(Qt::Key_S, Qt::ControlModifier);
    w.idle();
    CHECK(m->selectedOrigin() == QStringLiteral("library"));
    const QString id = m->selectedId();
    CHECK(fileTool(w.userLibrary() + QStringLiteral("/tools/") + id + QStringLiteral(".tool.json")).name ==
          "Keyboard tool");
    // From the list, Enter opens its fields; Esc reverts an edit, then closes the manager.
    QQuickItem* view = w.shown(QStringLiteral("toolLibraryView"));
    view->forceActiveFocus();
    w.key(Qt::Key_Return);
    REQUIRE(QTest::qWaitFor([&] { return name->hasActiveFocus(); }, 5000));
    w.type(QStringLiteral(" 2"));
    CHECK(m->dirty());
    w.key(Qt::Key_Escape);
    CHECK_FALSE(m->dirty());
    CHECK(w.field("name") == QStringLiteral("Keyboard tool"));
    CHECK(m->shown());
    w.key(Qt::Key_Escape);
    CHECK_FALSE(m->shown());
    // With unsaved edits the manager stays (and says why).
    m->show();
    w.idle();
    m->setValue(QStringLiteral("name"), QStringLiteral("Unsaved"));
    m->hide();
    CHECK(m->shown());
    CHECK(m->status().contains(QStringLiteral("Save or revert")));
    m->revert();
    m->hide();
    CHECK_FALSE(m->shown());
    w.checkNoWarnings();
}

TEST_CASE("5,000 tools: the list stays responsive and only visible rows exist", "[cam][tools][wallclock]")
{
    Window w;
    QTemporaryDir share;
    const auto materials = w.stack()->materials();
    auto shop = writeTools(share.path(), 5000, materials);
    w.shareFolder(share.path());
    ToolLibraryModel* m = w.tools();
    Watchdog dog;
    // Opening: the catalog of 5,000 tools is made on the UI thread.
    QElapsedTimer open;
    open.start();
    w.open();
    const qint64 openMs = open.elapsed();
    const double catalogMs = m->lastCatalogMs();
    ToolListModel* l = w.list();
    REQUIRE(l->count() == static_cast<int>(w.stack()->tools().size()));
    REQUIRE(l->count() >= 5000);
    const qint64 openGap = dog.worst;
    // Only the rows in view exist.
    QQuickItem* view = w.shown(QStringLiteral("toolLibraryView"));
    const auto rowItems = [&] {
        int n = 0;
        QQuickItem* content = view->property("contentItem").value<QQuickItem*>();
        for (QQuickItem* c : content->childItems())
            n += c->objectName().startsWith(QStringLiteral("toolRow.")) ? 1 : 0;
        return n;
    };
    const int created = rowItems();
    CHECK(created > 0);
    CHECK(created < 80);

    // Typing a search, one key at a time: each keystroke filters 5,000 tools.
    dog.reset();
    QQuickItem* search = w.shown(QStringLiteral("toolLibrarySearch"));
    search->forceActiveFocus();
    std::vector<double> filterMs;
    for (const QChar c : QStringLiteral("shop tool 12")) {
        QTest::keyClick(w.window(), c.toLatin1());
        QCoreApplication::processEvents();
        filterMs.push_back(l->lastFilterMs());
    }
    const qint64 typeGap = dog.worst;
    CHECK(l->count() > 0);
    CHECK(l->count() < 200);
    std::sort(filterMs.begin(), filterMs.end());
    // Clearing it, then scrolling to the end and back.
    w.typeInto(QStringLiteral("toolLibrarySearch"), QString());
    w.idle();
    dog.reset();
    QElapsedTimer scroll;
    scroll.start();
    QMetaObject::invokeMethod(view, "positionViewAtEnd");
    QTest::qWait(100);
    QMetaObject::invokeMethod(view, "positionViewAtBeginning");
    QTest::qWait(100);
    view->forceActiveFocus();
    w.key(Qt::Key_End);
    w.key(Qt::Key_PageUp);
    w.key(Qt::Key_Home);
    const qint64 scrollGap = dog.worst;
    CHECK(rowItems() < 80);

    // A rescan of the whole share after another program rewrote every file: read on a worker.
    for (lib::Tool& t : shop)
        t.name += " (rev 2)";
    {
        QDir().mkpath(share.path() + QStringLiteral("/tools"));
        for (const lib::Tool& t : shop) {
            QFile f(toolPath(share.path(), t));
            REQUIRE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(QByteArray::fromStdString(api::cam_tools::toolJson(t)));
        }
    }
    dog.reset();
    QElapsedTimer rescan;
    rescan.start();
    w.click("toolLibraryRefresh");
    CHECK(m->refreshing());
    REQUIRE(QTest::qWaitFor(
        [&] {
            return !m->refreshing() && w.stack()->tool(shop[17].id) &&
                   w.stack()->tool(shop[17].id)->name == shop[17].name;
        },
        kTimeoutMs));
    const qint64 rescanMs = rescan.elapsed();
    w.idle();
    const qint64 rescanGap = dog.worst;
    CHECK(l->get(l->rowOf(QStringLiteral("library"), qs(shop[17].id.value.toString())))
              .value(QStringLiteral("name"))
              .toString() == qs(shop[17].name));
    const double catalogAfterMs = m->lastCatalogMs();

    WARN("5,000 tools in a shared folder ("
         << l->count() << " rows): open " << openMs << " ms (catalog " << catalogMs << " ms, worst UI-thread gap "
         << openGap << " ms); " << created << " row items exist; filter per keystroke median "
         << filterMs[filterMs.size() / 2] << " ms, worst " << filterMs.back()
         << " ms (worst UI-thread gap while typing " << typeGap << " ms); scrolling worst gap " << scrollGap
         << " ms; rescan of 5,000 rewritten files " << rescanMs << " ms on a worker, catalog " << catalogAfterMs
         << " ms, worst UI-thread gap " << rescanGap << " ms");
    // The budgets are for an optimised build (ui_budget.h). This case's UI-thread work is all standard containers and
    // strings (5,000 items copied, lower-cased and filtered), which an MSVC Debug build's checked iterators slow about
    // tenfold rather than the fourfold kDebugBudgetScale allows for: twice that there.
    const double scale = ray::test::kDebugBudgetScale > 1 ? 2.0 : 1.0;
    CHECK(openGap < scale * ray::test::uiBudgetMs(50));
    CHECK(typeGap < scale * ray::test::uiBudgetMs(50));
    CHECK(scrollGap < scale * ray::test::uiBudgetMs(50));
    CHECK(rescanGap < scale * ray::test::uiBudgetMs(50));
    CHECK(filterMs.back() < scale * ray::test::uiBudgetMs(33)); // the High tier's stall budget (PLAN.md 9)
    w.checkNoWarnings();
}

TEST_CASE("screenshots of the manager", "[cam][tools][shots]")
{
    if (qEnvironmentVariable("RAY_T046_SHOTS").isEmpty())
        SKIP("RAY_T046_SHOTS is not set");
    RAY_REQUIRE_RHI();
    Window w;
    w.open();
    ToolLibraryModel* m = w.tools();
    w.shotsBothThemes("t046_list");
    // A built-in drill with its fields, read-only, in a holder.
    lib::Tool pick;
    for (const lib::Tool& t : w.stack()->tools()) {
        if (t.type == lib::ToolType::BallEndMill) {
            pick = t;
            break;
        }
    }
    w.selectRow(QStringLiteral("library"), qs(pick.id.value.toString()));
    const QVariantList holders = m->holderChoices();
    if (!holders.isEmpty())
        m->setPreviewHolder(holders.front().toMap().value(QStringLiteral("id")).toString());
    w.idle();
    w.shotsBothThemes("t046_builtin");
    // A copy being edited, with a problem shown.
    m->setPreviewHolder(QString());
    m->duplicate();
    w.typeInto(QStringLiteral("toolInput.fluteLength"), QStringLiteral("-2"));
    CHECK(w.field("fluteLength") == QStringLiteral("-2"));
    w.idle();
    w.shotsBothThemes("t046_editor");
    m->revert();
    // Every starter type's side view, one after another (for review).
    std::set<lib::ToolType> seen;
    for (const lib::Tool& t : w.stack()->tools()) {
        if (!seen.insert(t.type).second)
            continue;
        w.selectRow(QStringLiteral("library"), qs(t.id.value.toString()));
        w.shot(("t046_type_" + std::string(api::cam_tools::toolTypeName(t.type))).c_str());
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
