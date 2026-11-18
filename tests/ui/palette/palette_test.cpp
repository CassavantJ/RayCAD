// The command palette, keyboard shortcuts and context menus (task T-021), on real Qt Quick windows with an
// application session (AppHost::startSession) whose user data folder is a temporary RAYCAD_USER_DATA_DIR.
//
//   - every registered action appears in the palette (found by its title); "sav" puts Save first
//   - Enter runs the highlighted action; the argument form generated from the schema hands valid JSON to it
//   - shortcuts are remappable, a conflict names the other action, the map survives an app restart
//   - a shortcut does not fire while a text field has the focus
//   - the context menu at the cursor lists the expected actions with nothing selected and with a face selected
//   - nothing is created before first use; disabled commands say why; long-running ones use the jobs list
//   - with a document open the file, edit and export commands enable; a read-only one disables Save (Save As hint);
//     file commands that need a path use the native file dialog instead of the form (T-019)
#include "app_context.h"
#include "app_host.h"
#include "app_options.h"
#include "files.h"
#include "palette_commands.h"
#include "palette_model.h"
#include "palette_schema.h"
#include "scene_controller.h"
#include "selection_model.h"
#include "session.h"
#include "shortcuts.h"
#include "theme_object.h"
#include "viewport_item.h"
#include "viewport_navigation.h"

#include <ray/api.h>

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTemporaryDir>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>
#include <QtGui/QMouseEvent>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>
#include <QtQuick/QSGRendererInterface>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace ray;
using ray::ui::AppHost;
using ray::ui::CommandCatalog;
using ray::ui::PaletteModel;
using ray::ui::ShortcutManager;

namespace {

const QString kBoxHole = QStringLiteral(RAY_KERNEL_TEST_DATA_DIR "/box_hole.step");
constexpr int kOpenTimeoutMs = 120000;

// Whether the viewport can render (and so answer a pick), asked of a real, shown window: QQuickWindow::graphicsApi()
// only says what would be requested, and where the platform has no OpenGL (the offscreen platform on Linux) Qt Quick
// falls back to its software backend afterwards ("No QRhi found for window").
bool rhiSceneGraph()
{
    static const bool rhi = [] {
        QQuickWindow probe;
        probe.resize(64, 64);
        probe.show();
        if (!QTest::qWaitForWindowExposed(&probe))
            return false;
        (void)probe.grabWindow(); // initialises the scene graph
        const QSGRendererInterface* renderer = probe.rendererInterface();
        return renderer != nullptr && QSGRendererInterface::isApiRhiBased(renderer->graphicsApi());
    }();
    return rhi;
}

// Calls of the test actions, from any thread.
struct Calls {
    std::mutex mutex;
    std::vector<std::pair<std::string, std::string>> list; // id, args JSON
    void add(std::string id, std::string_view args)
    {
        std::lock_guard lock(mutex);
        list.emplace_back(std::move(id), std::string(args));
    }
    std::size_t count(const std::string& id)
    {
        std::lock_guard lock(mutex);
        std::size_t n = 0;
        for (const auto& c : list)
            n += c.first == id ? 1 : 0;
        return n;
    }
    std::string last(const std::string& id)
    {
        std::lock_guard lock(mutex);
        for (auto it = list.rbegin(); it != list.rend(); ++it)
            if (it->first == id)
                return it->second;
        return {};
    }
};

constexpr const char* kRecordSchema = R"({"type":"object","additionalProperties":false,
    "required":["name","count","mode"],
    "properties":{
      "name":{"type":"string","minLength":1,"description":"A name for the call"},
      "count":{"type":"integer","minimum":0,"description":"How many"},
      "mode":{"enum":["fast","exact"],"description":"How to do it"},
      "flag":{"type":"boolean","default":false,"description":"A switch"},
      "scale":{"type":"number","default":1.5,"description":"A factor"},
      "file":{"type":"string","description":"An input file"}}})";

void addTestActions(api::IApp& app, const std::shared_ptr<Calls>& calls)
{
    auto record = [calls](const char* id) {
        return [calls, id](std::string_view args, api::Caller, JobContext&) -> Result<api::ActionResult> {
            calls->add(id, args);
            return api::ActionResult{R"({"recorded":true})", {}};
        };
    };
    REQUIRE(app.actions().add(api::ActionInfo{"test.ping", "Ping Test Action", "Test", "Records that it ran",
                                              R"({"type":"object","properties":{}})", "", false, false, false},
                              record("test.ping")));
    REQUIRE(
        app.actions().add(api::ActionInfo{"test.record", "Record Test Call", "Test",
                                          "Records the arguments it was given", kRecordSchema, "", false, false, false},
                          record("test.record")));
    REQUIRE(
        app.actions().add(api::ActionInfo{"test.slow", "Slow Test Job", "Test", "Runs for a moment on a worker",
                                          R"({"type":"object","properties":{}})", "", false, false, true},
                          [calls](std::string_view args, api::Caller, JobContext& ctx) -> Result<api::ActionResult> {
                              for (int i = 0; i < 30; ++i) {
                                  if (auto st = ctx.cancel.check(); !st)
                                      return tl::unexpected<Error>(st.error());
                                  ctx.progress.set(i / 30.0, "working");
                                  std::this_thread::sleep_for(std::chrono::milliseconds(10));
                              }
                              calls->add("test.slow", args);
                              return api::ActionResult{"{}", {}};
                          }));
}

// One application window with the real session (AppHost::startSession, T-019). The user data folder is set through
// RAYCAD_USER_DATA_DIR too.
struct Window {
    QTemporaryDir cache;
    std::unique_ptr<AppHost> host;
    std::shared_ptr<Calls> calls = std::make_shared<Calls>();

    explicit Window(const QString& userData, bool session = true, const char* theme = "dark",
                    QSize size = QSize(1440, 900))
    {
        qputenv("RAYCAD_USER_DATA_DIR", QDir::toNativeSeparators(userData).toLocal8Bit());
        ui::AppOptions options;
        options.theme = QString::fromLatin1(theme);
        options.glass = false;
        options.size = size;
        host = std::make_unique<AppHost>(options, cache.path());
        REQUIRE(host->optionError().isEmpty());
        REQUIRE(host->load());
        host->files()->setUseNativeDialogs(false);
        host->show();
        REQUIRE(QTest::qWaitForWindowExposed(host->window()));
        if (session) {
            // As runApp does: after the window is up.
            REQUIRE(host->startSession(userData.toStdString()));
            REQUIRE(QDir::cleanPath(QString::fromStdString(app()->userDataDir())) == QDir::cleanPath(userData));
            addTestActions(*app(), calls);
        }
        if (ui::ViewportItem* v = host->viewport()) {
            v->setAnimationsEnabled(false);
            v->forceActiveFocus();
        }
        host->scene()->setPreset(render::presetFor(render::Tier::Medium));
        grab();
    }

