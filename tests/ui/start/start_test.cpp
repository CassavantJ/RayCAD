// The start screen, file handling, jobs and crash recovery (task T-019), on real Qt Quick windows with the real
// application session (ray::api::makeApp through AppHost::startSession):
//
//   - the app starts on the start screen; recent files show name, folder and time; a missing one is marked and can
//     be removed; the screen works from the keyboard
//   - opening a STEP file shows staged progress in the jobs list and the status bar; Esc cancels at once
//   - dropping files opens them (several: the first opens, the others are imported into it)
//   - STEP -> Save As .raycad -> close -> reopen from the recent list without the STEP file: same bodies, same hash
//   - undo and redo of an import through the actions (and their shortcuts) update the view and the tree
//   - the dirty marker in the window title and the unsaved-changes prompt on close, open-over and quit
//   - a file locked by another session opens read-only and says who holds it; Save goes to Save As
//   - a pending recovery (test double) is offered at launch; restore reaches the journalled state hash, discard
//     forgets it
//   - no UI-thread stall over 50 ms while a mid-size corpus file opens
//   - warm start to the first frame, reported against PLAN.md section 9
//
// Screenshots: with RAY_T019_SHOTS set to a folder, some steps save the window there.
#include "../scene_graph_probe.h"
#include "../ui_budget.h"
#include "../ui_watchdog.h"
#include "../window_focus.h"
#include "app_context.h"
#include "app_host.h"
#include "app_options.h"
#include "files.h"
#include "scene_controller.h"
#include "session.h"
#include "session_recovery.h"
#include "test_screen.h"
#include "theme_object.h"
#include "viewport_item.h"

#include <ray/api.h>
#include <ray/store.h>

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QMimeData>
#include <QtCore/QProcess>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtGui/QDragEnterEvent>
#include <QtGui/QDropEvent>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>
#include <QtQuick/QSGRendererInterface>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace ray;
using ray::ui::AppHost;
using ray::ui::AppOptions;
using ray::ui::Files;
using ray::ui::Session;

namespace {

const QString kBoxHole = QStringLiteral(RAY_KERNEL_TEST_DATA_DIR "/box_hole.step");
const QString kAssembly = QStringLiteral(RAY_KERNEL_TEST_DATA_DIR "/assembly_mm.step");
// One of the larger committed corpus files (about 0.8 MB).
const QString kMidSize = QStringLiteral(RAY_CORPUS_DIR "/nist-pmi/nist_ctc_04_asme1_rd_geometry.stp");

constexpr int kOpenTimeoutMs = 180000;
constexpr const char* kParamId = "6f1c1d4e-2b7a-4c39-9a51-0d2e8f3b7c11";

using ray::uitest::rhiSceneGraph;

// A crash-recovery source standing in for the app's journals (the dialog's rows, discard and the recover flow).
// "Recovering" just opens the file.
struct FakeRecovery final : ui::IRecoverySource {
    std::vector<ui::PendingRecovery> items;
    api::IApp* app = nullptr; // set once the window's session exists
    std::shared_ptr<std::vector<QString>> discarded = std::make_shared<std::vector<QString>>();
    std::shared_ptr<std::vector<QString>> recovered = std::make_shared<std::vector<QString>>();
    std::vector<ui::PendingRecovery> pending() override { return items; }
    JobHandle<api::OpenDocument*> recover(const ui::PendingRecovery& p) override
    {
        recovered->push_back(p.id);
        return app->open(p.filePath.toStdString());
    }
    Status discard(const ui::PendingRecovery& p) override
    {
        discarded->push_back(p.id);
        return {};
    }
};

// One application window with its own cache and per-user data folders.
struct Window {
    QTemporaryDir cache;
    QTemporaryDir userData;
    std::unique_ptr<AppHost> host;

    explicit Window(const char* theme = "dark", bool session = true, std::unique_ptr<ui::IRecoverySource> recovery = {},
                    const QString& userDataDir = {})
    {
        AppOptions options;
        options.theme = QString::fromLatin1(theme);
        options.glass = false;
        options.size = QSize(1440, 900);
        host = std::make_unique<AppHost>(options, cache.path());
        REQUIRE(host->optionError().isEmpty());
        REQUIRE(host->load());
        host->files()->setUseNativeDialogs(false);
        if (recovery)
            host->setRecoverySource(std::move(recovery));
        host->show();
        REQUIRE(QTest::qWaitForWindowExposed(host->window()));
        REQUIRE(host->viewport() != nullptr);
        host->viewport()->setAnimationsEnabled(false);
        host->scene()->setPreset(render::presetFor(render::Tier::Medium));
        grab(); // the first frame: runApp starts the session only after it
        if (session)
            start(userDataDir);
    }

    void start(const QString& userDataDir = {})
    {
        const QString dir = userDataDir.isEmpty() ? userData.path() : userDataDir;
        REQUIRE(host->startSession(dir.toStdString()));
    }

    QQuickWindow* window() const { return host->window(); }
    Session* session() const { return host->session(); }
    Files* files() const { return host->files(); }
    api::IApp* app() const { return host->app(); }
    ui::SceneController* scene() const { return host->scene(); }
    ui::AppContext* context() const { return host->context(); }
    QImage grab() const { return host->window()->grabWindow(); }

