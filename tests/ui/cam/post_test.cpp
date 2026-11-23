// Posting, the G-code viewer and file output (task T-047) in the desktop app, on the real Qt Quick window with the real
// application session:
//
//   - a drill setup posted from the CAM tree's setup row: the post defaults to the machine's, its declared properties
//     with the machine definition's values as defaults, the unverified notice (which changes nothing in the file),
//     the program number, the folder, the file name pattern, progress, the file written byte for byte as the post
//     produced it, the buttons in the fixed footer whatever the body's scroll
//   - the gate in the dialog: every blocking operation listed by name with its reason, Generate where that is the fix,
//     Post disabled, post() and the cam.post action refused, no file; Generate clears it
//   - the post's errors in full, attached to the operation they name, no file and no partial file
//   - the viewer: tokens in the theme's colours in both themes, find, copy, the keyboard; a line highlights its move in
//     the view, a move picked in the view selects its lines; a line without a move says so
//   - remembered per setup (the post, its properties, the folder, the rule), the program number written back to the
//     setup, every overwrite rule, a loopback SMB folder with a locked file, and the same bytes on a second post
//   - keyboard: Ctrl+P opens the dialog, Tab stays inside it, Enter posts, Esc closes; the viewer's keys
//   - T-059: the cycle-time estimate in the CAM tree (per setup and per operation) and in the dialog's summary
//   - [wallclock] a program of 200,000 lines: the UI thread within its budget while it is posted and shown, and the
//     time per scroll step measured over the whole program
//
// Screenshots: with RAY_T047_SHOTS set to a folder, the window is saved there in both themes.
#include "../scene_graph_probe.h"
#include "../ui_budget.h"
#include "../window_focus.h"
#include "app_context.h"
#include "app_host.h"
#include "app_options.h"
#include "cam/cycletime_logic.h"
#include "cam/operation_view.h"
#include "cam/post_tool.h"
#include "cam/post_view.h"
#include "cam/setup_tool.h"
#include "cam/setup_tree.h"
#include "engine_test_util.h" // the CAM engine tests' test-only operation type and library JSON (tests/unit/cam)
#include "files.h"
#include "palette_commands.h"
#include "scene_controller.h"
#include "selection_model.h"
#include "session.h"
#include "theme_object.h"
#include "viewport_item.h"
#include "viewport_navigation.h"

#include <ray/sim.h>
#include <ray_api/cam_cycletime.h>
#include <ray_api/cam_operation.h>
#include <ray_api/cam_setup.h>
#include <ray_api/cam_tools.h>

#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtGui/QClipboard>
#include <QtGui/QFontMetricsF>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>
#include <QtGui/QMouseEvent>
#include <QtQml/QQmlApplicationEngine>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>
#include <QtTest/QTest>

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace ray;
using Catch::Matchers::ContainsSubstring;
using ray::ui::AppHost;
using ray::ui::AppOptions;
using ray::ui::GcodeModel;
using ray::ui::PostToolModel;
using ray::ui::SetupTreeModel;
using ray::ui::ToolpathDisplay;
namespace ct = ray::cam::test;
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
const QString kDrill6 = QStringLiteral("3d2ce280-3863-4869-9ef4-b75ed93ce20e"); // 6 mm HSS-Co jobber drill
// The shipped "Generic VMC" (haas-ngc default post, postProperties smoothing medium), unverified.
const std::string kVmc = "b903ede7-5123-4c26-bc15-466e92fba4c6";

// "ui.long": a zigzag of `moves` linear moves at the bottom height, for the 200,000-line program.
class LongOperation final : public cam::IOperationType {
public:
    std::string_view typeName() const override { return "ui.long"; }
    std::string_view paramsSchemaJson() const override
    {
        return R"({"type":"object","title":"Long path","properties":{"moves":{"type":"integer","title":"Moves",)"
               R"("description":"How many","minimum":1,"default":1000}}})";
    }
    Status validate(const cam::GenerateInput&) const override { return {}; }
    Result<cam::Toolpath> generate(const cam::GenerateInput& in, JobContext& ctx) const override
    {
        const QJsonObject params = QJsonDocument::fromJson(QByteArray::fromStdString(in.op.paramsJson)).object();
        const int moves = params.value(QStringLiteral("moves")).toInt(1000);
        cam::Toolpath tp{};
        cam::Move rapid{};
        rapid.kind = cam::MoveKind::Rapid;
        rapid.role = cam::MoveRole::Approach;
        rapid.z = toNm(in.op.heights.clearance);
        tp.moves.push_back(rapid);
        const auto feed = static_cast<std::int32_t>(in.op.cutting.feedCut * 1000.0);
        for (int i = 0; i < moves; ++i) {
            if (i % 4096 == 0) {
                if (auto st = ctx.cancel.check(); !st)
                    return tl::unexpected(st.error());
            }
            cam::Move m{};
            m.kind = cam::MoveKind::Linear;
            m.role = cam::MoveRole::Cut;
            const int row = i / 100, col = i % 100;
            m.x = toNm(row % 2 == 0 ? col * 0.5 : (99 - col) * 0.5);
            m.y = toNm(row * 0.05);
            m.z = toNm(in.op.heights.bottom);
            m.feedMicronPerMin = feed;
            tp.moves.push_back(m);
        }
        return tp;
    }
};

struct Window {
    QTemporaryDir cache;
    QTemporaryDir userData;
    QTemporaryDir out; // the output folder
    std::unique_ptr<AppHost> host;
    int confirms = 0;

    explicit Window(QSize size = QSize(1440, 900))
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
        host->app()->operationTypes()->add(std::make_unique<ct::TestOperation>());
        host->app()->operationTypes()->add(std::make_unique<LongOperation>());
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
    void sendMouse(QEvent::Type type, QPointF scenePos, Qt::MouseButton button, Qt::MouseButtons buttons,
                   Qt::KeyboardModifiers mods = Qt::NoModifier) const
    {
        QMouseEvent e(type, scenePos, window()->mapToGlobal(scenePos), button, buttons, mods);
        QGuiApplication::sendEvent(window(), &e);
    }
    void clickScene(QPointF p, Qt::KeyboardModifiers mods = Qt::NoModifier) const
    {
        sendMouse(QEvent::MouseMove, p, Qt::NoButton, Qt::NoButton, mods);
        QCoreApplication::processEvents();
        sendMouse(QEvent::MouseButtonPress, p, Qt::LeftButton, Qt::LeftButton, mods);
        sendMouse(QEvent::MouseButtonRelease, p, Qt::LeftButton, Qt::NoButton, mods);
        QCoreApplication::processEvents();
    }
    QPointF centreOf(QQuickItem* item) const
    {
        return item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
    }
    // Scrolls the dialog's body so `item` is in view.
    void inView(QQuickItem* item) const
    {
        QQuickItem* flick = find(QStringLiteral("postDialogScroll"));
        if (flick == nullptr || !flick->isAncestorOf(item))
            return;
        const double contentY = flick->property("contentY").toDouble();
        const double h = flick->height();
        const double y = item->mapToItem(flick, QPointF(0, 0)).y();
        const double maxY = std::max(0.0, flick->property("contentHeight").toDouble() - h);
        if (y < 0 || y + item->height() > h)
            flick->setProperty("contentY", std::clamp(contentY + y - h / 3, 0.0, maxY));
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
        click(shown(QStringLiteral("workspaceTab.%1").arg(index)));
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
    Result<api::ActionResult> invoke(const std::string& action, const std::string& args) const
    {
        JobHandle<api::ActionResult> job = host->app()->actions().invoke(action, args, api::Caller::User);
        REQUIRE(QTest::qWaitFor([&] { return job.done(); }, kTimeoutMs));
        return job.wait();
    }
    QString bodyId() const
    {
        const api::viewer::Body* b = host->scene()->body(0);
        REQUIRE(b != nullptr);
        return QString::fromStdString(b->id.value.toString());
    }
    std::map<OperationId, cam::OpState> statuses() const
    {
        return host->app()->cam()->status(document()->state(), document()->build());
    }
    void generate(std::uint64_t op) const
    {
        auto job = host->app()->cam()->generate(document()->state(), document()->build(), ct::opId(op));
        REQUIRE(QTest::qWaitFor([&] { return job.done(); }, kTimeoutMs * 3));
        REQUIRE(
            QTest::qWaitFor([&] { return statuses().at(ct::opId(op)).status == cam::OpStatus::UpToDate; }, kTimeoutMs));
    }
    void shot(const QString& name) const
    {
        const QString dir = qEnvironmentVariable("RAY_T047_SHOTS");
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
    // The dialog's state is final.
    void posted(const char* state) const
    {
        const bool ok = QTest::qWaitFor([&] { return post()->state() == QLatin1String(state); }, kTimeoutMs * 2);
        INFO("post state '" << post()->state().toStdString() << "', status '" << post()->statusText().toStdString()
                            << "'");
        for (const QVariant& m : post()->messages())
            UNSCOPED_INFO(m.toMap().value(QStringLiteral("text")).toString().toStdString());
        REQUIRE(ok);
    }
    void checked() const
    {
        REQUIRE(QTest::qWaitFor([&] { return post()->active() && !post()->checking(); }, kTimeoutMs));
    }
    // The chosen post has been read.
    void postRead() const
    {
        const bool ok = QTest::qWaitFor([&] { return post()->postState() != QLatin1String("loading"); }, kTimeoutMs);
        INFO("post '" << post()->postId().toStdString() << "': state '" << post()->postState().toStdString() << "', "
                      << post()->postMessage().toStdString());
        REQUIRE(ok);
        REQUIRE(post()->postState() == QLatin1String("ok"));
    }
};

std::string jsonText(const QJsonObject& o)
{
    return QJsonDocument(o).toJson(QJsonDocument::Compact).toStdString();
}

QByteArray readFile(const QString& path)
{
    QFile f(path);
    REQUIRE(f.open(QIODevice::ReadOnly));
    return f.readAll();
}

// A setup on the plate with the shipped Generic VMC (embedded from the library).
void addSetup(const Window& w, int n = 1)
{
    const lib::MachineDef* m = w.host->app()->libraries()->machine(MachineId{Uuid::fromString(kVmc)});
    REQUIRE(m != nullptr);
    const std::string args = R"({"id":")" + ct::id('a', static_cast<std::uint64_t>(n)) + R"(","name":"Op )" +
                             std::to_string(n) + R"( - top","partBodies":[")" + w.bodyId().toStdString() +
                             R"("],"machine":")" + kVmc + R"(","library":{"machines":[)" +
                             api::cam_setup::machineJson(*m) + "]}}";
    (void)w.invokeResult("cam.setup.add", args);
    REQUIRE(QTest::qWaitFor([&] { return w.tree()->setupCount() >= n; }, kTimeoutMs));
}

// The holes' bore faces as references (model.reference).
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
        refs.push_back(
            QJsonDocument::fromJson(QByteArray::fromStdString(w.invokeResult("model.reference", jsonText(args))))
                .object()
                .value(QStringLiteral("ref")));
    }
    return refs;
}