    api::IApp* app() const { return host->app(); }
    QQuickWindow* window() const { return host->window(); }
    CommandCatalog* commands() const { return host->commands(); }
    ShortcutManager* shortcuts() const { return host->shortcuts(); }
    ui::AppContext* context() const { return host->context(); }
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
    // A QObject (popups are not items) that is a direct child of an item in the visual tree.
    QObject* findObject(const QString& objectName) const { return findObjectIn(window()->contentItem(), objectName); }
    static QObject* findObjectIn(QQuickItem* item, const QString& objectName)
    {
        if (QObject* found = item->findChild<QObject*>(objectName, Qt::FindDirectChildrenOnly))
            return found;
        for (QQuickItem* child : item->childItems()) {
            if (QObject* found = findObjectIn(child, objectName))
                return found;
        }
        return nullptr;
    }
    // The item, if it exists and is shown.
    QQuickItem* shown(const QString& objectName) const
    {
        QQuickItem* item = find(objectName);
        return item != nullptr && item->isVisible() ? item : nullptr;
    }

    void checkNoWarnings() const
    {
        for (const QString& warning : host->warnings())
            UNSCOPED_INFO(warning.toStdString());
        CHECK(host->warnings().isEmpty());
    }

    // ---- the palette ----
    bool paletteOpen() const
    {
        QObject* popup = findObject(QStringLiteral("commandPalette"));
        return popup != nullptr && popup->property("opened").toBool();
    }
    QQuickItem* search() const { return find(QStringLiteral("paletteSearch")); }
    PaletteModel* model() const
    {
        QQuickItem* list = find(QStringLiteral("paletteList"));
        return list ? qobject_cast<PaletteModel*>(list->property("model").value<QObject*>()) : nullptr;
    }
    int currentRow() const { return find(QStringLiteral("paletteList"))->property("currentIndex").toInt(); }
    QString currentId() const { return model()->get(currentRow()).value(QStringLiteral("commandId")).toString(); }
    void openPalette() const
    {
        QTest::keyClick(window(), Qt::Key_K, Qt::ControlModifier);
        REQUIRE(QTest::qWaitFor([&] { return paletteOpen(); }, 3000));
        REQUIRE(search() != nullptr);
        REQUIRE(search()->hasActiveFocus());
        REQUIRE(model() != nullptr);
    }
    void typeQuery(const QString& text) const
    {
        search()->setProperty("text", QString());
        for (const QChar ch : text)
            QTest::keyClick(window(), ch.toLatin1());
        REQUIRE(search()->property("text").toString() == text);
    }

    // ---- input, in window coordinates ----
    void mouse(QEvent::Type type, QPointF pos, Qt::MouseButton button, Qt::MouseButtons buttons,
               Qt::KeyboardModifiers mods = Qt::NoModifier) const
    {
        QMouseEvent e(type, pos, window()->mapToGlobal(pos), button, buttons, mods);
        QGuiApplication::sendEvent(window(), &e);
    }
    void click(QPointF pos, Qt::MouseButton button = Qt::LeftButton) const
    {
        mouse(QEvent::MouseButtonPress, pos, button, button);
        mouse(QEvent::MouseButtonRelease, pos, button, Qt::NoButton);
    }
    QPointF centreOf(QQuickItem* item) const
    {
        return item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
    }

    bool waitFinished(const QString& id, int timeoutMs = 10000) const
    {
        bool done = false;
        auto c = QObject::connect(commands(), &CommandCatalog::finished,
                                  [&](const QString& which, bool, const QString&) { done = done || which == id; });
        const bool ok = QTest::qWaitFor([&] { return done; }, timeoutMs);
        QObject::disconnect(c);
        return ok;
    }
};

QStringList menuTexts(const Window& w, const QStringList& ids)
{
    QStringList present;
    for (const QString& id : ids) {
        if (w.shown(QStringLiteral("contextItem.") + id))
            present << id;
    }
    return present;
}

std::optional<QPointF> project(const Window& w, const Vec3& world)
{
    ui::ViewportItem* view = w.host->viewport();
    const auto p = ui::nav::project(view->camera(), world, view->width(), view->height());
    if (!p)
        return std::nullopt;
    return view->mapToScene(*p);
}

// The middle of the top face of the box_hole part, away from the hole.
QPointF topFacePoint(const Window& w)
{
    const auto box = w.host->viewport()->viewport()->sceneBounds();
    REQUIRE(box.has_value());
    const Box3& b = *box;
    const auto at = project(w, Vec3{b.min.x + (b.max.x - b.min.x) * 0.2, b.min.y + (b.max.y - b.min.y) * 0.5, b.max.z});
    REQUIRE(at.has_value());
    return *at;
}

void openPart(Window& w)
{
    REQUIRE(w.host->files()->open(kBoxHole));
    REQUIRE(QTest::qWaitFor(
        [&] {
            return !w.host->files()->busy() && w.host->session()->hasDocument() && w.host->scene()->ready() &&
                   !w.host->scene()->refining() && !w.host->viewport()->animating();
        },
        kOpenTimeoutMs));
    w.grab();
}

QString screenshotDir()
{
    return qEnvironmentVariable("RAY_UI_SCREENSHOT_DIR");
}

void saveShot(const Window& w, const QString& name)
{
    QTest::qWait(250);
    const QImage image = w.grab();
    const QString path = QDir(screenshotDir()).filePath(name);
    REQUIRE(image.save(path, "PNG"));
    UNSCOPED_INFO("screenshot " << path.toStdString());
}

} // namespace