    QQuickItem* find(const char* objectName) const { return findItem(window()->contentItem(), objectName); }
    static QQuickItem* findItem(QQuickItem* item, const char* objectName)
    {
        if (item->objectName() == QLatin1String(objectName))
            return item;
        for (QQuickItem* child : item->childItems()) {
            if (QQuickItem* found = findItem(child, objectName))
                return found;
        }
        return nullptr;
    }
    bool shown(const char* objectName) const
    {
        QQuickItem* item = find(objectName);
        return item != nullptr && item->isVisible();
    }
    void click(const char* objectName) const
    {
        QQuickItem* item = find(objectName);
        INFO("clicking " << objectName);
        REQUIRE(item != nullptr);
        REQUIRE(item->isVisible());
        REQUIRE(QMetaObject::invokeMethod(item, "clicked"));
    }
    void mouseClick(QQuickItem* item) const
    {
        const QPointF centre = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
        QTest::mouseClick(window(), Qt::LeftButton, Qt::NoModifier, centre.toPoint());
    }

    // Shortcuts only fire in the active window. True if the OS let this one become active.
    bool activate() const
    {
        return ray::test::focusWindow(window(), 3000); // window_focus.h
    }

    void checkNoWarnings() const
    {
        for (const QString& warning : host->warnings())
            UNSCOPED_INFO(warning.toStdString());
        CHECK(host->warnings().isEmpty());
    }

    // Nothing runs: no open, no app job, no rebuild, no meshing; what is shown is meshed.
    bool settled() const
    {
        if (files()->busy() || session()->jobCount() > 0 || host->viewport()->animating())
            return false;
        api::OpenDocument* od = app()->active();
        if (od != nullptr && od->document->rebuildPending())
            return false;
        return scene()->bodyCount() == 0 || scene()->ready();
    }
    void waitSettled(int ms = kOpenTimeoutMs) const
    {
        REQUIRE(QTest::qWaitFor([&] { return settled(); }, ms));
        grab();
    }

    // Opens a file and waits until it is shown.
    void open(const QString& path) const
    {
        REQUIRE(files()->open(path));
        waitSettled();
        INFO(context()->message().toStdString());
        REQUIRE(session()->hasDocument());
    }

    std::string stateHash() const
    {
        api::OpenDocument* od = app()->active();
        REQUIRE(od != nullptr);
        return od->document->state()->stateHash().hex();
    }
    QStringList bodyNames() const
    {
        QStringList out;
        for (int i = 0; i < scene()->bodyCount(); ++i)
            out << scene()->bodyName(i);
        return out;
    }