// Drill operation c<n> ("Drill <n>") on the first `holes` holes with the 6 mm drill, peck drilling.
void addDrill(const Window& w, std::uint64_t n, std::size_t holes, double rpm = 1500)
{
    const lib::Tool* t = w.host->app()->libraries()->tool(ToolId{Uuid::fromString(kDrill6.toStdString())});
    REQUIRE(t != nullptr);
    QJsonObject args{{QStringLiteral("id"), QString::fromStdString(ct::id('c', n))},
                     {QStringLiteral("setup"), QString::fromStdString(ct::id('a', 1))},
                     {QStringLiteral("type"), QStringLiteral("drill")},
                     {QStringLiteral("name"), QStringLiteral("Drill %1").arg(n)},
                     {QStringLiteral("tool"), QJsonObject{{QStringLiteral("tool"), kDrill6}}},
                     {QStringLiteral("cutting"), QJsonObject{{QStringLiteral("rpm"), rpm},
                                                             {QStringLiteral("feedCut"), 150},
                                                             {QStringLiteral("feedPlunge"), 150},
                                                             {QStringLiteral("coolant"), QStringLiteral("flood")}}},
                     {QStringLiteral("heights"), QJsonObject{{QStringLiteral("clearance"), 15},
                                                             {QStringLiteral("retract"), 5},
                                                             {QStringLiteral("feedPlane"), 5},
                                                             {QStringLiteral("top"), 0},
                                                             {QStringLiteral("bottom"), 0}}},
                     {QStringLiteral("geometry"), holeRefs(w, holes)},
                     {QStringLiteral("params"), QJsonObject{{QStringLiteral("cycle"), QStringLiteral("peck")}}}};
    args.insert(
        QStringLiteral("library"),
        QJsonObject{
            {QStringLiteral("tools"),
             QJsonArray{QJsonDocument::fromJson(QByteArray::fromStdString(api::cam_tools::toolJson(*t))).object()}}});
    (void)w.invokeResult("cam.operation.add", jsonText(args));
}

// The ink of one glyph cell of a line of the G-code viewer (a one-line Text in a monospaced font), as the largest
// colour difference from the line's background: in the middle of the cell (the inner sixth across and the middle eighth
// of the capital height: where a zero's slash or dot is and an O has nothing) and in the whole cell (the strokes).
struct GlyphInk {
    int middle = 0, stroke = 0;
    std::string where;
};
GlyphInk glyphInk(const QImage& img, QQuickItem* text, int column)
{
    const qreal dpr = img.devicePixelRatio();
    const QFontMetricsF fm(text->property("font").value<QFont>());
    const double adv = fm.horizontalAdvance(QLatin1Char('0'));
    const double top = (text->height() - fm.height()) / 2; // AlignVCenter
    const double base = top + fm.ascent();
    const double cap = fm.capHeight();
    const QPointF bgAt = text->mapToScene(QPointF(column * adv + adv / 2, std::max(0.0, base - cap - 2))) * dpr;
    const QColor bg = img.pixelColor(bgAt.toPoint());
    const auto maxDiff = [&](double x0, double x1, double y0, double y1) {
        const QPointF a = text->mapToScene(QPointF(x0, y0)) * dpr, b = text->mapToScene(QPointF(x1, y1)) * dpr;
        // The pixels whose centres are inside (at least the one nearest the middle).
        const int px0 = int(std::ceil(a.x() - 0.5)), py0 = int(std::ceil(a.y() - 0.5));
        const int px1 = std::max(px0, int(std::floor(b.x() - 0.5)));
        const int py1 = std::max(py0, int(std::floor(b.y() - 0.5)));
        int best = 0;
        for (int y = py0; y <= py1; ++y) {
            for (int x = px0; x <= px1; ++x) {
                const QColor c = img.pixelColor(x, y);
                best = std::max(best, std::abs(c.red() - bg.red()) + std::abs(c.green() - bg.green()) +
                                          std::abs(c.blue() - bg.blue()));
            }
        }
        return best;
    };
    GlyphInk out;
    out.where = "cell " + std::to_string(column) + ": text at " +
                std::to_string(text->mapToScene(QPointF(0, 0)).x() * dpr) + ", " +
                std::to_string(text->mapToScene(QPointF(0, 0)).y() * dpr) + " h " +
                std::to_string(text->height() * dpr) + " adv " + std::to_string(adv * dpr) + " base " +
                std::to_string(base * dpr) + " cap " + std::to_string(cap * dpr) + " bg " + bg.name().toStdString() +
                " at " + std::to_string(bgAt.y());
    out.middle = maxDiff(column * adv + 0.42 * adv, column * adv + 0.58 * adv, base - 0.56 * cap, base - 0.44 * cap);
    out.stroke = maxDiff(column * adv, (column + 1) * adv, base - cap, base);
    return out;
}

// Makes setup row 0 current by a click on its name (the row's tool buttons are at its right).
void selectSetupRow(const Window& w)
{
    QQuickItem* row = w.shown(QStringLiteral("setupRow.0"));
    w.grab();
    w.clickScene(row->mapToScene(QPointF(row->width() * 0.22, std::min(12.0, row->height() / 2))));
    REQUIRE(QTest::qWaitFor([&] { return w.tree()->currentIndex() == 0; }, 5000));
}

// The dialog for setup row 0, opened from the row's Post button as the user does.
void openFromRow(const Window& w)
{
    selectSetupRow(w);
    w.click(w.shown(QStringLiteral("camRowPost.0")));
    REQUIRE(QTest::qWaitFor([&] { return w.post()->active(); }, 5000));
    w.shown(QStringLiteral("postDialog"));
    w.checked();
}

QRectF sceneRect(QQuickItem* item)
{
    return QRectF(item->mapToScene(QPointF(0, 0)), QSizeF(item->width(), item->height()));
}

// A plate with setup 1 and drill 1 (generated), in the CAM workspace.
void drillSetup(Window& w, std::size_t holes = 3)
{
    w.open(kPlate);
    w.toWorkspace(ui::kCamWorkspace);
    addSetup(w);
    addDrill(w, 1, holes);
    w.generate(1);
}

} // namespace