TEST_CASE("the argument form is generated from the JSON Schema and builds valid JSON", "[palette][schema]")
{
    const QString schema = QString::fromLatin1(kRecordSchema);
    const QVariantList fields = ui::palette::formFields(QStringLiteral("test.record"), schema);
    REQUIRE(fields.size() == 6);
    QStringList names;
    for (const QVariant& f : fields)
        names << f.toMap().value(QStringLiteral("name")).toString();
    // Required first, in the schema's order, then the optional ones by name.
    CHECK(names == QStringList({"name", "count", "mode", "file", "flag", "scale"}));
    const QVariantMap mode = fields[2].toMap();
    CHECK(mode.value(QStringLiteral("kind")).toString() == QStringLiteral("enum"));
    CHECK(mode.value(QStringLiteral("choices")).toStringList() == QStringList({"fast", "exact"}));
    const QVariantMap file = fields[3].toMap();
    CHECK(file.value(QStringLiteral("path")).toBool());
    CHECK_FALSE(file.value(QStringLiteral("save")).toBool());
    CHECK(ui::palette::needsForm(schema));
    // "document" is never shown and never makes a form necessary: actions use the active document without it.
    const QString documentOnly =
        QStringLiteral(R"({"type":"object","properties":{"document":{"type":"string"}},"required":["document"]})");
    CHECK_FALSE(ui::palette::needsForm(documentOnly));
    CHECK(ui::palette::formFields(QStringLiteral("x"), documentOnly).isEmpty());

    auto build = [&](QVariantMap values) {
        return ui::palette::buildArgs(schema, values);
    };
    auto missing = build({{"name", "a"}, {"count", ""}, {"mode", "fast"}});
    CHECK_FALSE(missing.ok);
    CHECK(missing.field == QStringLiteral("count"));
    CHECK_FALSE(build({{"name", "a"}, {"count", "1.5"}, {"mode", "fast"}}).ok);
    CHECK_FALSE(build({{"name", "a"}, {"count", "x"}, {"mode", "fast"}}).ok);
    auto ok = build({{"name", "Part 7"}, {"count", " 12 "}, {"mode", "exact"}, {"flag", true}, {"scale", ""}});
    REQUIRE(ok.ok);
    const QJsonObject o = QJsonDocument::fromJson(ok.json.toUtf8()).object();
    CHECK(o.value("name").toString() == QStringLiteral("Part 7"));
    CHECK(o.value("count").toInteger() == 12);
    CHECK(o.value("mode").toString() == QStringLiteral("exact"));
    CHECK(o.value("flag").toBool());
    CHECK_FALSE(o.contains("scale")); // empty optional: the default applies
    CHECK_FALSE(o.contains("file"));
}

TEST_CASE("nothing is created before first use", "[palette][lazy]")
{
    QTemporaryDir data;
    Window w(data.path());
    CHECK(w.findObject(QStringLiteral("commandPalette")) == nullptr);
    CHECK(w.findObject(QStringLiteral("shortcutSettings")) == nullptr);
    CHECK(w.findObject(QStringLiteral("contextMenu")) == nullptr);
    CHECK(w.find(QStringLiteral("paletteLoader"))->property("item").value<QObject*>() == nullptr);
    CHECK(w.find(QStringLiteral("shortcutSettingsLoader"))->property("item").value<QObject*>() == nullptr);
    // No shortcut file or use statistics were read or written.
    CHECK_FALSE(w.shortcuts()->loaded());
    CHECK_FALSE(QFileInfo::exists(QDir(data.path()).filePath(QStringLiteral("palette-usage.json"))));

    // Tab passes through the top bar's search field without opening the palette; a click opens it.
    QQuickItem* field = w.find(QStringLiteral("commandSearch"));
    REQUIRE(field != nullptr);
    field->forceActiveFocus(Qt::TabFocusReason);
    QTest::qWait(50);
    CHECK_FALSE(w.paletteOpen());
    w.click(w.centreOf(field));
    REQUIRE(QTest::qWaitFor([&] { return w.paletteOpen(); }, 3000));
    CHECK(w.search()->hasActiveFocus());
    CHECK_FALSE(field->hasActiveFocus());
    QTest::keyClick(w.window(), Qt::Key_Escape);
    REQUIRE(QTest::qWaitFor([&] { return !w.paletteOpen(); }, 3000));
    // The focus goes back to the view, not to the search field (which would reopen the palette).
    CHECK(w.host->viewport()->hasActiveFocus());
    w.checkNoWarnings();
}

TEST_CASE("every registered action appears in the palette; typing sav puts Save first", "[palette]")
{
    QTemporaryDir data;
    Window w(data.path());
    w.openPalette();
    PaletteModel& model = *w.model();
    const std::vector<api::ActionInfo> all = w.app()->actions().all();
    REQUIRE(all.size() > 15);
    // An empty query lists everything.
    CHECK(model.count() >= static_cast<int>(all.size()));
    for (const api::ActionInfo& a : all) {
        const QString title = QString::fromStdString(a.title);
        INFO("action " << a.id << " '" << a.title << "'");
        w.search()->setProperty("text", title);
        const int row = model.indexOf(QString::fromStdString(a.id));
        CHECK(row >= 0);
        CHECK(model.indexOfTitle(title) >= 0);
    }
    // The view commands are there too.
    w.search()->setProperty("text", QStringLiteral("fit all"));
    CHECK(model.indexOf(QStringLiteral("view.fitAll")) >= 0);

    w.typeQuery(QStringLiteral("sav"));
    REQUIRE(model.count() > 0);
    CHECK(model.get(0).value(QStringLiteral("commandId")).toString() == QStringLiteral("file.save"));
    CHECK(model.get(0).value(QStringLiteral("title")).toString() == QStringLiteral("Save"));
    CHECK(w.currentRow() == 0);
    // Its shortcut is shown, and why it cannot run now (no document is open).
    CHECK(model.get(0).value(QStringLiteral("shortcut")).toString() ==
          QKeySequence(QStringLiteral("Ctrl+S")).toString(QKeySequence::NativeText));
    CHECK_FALSE(model.get(0).value(QStringLiteral("available")).toBool());
    CHECK(model.get(0).value(QStringLiteral("reason")).toString().contains(QStringLiteral("document")));
    QQuickItem* row = w.find(QStringLiteral("paletteRow.file.save"));
    REQUIRE(row != nullptr);
    QQuickItem* detail = Window::findItem(row, QStringLiteral("detail"));
    REQUIRE(detail != nullptr);
    CHECK(detail->isVisible());

    // Down moves the highlight; Esc closes.
    QTest::keyClick(w.window(), Qt::Key_Down);
    CHECK(w.currentRow() == 1);
    QTest::keyClick(w.window(), Qt::Key_Up);
    CHECK(w.currentRow() == 0);
    QTest::keyClick(w.window(), Qt::Key_Escape);
    REQUIRE(QTest::qWaitFor([&] { return !w.paletteOpen(); }, 3000));
    w.checkNoWarnings();
}

