// Replacing an imported part with a new revision in the desktop app (task T-084), on the real Qt Quick window with the
// real application session: the palette command opens the dialog with the part's import chosen; the new revision is
// matched and the summary says what changed ("2 faces changed, 1 new, 1 removed"), tinting changed faces amber, the new
// face green and drawing a dashed box where the removed face was; a row shows its face; the reference review opens by
// itself with the operation whose face changed; Undo puts the old revision back; a refusal says why and changes
// nothing.
//
// Screenshots: with RAY_T084_SHOTS set to a folder, the window is saved there in both themes (t084_*.png).
#include "../scene_graph_probe.h"
#include "../window_focus.h"
#include "app_context.h"
#include "app_host.h"
#include "app_options.h"
#include "files.h"
#include "model/directedit_review.h"
#include "model/replace_import.h"
#include "palette_commands.h"
#include "scene_controller.h"
#include "selection_model.h"
#include "session.h"
#include "theme_object.h"
#include "viewport_item.h"

#include "engine_test_util.h" // tests/unit/cam: the test-only operation type and the library items' JSON

#include <QtCore/QDir>
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

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace ray;
using ray::ui::AppHost;
using ray::ui::AppOptions;
using ray::ui::ReferenceReviewModel;
using ray::ui::ReplaceImportModel;
namespace ct = ray::cam::test;

namespace {

constexpr int kTimeoutMs = 120000;

template <class T>
T take(Result<T> r)
{
    INFO((r ? std::string() : r.error().message));
    REQUIRE(r.has_value());
    return std::move(*r);
}

// A 60 x 40 x 20 block with holes; `revision`: one 4 mm hole added at (30, 32), the 8 mm hole at (45, 20) removed.
kernel::Shape block(bool revision)
{
    JobContext ctx;
    kernel::Shape s = take(kernel::makeBox({0, 0, 0}, {60, 40, 20}));
    auto cut = [&](Vec3 at, double r) {
        s = take(kernel::boolean(kernel::BoolOp::Cut, s, take(kernel::makeCylinder(at, {0, 0, 1}, r, 22)), ctx)).shape;
    };
    cut({15, 20, -1}, 3);
    if (revision)
        cut({30, 32, -1}, 2);
    else
        cut({45, 20, -1}, 4);
    return s;
}

struct Window {
    QTemporaryDir cache;
    QTemporaryDir userData;
    QTemporaryDir files;
    std::unique_ptr<AppHost> host;

    Window()
    {
        AppOptions options;
        options.theme = QStringLiteral("dark");
        options.glass = false;
        options.size = QSize(1440, 900);
        host = std::make_unique<AppHost>(options, cache.path());
        REQUIRE(host->optionError().isEmpty());
        const bool loaded = host->load();
        INFO(host->errorString().toStdString() << " " << host->warnings().join(QStringLiteral("; ")).toStdString());
        REQUIRE(loaded);
        host->show();
        REQUIRE(QTest::qWaitForWindowExposed(host->window()));
        REQUIRE(ray::test::focusWindow(host->window()));
        REQUIRE(host->viewport() != nullptr);
        REQUIRE(host->startSession(userData.path().toStdString()));
        host->app()->operationTypes()->add(std::make_unique<ct::TestOperation>());
        host->files()->setUseNativeDialogs(false);
        host->viewport()->setAnimationsEnabled(false);
        host->scene()->setPreset(render::presetFor(render::Tier::Medium));
        grab();
    }