// ======================================================================================================== posting
TEST_CASE("a setup is posted from the CAM tree: post, properties, notice, folder, name, the file", "[cam][post][e2e]")
{
    RAY_REQUIRE_RHI();
    Window w;
    drillSetup(w);
    openFromRow(w);
    PostToolModel* p = w.post();

    // The post: the machine's default; every shipped post on offer; its properties with the machine's values.
    w.postRead();
    CHECK(p->postId() == QStringLiteral("haas-ngc"));
    for (const char* name : {"generic-iso", "fanuc", "haas-classic", "haas-ngc"})
        CHECK(p->posts().contains(QLatin1String(name)));
    CHECK(p->form()->fieldCount() > 10);
    CHECK(p->form()->valueOf(QStringLiteral("smoothing")).toString() == QStringLiteral("medium")); // the machine's
    CHECK_FALSE(p->machineName().isEmpty());
    CHECK_FALSE(p->machineVerified());
    // The notice: unverified post and machine, how to prove the program out; it cannot be closed. The shipped post
    // declares no control it has run on (verifiedOn), so it says it is not proven.
    CHECK(p->postProof() == QStringLiteral("unproven"));
    CHECK_FALSE(p->postProven());
    const QString notice = w.shown(QStringLiteral("postNoticeText"))->property("text").toString();
    CHECK(notice.startsWith(QStringLiteral("This post has not been proven on a real machine")));
    CHECK(notice.contains(QStringLiteral("Dry-run the program before cutting")));
    CHECK(notice.contains(QStringLiteral("air cut")));
    CHECK(notice.contains(QStringLiteral("single block")));
    // It never claims a post or a machine is verified or safe.
    CHECK_FALSE(notice.contains(QStringLiteral("is verified"), Qt::CaseInsensitive));
    CHECK_FALSE(notice.contains(QStringLiteral("safe"), Qt::CaseInsensitive));
    CHECK(w.find(QStringLiteral("postNoticeClose")) == nullptr);
    // Nothing blocks; the folder is still to be chosen.
    CHECK(p->blockerList().empty());
    CHECK_FALSE(w.shown(QStringLiteral("postPost"))->isEnabled());
    CHECK(p->postBlockedReason() == QStringLiteral("Choose the folder to write the program to"));

    // The folder, a pattern and a property, by the keyboard.
    const QString dir = QDir::toNativeSeparators(w.out.path());
    w.typeInto(QStringLiteral("postInput.directory"), dir);
    w.typeInto(QStringLiteral("postInput.pattern"), QStringLiteral("O{program:05}"));
    REQUIRE(QTest::qWaitFor([&] { return p->fileName() == QStringLiteral("O01001.nc"); }, 5000));
    CHECK(w.shown(QStringLiteral("postFileName"))->property("text").toString() == QStringLiteral("O01001.nc"));
    p->form()->setValue(QStringLiteral("smoothing"), QStringLiteral("finish"));
    REQUIRE(QTest::qWaitFor([&] { return p->canPost(); }, 5000));
    CHECK(w.shown(QStringLiteral("postPost"))->isEnabled());

    // The action buttons are in the fixed footer: outside the scrolling body, inside the card, wherever it scrolls.
    QQuickItem* card = w.shown(QStringLiteral("postDialog"));
    QQuickItem* scroll = w.shown(QStringLiteral("postDialogScroll"));
    QQuickItem* postButton = w.shown(QStringLiteral("postPost"));
    CHECK_FALSE(scroll->isAncestorOf(postButton));
    for (const double y : {0.0, 1e6}) {
        scroll->setProperty(
            "contentY",
            std::clamp(y, 0.0, std::max(0.0, scroll->property("contentHeight").toDouble() - scroll->height())));
        QCoreApplication::processEvents();
        CHECK(sceneRect(card).contains(sceneRect(postButton)));
        CHECK(sceneRect(card).contains(sceneRect(w.shown(QStringLiteral("postCancel")))));
        CHECK(sceneRect(postButton).top() >= sceneRect(scroll).bottom());
    }
    scroll->setProperty("contentY", 0.0);
    w.shotsBothThemes(QStringLiteral("t047_post_dialog"));

    // The keyboard: Tab walks the dialog's controls and stays inside it (the footer's buttons included).
    w.shown(QStringLiteral("postCombo"))->forceActiveFocus();
    bool reachedPost = false;
    std::string walk;
    for (int i = 0; i < 80; ++i) {
        QTest::keyClick(w.window(), Qt::Key_Tab);
        QQuickItem* f = w.window()->activeFocusItem();
        REQUIRE(f != nullptr);
        walk += " " + f->objectName().toStdString() + "(" + f->metaObject()->className() + ")";
        INFO("Tab walk:" << walk);
        REQUIRE(card->isAncestorOf(f));
        reachedPost = reachedPost || f == postButton;
    }
    CHECK(reachedPost);
    w.shown(QStringLiteral("postCombo"))->forceActiveFocus();
    QTest::keyClick(w.window(), Qt::Key_Backtab, Qt::ShiftModifier);
    CHECK(w.window()->activeFocusItem() == w.shown(QStringLiteral("postCancel")));

    // Post: on workers, with progress; the file is written; the state passes through the steps.
    QStringList states;
    QObject::connect(p, &PostToolModel::stateChanged, [&] {
        if (states.isEmpty() || states.back() != p->state())
            states.push_back(p->state());
    });
    w.click(QStringLiteral("postPost"));
    w.posted("done");
    CHECK(states.contains(QStringLiteral("posting")));
    CHECK(states.contains(QStringLiteral("writing")));
    CHECK(p->writes() == 1);
    const QString file = QDir(w.out.path()).filePath(QStringLiteral("O01001.nc"));
    CHECK(QDir::toNativeSeparators(p->resultPath()) == QDir::toNativeSeparators(file));
    const QByteArray bytes = readFile(file);
    CHECK(bytes.contains("O01001"));
    CHECK(bytes.contains("G187 P3")); // the property changed in the dialog
    CHECK(bytes.contains("G83"));
    CHECK(bytes.contains("\r\n"));
    // Byte for byte what the post produced: the viewer holds the same text.
    REQUIRE(w.gcode()->hasProgram());
    CHECK(QByteArray::fromStdString(*w.gcode()->program()->index->text) == bytes);
    CHECK(QDir(w.out.path()).entryList(QDir::Files | QDir::Hidden) == QStringList{QStringLiteral("O01001.nc")});
    CHECK(w.shown(QStringLiteral("postStatus"))->property("text").toString() == QStringLiteral("Written O01001.nc"));
    CHECK(w.shown(QStringLiteral("postResultPath"))->property("text").toString() == p->resultPath());
    CHECK(w.confirms == 0); // the user's own click needs no confirmation
    w.shotsBothThemes(QStringLiteral("t047_post_done"));

    // Show G-code: the dialog closes, the viewer opens.
    w.click(QStringLiteral("postShowGcode"));
    REQUIRE(QTest::qWaitFor([&] { return !p->active() && w.gcode()->isOpen(); }, 5000));
    w.shown(QStringLiteral("gcodeView"));
    w.checkNoWarnings();
}

// M3 closeout: whether a post is proven on a machine comes from its declaration (verifiedOn) and only for the posts
// shipped with RayCAD; a post in the user's own library, even a copy of a shipped one or one with a shipped post's
// name, is not labelled either way (the machine definition's own notice stays). The notice never blocks posting.
TEST_CASE("the notice labels a shipped post unproven, a post of the user's library neither way", "[cam][post][notice]")
{
    RAY_REQUIRE_RHI();
    Window w;
    drillSetup(w);
    // The user's library (the writable root) gets two posts: a copy of haas-ngc under its own name, and one that
    // replaces haas-ngc itself (a higher root wins).
    std::string userRoot;
    for (const lib::LibraryStack::RootInfo& r : w.host->app()->libraries()->roots()) {
        if (r.writable)
            userRoot = r.dir;
    }
    REQUIRE_FALSE(userRoot.empty());
    const QString posts = QDir(QString::fromStdString(userRoot)).filePath(QStringLiteral("posts"));
    REQUIRE(QDir().mkpath(posts));
    const QString shipped = QStringLiteral(RAY_SHIPPED_POSTS_DIR "/haas-ngc.post.js");
    REQUIRE(QFile::copy(shipped, QDir(posts).filePath(QStringLiteral("shop-ngc.post.js"))));

    openFromRow(w);
    PostToolModel* p = w.post();
    w.postRead();
    CHECK(p->postId() == QStringLiteral("haas-ngc"));
    CHECK(p->postProof() == QStringLiteral("unproven"));
    CHECK(p->notice().startsWith(QStringLiteral("This post has not been proven on a real machine")));
    // It does not block: with a folder, Post is enabled.
    p->setValue(QStringLiteral("directory"), QDir::toNativeSeparators(w.out.path()));
    REQUIRE(QTest::qWaitFor([&] { return p->canPost(); }, kTimeoutMs));

    // The user's post: no word about the post; the machine definition is still unverified, and the notice says that.
    REQUIRE(QTest::qWaitFor([&] { return p->posts().contains(QStringLiteral("shop-ngc")); }, kTimeoutMs));
    p->setPost(QStringLiteral("shop-ngc"));
    REQUIRE(QTest::qWaitFor(
        [&] { return p->postState() == QLatin1String("ok") && p->postId() == QLatin1String("shop-ngc"); }, kTimeoutMs));
    CHECK(p->postProof().isEmpty());
    CHECK_FALSE(p->postProven());
    const QString notice = w.shown(QStringLiteral("postNoticeText"))->property("text").toString();
    CHECK_FALSE(notice.contains(QStringLiteral("This post")));
    CHECK(notice.startsWith(QStringLiteral("The machine definition (")));
    CHECK(notice.contains(QStringLiteral("Dry-run the program before cutting")));
    REQUIRE(QTest::qWaitFor([&] { return p->canPost(); }, kTimeoutMs));
    p->close();

    // haas-ngc in the user's library replaces the shipped one: not labelled either.
    REQUIRE(QFile::copy(shipped, QDir(posts).filePath(QStringLiteral("haas-ngc.post.js"))));
    openFromRow(w);
    w.postRead();
    CHECK(p->postId() == QStringLiteral("haas-ngc"));
    CHECK(p->postProof().isEmpty());
    p->close();
    w.checkNoWarnings();
}