TEST_CASE("Enter runs the highlighted action; the generated form hands valid JSON to it", "[palette][form]")
{
    QTemporaryDir data;
    Window w(data.path());
    ui::AppContext& context = *w.context();

    // No required arguments: Enter runs it at once and the banner shows the result.
    w.openPalette();
    w.typeQuery(QStringLiteral("ping test"));
    REQUIRE(w.currentId() == QStringLiteral("test.ping"));
    QTest::keyClick(w.window(), Qt::Key_Return);
    REQUIRE(QTest::qWaitFor([&] { return w.calls->count("test.ping") == 1; }, 5000));
    CHECK(w.calls->last("test.ping") == "{}");
    CHECK_FALSE(w.paletteOpen());
    REQUIRE(QTest::qWaitFor([&] { return context.message().contains(QStringLiteral("Ping Test Action")); }, 5000));
    CHECK_FALSE(context.messageIsError());

    // The most used command comes first when the query is empty, and the use count is saved.
    w.openPalette();
    CHECK(w.model()->get(0).value(QStringLiteral("commandId")).toString() == QStringLiteral("test.ping"));
    CHECK(w.model()->get(0).value(QStringLiteral("section")).toString() == QStringLiteral("recent"));
    CHECK(QFileInfo::exists(QDir(data.path()).filePath(QStringLiteral("palette-usage.json"))));

    // Required arguments: Enter opens the form first.
    w.typeQuery(QStringLiteral("record test"));
    REQUIRE(w.currentId() == QStringLiteral("test.record"));
    QTest::keyClick(w.window(), Qt::Key_Return);
    REQUIRE(w.shown(QStringLiteral("paletteForm")) != nullptr);
    CHECK(w.calls->count("test.record") == 0);
    QQuickItem* name = w.find(QStringLiteral("formField.name"));
    QQuickItem* count = w.find(QStringLiteral("formField.count"));
    QQuickItem* mode = w.find(QStringLiteral("formField.mode"));
    QQuickItem* flag = w.find(QStringLiteral("formField.flag"));
    QQuickItem* file = w.find(QStringLiteral("formField.file"));
    REQUIRE(name != nullptr);
    REQUIRE(count != nullptr);
    REQUIRE(mode != nullptr);
    REQUIRE(flag != nullptr);
    REQUIRE(file != nullptr);
    CHECK(w.shown(QStringLiteral("browse.file")) != nullptr);
    CHECK(w.find(QStringLiteral("formField.document")) == nullptr);
    REQUIRE(QTest::qWaitFor([&] { return name->hasActiveFocus(); }, 2000));

    // Missing and malformed values are reported in the form; nothing runs.
    for (const char ch : std::string("Part 7"))
        QTest::keyClick(w.window(), ch);
    QTest::keyClick(w.window(), Qt::Key_Return);
    REQUIRE(w.shown(QStringLiteral("formError")) != nullptr);
    CHECK(w.find(QStringLiteral("formError"))->property("text").toString().contains(QStringLiteral("Count")));
    count->setProperty("text", QStringLiteral("twelve"));
    QTest::keyClick(w.window(), Qt::Key_Return);
    CHECK(w.find(QStringLiteral("formError"))->property("text").toString().contains(QStringLiteral("number")));
    CHECK(w.calls->count("test.record") == 0);

    count->setProperty("text", QStringLiteral("12"));
    mode->setProperty("currentIndex", 1);
    flag->setProperty("checked", true);
    file->setProperty("text", QStringLiteral("parts/plate.step"));
    name->forceActiveFocus();
    QTest::keyClick(w.window(), Qt::Key_Return);
    REQUIRE(QTest::qWaitFor([&] { return w.calls->count("test.record") == 1; }, 5000));
    CHECK_FALSE(w.paletteOpen());
    QJsonParseError error{};
    const QJsonDocument args = QJsonDocument::fromJson(QByteArray::fromStdString(w.calls->last("test.record")), &error);
    INFO("args " << w.calls->last("test.record"));
    REQUIRE(error.error == QJsonParseError::NoError);
    REQUIRE(args.isObject());
    const QJsonObject o = args.object();
    CHECK(o.value("name").toString() == QStringLiteral("Part 7"));
    CHECK(o.value("count").toInteger() == 12);
    CHECK(o.value("mode").toString() == QStringLiteral("exact"));
    CHECK(o.value("flag").toBool());
    CHECK(o.value("file").toString() == QStringLiteral("parts/plate.step"));
    CHECK(o.value("scale").toDouble() == 1.5); // the registry filled in the default

    // Esc in the form goes back to the list, a second Esc closes.
    w.openPalette();
    w.typeQuery(QStringLiteral("record test"));
    QTest::keyClick(w.window(), Qt::Key_Return);
    REQUIRE(w.shown(QStringLiteral("paletteForm")) != nullptr);
    QTest::keyClick(w.window(), Qt::Key_Escape);
    CHECK(w.shown(QStringLiteral("paletteForm")) == nullptr);
    CHECK(w.paletteOpen());
    QTest::keyClick(w.window(), Qt::Key_Escape);
    REQUIRE(QTest::qWaitFor([&] { return !w.paletteOpen(); }, 3000));

    // A mouse click runs a row too.
    w.openPalette();
    w.typeQuery(QStringLiteral("ping test"));
    QQuickItem* row = w.find(QStringLiteral("paletteRow.test.ping"));
    REQUIRE(row != nullptr);
    w.click(w.centreOf(row));
    REQUIRE(QTest::qWaitFor([&] { return w.calls->count("test.ping") == 2; }, 5000));
    w.checkNoWarnings();
}