    void shot(const char* name) const
    {
        const QString dir = qEnvironmentVariable("RAY_T019_SHOTS");
        if (dir.isEmpty())
            return;
        QDir().mkpath(dir);
        grab().save(QDir(dir).filePath(QString::fromLatin1(name) + QStringLiteral(".png")), "PNG");
    }
};

// The canonical arguments of a param.set command (also used as a journalled command).
std::string paramArgs(const char* name, const char* expression)
{
    return std::string("{\"expression\":\"") + expression + "\",\"id\":\"" + kParamId +
           "\",\"kind\":\"length\",\"name\":\"" + name + "\"}";
}

// Opens `step`, saves it as `raycad` and closes it again.
void makeRaycad(Window& w, const QString& step, const QString& raycad)
{
    w.open(step);
    w.files()->saveAs(raycad);
    REQUIRE(QTest::qWaitFor([&] { return !w.session()->dirty() && w.session()->documentPath() == raycad; }, 60000));
    w.files()->close();
    REQUIRE(QTest::qWaitFor([&] { return !w.session()->hasDocument(); }, 10000));
    REQUIRE(QFileInfo::exists(raycad));
}

void dragAndDrop(Window& w, const QList<QUrl>& urls, bool* accepted)
{
    QMimeData mime;
    mime.setUrls(urls);
    QQuickItem* area = w.find("viewportArea");
    REQUIRE(area != nullptr);
    const QPoint at = area->mapToScene(QPointF(area->width() / 2, area->height() / 2)).toPoint();
    QDragEnterEvent enter(QPointF(at), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QGuiApplication::sendEvent(w.window(), &enter);
    *accepted = enter.isAccepted();
    if (!*accepted) {
        QDragLeaveEvent leave;
        QGuiApplication::sendEvent(w.window(), &leave);
        return;
    }
    QDropEvent drop(QPointF(at), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QGuiApplication::sendEvent(w.window(), &drop);
}

} // namespace

TEST_CASE("the app starts on the start screen with recent files; a missing one can be removed", "[start]")
{
    // A recent list with one file that exists and one that does not.
    QTemporaryDir data;
    QTemporaryDir files;
    const QString present = QDir(files.path()).filePath(QStringLiteral("bracket.raycad"));
    const QString missing = QDir(files.path()).filePath(QStringLiteral("moved away.raycad"));
    {
        QFile f(present);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write("not opened in this test");
    }
    {
        QFile list(QDir(data.path()).filePath(QStringLiteral("recent-files.json")));
        REQUIRE(list.open(QIODevice::WriteOnly));
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const QString json = QStringLiteral(R"([{"openedMs":%1,"path":"%2"},{"openedMs":%3,"path":"%4"}])")
                                 .arg(now - 60000)
                                 .arg(QDir::fromNativeSeparators(present))
                                 .arg(now - 86400000LL * 3)
                                 .arg(QDir::fromNativeSeparators(missing));
        list.write(json.toUtf8());
    }

    // Before the session: the start screen is there (first frame), its buttons wait for the session.
    Window w("dark", false);
    CHECK(w.shown("startScreen"));
    CHECK_FALSE(w.find("openButton")->isEnabled());
    CHECK(w.shown("recentHint"));
    CHECK(w.window()->title() == QStringLiteral("RayCAD"));

    w.start(data.path());
    CHECK(w.find("openButton")->isEnabled());
    CHECK(w.find("newPartButton")->isEnabled());
    QAbstractItemModel* recent = w.session()->recentFiles();
    REQUIRE(recent->rowCount() == 2);
    CHECK_FALSE(w.shown("recentHint"));
    REQUIRE(QTest::qWaitFor([&] { return w.shown("recent.1"); }, 5000));
    const QHash<int, QByteArray> roles = recent->roleNames();
    auto role = [&](const char* name) {
        return roles.key(QByteArray(name));
    };
    CHECK(recent->data(recent->index(0, 0), role("fileName")).toString() == QStringLiteral("bracket.raycad"));
    CHECK(recent->data(recent->index(0, 0), role("folder")).toString() == QDir::toNativeSeparators(files.path()));
    CHECK(recent->data(recent->index(0, 0), role("openedText")).toString().startsWith(QStringLiteral("Today")));
    CHECK(recent->data(recent->index(0, 0), role("thumbnail")).toString().isEmpty());
    CHECK(w.find("recent.0")->findChild<QQuickItem*>(QStringLiteral("recentThumbnail")) != nullptr);
    // Existence is checked on a worker.
    REQUIRE(QTest::qWaitFor([&] { return recent->data(recent->index(1, 0), role("missing")).toBool(); }, 10000));
    CHECK_FALSE(recent->data(recent->index(0, 0), role("missing")).toBool());
    w.grab();
    CHECK(w.shown("recentRemove.1")); // a missing file always shows its remove button
    w.shot("start-recent-dark");

    // Keyboard: Tab reaches both buttons and the list; Delete removes the current file from the list.
    QQuickWindow* window = w.window();
    window->requestActivate();
    (void)QTest::qWaitForWindowActive(window, 2000);
    QSet<QString> stops;
    for (int i = 0; i < 30; ++i) {
        QTest::keyClick(window, Qt::Key_Tab);
        for (QQuickItem* it = window->activeFocusItem(); it != nullptr; it = it->parentItem()) {
            if (!it->objectName().isEmpty()) {
                stops << it->objectName();
                break;
            }
        }
    }
    INFO("focus stops: " << QStringList(stops.values()).join(QStringLiteral(", ")).toStdString());
    CHECK(stops.contains(QStringLiteral("openButton")));
    CHECK(stops.contains(QStringLiteral("newPartButton")));
    // The list gives the focus to its current tile.
    CHECK(std::any_of(stops.begin(), stops.end(),
                      [](const QString& s) { return s.startsWith(QStringLiteral("recent.")); }));
    QQuickItem* list = w.find("recentList");
    list->forceActiveFocus(Qt::TabFocusReason);
    QTest::keyClick(window, Qt::Key_Right);
    CHECK(list->property("currentIndex").toInt() == 1);

    // The missing file's remove button takes it off the list, which is stored.
    w.click("recentRemove.1");
    REQUIRE(QTest::qWaitFor([&] { return recent->rowCount() == 1; }, 5000));
    QFile stored(QDir(data.path()).filePath(QStringLiteral("recent-files.json")));
    REQUIRE(stored.open(QIODevice::ReadOnly));
    CHECK_FALSE(QString::fromUtf8(stored.readAll()).contains(QStringLiteral("moved away")));
    stored.close();
    // Delete on the current tile does the same.
    list->forceActiveFocus(Qt::TabFocusReason);
    list->setProperty("currentIndex", 0);
    QTest::keyClick(window, Qt::Key_Delete);
    REQUIRE(QTest::qWaitFor([&] { return recent->rowCount() == 0; }, 5000));
    CHECK(w.shown("recentHint"));
    w.checkNoWarnings();
}

TEST_CASE("the start screen and its recent files in the light theme", "[start][light]")
{
    Window w("light");
    CHECK(w.shown("startScreen"));
    CHECK(w.session()->recentCount() == 0);
    CHECK(w.shown("recentHint"));
    w.shot("start-empty-light");
    w.checkNoWarnings();
}

TEST_CASE("opening a STEP file shows staged progress; Esc cancels it at once", "[open][progress][wallclock]")
{
    Window w("dark");
    Session& session = *w.session();
    (void)w.activate();
    w.host->viewport()->forceActiveFocus(); // Esc reaches the job cancel through the window or the view

    // Esc while the file is read: the job leaves the list and the status bar at once, its result is dropped.
    REQUIRE(w.files()->open(kMidSize));
    CHECK(session.jobActive());
    CHECK(w.shown("jobSlot"));
    CHECK(w.files()->busy());
    QTest::qWait(150);
    QElapsedTimer t;
    t.start();
    QTest::keyClick(w.window(), Qt::Key_Escape);
    CHECK(t.elapsed() < ray::test::uiBudgetMs(100));
    CHECK_FALSE(session.jobActive());
    CHECK_FALSE(w.files()->busy());
    CHECK(w.context()->statusText() == QStringLiteral("Opening cancelled"));
    QTest::qWait(3000);
    CHECK_FALSE(session.hasDocument());
    CHECK(w.shown("startScreen"));

    // A full open: the stages of the import show in the jobs list and the status bar's slot.
    QSet<QString> stages;
    QStringList slotTitles;
    QObject::connect(&session, &Session::jobsChanged, [&] {
        for (const auto& row : session.jobsModel()->rows()) {
            if (!row.stage.isEmpty())
                stages << row.stage;
        }
        if (session.jobActive() && (slotTitles.isEmpty() || slotTitles.constLast() != session.jobTitle()))
            slotTitles << session.jobTitle();
    });
    w.find("shell")->setProperty("jobsPanelOpen", true);
    bool panelShot = false;
    QTimer shooter;
    QObject::connect(&shooter, &QTimer::timeout, [&] {
        if (!panelShot && w.shown("jobsPanel") && session.jobCount() > 0 &&
            !session.jobsModel()->rows()[0].stage.isEmpty()) {
            panelShot = true;
            w.shot("open-jobs-panel-dark");
        }
    });
    shooter.start(50);
    REQUIRE(w.files()->open(kMidSize));
    w.waitSettled();
    shooter.stop();
    INFO("stages: " << QStringList(stages.values()).join(QStringLiteral(", ")).toStdString());
    INFO("slot: " << slotTitles.join(QStringLiteral(" | ")).toStdString());
    // The kernel's stages ("Reading file", "Transferring", "Healing", ...; progress is reported at most 30 times a
    // second, so a fast machine may skip the early ones) show in the list and in the status bar's slot.
    const QSet<QString> kernelStages{QStringLiteral("Reading file"), QStringLiteral("Transferring"),
                                     QStringLiteral("Healing")};
    CHECK(stages.size() >= 2);
    CHECK(stages.intersects(kernelStages));
    CHECK(std::any_of(slotTitles.begin(), slotTitles.end(), [&](const QString& s) {
        return std::any_of(kernelStages.begin(), kernelStages.end(),
                           [&](const QString& k) { return s.endsWith(QStringLiteral(": ") + k); });
    }));
    REQUIRE(session.hasDocument());
    CHECK(session.documentTitle() == QStringLiteral("nist_ctc_04_asme1_rd_geometry"));
    CHECK(w.scene()->bodyCount() >= 1);
    CHECK_FALSE(w.shown("startScreen"));
    CHECK_FALSE(w.shown("jobsPanel")); // nothing runs any more
    w.shot("part-open-dark");
    w.checkNoWarnings();
}

TEST_CASE("dropping files opens them; further STEP files are imported into the first", "[drop]")
{
    Window w("dark");
    bool accepted = true;
    dragAndDrop(w, {QUrl::fromLocalFile(QStringLiteral(RAY_CORPUS_DIR "/ezdxf/Minimal_DXF_AC1009.dxf"))}, &accepted);
    CHECK_FALSE(accepted);

    dragAndDrop(w, {QUrl::fromLocalFile(kBoxHole)}, &accepted);
    CHECK(accepted);
    REQUIRE(QTest::qWaitFor([&] { return w.session()->hasDocument(); }, kOpenTimeoutMs));
    w.waitSettled();
    CHECK(w.session()->documentTitle() == QStringLiteral("box_hole"));
    CHECK(w.scene()->bodyCount() == 1);

    // Two files at once: a new document from the first, the second imported into it; the old document goes.
    dragAndDrop(w, {QUrl::fromLocalFile(kAssembly), QUrl::fromLocalFile(kBoxHole)}, &accepted);
    CHECK(accepted);
    // box_hole has unsaved changes (a STEP import is a new document): the prompt asks first.
    REQUIRE(QTest::qWaitFor([&] { return w.files()->promptVisible(); }, 5000));
    CHECK(w.files()->promptReason() == QStringLiteral("open"));
    w.click("unsavedDiscard");
    REQUIRE(
        QTest::qWaitFor([&] { return w.session()->documentTitle() == QStringLiteral("assembly_mm"); }, kOpenTimeoutMs));
    REQUIRE(QTest::qWaitFor([&] { return w.settled() && w.scene()->features().size() == 2; }, kOpenTimeoutMs));
    CHECK(w.app()->documents().size() == 1);
    CHECK(w.scene()->bodyCount() >= 3);
    w.checkNoWarnings();
}

TEST_CASE("STEP, Save As .raycad, close, reopen from the recent list: same bodies and state hash", "[roundtrip]")
{
    QTemporaryDir work;
    const QString step = QDir(work.path()).filePath(QStringLiteral("plate with hole.step"));
    REQUIRE(QFile::copy(kBoxHole, step));
    const QString raycad = QDir(work.path()).filePath(QStringLiteral("plate with hole.raycad"));

    Window w("dark");
    w.open(step);
    const std::string hash = w.stateHash();
    const QStringList names = w.bodyNames();
    const int bodies = w.scene()->bodyCount();
    REQUIRE(bodies == 1);
    const BodyId body = w.scene()->body(0)->id;
    CHECK(w.session()->dirty());

    w.files()->saveAs(raycad);
    REQUIRE(QTest::qWaitFor([&] { return !w.session()->dirty() && w.session()->documentPath() == raycad; }, 60000));
    CHECK(w.window()->title() == QStringLiteral("plate with hole - RayCAD"));
    w.files()->close();
    REQUIRE(QTest::qWaitFor([&] { return !w.session()->hasDocument(); }, 10000));
    CHECK_FALSE(w.files()->promptVisible()); // it was saved: no question
    CHECK(w.shown("startScreen"));
    CHECK(w.scene()->bodyCount() == 0);
    REQUIRE(QFile::remove(step));

    // The recent list has it first; a click on the tile opens it.
    REQUIRE(QTest::qWaitFor([&] { return w.session()->recentCount() == 1 && w.shown("recent.0"); }, 5000));
    w.grab();
    w.mouseClick(w.find("recent.0"));
    REQUIRE(QTest::qWaitFor([&] { return w.session()->hasDocument(); }, kOpenTimeoutMs));
    // The scene gets the reopened body after the document: settled() counts an empty scene as settled, and on a
    // slow machine (the CI runners, Debug) it was still empty when checked.
    REQUIRE(QTest::qWaitFor([&] { return w.scene()->bodyCount() == bodies; }, kOpenTimeoutMs));
    w.waitSettled();
    CHECK(w.session()->documentPath() == raycad);
    CHECK_FALSE(w.session()->dirty());
    CHECK(w.stateHash() == hash);
    REQUIRE(w.scene()->bodyCount() == bodies);
    CHECK(w.bodyNames() == names);
    CHECK(w.scene()->body(0)->id == body);
    CHECK(w.context()->message().isEmpty());
    w.checkNoWarnings();
}

TEST_CASE("undo and redo of an import through the actions update the view and the tree", "[undo]")
{
    Window w("dark");
    w.open(kBoxHole);
    REQUIRE(w.scene()->bodyCount() == 1);
    REQUIRE(w.scene()->features().size() == 1);

    // Import into the current document: a second import feature.
    w.files()->importFile(kAssembly);
    REQUIRE(QTest::qWaitFor([&] { return w.settled() && w.scene()->features().size() == 2; }, kOpenTimeoutMs));
    const int both = w.scene()->bodyCount();
    CHECK(both > 1);
    CHECK(w.session()->canUndo());
    QAbstractItemModel* tree = w.scene()->tree();
    const int treeRows = tree->rowCount();

    // Undo through the action registry: the second import goes from the tree and the view.
    REQUIRE(w.context()->invoke(QStringLiteral("edit.undo")));
    REQUIRE(QTest::qWaitFor([&] { return w.settled() && w.scene()->features().size() == 1; }, kOpenTimeoutMs));
    CHECK(w.scene()->bodyCount() == 1);
    CHECK(tree->rowCount() < treeRows);
    CHECK(w.session()->canRedo());

    // Redo by its shortcut (Ctrl+Y), undo again by Ctrl+Z, each exactly once (the Shortcuts singleton, T-021). They
    // need the active window; if the OS keeps another window in front, the actions run directly instead.
    const bool active = w.activate();
    if (!active)
        WARN("the test window could not be made active: the shortcuts are not exercised");
    if (active)
        QTest::keyClick(w.window(), Qt::Key_Y, Qt::ControlModifier);
    else
        REQUIRE(w.context()->invoke(QStringLiteral("edit.redo")));
    REQUIRE(QTest::qWaitFor([&] { return w.settled() && w.scene()->features().size() == 2; }, kOpenTimeoutMs));
    CHECK(w.scene()->bodyCount() == both);
    CHECK(tree->rowCount() == treeRows);
    if (active)
        QTest::keyClick(w.window(), Qt::Key_Z, Qt::ControlModifier);
    else
        REQUIRE(w.context()->invoke(QStringLiteral("edit.undo")));
    REQUIRE(QTest::qWaitFor([&] { return w.settled() && w.scene()->features().size() == 1; }, kOpenTimeoutMs));
    // Once, not twice (a second undo would remove the first import too).
    QTest::qWait(300);
    CHECK(w.app()->active()->document->undoCount() == 1);
    REQUIRE(w.context()->invoke(QStringLiteral("edit.redo")));
    REQUIRE(QTest::qWaitFor([&] { return w.settled() && w.scene()->features().size() == 2; }, kOpenTimeoutMs));

    // Undo down to the empty document: the view empties; one more undo is a note, not an error.
    REQUIRE(w.context()->invoke(QStringLiteral("edit.undo")));
    REQUIRE(w.context()->invoke(QStringLiteral("edit.undo")));
    REQUIRE(QTest::qWaitFor([&] { return w.settled() && w.scene()->features().empty(); }, kOpenTimeoutMs));
    CHECK(w.scene()->bodyCount() == 0);
    CHECK_FALSE(w.session()->canUndo());
    REQUIRE(w.context()->invoke(QStringLiteral("edit.undo")));
    CHECK(w.context()->statusText() == QStringLiteral("Nothing to undo"));
    // (The shortcuts above ran through the command palette's catalog, which reports results in the banner.)
    CHECK_FALSE(w.context()->messageIsError());
    w.checkNoWarnings();
}

TEST_CASE("the dirty marker and the unsaved-changes prompt on close, open-over and quit", "[dirty][prompt]")
{
    QTemporaryDir work;
    const QString raycad = QDir(work.path()).filePath(QStringLiteral("cover.raycad"));
    Window w("dark");
    QGuiApplication::setQuitOnLastWindowClosed(false);
    w.open(kBoxHole);
    CHECK(w.session()->dirty());
    CHECK(w.window()->title() == QStringLiteral("box_hole* - RayCAD"));

    // Close: the prompt asks; Cancel keeps the document.
    REQUIRE(w.context()->invoke(QStringLiteral("file.close")));
    REQUIRE(w.files()->promptVisible());
    w.grab();
    CHECK(w.shown("unsavedChangesDialog"));
    CHECK(w.files()->promptReason() == QStringLiteral("close"));
    w.shot("unsaved-changes-dark");
    w.click("unsavedCancel");
    CHECK_FALSE(w.files()->promptVisible());
    CHECK(w.session()->hasDocument());
    // Esc is Cancel too.
    (void)w.activate();
    w.files()->close();
    REQUIRE(w.files()->promptVisible());
    w.grab();
    QTest::qWait(100); // the dialog takes the focus once it is up
    QTest::keyClick(w.window(), Qt::Key_Escape);
    CHECK_FALSE(w.files()->promptVisible());
    CHECK(w.session()->hasDocument());

    // Close, Save: a document never saved needs a file name first (the Save As dialog); then it closes.
    QSignalSpy saveAsAsked(w.files(), &Files::saveAsDialogRequested);
    w.files()->close();
    REQUIRE(w.files()->promptVisible());
    w.click("unsavedSave");
    REQUIRE(saveAsAsked.count() == 1);
    CHECK(saveAsAsked.at(0).at(0).toString() == QStringLiteral("box_hole.raycad"));
    w.files()->saveAs(raycad, false);
    REQUIRE(QTest::qWaitFor([&] { return !w.session()->hasDocument(); }, 60000));
    CHECK(QFileInfo::exists(raycad));
    CHECK(w.shown("startScreen"));

    // Reopen it: clean. A change makes it dirty again.
    w.open(raycad);
    CHECK_FALSE(w.session()->dirty());
    CHECK(w.window()->title() == QStringLiteral("cover - RayCAD"));
    REQUIRE(w.context()->invoke(QStringLiteral("param.set"), QString::fromStdString(paramArgs("width", "25 mm"))));
    REQUIRE(QTest::qWaitFor([&] { return w.session()->dirty(); }, 10000));
    CHECK(w.window()->title() == QStringLiteral("cover* - RayCAD"));

    // Opening another file over it asks; Discard: the new one opens and the old one closes.
    REQUIRE(w.files()->open(kBoxHole));
    REQUIRE(w.files()->promptVisible());
    CHECK(w.files()->promptReason() == QStringLiteral("open"));
    CHECK(w.files()->promptDocument() == QStringLiteral("cover"));
    w.click("unsavedDiscard");
    REQUIRE(
        QTest::qWaitFor([&] { return w.session()->documentTitle() == QStringLiteral("box_hole"); }, kOpenTimeoutMs));
    w.waitSettled();
    CHECK(w.app()->documents().size() == 1);

    // Quit (closing the window) asks too; Cancel keeps the window, Discard closes it.
    REQUIRE(w.session()->dirty());
    w.window()->close();
    REQUIRE(QTest::qWaitFor([&] { return w.files()->promptVisible(); }, 5000));
    CHECK(w.window()->isVisible());
    CHECK(w.files()->promptReason() == QStringLiteral("quit"));
    w.click("unsavedCancel");
    CHECK(w.window()->isVisible());
    QSignalSpy quit(w.files(), &Files::quitApproved);
    w.window()->close();
    REQUIRE(QTest::qWaitFor([&] { return w.files()->promptVisible(); }, 5000));
    w.click("unsavedDiscard");
    REQUIRE(QTest::qWaitFor([&] { return quit.count() == 1; }, 5000));
    REQUIRE(QTest::qWaitFor([&] { return !w.window()->isVisible(); }, 5000));
    w.checkNoWarnings();
}

TEST_CASE("a file another session has open opens read-only and says who holds it", "[lock]")
{
    QTemporaryDir work;
    const QString raycad = QDir(work.path()).filePath(QStringLiteral("shared part.raycad"));
    Window w("dark");
    makeRaycad(w, kBoxHole, raycad);

    // Another session holds the write lock.
    store::OpenInfo info{};
    auto other = store::IDocumentFile::open(raycad.toStdString(), store::OpenMode::ReadWrite, &info);
    REQUIRE(other.has_value());
    REQUIRE_FALSE(info.openedReadOnlyBecauseLocked);

    w.open(raycad);
    CHECK(w.session()->readOnly());
    CHECK_FALSE(w.session()->lockHolder().isEmpty());
    CHECK_FALSE(w.session()->lockSince().isEmpty());
    CHECK(w.window()->title() == QStringLiteral("shared part (read-only) - RayCAD"));
    w.grab();
    REQUIRE(w.shown("readOnlyNotice"));
    const QString text = w.find("readOnlyHolder")->property("text").toString();
    INFO(text.toStdString());
    CHECK(text.contains(w.session()->lockHolder()));
    CHECK(text.contains(w.session()->lockSince()));
    CHECK(w.scene()->bodyCount() == 1);
    w.shot("read-only-dark");

    // Save cannot write the locked file: it offers Save As; the notice's button does too.
    QSignalSpy saveAsAsked(w.files(), &Files::saveAsDialogRequested);
    REQUIRE(w.context()->invoke(QStringLiteral("file.save")));
    CHECK(saveAsAsked.count() == 1);
    w.click("readOnlySaveAs");
    CHECK(saveAsAsked.count() == 2);
    const QString copy = QDir(work.path()).filePath(QStringLiteral("my copy.raycad"));
    w.files()->saveAs(copy);
    REQUIRE(QTest::qWaitFor([&] { return w.session()->documentPath() == copy; }, 60000));
    CHECK_FALSE(w.session()->readOnly());
    CHECK_FALSE(w.shown("readOnlyNotice"));
    other->reset();
    w.checkNoWarnings();
}

TEST_CASE("the recovery prompt lists pending recoveries; recover and discard go to the source", "[recovery][double]")
{
    QTemporaryDir work;
    const QString raycad = QDir(work.path()).filePath(QStringLiteral("journalled.raycad"));
    {
        Window maker("dark");
        makeRaycad(maker, kBoxHole, raycad);
    }

    auto fake = std::make_unique<FakeRecovery>();
    FakeRecovery* source = fake.get();
    ui::PendingRecovery saved;
    saved.id = QStringLiteral("journal-1");
    saved.filePath = raycad;
    saved.title = QStringLiteral("journalled");
    saved.lastWriteMs = QDateTime::currentMSecsSinceEpoch() - 120000;
    saved.steps = 2;
    saved.damagedTail = true;
    ui::PendingRecovery untitled;
    untitled.id = QStringLiteral("journal-2");
    untitled.title = QStringLiteral("Untitled");
    untitled.unsaved = true;
    untitled.steps = 1;
    fake->items = {saved, untitled};

    Window w("dark", true, std::move(fake));
    source->app = w.app();
    REQUIRE(QTest::qWaitFor([&] { return w.shown("recoveryDialog"); }, 5000));
    CHECK(w.session()->recoveries().size() == 2);
    CHECK(w.shown("recoveryRestore.0"));
    CHECK(w.shown("recoveryDamaged.0"));
    CHECK_FALSE(w.shown("recoveryDamaged.1"));
    w.shot("recovery-dark");

    // "Decide later" hides it (the journals stay); Esc does the same.
    w.click("recoveryLater");
    CHECK_FALSE(w.session()->recoveryPromptVisible());
    w.grab();
    CHECK_FALSE(w.shown("recoveryDialog"));
    w.session()->checkRecovery();
    REQUIRE(w.session()->recoveryPromptVisible());

    w.click("recoveryDiscard.1");
    REQUIRE(source->discarded->size() == 1);
    CHECK(source->discarded->front() == QStringLiteral("journal-2"));
    CHECK(w.session()->recoveries().size() == 1);
    w.grab();
    CHECK(w.shown("recoveryDialog"));

    QSignalSpy recoveredSpy(w.files(), &Files::recoveryRestored);
    w.click("recoveryRestore.0");
    REQUIRE(source->recovered->size() == 1);
    CHECK(source->recovered->front() == QStringLiteral("journal-1"));
    REQUIRE(QTest::qWaitFor([&] { return w.session()->hasDocument(); }, kOpenTimeoutMs));
    w.waitSettled();
    CHECK_FALSE(w.session()->recoveryPromptVisible());
    CHECK(w.session()->documentPath() == raycad);
    REQUIRE(recoveredSpy.count() == 1);
    CHECK(recoveredSpy.at(0).at(0).toString() == QStringLiteral("journalled"));
    w.checkNoWarnings();
}

TEST_CASE("crash recovery end to end: a session that ended without saving is recovered at the next start",
          "[recovery][journal]")
{
    QTemporaryDir data; // the per-user data folder both sessions share (journals live in <data>/recovery)
    QTemporaryDir work;
    const QString raycad = QDir(work.path()).filePath(QStringLiteral("journalled.raycad"));
    std::string expected;
    {
        Window w("dark", true, {}, data.path());
        makeRaycad(w, kBoxHole, raycad);
        w.open(raycad);
        REQUIRE(w.context()->invoke(QStringLiteral("param.set"), QString::fromStdString(paramArgs("width", "25 mm"))));
        REQUIRE(QTest::qWaitFor([&] { return w.session()->dirty(); }, 10000));
        REQUIRE(w.context()->invoke(QStringLiteral("body.set"),
                                    QStringLiteral(R"({"id":"%1","name":"Recovered plate"})")
                                        .arg(QString::fromStdString(w.scene()->body(0)->id.value.toString()))));
        REQUIRE(QTest::qWaitFor([&] { return w.session()->undoText().contains(QStringLiteral("Recovered plate")); },
                                10000));
        expected = w.stateHash();
        CHECK(w.context()->message().isEmpty()); // no "crash recovery is off" warning
        // The session ends without saving or closing the document (the app keeps its journal), like a crash.
    }

    Window w("dark", true, {}, data.path());
    REQUIRE(QTest::qWaitFor([&] { return w.shown("recoveryDialog"); }, 5000));
    REQUIRE(w.session()->recoveries().size() == 1);
    CHECK(w.session()->recoveries().front().toMap().value(QStringLiteral("title")).toString() ==
          QStringLiteral("journalled"));
    w.shot("recovery-real-dark");

    // Later, then open the file: the app refuses until the changes are recovered or discarded, so the prompt is back.
    w.click("recoveryLater");
    CHECK_FALSE(w.session()->recoveryPromptVisible());
    REQUIRE(w.files()->open(raycad));
    REQUIRE(QTest::qWaitFor([&] { return !w.files()->busy(); }, kOpenTimeoutMs));
    CHECK_FALSE(w.session()->hasDocument());
    REQUIRE(w.session()->recoveryPromptVisible());
    CHECK(w.context()->message().isEmpty());

    // Recover: the document reopens with the journalled changes, the same state hash, still unsaved.
    w.grab();
    w.click("recoveryRestore.0");
    REQUIRE(QTest::qWaitFor([&] { return w.session()->hasDocument(); }, kOpenTimeoutMs));
    w.waitSettled();
    CHECK(w.session()->documentPath() == raycad);
    CHECK(w.stateHash() == expected);
    CHECK(w.session()->dirty());
    CHECK(w.window()->title() == QStringLiteral("journalled* - RayCAD"));
    REQUIRE(w.scene()->bodyCount() == 1);
    CHECK(w.scene()->bodyName(0) == QStringLiteral("Recovered plate"));
    INFO(w.context()->message().toStdString());
    CHECK_FALSE(w.context()->messageIsError());

    // "Don't save" on close deletes the journal: nothing is pending any more.
    w.files()->close();
    REQUIRE(w.files()->promptVisible());
    w.click("unsavedDiscard");
    REQUIRE(QTest::qWaitFor([&] { return !w.session()->hasDocument(); }, 10000));
    CHECK(w.app()->pendingRecoveries().empty());
    w.checkNoWarnings();
}
TEST_CASE("no UI-thread stall over 50 ms while a mid-size corpus file opens", "[stall][wallclock]")
{
    // The software backend rasterises every frame on the UI thread, so there this would measure Qt's software
    // renderer, not RayCAD. Every other test here needs no rendering viewport and runs on any scene graph.
    if (!rhiSceneGraph())
        SKIP("the scene graph has no QRhi (software backend): frames are rasterised on the UI thread here");
    Window w("dark");
    REQUIRE(QFileInfo::exists(kMidSize));

    // Watchdog (ui_watchdog.h): the worst time the UI thread was busy or blocked between two of its 2 ms ticks.
    ray::test::UiWatchdog dog;
    dog.start();
    REQUIRE(w.files()->open(kMidSize));
    REQUIRE(ray::test::waitFor([&] { return w.settled(); }, kOpenTimeoutMs));
    ray::test::waitMs(300);
    dog.stop();
    WARN("UI-thread watchdog during the open of a mid-size corpus file: worst "
         << dog.worst().describe() << "; longest gap " << dog.worstWall().describe());
    CHECK(dog.worstStallMs() < ray::test::uiBudgetMs(50));
    CHECK(w.scene()->bodyCount() >= 1);
}

TEST_CASE("warm start to the first frame, against PLAN.md section 9", "[bench][wallclock]")
{
    // --bench-start prints the time from process creation to the first frame swapped and the session's start.
    QTemporaryDir data;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("RAYCAD_USER_DATA_DIR"), data.path());
    QStringList arguments{QStringLiteral("--bench-start"), QStringLiteral("--size"), QStringLiteral("1440x900")};
    // RAY_TEST_SCREEN: --size would centre the window on the primary screen; a saved window of the same size on the
    // test screen puts it there instead (the start then also reads that one small settings file).
    if (const std::optional<QRect> g = ray::test::screen::childWindowGeometry(QSize(1440, 900))) {
        QFile f(QDir(data.path()).filePath(QStringLiteral("settings.json")));
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(QStringLiteral(R"({"format":"raycad.settings","version":1,"window":{"x":%1,"y":%2,"width":%3,)"
                               R"("height":%4}})")
                    .arg(g->x())
                    .arg(g->y())
                    .arg(g->width())
                    .arg(g->height())
                    .toUtf8());
        arguments = QStringList{QStringLiteral("--bench-start")};
    }
    const QRegularExpression frameRe(QStringLiteral("first_frame_ms=([0-9.]+)"));
    const QRegularExpression sessionRe(QStringLiteral("session_ms=([0-9.]+)"));
    std::vector<double> frames, sessions;
    for (int run = 0; run < 7; ++run) {
        QProcess p;
        p.setProcessEnvironment(env);
        p.start(QStringLiteral(RAY_RAYCAD_EXE), arguments);
        REQUIRE(ray::test::screen::waitForChild(p, 60000));
        const QString out = QString::fromUtf8(p.readAllStandardOutput());
        INFO(out.toStdString() << QString::fromUtf8(p.readAllStandardError()).toStdString());
        REQUIRE(p.exitCode() == 0);
        const auto frame = frameRe.match(out);
        const auto session = sessionRe.match(out);
        REQUIRE(frame.hasMatch());
        REQUIRE(session.hasMatch());
        if (run >= 2) { // the first two warm the caches (PLAN.md 9: median of the runs after 2 discarded)
            frames.push_back(frame.captured(1).toDouble());
            sessions.push_back(session.captured(1).toDouble());
        }
    }
    std::sort(frames.begin(), frames.end());
    std::sort(sessions.begin(), sessions.end());
    const double frame = frames[frames.size() / 2];
    const double session = sessions[sessions.size() / 2];
    WARN("warm start to the first frame: median " << frame << " ms over " << frames.size()
                                                  << " runs (PLAN.md 9: Low 2500, Medium 1500, High 1000 ms); "
                                                  << "session start after it: median " << session << " ms");
    // Absolute budgets are checked on the reference machine (PLAN.md 9); here only the loosest one.
    CHECK(frame < ray::test::uiBudgetMs(2500));
}

int main(int argc, char** argv)
{
    // The real user profile is never touched: anything that falls back to the default per-user folder uses this.
    QTemporaryDir profile;
    qputenv("RAYCAD_USER_DATA_DIR", QDir::toNativeSeparators(profile.path()).toUtf8());
    QGuiApplication app(argc, argv);
    QGuiApplication::setQuitOnLastWindowClosed(false);
    return Catch::Session().run(argc, argv);
}