TEST_CASE("posting is blocked with a named reason per operation; Generate fixes what it can; no path writes",
          "[cam][post][gate]")
{
    RAY_REQUIRE_RHI();
    Window w;
    w.open(kPlate);
    w.toWorkspace(ui::kCamWorkspace);
    addSetup(w);
    addDrill(w, 1, 2); // never generated
    REQUIRE(w.invokeResult("cam.library.embed", ct::library(ct::toolJson(1))).size() > 0);
    // Test operations: 2 fails, 3 waits at the gate (generating), 4 on a face reference whose names are wrong.
    const auto add = [&](std::uint64_t n, const std::string& geom, const char* mode) {
        const doc::Command c = ct::opAdd(n, 1, 1, geom, std::string(R"({"mode":")") + mode + "\"}");
        (void)w.invokeResult(c.name, c.argsJson);
    };
    add(2, "[]", "fail");
    add(3, "[]", "block");
    {
        const QJsonObject ref =
            QJsonDocument::fromJson(
                QByteArray::fromStdString(w.invokeResult(
                    "model.reference", jsonText(QJsonObject{{QStringLiteral("body"), w.bodyId()},
                                                            {QStringLiteral("kind"), QStringLiteral("face")},
                                                            {QStringLiteral("index"), 0}}))))
                .object()
                .value(QStringLiteral("ref"))
                .toObject();
        QJsonObject broken = ref;
        QJsonArray names;
        for (int i = 0; i < ref.value(QStringLiteral("names")).toArray().size(); ++i)
            names.append(QStringLiteral("0102030405060708091011121314151617181920212223242526272829303132"));
        broken.insert(QStringLiteral("names"), names);
        broken.remove(QStringLiteral("neighbourNames"));
        broken.remove(QStringLiteral("hint"));
        add(4, QJsonDocument(QJsonArray{broken}).toJson(QJsonDocument::Compact).toStdString(), "plain");
    }
    // 2 failed, 3 generating.
    {
        auto job = w.host->app()->cam()->generate(w.document()->state(), w.document()->build(), ct::opId(2));
        REQUIRE(QTest::qWaitFor([&] { return job.done(); }, kTimeoutMs));
    }
    ct::Gate::instance().open = false;
    (void)w.host->app()->cam()->generate(w.document()->state(), w.document()->build(), ct::opId(3));
    REQUIRE(QTest::qWaitFor([&] { return ct::Gate::instance().waiting.load() == 1; }, kTimeoutMs));

    // Ctrl+P on the setup row (the command in the CAM context).
    selectSetupRow(w);
    w.view()->forceActiveFocus();
    QTest::keyClick(w.window(), Qt::Key_P, Qt::ControlModifier);
    REQUIRE(QTest::qWaitFor([&] { return w.post()->active(); }, 5000));
    w.checked();
    PostToolModel* p = w.post();
    p->setValue(QStringLiteral("directory"), QDir::toNativeSeparators(w.out.path()));
    const auto reasons = [&] {
        std::vector<std::string> out;
        for (const auto& b : p->blockerList())
            out.push_back(b.name + "=" + std::string(api::cam_post::reasonKey(b.reason)));
        return out;
    };
    REQUIRE(QTest::qWaitFor([&] { return p->blockerList().size() == 4; }, kTimeoutMs));
    CHECK(reasons() ==
          std::vector<std::string>{"Drill 1=noToolpath", "Op 2=failed", "Op 3=generating", "Op 4=referenceBroken"});
    // Each by name and reason in the dialog; Generate only where it is the fix.
    for (int i = 0; i < 4; ++i) {
        const QString name = w.shown(QStringLiteral("postBlockerName.%1").arg(i))->property("text").toString();
        CHECK(name.startsWith(QString::fromStdString(p->blockerList()[static_cast<std::size_t>(i)].name)));
        CHECK(!w.shown(QStringLiteral("postBlockerMessage.%1").arg(i))->property("text").toString().isEmpty());
        QQuickItem* gen = w.find(QStringLiteral("postGenerate.%1").arg(i));
        REQUIRE(gen != nullptr);
        CHECK(gen->isVisible() == (i == 0));
    }
    CHECK(w.shown(QStringLiteral("postBlockerMessage.1"))
              ->property("text")
              .toString()
              .contains(QStringLiteral("Test generator failed")));
    CHECK_FALSE(p->canPost());
    CHECK_FALSE(w.shown(QStringLiteral("postPost"))->isEnabled());
    w.shotsBothThemes(QStringLiteral("t047_post_blocked"));
    // No path writes: post() itself, the command and the cam.post action all refuse.
    CHECK_FALSE(p->post());
    CHECK(p->runs() == 0);
    p->close();
    // The registry's cam.post from the palette opens the dialog (the UI command fronts it): still refused.
    const auto command = w.host->commands()->find(QStringLiteral("cam.post"));
    REQUIRE(command.has_value());
    INFO("tree row " << w.tree()->currentIndex() << " of " << w.tree()->count());
    INFO("cam.post: " << w.host->commands()->disabledReason(*command).toStdString());
    REQUIRE(w.host->commands()->trigger(QStringLiteral("cam.post")));
    REQUIRE(QTest::qWaitFor([&] { return w.post()->active(); }, 5000));
    w.checked();
    CHECK_FALSE(w.post()->post());
    CHECK(w.post()->runs() == 0);
    w.post()->close();
    const std::string args = jsonText(QJsonObject{{QStringLiteral("setup"), QString::fromStdString(ct::id('a', 1))},
                                                  {QStringLiteral("directory"), QDir::toNativeSeparators(w.out.path())},
                                                  {QStringLiteral("overwrite"), QStringLiteral("replace")}});
    auto refused = w.invoke("cam.post", args);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == ErrorCode::PostFailed);
    for (const char* name : {"Drill 1: ", "Op 2: ", "Op 3: ", "Op 4: "})
        CHECK_THAT(refused.error().message, ContainsSubstring(name));
    CHECK(QDir(w.out.path()).entryList(QDir::Files | QDir::Hidden).isEmpty());

    // Generate fixes the never-generated drill; the rest stay named.
    REQUIRE(w.post()->open(QString::fromStdString(ct::id('a', 1))));
    w.checked();
    w.click(QStringLiteral("postGenerate.0"));
    REQUIRE(QTest::qWaitFor([&] { return w.post()->blockerList().size() == 3; }, kTimeoutMs * 2));
    CHECK(w.post()->blockerList().front().name == "Op 2");
    // The generation ends: the generating one goes too; suppressing the others leaves nothing in the way.
    ct::Gate::instance().open = true;
    REQUIRE(QTest::qWaitFor([&] { return w.post()->blockerList().size() == 2; }, kTimeoutMs * 2));
    for (const char* id : {"c0000000-0000-4000-8000-000000000002", "c0000000-0000-4000-8000-000000000004"})
        (void)w.invokeResult("cam.operation.suppress", std::string(R"({"id":")") + id + R"(","suppressed":true})");
    REQUIRE(QTest::qWaitFor([&] { return w.post()->blockerList().empty() && !w.post()->checking(); }, kTimeoutMs));
    w.post()->setValue(QStringLiteral("directory"), QDir::toNativeSeparators(w.out.path()));
    REQUIRE(QTest::qWaitFor([&] { return w.post()->canPost(); }, kTimeoutMs));
    w.post()->close();
    w.checkNoWarnings();
}