TEST_CASE("commands that cannot run say why; long-running ones use the job slot", "[palette][state]")
{
    QTemporaryDir data;
    Window w(data.path());
    CommandCatalog& commands = *w.commands();
    auto reason = [&](const char* id) {
        return commands.info(QString::fromLatin1(id)).value("reason").toString();
    };
    CHECK(reason("file.save") == QStringLiteral("Open or create a document first"));
    CHECK(reason("edit.undo") == QStringLiteral("Open or create a document first"));
    CHECK(reason("file.new").isEmpty());
    CHECK(reason("view.fitSelection") == QStringLiteral("Nothing is selected"));

    // Triggering a disabled command shows the reason in the banner and runs nothing.
    CHECK_FALSE(commands.trigger(QStringLiteral("file.save")));
    CHECK(w.context()->messageIsError());
    CHECK(w.context()->message().contains(QStringLiteral("Open or create a document first")));

    REQUIRE(commands.run(QStringLiteral("file.new")));
    REQUIRE(w.waitFinished(QStringLiteral("file.new")));
    REQUIRE(w.app()->active() != nullptr);
    CHECK(reason("file.save").isEmpty());
    CHECK(reason("edit.undo") == QStringLiteral("There is nothing to undo"));
    w.app()->active()->readOnly = true;
    CHECK(reason("file.save").contains(QStringLiteral("read-only")));
    CHECK(reason("edit.undo").contains(QStringLiteral("read-only")));
    CHECK(reason("file.saveAs").isEmpty());
    w.app()->active()->readOnly = false;

    // A long-running action is an app job: the jobs list and the status bar's slot show it while it runs.
    ui::Session& session = *w.host->session();
    // (The new document's first rebuild may show briefly too: wait for the action's own job.)
    auto slowShown = [&] {
        return session.jobTitle().startsWith(QStringLiteral("Slow Test Job"));
    };
    REQUIRE(commands.run(QStringLiteral("test.slow")));
    REQUIRE(QTest::qWaitFor(slowShown, 3000));
    REQUIRE(w.waitFinished(QStringLiteral("test.slow")));
    REQUIRE(QTest::qWaitFor([&] { return !session.jobActive(); }, 3000));
    CHECK(w.calls->count("test.slow") == 1);
    // Cancel in the job slot (or Esc) cancels it.
    REQUIRE(commands.run(QStringLiteral("test.slow")));
    REQUIRE(QTest::qWaitFor(slowShown, 3000));
    session.cancelForegroundJob();
    CHECK_FALSE(session.jobActive());
    REQUIRE(w.waitFinished(QStringLiteral("test.slow")));
    CHECK(w.calls->count("test.slow") == 1);
    CHECK(w.context()->message().contains(QStringLiteral("cancelled")));
    w.checkNoWarnings();
}

TEST_CASE("file and edit commands follow the session's document; path commands use the file dialogs",
          "[palette][session]")
{
    // Needs no rendering viewport (no pick, no pixels): runs on every scene graph.
    QTemporaryDir data;
    QTemporaryDir work;
    Window w(data.path());
    CommandCatalog& commands = *w.commands();
    ui::Files& files = *w.host->files();
    auto reason = [&](const char* id) {
        return commands.info(QString::fromLatin1(id)).value("reason").toString();
    };
    const char* needDocument[] = {"file.save",       "file.saveAs",    "file.close", "file.exportStep",
                                  "file.exportIges", "file.exportStl", "edit.undo",  "edit.redo"};
    for (const char* id : needDocument) {
        INFO(id);
        CHECK(reason(id) == QStringLiteral("Open or create a document first"));
    }
    CHECK(reason("file.open").isEmpty());
    CHECK(reason("file.import").isEmpty());

    // Interactively, open asks for a file with the native dialog (not the palette's form), and its shortcut does
    // it exactly once.
    QSignalSpy openAsked(&files, &ui::Files::openDialogRequested);
    QSignalSpy formAsked(&commands, &CommandCatalog::formRequested);
    REQUIRE(commands.trigger(QStringLiteral("file.open")));
    CHECK(openAsked.count() == 1);
    CHECK(formAsked.count() == 0);
    w.window()->requestActivate();
    if (QTest::qWaitForWindowActive(w.window(), 3000)) {
        w.host->viewport()->forceActiveFocus();
        QTest::keyClick(w.window(), Qt::Key_O, Qt::ControlModifier);
        QTest::qWait(100);
        CHECK(openAsked.count() == 2);
        // Ctrl+N makes exactly one new document.
        QTest::keyClick(w.window(), Qt::Key_N, Qt::ControlModifier);
        REQUIRE(QTest::qWaitFor([&] { return w.app()->active() != nullptr; }, 3000));
        QTest::qWait(100);
        CHECK(w.app()->documents().size() == 1);
        CHECK_FALSE(files.promptVisible());
        REQUIRE(w.app()->close(*w.app()->active()).has_value());
        REQUIRE(QTest::qWaitFor([&] { return !w.host->session()->hasDocument(); }, 3000));
    } else {
        WARN("the test window could not be made active: the shortcut part is skipped");
    }

    // A part open: the file, edit and export commands are available (undo: the import).
    openPart(w);
    for (const char* id : {"file.save", "file.saveAs", "file.close", "file.exportStep", "file.exportIges",
                           "file.exportStl", "edit.undo"}) {
        INFO(id);
        CHECK(reason(id).isEmpty());
    }
    CHECK(reason("edit.redo") == QStringLiteral("There is nothing to redo"));
    // Save of a never-saved document and the export commands ask for a file with the native dialog.
    QSignalSpy saveAsAsked(&files, &ui::Files::saveAsDialogRequested);
    QSignalSpy exportAsked(&files, &ui::Files::exportDialogRequested);
    REQUIRE(commands.trigger(QStringLiteral("file.saveAs")));
    REQUIRE(commands.trigger(QStringLiteral("file.exportStl")));
    CHECK(saveAsAsked.count() == 1);
    REQUIRE(exportAsked.count() == 1);
    CHECK(exportAsked.at(0).at(0).toString() == QStringLiteral("stl"));
    CHECK(formAsked.count() == 0);
    // With a path from the generated form, Files runs it.
    const QString saved = QDir(work.path()).filePath(QStringLiteral("palette part.raycad"));
    REQUIRE(commands.run(QStringLiteral("file.saveAs"),
                         QStringLiteral(R"({"path":"%1"})").arg(QDir::fromNativeSeparators(saved))));
    REQUIRE(QTest::qWaitFor([&] { return w.host->session()->documentPath() == saved && !w.host->session()->dirty(); },
                            60000));
    REQUIRE(w.app()->close(*w.app()->active()).has_value());
    REQUIRE(QTest::qWaitFor([&] { return !w.host->session()->hasDocument(); }, 3000));

    // Another session holds the file's write lock: it opens read-only; Save is disabled with the Save As hint.
    store::OpenInfo info{};
    auto other = store::IDocumentFile::open(saved.toStdString(), store::OpenMode::ReadWrite, &info);
    REQUIRE(other.has_value());
    REQUIRE(files.open(saved));
    REQUIRE(QTest::qWaitFor([&] { return w.host->session()->readOnly(); }, 60000));
    CHECK(reason("file.save") == QStringLiteral("The document is read-only; use Save As"));
    CHECK(reason("file.saveAs").isEmpty());
    CHECK(reason("file.exportStep").isEmpty());
    CHECK_FALSE(commands.trigger(QStringLiteral("file.save")));
    CHECK(w.context()->message().contains(QStringLiteral("Save As")));
    other->reset();
    w.checkNoWarnings();
}