    QQuickWindow* window() const { return host->window(); }
    template <class T>
    T* singleton(const char* name) const
    {
        auto* s = host->engine()->singletonInstance<T*>(QStringLiteral("RayCad.Ui"), QString::fromLatin1(name));
        REQUIRE(s != nullptr);
        return s;
    }
    ReplaceImportModel* replacement() const { return singleton<ReplaceImportModel>("Replacement"); }
    ReferenceReviewModel* review() const { return singleton<ReferenceReviewModel>("References"); }
    QImage grab() const { return host->window()->grabWindow(); }
    QQuickItem* find(const QString& objectName) const { return findItem(window()->contentItem(), objectName); }
    static QQuickItem* findItem(QQuickItem* item, const QString& objectName)
    {
        if (item->objectName() == objectName)
            return item;
        for (QQuickItem* child : item->childItems())
            if (QQuickItem* found = findItem(child, objectName))
                return found;
        return nullptr;
    }
    QQuickItem* shown(const QString& objectName) const
    {
        QQuickItem* item = nullptr;
        REQUIRE(QTest::qWaitFor([&] { return (item = find(objectName)) != nullptr && item->isVisible(); }, 10000));
        return item;
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
    void click(const QString& objectName) const { click(shown(objectName)); }
    doc::Document* document() const
    {
        api::OpenDocument* od = host->app()->active();
        REQUIRE(od != nullptr);
        return od->document.get();
    }
    bool settled() const
    {
        doc::Document* d = host->app()->active() ? host->app()->active()->document.get() : nullptr;
        return d != nullptr && !d->rebuildPending() && !host->scene()->meshing() && !review()->busy() &&
               !replacement()->busy();
    }
    void settle() const
    {
        for (int pass = 0; pass < 2; ++pass) {
            REQUIRE(QTest::qWaitFor([&] { return settled(); }, kTimeoutMs));
            QTest::qWait(50);
        }
    }
    std::string invokeResult(const std::string& action, const std::string& args) const
    {
        JobHandle<api::ActionResult> job = host->app()->actions().invoke(action, args, api::Caller::User);
        REQUIRE(QTest::qWaitFor([&] { return job.done(); }, kTimeoutMs));
        auto r = job.wait();
        if (!r)
            FAIL(action << " failed: " << r.error().message);
        return r->resultJson;
    }
    QString bodyId() const
    {
        const api::viewer::Body* b = host->scene()->body(0);
        REQUIRE(b != nullptr);
        return QString::fromStdString(b->id.value.toString());
    }
    std::string refJson(std::uint32_t face) const
    {
        const QJsonObject args{{QStringLiteral("body"), bodyId()},
                               {QStringLiteral("kind"), QStringLiteral("face")},
                               {QStringLiteral("index"), static_cast<int>(face)}};
        const QJsonObject r =
            QJsonDocument::fromJson(
                QByteArray::fromStdString(
                    invokeResult("model.reference", QJsonDocument(args).toJson(QJsonDocument::Compact).toStdString())))
                .object();
        return QJsonDocument(r.value(QStringLiteral("ref")).toObject()).toJson(QJsonDocument::Compact).toStdString();
    }
    std::uint32_t topFace() const
    {
        const api::viewer::Body* b = host->scene()->body(0);
        for (std::uint32_t i = 0; i < b->shape.count(kernel::SubKind::Face); ++i) {
            const auto s = kernel::surfaceInfo(b->shape, {kernel::SubKind::Face, i});
            if (s && s->type == kernel::SurfaceType::Plane && std::abs(s->bounds.min.z - 20) < 1e-6 &&
                std::abs(s->bounds.max.z - 20) < 1e-6)
                return i;
        }
        FAIL_CHECK("no top face");
        return 0;
    }
    void shot(const char* name) const
    {
        const QString dir = qEnvironmentVariable("RAY_T084_SHOTS");
        if (dir.isEmpty())
            return;
        QDir().mkpath(dir);
        sendMouse(QEvent::MouseMove, QPointF(window()->width() / 2.0, window()->height() - 4.0), Qt::NoButton,
                  Qt::NoButton);
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
};

} // namespace

TEST_CASE("replacing an imported part with a new revision: the dialog, the coloured summary, the hand-over, undo",
          "[replaceimport]")
{
    RAY_REQUIRE_RHI();
    ct::Gate::instance().reset();
    Window w;
    const std::string revA = QDir(w.files.path()).filePath(QStringLiteral("bracket rev A.stp")).toStdString();
    const std::string revB = QDir(w.files.path()).filePath(QStringLiteral("bracket rev B.stp")).toStdString();
    {
        JobContext ctx;
        REQUIRE(kernel::exportStep(revA, std::vector<kernel::Shape>{block(false)}, ctx));
        REQUIRE(kernel::exportStep(revB, std::vector<kernel::Shape>{block(true)}, ctx));
    }
    REQUIRE(w.host->files()->open(QString::fromStdString(revA)));
    REQUIRE(QTest::qWaitFor([&] { return w.host->session()->hasDocument() && w.settled(); }, 5 * kTimeoutMs));
    w.settle();
    w.host->viewport()->fitAll();
    ReplaceImportModel* r = w.replacement();

    // A CAM operation on the top face: the new hole runs through it, so its reference needs review after the replace.
    const std::string body = w.bodyId().toStdString();
    const std::string top = w.refJson(w.topFace());
    w.invokeResult("cam.library.embed", ct::library(ct::toolJson(1), {}, ct::machineJson(1)));
    w.invokeResult("cam.setup.add", ct::setupAdd(1, "\"" + body + "\"").argsJson);
    w.invokeResult("cam.operation.add", ct::opAdd(1, 1, 1, "[" + top + "]").argsJson);
    w.settle();
    const std::string before = w.document()->state()->stateHash().hex();
    const std::size_t faces = w.host->scene()->body(0)->shape.count(kernel::SubKind::Face);

    // The palette command opens the dialog with the only import chosen.
    REQUIRE(w.host->commands()->trigger(QStringLiteral("replaceImport.open")));
    w.shown(QStringLiteral("replaceImport"));
    CHECK(r->shown());
    CHECK(r->feature() == QString::fromStdString(w.document()->state()->features.front().id.value.toString()));
    CHECK(w.shown(QStringLiteral("replaceImportCurrent"))
              ->property("text")
              .toString()
              .contains(QStringLiteral("bracket rev A.stp")));
    CHECK_FALSE(w.shown(QStringLiteral("replaceImportReplace"))->property("enabled").toBool());
    // The file dialog's answer (the native dialog itself is not driven).
    r->setPath(QUrl::fromLocalFile(QString::fromStdString(revB)).toString());
    CHECK(r->fileName() == QStringLiteral("bracket rev B.stp"));
    REQUIRE(QTest::qWaitFor(
        [&] { return w.find(QStringLiteral("replaceImportReplace"))->property("enabled").toBool(); }, 5000));
    w.shotsBothThemes("t084_dialog");

    // Replace: one undo step, then the summary.
    const std::size_t undo = w.document()->undoCount();
    w.click(QStringLiteral("replaceImportReplace"));
    REQUIRE(QTest::qWaitFor([&] { return r->hasResult(); }, kTimeoutMs));
    w.settle();
    CHECK(w.document()->undoCount() == undo + 1);
    CHECK(r->summary() == QStringLiteral("2 faces changed, 1 new, 1 removed"));
    CHECK(r->changedCount() == 2);
    CHECK(r->newCount() == 1);
    CHECK(r->removedCount() == 1);
    CHECK(r->toolpaths() == 1);
    CHECK(r->notes().contains(QStringLiteral("1 toolpath uses this part and needs regenerating.")));
    CHECK(w.shown(QStringLiteral("replaceImportSummary"))->property("text").toString() == r->summary());
    REQUIRE(r->rows().size() == 4);
    // Coloured in the view: the changed faces amber, the new face green, a dashed box where the removed face was.
    REQUIRE(QTest::qWaitFor([&] { return r->tintedFaces(QStringLiteral("changed")) == 2; }, kTimeoutMs));
    CHECK(r->tintedFaces(QStringLiteral("new")) == 1);
    CHECK(r->removedBoxes() == 1);
    // The review opens by itself with the operation whose face changed.
    REQUIRE(QTest::qWaitFor([&] { return w.review()->shown() && !w.review()->busy(); }, kTimeoutMs));
    CHECK(w.review()->count() >= 1);
    w.shown(QStringLiteral("referenceReview"));
    w.shotsBothThemes("t084_summary");
    // The dialog leaves the view cube and its view buttons free, and with the review open below it both are whole, at
    // this size and at the shell's minimum window size (their lists scroll).
    {
        const auto sceneRect = [](QQuickItem* item) {
            return item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
        };
        const QSize normal = w.window()->size();
        const QSize minimum(w.window()->minimumWidth(), w.window()->minimumHeight());
        for (const QSize size : {normal, minimum}) {
            if (w.window()->size() != size) {
                w.window()->resize(size);
                REQUIRE(QTest::qWaitFor([&] { return w.window()->size() == size; }, 5000));
                QTest::qWait(100);
            }
            w.grab();
            const QRectF area = sceneRect(w.shown(QStringLiteral("viewportArea")));
            const QRectF cube = sceneRect(w.shown(QStringLiteral("viewCubeColumn")));
            const QRectF dialog = sceneRect(w.shown(QStringLiteral("replaceImport")));
            const QRectF panel = sceneRect(w.shown(QStringLiteral("referenceReview")));
            INFO("window " << size.width() << " x " << size.height() << "; cube " << cube.x() << "," << cube.y() << " "
                           << cube.width() << "x" << cube.height() << "; dialog " << dialog.x() << "," << dialog.y()
                           << " " << dialog.width() << "x" << dialog.height() << "; review " << panel.x() << ","
                           << panel.y() << " " << panel.width() << "x" << panel.height());
            CHECK_FALSE(dialog.intersects(cube));
            CHECK_FALSE(dialog.intersects(panel));
            CHECK(area.contains(dialog));
            CHECK(area.contains(panel));
            // Each whole: its title, its list (some of it) and its buttons inside it.
            for (const char* name : {"replaceImportTitle", "replaceImportDone"})
                CHECK(dialog.contains(sceneRect(w.shown(QString::fromLatin1(name)))));
            CHECK(sceneRect(w.shown(QStringLiteral("replaceImportList"))).height() > 0);
            CHECK(panel.contains(sceneRect(w.shown(QStringLiteral("referenceReviewTitle")))));
            const QRectF footer = sceneRect(w.find(QStringLiteral("referenceReviewFooter")));
            CHECK(footer.top() >= panel.top());
            CHECK(footer.bottom() <= panel.bottom());
            CHECK(sceneRect(w.shown(QStringLiteral("referenceReviewList"))).height() > 0);
            if (size == minimum)
                w.shotsBothThemes("t084_summary_minimum_window");
        }
        w.window()->resize(normal);
        REQUIRE(QTest::qWaitFor([&] { return w.window()->size() == normal; }, 5000));
        QTest::qWait(100);
    }
    // A row shows its face.
    int newRow = -1;
    for (int i = 0; i < r->rows().size(); ++i)
        if (r->rows().at(i).toMap().value(QStringLiteral("fate")).toString() == QStringLiteral("new"))
            newRow = i;
    REQUIRE(newRow >= 0);
    w.click(QStringLiteral("replaceImportRow.%1").arg(newRow));
    CHECK(r->current() == newRow);
    CHECK(r->highlighted().size() == 1);
    CHECK(w.host->selection()->toolHighlight().size() == 1);
    w.shotsBothThemes("t084_row");
    // The Review button opens the review (already open: it stays).
    w.click(QStringLiteral("replaceImportReview"));
    CHECK(w.review()->shown());

    // Undo: the old revision back, the tints gone.
    w.click(QStringLiteral("replaceImportUndo"));
    REQUIRE(QTest::qWaitFor([&] { return !r->hasResult(); }, kTimeoutMs));
    w.settle();
    CHECK(w.document()->state()->stateHash().hex() == before);
    CHECK(w.host->scene()->body(0)->shape.count(kernel::SubKind::Face) == faces);
    CHECK(r->tintedFaces(QStringLiteral("changed")) == 0);
    CHECK(r->removedBoxes() == 0);
    CHECK(r->message().contains(QStringLiteral("undone")));

    // A refusal: the very file the part came from. Said plainly; nothing changes.
    r->setPath(QString::fromStdString(revA));
    w.click(QStringLiteral("replaceImportReplace"));
    REQUIRE(QTest::qWaitFor([&] { return !r->busy() && r->messageIsError(); }, kTimeoutMs));
    CHECK(r->message().contains(QStringLiteral("very file the part was imported from")));
    CHECK(r->message().contains(QStringLiteral("Nothing was changed")));
    CHECK(w.document()->state()->stateHash().hex() == before);
    w.shotsBothThemes("t084_refused");
    w.click(QStringLiteral("replaceImportDone"));
    CHECK_FALSE(r->shown());
}