TEST_CASE("the post's refusals are shown in full on their operation; no file and no partial file",
          "[cam][post][refusal]")
{
    RAY_REQUIRE_RHI();
    Window w;
    w.open(kPlate);
    w.toWorkspace(ui::kCamWorkspace);
    addSetup(w);
    // A test operation (its generator checks no spindle speed) far above the machine's spindle: the post refuses it.
    REQUIRE(w.invokeResult("cam.library.embed", ct::library(ct::toolJson(1))).size() > 0);
    const doc::Command c = ct::opAdd(1, 1, 1);
    (void)w.invokeResult(c.name, c.argsJson);
    (void)w.invokeResult("cam.operation.edit", R"({"id":")" + ct::id('c', 1) + R"(","cutting":{"rpm":30000}})");
    w.generate(1);
    openFromRow(w);
    PostToolModel* p = w.post();
    w.postRead();
    w.typeInto(QStringLiteral("postInput.directory"), QDir::toNativeSeparators(w.out.path()));
    REQUIRE(QTest::qWaitFor([&] { return p->canPost(); }, 5000));
    // Enter in a field posts.
    QTest::keyClick(w.window(), Qt::Key_Return);
    w.posted("failed");
    REQUIRE_FALSE(p->messages().isEmpty());
    const QVariantMap m = p->messages().front().toMap();
    CHECK(m.value(QStringLiteral("kind")).toString() == QStringLiteral("error"));
    CHECK(m.value(QStringLiteral("text")).toString().contains(QStringLiteral("rpm is above the machine")));
    CHECK(m.value(QStringLiteral("opName")).toString() == QStringLiteral("Op 1")); // attached to the operation
    CHECK(m.value(QStringLiteral("scriptLine")).toInt() > 0);                      // and to the script's line
    CHECK(
        w.shown(QStringLiteral("postMessageText.0"))->property("text").toString().startsWith(QStringLiteral("Op 1: ")));
    w.shotsBothThemes(QStringLiteral("t047_post_error"));
    CHECK(w.shown(QStringLiteral("postStatus"))
              ->property("text")
              .toString()
              .startsWith(QStringLiteral("Nothing was written")));
    CHECK(QDir(w.out.path()).entryList(QDir::Files | QDir::Hidden).isEmpty());
    CHECK(p->writes() == 0);
    // A program number the control cannot take: refused by the post with its reason.
    w.typeInto(QStringLiteral("postInput.programNumber"), QStringLiteral("123456"));
    QTest::keyClick(w.window(), Qt::Key_Return);
    w.posted("failed");
    bool named = false;
    for (const QVariant& v : p->messages())
        named = named ||
                v.toMap().value(QStringLiteral("text")).toString().contains(QStringLiteral("program number 123456"));
    CHECK(named);
    CHECK(QDir(w.out.path()).entryList(QDir::Files | QDir::Hidden).isEmpty());
    // Esc closes the dialog.
    QQuickItem* focus = w.window()->activeFocusItem();
    INFO("focus on " << (focus ? focus->objectName().toStdString() + " (" + focus->metaObject()->className() + ")"
                               : std::string("nothing"))
                     << "; focus window " << (QGuiApplication::focusWindow() == w.window() ? "this" : "another or none")
                     << "; state '" << p->state().toStdString() << "', busy " << p->busy() << "; foreground job "
                     << (w.host->session()->jobActive() ? w.host->session()->jobTitle().toStdString() : "none"));
    QTest::keyClick(w.window(), Qt::Key_Escape);
    REQUIRE(QTest::qWaitFor([&] { return !p->active(); }, 5000));

    // Esc closes it while a job runs too. The shell's Esc cancels the foreground job, but not under the dialog: two
    // enabled shortcuts for one key are ambiguous to Qt, which then gives the key to neither (nor to the focused
    // field), so Esc did nothing while, say, the rebuild after the dialog's own write-back of the program number ran.
    QObject* shellEsc = nullptr;
    for (QObject* root : w.host->engine()->rootObjects()) {
        if (shellEsc == nullptr)
            shellEsc = root->findChild<QObject*>(QStringLiteral("shellCancelJobShortcut"));
    }
    REQUIRE(shellEsc != nullptr);
    openFromRow(w);
    w.postRead();
    w.host->files()->exportTo(QStringLiteral("step"), QDir(w.out.path()).filePath(QStringLiteral("plate.step")));
    if (QTest::qWaitFor([&] { return w.host->session()->jobActive(); }, 5000)) {
        INFO("dialog open " << p->active() << ", job '" << w.host->session()->jobTitle().toStdString() << "'");
        for (const QString& warning : w.host->warnings())
            UNSCOPED_INFO(warning.toStdString());
        CHECK_FALSE(shellEsc->property("enabled").toBool());
        QTest::keyClick(w.window(), Qt::Key_Escape);
        REQUIRE(QTest::qWaitFor([&] { return !p->active(); }, 5000));
    } else {
        WARN("the export ended before it was seen running: Esc under the dialog with a job running was not tried");
        p->close();
    }
    REQUIRE(QTest::qWaitFor([&] { return !w.host->session()->jobActive(); }, kTimeoutMs));
    w.checkNoWarnings();
}

// ======================================================================================================== viewer
TEST_CASE("the G-code viewer: colours, find, copy, keys; a line highlights its move and a picked move its lines",
          "[cam][post][viewer]")
{
    RAY_REQUIRE_RHI();
    Window w;
    drillSetup(w, 3);
    openFromRow(w);
    w.postRead();
    w.post()->setValue(QStringLiteral("directory"), QDir::toNativeSeparators(w.out.path()));
    REQUIRE(QTest::qWaitFor([&] { return w.post()->canPost(); }, 5000));
    REQUIRE(w.post()->post());
    w.posted("done");
    w.click(QStringLiteral("postShowGcode"));
    GcodeModel* g = w.gcode();
    REQUIRE(QTest::qWaitFor([&] { return g->isOpen(); }, 5000));
    w.shown(QStringLiteral("gcodeView"));
    const camui::GcodeIndex& index = *g->program()->index;
    REQUIRE(g->lineCount() == static_cast<int>(index.lineCount()));
    CHECK(w.host->selection()->pathPicking()); // clicks in the view pick moves while the viewer is open

    // Colours: the theme's G-code tokens, in both themes.
    int gLine = -1;
    for (int i = 0; i < g->lineCount() && gLine < 0; ++i) {
        if (index.line(static_cast<std::size_t>(i)).rfind("G83", 0) == 0 ||
            index.line(static_cast<std::size_t>(i)).find(" G83") != std::string_view::npos)
            gLine = i;
    }
    REQUIRE(gLine >= 0);
    for (const char* theme : {"dark", "light"}) {
        w.host->theme()->setProperty("mode", QLatin1String(theme));
        QCoreApplication::processEvents();
        const QString styled = g->data(g->index(gLine), GcodeModel::StyledRole).toString();
        INFO(theme << ": " << styled.toStdString());
        const ui::ThemeObject* t = w.host->theme();
        CHECK(styled.contains(t->gcodeGWord().name()));
        CHECK(styled.contains(t->gcodeAxis().name()));
        const QString comment = g->data(g->index(1), GcodeModel::StyledRole).toString();
        const bool coloured =
            comment.contains(t->gcodeComment().name()) || comment.contains(t->gcodeLineNumber().name());
        CHECK(coloured);
    }
    w.host->theme()->setProperty("mode", QStringLiteral("dark"));

    // A line with a move: its move is highlighted in the view (the path highlight on the operation's backplot).
    int moveLine = -1;
    for (int i = 0; i < g->lineCount() && moveLine < 0; ++i) {
        if (index.moveOf(static_cast<std::size_t>(i)) &&
            index.line(static_cast<std::size_t>(i)).find("G83") != std::string_view::npos)
            moveLine = i;
    }
    REQUIRE(moveLine >= 0);
    QQuickItem* list = w.shown(QStringLiteral("gcodeList"));
    QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, moveLine), Q_ARG(int, 4 /* Contain */));
    QQuickItem* row = nullptr;
    REQUIRE(QTest::qWaitFor(
        [&] { return (row = w.find(QStringLiteral("gcodeLine.%1").arg(moveLine))) && row->isVisible(); }, 5000));
    w.clickScene(w.centreOf(row));
    REQUIRE(QTest::qWaitFor([&] { return g->currentLine() == moveLine && g->highlighted(); }, kTimeoutMs));
    const auto hit = g->highlightedHit();
    REQUIRE(hit);
    const camui::MoveRef move = *index.moveOf(static_cast<std::size_t>(moveLine));
    CHECK(hit->moveIndex == move.move);
    bool onDrill = false;
    for (const ToolpathDisplay::Shown& s : w.display()->shown())
        onDrill = onDrill || (s.item == hit->item && s.op == ct::opId(1));
    CHECK(onDrill);
    const auto hits = w.host->selection()->hits(); // what the viewport is asked to highlight
    CHECK(std::any_of(hits.begin(), hits.end(),
                      [&](const render::PickHit& h) { return h.item == hit->item && h.moveIndex == hit->moveIndex; }));
    CHECK(g->lineInfo().contains(QStringLiteral("Drill 1")));
    w.shotsBothThemes(QStringLiteral("t047_viewer_highlight"));
    // The program number: its letter O and its zeros are told apart, in both themes (the mono font's zero has a slash
    // or a dot in its middle, where an O has none; at the viewer's size the mark has to be as dark as the strokes).
    {
        int oLine = -1;
        for (int i = 0; i < g->lineCount() && oLine < 0; ++i) {
            const std::string_view l = index.line(static_cast<std::size_t>(i));
            if (l.size() > 2 && l[0] == 'O' && l[1] == '0')
                oLine = i;
        }
        REQUIRE(oLine >= 0);
        QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, oLine), Q_ARG(int, 4 /* Contain */));
        QQuickItem* line = nullptr;
        REQUIRE(QTest::qWaitFor(
            [&] { return (line = w.find(QStringLiteral("gcodeLine.%1").arg(oLine))) && line->isVisible(); }, 5000));
        QQuickItem* text = Window::findItem(line, QStringLiteral("gcodeText"));
        REQUIRE(text != nullptr);
        INFO("mono font " << text->property("font").value<QFont>().family().toStdString());
        for (const char* theme : {"dark", "light"}) {
            INFO("theme " << theme);
            w.host->theme()->setProperty("mode", QLatin1String(theme));
            QTest::qWait(200);
            const QImage img = w.grab();
            const GlyphInk o = glyphInk(img, text, 0), zero = glyphInk(img, text, 1);
            INFO(o.where << "; " << zero.where);
            INFO(theme << ", " << text->property("font").value<QFont>().family().toStdString() << ": O middle "
                       << o.middle << " of stroke " << o.stroke << "; 0 middle " << zero.middle << " of stroke "
                       << zero.stroke);
            CHECK(o.stroke > 60);
            CHECK(zero.stroke > 60);
            CHECK(o.middle * 4 <= o.stroke);
            CHECK(zero.middle * 10 >= zero.stroke * 6);
        }
        w.host->theme()->setProperty("mode", QStringLiteral("dark"));
    }
    // A line with no move (the program start): nothing highlighted, said quietly.
    g->selectLine(0);
    CHECK_FALSE(g->highlighted());
    CHECK_FALSE(w.host->selection()->pathHighlight().has_value());
    CHECK(g->lineInfo().contains(QStringLiteral("no toolpath move")));

    // The other way: a move picked in the view selects its lines. First through the selection's own entry point (what
    // a click does), then with a real click on a point of the drawn toolpath clear of the panels.
    const camui::MoveRef target = *index.moveOf(static_cast<std::size_t>(moveLine));
    w.host->selection()->applyClick(render::PickHit{hit->item, kernel::SubKind::Edge, target.move, Vec3{}, target.move},
                                    Qt::NoModifier);
    REQUIRE(QTest::qWaitFor([&] { return g->currentLine() >= 0; }, 5000));
    const auto lines = index.linesOf(target);
    REQUIRE(lines);
    CHECK(g->currentLine() == static_cast<int>(lines->first));
    CHECK(g->pickedFirst() == static_cast<int>(lines->first));
    CHECK(g->pickedLast() == static_cast<int>(lines->second));
    CHECK(w.host->selection()->count() == 0); // the body selection is untouched
    {
        // A real click: a rapid segment of the backplot well above the plate, away from the viewer.
        std::shared_ptr<const sim::Backplot> bp;
        for (const ToolpathDisplay::Shown& s : w.display()->shown())
            if (s.item == hit->item)
                bp = s.backplot;
        REQUIRE(bp != nullptr);
        const QRectF viewer = sceneRect(w.shown(QStringLiteral("gcodeView"))).adjusted(-12, -12, 12, 12);
        const QRectF viewRect = sceneRect(w.view()).adjusted(30, 60, -30, -30);
        std::optional<std::pair<QPointF, std::uint32_t>> spot;
        const auto& v = bp->lineVertices;
        for (std::size_t s = 1; s < v.size() && !spot; ++s) {
            if (v[s].moveIndex != v[s - 1].moveIndex || v[s].moveIndex == 0xFFFFFFFFu)
                continue;
            const Vec3 mid{(v[s].x + v[s - 1].x) / 2.0, (v[s].y + v[s - 1].y) / 2.0, (v[s].z + v[s - 1].z) / 2.0};
            if (mid.z < 7.0)
                continue; // above the plate, clear of its edges
            const auto p = ui::nav::project(w.view()->camera(), mid, w.view()->width(), w.view()->height());
            if (!p)
                continue;
            const QPointF sp = w.view()->mapToScene(*p);
            if (viewRect.contains(sp) && !viewer.contains(sp) &&
                index.linesOf(camui::MoveRef{move.section, v[s].moveIndex}))
                spot = std::make_pair(sp, v[s].moveIndex);
        }
        if (spot) {
            g->selectLine(0);
            w.clickScene(spot->first);
            const bool picked = QTest::qWaitFor([&] { return g->pickedFirst() >= 0; }, 10000);
            INFO("clicked move " << spot->second << " at " << spot->first.x() << ", " << spot->first.y());
            CHECK(picked);
            if (picked) {
                const auto m = index.moveOf(static_cast<std::size_t>(g->currentLine()));
                REQUIRE(m);
                // Within a pixel or two the renderer may pick the neighbouring move of the same segment chain.
                CHECK(std::abs(static_cast<long>(m->move) - static_cast<long>(spot->second)) <= 1);
            }
        } else {
            WARN("no point of the drawn toolpath clear of the panels: the real click was not made");
        }
    }

    // Find: case-insensitive, next and previous, the count; Enter in the field goes on.
    w.typeInto(QStringLiteral("gcodeFind"), QStringLiteral("g83"));
    REQUIRE(QTest::qWaitFor([&] { return g->matchCount() > 0; }, 5000));
    const int matches = g->matchCount();
    CHECK(matches == static_cast<int>(camui::findAll(index, "G83").size()));
    const int first = g->currentLine();
    CHECK(index.line(static_cast<std::size_t>(first)).find("G83") != std::string_view::npos);
    QTest::keyClick(w.window(), Qt::Key_Return);
    CHECK(g->currentMatch() == (matches > 1 ? 1 : 0));
    g->findPrevious();
    CHECK(g->currentLine() == first);
    CHECK(w.shown(QStringLiteral("gcodeMatches"))->property("text").toString().endsWith(QString::number(matches)));
    w.typeInto(QStringLiteral("gcodeFind"), QString());

    // Keys in the list: Down moves the line, Shift+Down extends, Ctrl+C copies exactly those lines (line endings
    // included), Ctrl+A all of them.
    w.shown(QStringLiteral("gcodeList"))->forceActiveFocus(); // as a click on a line does
    g->selectLine(2);
    QTest::keyClick(w.window(), Qt::Key_Down);
    CHECK(g->currentLine() == 3);
    QTest::keyClick(w.window(), Qt::Key_Down, Qt::ShiftModifier);
    CHECK(g->selectionFirst() == 3);
    CHECK(g->selectionLast() == 4);
    QTest::keyClick(w.window(), Qt::Key_C, Qt::ControlModifier);
    const std::string_view want = index.lines(3, 4);
    CHECK(QGuiApplication::clipboard()->text() == QString::fromUtf8(want.data(), static_cast<qsizetype>(want.size())));
    QTest::keyClick(w.window(), Qt::Key_A, Qt::ControlModifier);
    CHECK(g->selectionFirst() == 0);
    CHECK(g->selectionLast() == g->lineCount() - 1);
    REQUIRE(g->copyAll());
    CHECK(QGuiApplication::clipboard()->text().toUtf8() == QByteArray::fromStdString(*index.text));
    // Read-only: typing changes nothing.
    QTest::keyClick(w.window(), 'X');
    QTest::keyClick(w.window(), '9');
    CHECK(QByteArray::fromStdString(*g->program()->index->text) == readFile(g->path()));
    // Esc closes the viewer and ends path picking.
    QTest::keyClick(w.window(), Qt::Key_Escape);
    REQUIRE(QTest::qWaitFor([&] { return !g->isOpen(); }, 5000));
    CHECK_FALSE(w.host->selection()->pathPicking());
    CHECK_FALSE(w.host->selection()->pathHighlight().has_value());
    w.checkNoWarnings();
}