TEST_CASE("shortcuts are remappable, conflicts name the other action, the map persists", "[shortcuts]")
{
    QTemporaryDir data;
    const QString file = QDir(data.path()).filePath(QStringLiteral("shortcuts.json"));
    {
        Window w(data.path());
        ShortcutManager& shortcuts = *w.shortcuts();
        CHECK(shortcuts.sequence(QStringLiteral("file.save")) == QStringLiteral("Ctrl+S"));
        CHECK(shortcuts.sequence(QStringLiteral("ui.commandPalette")) == QStringLiteral("Ctrl+K"));
        CHECK(shortcuts.conflicts().isEmpty());

        // A key another action owns is a conflict, reported with that action's name; nothing changes.
        QVariantMap r = shortcuts.assign(QStringLiteral("test.ping"), QStringLiteral("Ctrl+S"));
        CHECK_FALSE(r.value("ok").toBool());
        CHECK(r.value("conflictId").toString() == QStringLiteral("file.save"));
        CHECK(r.value("conflictTitle").toString() == QStringLiteral("Save"));
        CHECK(r.value("message").toString().contains(QStringLiteral("Save")));
        CHECK(shortcuts.sequence(QStringLiteral("test.ping")).isEmpty());

        // A free key is taken and works at once.
        r = shortcuts.assign(QStringLiteral("test.ping"), QStringLiteral("Ctrl+Alt+P"));
        CHECK(r.value("ok").toBool());
        QTest::keyClick(w.window(), Qt::Key_P, Qt::ControlModifier | Qt::AltModifier);
        REQUIRE(QTest::qWaitFor([&] { return w.calls->count("test.ping") == 1; }, 5000));

        // Replacing moves the key over and leaves the other action without one.
        r = shortcuts.assign(QStringLiteral("test.record"), QStringLiteral("Ctrl+O"), true);
        CHECK(r.value("ok").toBool());
        CHECK(shortcuts.sequence(QStringLiteral("file.open")).isEmpty());
        CHECK(shortcuts.commandFor(QStringLiteral("Ctrl+O")) == QStringLiteral("test.record"));
        // Ctrl+O now asks for test.record's arguments.
        QTest::keyClick(w.window(), Qt::Key_O, Qt::ControlModifier);
        REQUIRE(QTest::qWaitFor([&] { return w.paletteOpen(); }, 3000));
        CHECK(w.shown(QStringLiteral("paletteForm")) != nullptr);
        QTest::keyClick(w.window(), Qt::Key_Escape);
        QTest::keyClick(w.window(), Qt::Key_Escape);
        REQUIRE(QTest::qWaitFor([&] { return !w.paletteOpen(); }, 3000));

        // The settings panel records a key, reports a conflict naming the other action, and replaces on request.
        w.commands()->openShortcutSettings();
        REQUIRE(QTest::qWaitFor(
            [&] {
                return w.findObject(QStringLiteral("shortcutSettings")) &&
                       w.findObject(QStringLiteral("shortcutSettings"))->property("opened").toBool();
            },
            3000));
        QQuickItem* filter = w.find(QStringLiteral("shortcutFilter"));
        REQUIRE(filter != nullptr);
        filter->setProperty("text", QStringLiteral("ping"));
        w.grab();
        QQuickItem* key = w.find(QStringLiteral("shortcutKey.test.ping"));
        REQUIRE(key != nullptr);
        w.click(w.centreOf(key));
        CHECK(shortcuts.recording());
        QTest::keyClick(w.window(), Qt::Key_N, Qt::ControlModifier); // New Document's key
        CHECK_FALSE(shortcuts.recording());
        REQUIRE(w.shown(QStringLiteral("shortcutConflict")) != nullptr);
        const QString text = w.find(QStringLiteral("shortcutConflictText"))->property("text").toString();
        INFO("conflict text: " << text.toStdString());
        CHECK(text.contains(QStringLiteral("New Document")));
        CHECK(shortcuts.sequence(QStringLiteral("test.ping")) == QStringLiteral("Ctrl+Alt+P"));
        // Recording did not run New Document.
        CHECK(w.app()->active() == nullptr);
        REQUIRE(QMetaObject::invokeMethod(w.find(QStringLiteral("conflictReplace")), "clicked"));
        CHECK(shortcuts.sequence(QStringLiteral("test.ping")) == QStringLiteral("Ctrl+N"));
        CHECK(shortcuts.sequence(QStringLiteral("file.new")).isEmpty());
        CHECK(w.shown(QStringLiteral("shortcutConflict")) == nullptr);
        CHECK(w.shown(QStringLiteral("shortcutReset.test.ping")) != nullptr);
        QTest::keyClick(w.window(), Qt::Key_Escape);
        w.checkNoWarnings();
    }
    // Only the differences from the defaults are stored.
    {
        QFile f(file);
        REQUIRE(f.open(QIODevice::ReadOnly));
        const QJsonObject map = QJsonDocument::fromJson(f.readAll()).object().value("shortcuts").toObject();
        CHECK(map.value("test.ping").toString() == QStringLiteral("Ctrl+N"));
        CHECK(map.value("test.record").toString() == QStringLiteral("Ctrl+O"));
        CHECK(map.value("file.open").toString().isEmpty());
        CHECK(map.contains("file.open"));
        CHECK(map.value("file.new").toString().isEmpty());
        CHECK_FALSE(map.contains("file.save"));
    }
    // A new app with the same user data folder has the same map.
    {
        Window w(data.path());
        ShortcutManager& shortcuts = *w.shortcuts();
        CHECK(shortcuts.sequence(QStringLiteral("test.ping")) == QStringLiteral("Ctrl+N"));
        CHECK(shortcuts.sequence(QStringLiteral("file.open")).isEmpty());
        CHECK(shortcuts.sequence(QStringLiteral("file.save")) == QStringLiteral("Ctrl+S"));
        QTest::keyClick(w.window(), Qt::Key_N, Qt::ControlModifier);
        REQUIRE(QTest::qWaitFor([&] { return w.calls->count("test.ping") == 1; }, 5000));
        CHECK(w.app()->active() == nullptr);

        // Reset one, then all.
        QVariantMap r = shortcuts.reset(QStringLiteral("file.new"));
        CHECK_FALSE(r.value("ok").toBool()); // Ctrl+N is test.ping's now
        CHECK(r.value("conflictTitle").toString() == QStringLiteral("Ping Test Action"));
        shortcuts.resetAll();
        CHECK(shortcuts.sequence(QStringLiteral("file.new")) == QStringLiteral("Ctrl+N"));
        CHECK(shortcuts.sequence(QStringLiteral("file.open")) == QStringLiteral("Ctrl+O"));
        CHECK(shortcuts.sequence(QStringLiteral("test.ping")).isEmpty());
        w.checkNoWarnings();
    }
    {
        Window w(data.path());
        CHECK(w.shortcuts()->sequence(QStringLiteral("file.open")) == QStringLiteral("Ctrl+O"));
    }
}

TEST_CASE("a shortcut does not fire while a text field has the focus", "[shortcuts][typing]")
{
    QTemporaryDir data;
    Window w(data.path());
    REQUIRE(w.shortcuts()->assign(QStringLiteral("test.ping"), QStringLiteral("Ctrl+Alt+P")).value("ok").toBool());
    REQUIRE(w.shortcuts()->assign(QStringLiteral("test.record"), QStringLiteral("R")).value("ok").toBool());

    // In the palette's search field: neither the chord nor the plain letter runs anything; the letter is typed.
    w.openPalette();
    w.search()->setProperty("text", QString());
    QTest::keyClick(w.window(), Qt::Key_P, Qt::ControlModifier | Qt::AltModifier);
    QTest::keyClick(w.window(), Qt::Key_R);
    QTest::qWait(100);
    CHECK(w.calls->count("test.ping") == 0);
    // The letter is typed (QTest may also type the chord's letter: Ctrl+Alt is AltGr on Windows).
    CHECK(w.search()->property("text").toString().endsWith(QLatin1Char('r')));
    CHECK(w.paletteOpen());
    QTest::keyClick(w.window(), Qt::Key_Escape);
    REQUIRE(QTest::qWaitFor([&] { return !w.paletteOpen(); }, 3000));

    // In the top bar's search field (reached with Tab, so the palette stays closed).
    QQuickItem* field = w.find(QStringLiteral("commandSearch"));
    field->forceActiveFocus(Qt::TabFocusReason);
    REQUIRE(field->hasActiveFocus());
    QTest::keyClick(w.window(), Qt::Key_P, Qt::ControlModifier | Qt::AltModifier);
    QTest::qWait(100);
    CHECK(w.calls->count("test.ping") == 0);

    // With the view focused it fires.
    w.host->viewport()->forceActiveFocus();
    QTest::keyClick(w.window(), Qt::Key_P, Qt::ControlModifier | Qt::AltModifier);
    REQUIRE(QTest::qWaitFor([&] { return w.calls->count("test.ping") == 1; }, 5000));
    w.checkNoWarnings();
}

TEST_CASE("the context menu at the cursor lists the actions for the current selection", "[context]")
{
    if (!rhiSceneGraph())
        SKIP("the scene graph has no QRhi (software backend): the viewport cannot render here");
    QTemporaryDir data;
    Window w(data.path());
    openPart(w);
    QQuickItem* area = w.find(QStringLiteral("viewportArea"));
    REQUIRE(area != nullptr);
    ui::SelectionModel& selection = *w.host->selection();
    const QStringList nothing = {"view.fitAll", "view.toggleProjection", "file.open", "file.import", "edit.undo",
                                 "edit.redo"};
    const QStringList element = {"view.fitSelection",
                                 "selection.selectBody",
                                 "view.hideSelectedBodies",
                                 "view.isolateSelectedBodies",
                                 "selection.copyProperties",
                                 "selection.clear",
                                 "edit.undo",
                                 "edit.redo"};

    // Nothing selected: right-click on empty background, away from the panels and the view cube.
    const QPointF empty = area->mapToScene(QPointF(area->width() * 0.12, area->height() * 0.75));
    REQUIRE_FALSE(w.host->viewport()->viewport()->sceneBounds() == std::nullopt);
    w.click(empty, Qt::RightButton);
    REQUIRE(QTest::qWaitFor([&] { return w.shown(QStringLiteral("contextItem.view.fitAll")) != nullptr; }, 3000));
    CHECK(menuTexts(w, nothing) == nothing);
    CHECK(menuTexts(w, {"view.fitSelection", "selection.selectBody", "selection.clear"}).isEmpty());
    CHECK(w.shown(QStringLiteral("contextItem.file.open"))->isEnabled());
    // The part opened as a document with one import feature (T-019): undo is available, redo is not.
    CHECK(w.shown(QStringLiteral("contextItem.edit.undo"))->isEnabled());
    CHECK_FALSE(w.shown(QStringLiteral("contextItem.edit.redo"))->isEnabled());
    // At the cursor: the first entry starts just below and right of the click point.
    const QPointF first = w.find(QStringLiteral("contextItem.view.fitAll"))->mapToScene(QPointF(0, 0));
    INFO("click " << empty.x() << "," << empty.y() << " first item " << first.x() << "," << first.y());
    CHECK(std::abs(first.x() - empty.x()) < 24);
    CHECK(std::abs(first.y() - empty.y()) < 24);
    // The standard views are a submenu.
    QObject* menu = w.findObject(QStringLiteral("contextMenu"));
    REQUIRE(menu != nullptr);
    CHECK(menu->property("opened").toBool());
    QTest::keyClick(w.window(), Qt::Key_Escape);
    REQUIRE(QTest::qWaitFor([&] { return !menu->property("opened").toBool(); }, 3000));

    // A right drag pans (the default navigation preset) and opens no menu.
    const render::Camera beforePan = w.host->viewport()->camera();
    w.mouse(QEvent::MouseButtonPress, empty, Qt::RightButton, Qt::RightButton);
    for (int i = 1; i <= 6; ++i)
        w.mouse(QEvent::MouseMove, empty + QPointF(8 * i, 0), Qt::NoButton, Qt::RightButton);
    w.mouse(QEvent::MouseButtonRelease, empty + QPointF(48, 0), Qt::RightButton, Qt::NoButton);
    QTest::qWait(100);
    CHECK_FALSE(menu->property("opened").toBool());
    const render::Camera afterPan = w.host->viewport()->camera();
    CHECK(std::abs(afterPan.target.x - beforePan.target.x) + std::abs(afterPan.target.y - beforePan.target.y) +
              std::abs(afterPan.target.z - beforePan.target.z) >
          1e-6);
    w.host->viewport()->fitAll();
    w.grab();

    // A face selected: the selection's commands, no open/import.
    const QPointF face = topFacePoint(w);
    w.click(face);
    REQUIRE(QTest::qWaitFor([&] { return selection.count() == 1 && !selection.pending() && !selection.infoBusy(); },
                            10000));
    REQUIRE(selection.entries().front().hit.kind == kernel::SubKind::Face);
    w.click(face, Qt::RightButton);
    REQUIRE(QTest::qWaitFor([&] { return menu->property("opened").toBool(); }, 3000));
    CHECK(menuTexts(w, element) == element);
    CHECK(menuTexts(w, {"file.open", "file.import", "view.fitAll", "selection.copyCoordinates"}).isEmpty());
    // Choosing "Select Body" selects the face's body.
    w.grab();
    QQuickItem* selectBody = w.shown(QStringLiteral("contextItem.selection.selectBody"));
    REQUIRE(selectBody != nullptr);
    w.click(w.centreOf(selectBody));
    REQUIRE(QTest::qWaitFor(
        [&] { return selection.count() == 1 && selection.entries().front().hit.kind == kernel::SubKind::Solid; },
        5000));
    REQUIRE(QTest::qWaitFor([&] { return !menu->property("opened").toBool(); }, 3000));

    // In the model tree: a right-click on a body row selects it and shows the body's commands.
    selection.clear();
    QQuickItem* row = w.find(QStringLiteral("bodyRow.0"));
    REQUIRE(row != nullptr);
    w.click(w.centreOf(row), Qt::RightButton);
    REQUIRE(QTest::qWaitFor([&] { return w.shown(QStringLiteral("contextItem.view.hideSelectedBodies")) != nullptr; },
                            3000));
    CHECK(selection.isBodySelected(0));
    CHECK(w.shown(QStringLiteral("contextItem.selection.selectBody")) == nullptr);
    w.grab(); // the menu is placed (moved to fit the window) when it is polished
    QQuickItem* hide = w.shown(QStringLiteral("contextItem.view.hideSelectedBodies"));
    w.click(w.centreOf(hide));
    REQUIRE(QTest::qWaitFor([&] { return !w.host->scene()->bodyVisible(0); }, 3000));
    // With a body hidden, "Show All Bodies" appears for an empty selection.
    const QVariantList entries = w.commands()->contextEntries();
    bool showAll = false;
    for (const QVariant& e : entries)
        showAll = showAll || e.toMap().value("id").toString() == QStringLiteral("view.showAllBodies");
    CHECK(showAll);
    w.checkNoWarnings();
}