// ======================================================================================================== settings
TEST_CASE("settings are remembered per setup, overwrite rules, a second post writes the same bytes, SMB",
          "[cam][post][output]")
{
    RAY_REQUIRE_RHI();
    Window w;
    drillSetup(w);
    openFromRow(w);
    PostToolModel* p = w.post();
    w.postRead();
    // Another post, a property, the folder, a pattern, a rule, a program number.
    p->setPost(QStringLiteral("fanuc"));
    REQUIRE(
        QTest::qWaitFor([&] { return p->postState() == QLatin1String("ok") && p->postId() == "fanuc"; }, kTimeoutMs));
    p->form()->setValue(
        QStringLiteral("sequenceNumbers"),
        p->form()->choicesOf(QStringLiteral("sequenceNumbers")).back().toMap().value(QStringLiteral("value")));
    const QVariant sequence = p->form()->valueOf(QStringLiteral("sequenceNumbers")).toVariant();
    p->setValue(QStringLiteral("directory"), QDir::toNativeSeparators(w.out.path()));
    p->setValue(QStringLiteral("pattern"), QStringLiteral("{setup} {program}"));
    p->setValue(QStringLiteral("overwrite"), QStringLiteral("ask"));
    p->setValue(QStringLiteral("programNumber"), QStringLiteral("2002"));
    // The output unit, the other way from the document's: shown beside the post, and remembered for the setup.
    const QString otherUnits =
        w.document()->state()->settings.units == UnitSystem::Inch ? QStringLiteral("metric") : QStringLiteral("inch");
    p->setValue(QStringLiteral("units"), otherUnits);
    CHECK(w.shown(QStringLiteral("postDialogOutput"))
              ->property("text")
              .toString()
              .endsWith(otherUnits == QLatin1String("inch") ? QStringLiteral("inches (G20)")
                                                            : QStringLiteral("millimetres (G21)")));
    REQUIRE(QTest::qWaitFor([&] { return p->canPost(); }, 5000));
    REQUIRE(p->post());
    w.posted("done");
    const QString file = p->resultPath();
    CHECK(QFileInfo(file).fileName() == QStringLiteral("Op_1_-_top 2002.nc"));
    const QByteArray firstBytes = readFile(file);
    // The program number went back to the setup (one undo step), the post too.
    REQUIRE(QTest::qWaitFor(
        [&] {
            const auto& s = w.document()->state()->cam->setups.front();
            return s.programNumber == 2002 && s.postId == "fanuc";
        },
        kTimeoutMs));
    p->close();

    // Opened again: everything as it was left.
    openFromRow(w);
    w.postRead();
    CHECK(p->postId() == QStringLiteral("fanuc"));
    CHECK(p->form()->valueOf(QStringLiteral("sequenceNumbers")).toVariant() == sequence);
    CHECK(p->values().value(QStringLiteral("directory")).toString() == QDir::toNativeSeparators(w.out.path()));
    CHECK(p->values().value(QStringLiteral("pattern")).toString() == QStringLiteral("{setup} {program}"));
    CHECK(p->values().value(QStringLiteral("overwrite")).toString() == QStringLiteral("ask"));
    CHECK(p->values().value(QStringLiteral("programNumber")).toString() == QStringLiteral("2002"));
    CHECK(p->values().value(QStringLiteral("units")).toString() == otherUnits);

    // Ask: the file exists, so the dialog asks. Keep both: a numbered name; Replace: the same bytes again (nothing
    // changed: posting is deterministic); Cancel: nothing written.
    REQUIRE(QTest::qWaitFor([&] { return p->canPost(); }, 5000));
    REQUIRE(p->post());
    w.posted("asking");
    CHECK(w.shown(QStringLiteral("postReplace"))->isVisible());
    w.shotsBothThemes(QStringLiteral("t047_post_ask"));
    w.click(QStringLiteral("postNewName"));
    w.posted("done");
    CHECK(QFileInfo(p->resultPath()).fileName() == QStringLiteral("Op_1_-_top 2002_2.nc"));
    CHECK(readFile(p->resultPath()) == firstBytes);
    REQUIRE(p->post());
    w.posted("asking");
    w.click(QStringLiteral("postReplace"));
    w.posted("done");
    CHECK(p->resultPath() == file);
    CHECK(readFile(file) == firstBytes);
    REQUIRE(p->post());
    w.posted("asking");
    w.click(QStringLiteral("postCancel"));
    w.posted("cancelled");
    // Refuse and replace without asking.
    p->setValue(QStringLiteral("overwrite"), QStringLiteral("refuse"));
    REQUIRE(p->post());
    w.posted("failed");
    CHECK(p->statusText().contains(QStringLiteral("overwrite rule")));
    p->setValue(QStringLiteral("overwrite"), QStringLiteral("replace"));
    REQUIRE(p->post());
    w.posted("done");
    CHECK(readFile(file) == firstBytes);
    CHECK(QDir(w.out.path()).entryList(QDir::Files | QDir::Hidden).size() == 2);

#ifdef _WIN32
    // A loopback SMB path (the drive's administrative share, there by default; nothing is shared or changed here).
    const QString local = QDir::toNativeSeparators(w.out.path());
    const QString unc = QStringLiteral("\\\\localhost\\%1$%2").arg(local.left(1), local.mid(2));
    if (QFileInfo(unc).isDir()) {
        p->setValue(QStringLiteral("directory"), unc);
        p->setValue(QStringLiteral("pattern"), QStringLiteral("smb {program}"));
        REQUIRE(QTest::qWaitFor([&] { return p->canPost(); }, 5000));
        REQUIRE(p->post());
        w.posted("done");
        const QString onShare = QDir(w.out.path()).filePath(QStringLiteral("smb 2002.nc"));
        CHECK(readFile(onShare) == firstBytes); // the same program, through the share
        // Held open by another program: refused with a clear message, the old file unchanged.
        HANDLE h = CreateFileW(reinterpret_cast<const wchar_t*>((unc + QStringLiteral("\\smb 2002.nc")).utf16()),
                               GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        REQUIRE(h != INVALID_HANDLE_VALUE);
        const QByteArray before = readFile(onShare);
        REQUIRE(p->post());
        w.posted("failed");
        CloseHandle(h);
        bool locked = false;
        for (const QVariant& m : p->messages())
            locked =
                locked ||
                m.toMap().value(QStringLiteral("text")).toString().contains(QStringLiteral("open in another program"));
        CHECK(locked);
        CHECK(readFile(onShare) == before);
        // A share that is not there.
        p->setValue(QStringLiteral("directory"), QStringLiteral("\\\\localhost\\RayCadNoSuchShare$\\cnc"));
        REQUIRE(p->post());
        w.posted("failed");
        bool unreachable = false;
        for (const QVariant& m : p->messages())
            unreachable =
                unreachable ||
                m.toMap().value(QStringLiteral("text")).toString().contains(QStringLiteral("cannot be reached"));
        CHECK(unreachable);
    } else {
        WARN("SMB not exercised in the window test: " << unc.toStdString() << " cannot be reached");
    }
#endif
    // A warning of the post (FANUC: programs 8000-9999 are often edit-protected) is shown in full; the file is written.
    p->setValue(QStringLiteral("directory"), QDir::toNativeSeparators(w.out.path()));
    p->setValue(QStringLiteral("pattern"), QStringLiteral("warned {program}"));
    p->setValue(QStringLiteral("programNumber"), QStringLiteral("8500"));
    REQUIRE(QTest::qWaitFor([&] { return p->canPost(); }, 5000));
    REQUIRE(p->post());
    w.posted("done");
    bool warned = false;
    for (const QVariant& v : p->messages()) {
        const QVariantMap m = v.toMap();
        warned = warned || (m.value(QStringLiteral("kind")).toString() == QStringLiteral("warning") &&
                            m.value(QStringLiteral("text")).toString().contains(QStringLiteral("edit-protected")));
    }
    CHECK(warned);
    CHECK(QFileInfo::exists(QDir(w.out.path()).filePath(QStringLiteral("warned 8500.nc"))));
    w.shown(QStringLiteral("postMessageText.0"));
    p->close();
    w.checkNoWarnings();
}

// ======================================================================================================= cycle time
// T-059: the cycle-time estimate in the CAM tree (a setup's in its secondary text and tooltip, an operation's in its
// name's tooltip: h:mm:ss, marked as an estimate, with what it assumed) and in the post dialog's summary; worked out
// on a worker after the statuses, again when a toolpath changes.
TEST_CASE("the cycle-time estimate: per operation and setup in the tree, in the post dialog, again when a toolpath "
          "changes",
          "[cam][post][cycletime]")
{
    RAY_REQUIRE_RHI();
    Window w;
    drillSetup(w);
    SetupTreeModel* tree = w.tree();
    const SetupId setup{Uuid::fromString(ct::id('a', 1))};
    const OperationId drill1 = ct::opId(1);
    // The estimate arrives (one job after the statuses) and is the whole setup's.
    REQUIRE(QTest::qWaitFor(
        [&] {
            const SetupTreeModel::CycleTimeEstimate* e = tree->cycleTimeOf(setup);
            return e != nullptr && e->input.complete && e->input.operations.size() == 1;
        },
        kTimeoutMs));
    const SetupTreeModel::CycleTimeEstimate* e = tree->cycleTimeOf(setup);
    // It is cam::estimateCycleTime of the toolpath the engine holds, on the setup's machine.
    const cam::CycleTime direct = api::cam_cycletime::estimate(e->input);
    CHECK(e->time.totalSec == direct.totalSec);
    CHECK(e->time.totalSec > 0);
    CHECK(e->time.cutSec > 0);
    CHECK(e->time.toolChanges == 1);
    CHECK(e->time.assumptions.accelerationUnknown);     // the shipped machine definitions publish no accelerations
    CHECK_FALSE(e->time.assumptions.toolChangeUnknown); // the Generic VMC states 4.2 s
    REQUIRE(tree->cycleTimeOf(drill1) != nullptr);
    CHECK(tree->cycleTimeOf(drill1)->totalSec == e->time.totalSec);
    const double firstTotal = e->time.totalSec; // e is replaced when the estimate is worked out again
    const QString total = camui::formatDuration(firstTotal);
    CHECK(QRegularExpression(QStringLiteral("^\\d+:\\d\\d:\\d\\d$")).match(total).hasMatch());

    // The setup row: its secondary line ends with the marked estimate; its tooltip has the split and the assumption.
    const int setupRow = tree->rowOf(QString::fromStdString(ct::id('a', 1)));
    const int opRow = tree->rowOf(QString::fromStdString(ct::id('c', 1)));
    REQUIRE((setupRow >= 0 && opRow >= 0));
    const QVariantMap s = tree->get(setupRow);
    CHECK(s.value(QStringLiteral("cycleTime")).toString() == QStringLiteral("est. ") + total);
    const QString setupTip = s.value(QStringLiteral("cycleTimeTip")).toString();
    CHECK(setupTip.startsWith(QStringLiteral("Estimated cycle time ") + total));
    CHECK(setupTip.contains(QStringLiteral("Machine accelerations unknown")));
    const QString details = w.shown(QStringLiteral("camRowDetails.%1").arg(setupRow))->property("text").toString();
    CHECK(details.endsWith(QStringLiteral("est. ") + total));
    // The operation row: in its name's tooltip only (its right slot, the badge and the tools, is untouched).
    const QVariantMap o = tree->get(opRow);
    CHECK(o.value(QStringLiteral("cycleTime")).toString().isEmpty());
    CHECK(o.value(QStringLiteral("cycleTimeTip")).toString().startsWith(QStringLiteral("Estimated cycle time ")));
    CHECK(o.value(QStringLiteral("status")).toString() == QStringLiteral("upToDate"));
    // Hovering the name shows the tooltip.
    QQuickItem* opItem = w.shown(QStringLiteral("operationRow.%1").arg(opRow));
    QQuickItem* name = nullptr;
    for (QQuickItem* child : opItem->findChildren<QQuickItem*>()) {
        if (child->objectName() == QLatin1String("camRowName"))
            name = child;
    }
    REQUIRE(name != nullptr);
    w.grab();
    w.sendMouse(QEvent::MouseMove, w.centreOf(name), Qt::NoButton, Qt::NoButton);
    REQUIRE(QTest::qWaitFor([&] { return name->property("hovered").toBool(); }, 5000));
    const auto tipShown = [&] {
        for (QQuickItem* child : name->childItems()) {
            if (child->property("active").toBool())
                return true; // the HoverToolTip's loader
        }
        return false;
    };
    CHECK(QTest::qWaitFor(tipShown, 5000));
    w.sendMouse(QEvent::MouseMove, QPointF(w.window()->width() / 2.0, w.window()->height() - 4.0), Qt::NoButton,
                Qt::NoButton);

    // The post dialog's summary says the same.
    openFromRow(w);
    QQuickItem* label = w.shown(QStringLiteral("postDialogCycleTime"));
    CHECK(label->property("text").toString() == setupTip);
    CHECK(w.post()->cycleTime() == setupTip);
    w.shotsBothThemes(QStringLiteral("t059_post_cycle_time"));
    w.post()->close();

    // A second operation: while it is not generated the setup has no total (the operation's own stays); generated,
    // the estimate is worked out again, counting both.
    const int runs = tree->cycleTimeRuns();
    addDrill(w, 2, 6);
    REQUIRE(QTest::qWaitFor(
        [&] {
            const SetupTreeModel::CycleTimeEstimate* x = tree->cycleTimeOf(setup);
            return x != nullptr && !x->input.complete;
        },
        kTimeoutMs));
    CHECK(tree->get(setupRow).value(QStringLiteral("cycleTime")).toString().isEmpty());
    CHECK(tree->get(setupRow)
              .value(QStringLiteral("cycleTimeTip"))
              .toString()
              .contains(QStringLiteral("Only the generated toolpaths are counted")));
    w.generate(2);
    REQUIRE(QTest::qWaitFor(
        [&] {
            const SetupTreeModel::CycleTimeEstimate* x = tree->cycleTimeOf(setup);
            return x != nullptr && x->input.complete && x->input.operations.size() == 2;
        },
        kTimeoutMs));
    CHECK(tree->cycleTimeRuns() > runs);
    const SetupTreeModel::CycleTimeEstimate* both = tree->cycleTimeOf(setup);
    CHECK(both->time.totalSec > firstTotal);
    CHECK(both->time.operations.size() == 2);
    CHECK(both->time.toolChanges == 1); // the same tool (number 1): no change between them
    CHECK(tree->get(setupRow).value(QStringLiteral("cycleTime")).toString() ==
          QStringLiteral("est. ") + camui::formatDuration(both->time.totalSec));
    w.checkNoWarnings();
}

// ======================================================================================================== wallclock
TEST_CASE("a 200,000-line program: the UI thread within budget while posting, and scrolling measured",
          "[cam][post][long][wallclock]")
{
    RAY_REQUIRE_RHI();
    Window w;
    w.open(kPlate);
    w.toWorkspace(ui::kCamWorkspace);
    addSetup(w);
    REQUIRE(w.invokeResult("cam.library.embed", ct::library(ct::toolJson(1))).size() > 0);
    doc::Command c = ct::opAdd(1, 1, 1, "[]", R"({"moves":200000})");
    c.argsJson.replace(c.argsJson.find("\"test.op\""), 9, "\"ui.long\"");
    (void)w.invokeResult(c.name, c.argsJson);
    w.generate(1);
    openFromRow(w);
    PostToolModel* p = w.post();
    w.postRead();
    p->setValue(QStringLiteral("directory"), QDir::toNativeSeparators(w.out.path()));
    REQUIRE(QTest::qWaitFor([&] { return p->canPost(); }, 5000));

    // The UI-thread watchdog while it is posted, written and handed to the viewer.
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
    QElapsedTimer total;
    total.start();
    clock.start();
    tick.start();
    REQUIRE(p->post());
    w.posted("done");
    const qint64 postMs = total.elapsed();
    w.click(QStringLiteral("postShowGcode"));
    GcodeModel* g = w.gcode();
    REQUIRE(QTest::qWaitFor([&] { return g->isOpen(); }, 5000));
    QTest::qWait(200);
    tick.stop();
    const int lines = g->lineCount();
    WARN("200k program: " << lines << " lines, " << g->program()->index->text->size()
                          << " bytes, posted and written in " << postMs << " ms; worst UI-thread gap " << worst
                          << " ms");
    CHECK(lines >= 200000);
    CHECK(worst < ray::test::uiBudgetMs(50));

    // Scrolling: page by page through the whole program, then jumps; each step waits for its frame.
    QQuickItem* list = w.shown(QStringLiteral("gcodeList"));
    const double rowHeight =
        w.find(QStringLiteral("gcodeLine.0")) ? w.find(QStringLiteral("gcodeLine.0"))->height() : 16;
    const double page = list->height();
    const double contentHeight = rowHeight * lines;
    const auto at = [](std::vector<double> v, double q) {
        std::sort(v.begin(), v.end());
        return v[static_cast<std::size_t>(q * static_cast<double>(v.size() - 1))];
    };
    // (1) UI-thread cost of a scroll step: the new position laid out (rows made for what comes into sight), for 400
    // jumps spread over the whole program and 200 single pages; the same cost anywhere in it shows it is virtualised.
    std::vector<double> jumpMs, pageMs;
    const auto layoutAt = [&](double y, std::vector<double>& into) {
        QElapsedTimer t;
        t.start();
        list->setProperty("contentY", y);
        QMetaObject::invokeMethod(list, "forceLayout");
        into.push_back(static_cast<double>(t.nsecsElapsed()) / 1e6);
        QCoreApplication::processEvents();
    };
    const int jumps = 400;
    for (int i = 0; i < jumps; ++i)
        layoutAt(std::min(contentHeight - page, (contentHeight - page) * i / (jumps - 1)), jumpMs);
    for (int i = 0; i < 200; ++i)
        layoutAt(std::max(0.0, contentHeight - page * (i + 2)), pageMs);
    // (2) A continuous scroll in the middle of the program, three lines a frame for 300 frames (a wheel or a flick):
    // the interval between frames shown.
    // The same number of frames of the window with nothing moving is the baseline: what scrolling adds is the
    // difference (the window draws the 3D view and the panels too, and the machine is shared).
    const auto frameIntervals = [&](double linesPerFrame) {
        std::vector<double> out;
        QElapsedTimer frameClock;
        double y = contentHeight / 2;
        qint64 lastFrame = -1;
        int frames = 0;
        // (frameSwapped comes from the render thread: the step is made on the UI thread, queued to `list`.)
        const auto conn = QObject::connect(w.window(), &QQuickWindow::frameSwapped, list, [&] {
            const qint64 now = frameClock.nsecsElapsed();
            if (lastFrame >= 0)
                out.push_back(static_cast<double>(now - lastFrame) / 1e6);
            lastFrame = now;
            ++frames;
            y += linesPerFrame * rowHeight;
            list->setProperty("contentY", y);
            w.window()->requestUpdate();
        });
        frameClock.start();
        list->setProperty("contentY", y);
        w.window()->requestUpdate();
        REQUIRE(QTest::qWaitFor([&] { return frames >= 300; }, 60000));
        QObject::disconnect(conn);
        return out;
    };
    const std::vector<double> baseMs = frameIntervals(0);
    const std::vector<double> frameMs = frameIntervals(3);
    const int rowsMade = static_cast<int>(list->property("contentItem").value<QQuickItem*>()->childItems().size());
    WARN("G-code viewer, " << lines << "-line program, " << page / rowHeight << " lines in view, " << rowsMade
                           << " row items alive: layout per jump across the program median " << at(jumpMs, 0.5)
                           << " ms, 95th " << at(jumpMs, 0.95) << " ms, worst " << at(jumpMs, 1.0)
                           << " ms; per page median " << at(pageMs, 0.5) << " ms, 95th " << at(pageMs, 0.95)
                           << " ms, worst " << at(pageMs, 1.0) << " ms; continuous scroll (3 lines a frame, "
                           << frameMs.size() << " frames): frame interval median " << at(frameMs, 0.5) << " ms, 95th "
                           << at(frameMs, 0.95) << " ms, worst " << at(frameMs, 1.0)
                           << " ms; the same window not scrolling: median " << at(baseMs, 0.5) << " ms, 95th "
                           << at(baseMs, 0.95) << " ms, worst " << at(baseMs, 1.0) << " ms");
    // Virtualised: rows exist for what is in sight (and a small cache), not for 200,000 lines.
    CHECK(rowsMade < 300);
    // Smooth: a new screenful of rows costs a few ms of the UI thread wherever it is in the program, a continuous
    // scroll adds little to the window's own frame time, and no frame stalls.
    CHECK(at(jumpMs, 0.95) < ray::test::uiBudgetMs(8));
    CHECK(at(pageMs, 0.95) < ray::test::uiBudgetMs(8));
    CHECK(at(frameMs, 0.5) < at(baseMs, 0.5) + ray::test::uiBudgetMs(4));
    CHECK(at(frameMs, 0.95) < at(baseMs, 0.95) + ray::test::uiBudgetMs(8));
    CHECK(at(frameMs, 1.0) < ray::test::uiBudgetMs(100));
    // The last line is reachable and right.
    g->selectLine(lines - 1);
    QMetaObject::invokeMethod(list, "positionViewAtEnd");
    QTest::qWait(100);
    REQUIRE(w.find(QStringLiteral("gcodeLine.%1").arg(lines - 1)) != nullptr);
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