// Not run by default: RAY_UI_SCREENSHOT_DIR=<folder> ray_ui_palette_tests "[screenshots]"
TEST_CASE("screenshots of the palette, the form, the shortcut settings and a context menu", "[.screenshots]")
{
    if (screenshotDir().isEmpty())
        SKIP("set RAY_UI_SCREENSHOT_DIR");
    if (!rhiSceneGraph())
        SKIP("no QRhi");
    QDir().mkpath(screenshotDir());
    QTemporaryDir data;
    for (const char* theme : {"dark", "light"}) {
        Window w(data.path(), true, theme);
        openPart(w);
        w.openPalette();
        w.typeQuery(QStringLiteral("ex"));
        saveShot(w, QStringLiteral("palette-query-%1.png").arg(QString::fromLatin1(theme)));
        if (QString::fromLatin1(theme) == QLatin1String("dark")) {
            w.search()->setProperty("text", QString());
            saveShot(w, QStringLiteral("palette-empty-dark.png"));
            w.typeQuery(QStringLiteral("record test"));
            QTest::keyClick(w.window(), Qt::Key_Return);
            w.find(QStringLiteral("formField.name"))->setProperty("text", QStringLiteral("Part 7"));
            saveShot(w, QStringLiteral("palette-form-dark.png"));
            QTest::keyClick(w.window(), Qt::Key_Return);
            saveShot(w, QStringLiteral("palette-form-error-dark.png"));
            QTest::keyClick(w.window(), Qt::Key_Escape);
            QTest::keyClick(w.window(), Qt::Key_Escape);
            QTest::qWait(200);
        }
        // Shortcut settings with a conflict.
        w.commands()->openShortcutSettings();
        QTest::qWait(200);
        w.find(QStringLiteral("shortcutFilter"))->setProperty("text", QStringLiteral("test"));
        w.grab();
        QQuickItem* key = w.find(QStringLiteral("shortcutKey.test.ping"));
        REQUIRE(key != nullptr);
        w.click(w.centreOf(key));
        QTest::keyClick(w.window(), Qt::Key_S, Qt::ControlModifier);
        saveShot(w, QStringLiteral("shortcut-settings-conflict-%1.png").arg(QString::fromLatin1(theme)));
        QTest::keyClick(w.window(), Qt::Key_Escape);
        QTest::keyClick(w.window(), Qt::Key_Escape);
        QTest::qWait(200);
        // Context menu over the part with a face selected.
        const QPointF face = topFacePoint(w);
        w.click(face);
        (void)QTest::qWaitFor([&] { return w.host->selection()->count() == 1 && !w.host->selection()->infoBusy(); },
                              10000);
        w.click(face, Qt::RightButton);
        QTest::qWait(200);
        saveShot(w, QStringLiteral("context-menu-face-%1.png").arg(QString::fromLatin1(theme)));
        QTest::keyClick(w.window(), Qt::Key_Escape);
        // And with nothing selected.
        w.host->selection()->clear();
        QQuickItem* area = w.find(QStringLiteral("viewportArea"));
        w.click(area->mapToScene(QPointF(area->width() * 0.12, area->height() * 0.6)), Qt::RightButton);
        QTest::qWait(200);
        saveShot(w, QStringLiteral("context-menu-empty-%1.png").arg(QString::fromLatin1(theme)));
        QTest::keyClick(w.window(), Qt::Key_Escape);
    }
}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    return Catch::Session().run(argc, argv);
}
