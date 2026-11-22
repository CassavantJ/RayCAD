// The operation template panel (task T-057) in the real window with the real application session, as a user does it:
// a 6061 plate like benchmark part 1 (built through the feature actions: four 1/4-20 UNC tapped through holes and four
// counterbored 0.266 in holes), a setup, the CAM tree's Templates button on the setup row, the "Tapped holes" set
// chosen with the keyboard, its preview (spot, #7 drill, tap) before anything is applied, Apply (one undo step), the
// tree showing which template each operation came from, a set that cannot be applied (no reamer of that size: the
// preview says which tool for which holes and Apply is disabled), saving an operation as a template from its row,
// renaming and deleting it, and re-applying a template from an operation's row. Every icon button of the panel and
// the tree has a tooltip and an accessible name (audited in each state).
//
// Screenshots (light theme): build/shots/t057_<n>_<step>.png (RAY_T057_SHOTS names another folder).
#include "../scene_graph_probe.h"
#include "../window_focus.h"
#include "app_context.h"
#include "app_host.h"
#include "app_options.h"
#include "cam/setup_tree.h"
#include "cam/templates_model.h"
#include "files.h"
#include "scene_controller.h"
#include "session.h"
#include "ui_commands.h"
#include "viewport_item.h"

#include <QtCore/QDir>
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

#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace ray;
using ray::ui::AppHost;
using ray::ui::AppOptions;
using ray::ui::SetupTreeModel;
using ray::ui::TemplatesModel;

namespace Catch {
template <>
struct StringMaker<QString> {
    static std::string convert(const QString& s) { return '"' + s.toStdString() + '"'; }
};
} // namespace Catch

namespace {

constexpr int kTimeoutMs = 60000;
constexpr double kIn = 25.4;

QRectF sceneRect(QQuickItem* i)
{
    return i->mapRectToScene(QRectF(0, 0, i->width(), i->height()));
}

std::string num(double v)
{
    std::ostringstream s;
    s.precision(17);
    s << v;
    return s.str();
}

std::string uuid(char kind, int n)
{
    static const char* const hex = "0123456789abcdef";
    std::string tail(12, '0');
    for (int i = 11; i >= 0; --i) {
        tail[static_cast<std::size_t>(i)] = hex[n % 16];
        n /= 16;
    }
    return std::string(1, kind) + "0000000-0000-4000-8000-" + tail;
}

// A sketch model of points (and a rectangle).
struct Sketch {
    std::vector<std::string> entities;
    int next = 1;
    std::string point(double x, double y)
    {
        const std::string id = uuid('e', next++);
        entities.push_back(R"({"id":")" + id + R"(","type":"point","x":)" + num(x) + R"(,"y":)" + num(y) + "}");
        return id;
    }
    void rect(double x0, double y0, double x1, double y1)
    {
        const std::string a = point(x0, y0), b = point(x1, y0), c = point(x1, y1), d = point(x0, y1);
        const auto line = [&](const std::string& p, const std::string& q) {
            entities.push_back(R"({"id":")" + uuid('e', next++) + R"(","type":"line","a":")" + p + R"(","b":")" + q +
                               "\"}");
        };
        line(a, b);
        line(b, c);
        line(c, d);
        line(d, a);
    }
    std::string json() const
    {
        std::string e;
        for (const std::string& x : entities)
            e += (e.empty() ? "" : ",") + x;
        return R"({"schema":1,"entities":[)" + e + R"(],"constraints":[]})";
    }
};

struct Window {
    QTemporaryDir cache;
    QTemporaryDir userData;
    std::unique_ptr<AppHost> host;
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
        REQUIRE(ray::test::focusWindow(host->window()));
        REQUIRE(host->startSession(userData.path().toStdString()));
        host->files()->setUseNativeDialogs(false);
        host->viewport()->setAnimationsEnabled(false);
    }
    QQuickWindow* window() const { return host->window(); }
    template <class T>
    T* singleton(const char* name) const
    {
        auto* s = host->engine()->singletonInstance<T*>(QStringLiteral("RayCad.Ui"), QString::fromLatin1(name));
        REQUIRE(s != nullptr);
        return s;
    }
    TemplatesModel* templates() const { return singleton<TemplatesModel>("CamTemplates"); }
    SetupTreeModel* tree() const { return singleton<SetupTreeModel>("CamTreeModel"); }
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
    // Popups live in the window's overlay, under the content item too.
    QQuickItem* find(const QString& objectName) const { return findItem(window()->contentItem(), objectName); }
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
        QTest::qWait(30);
        sendMouse(QEvent::MouseButtonPress, p, Qt::LeftButton, Qt::LeftButton);
        sendMouse(QEvent::MouseButtonRelease, p, Qt::LeftButton, Qt::NoButton);
        QCoreApplication::processEvents();
    }
    void click(const QString& objectName) const { clickScene(sceneRect(shown(objectName)).center()); }
    // A row tool of the CAM tree, shown while the pointer is on its row.
    void clickRowTool(const QString& row, const QString& tool) const
    {
        QQuickItem* r = shown(row);
        sendMouse(QEvent::MouseMove, sceneRect(r).center(), Qt::NoButton, Qt::NoButton);
        QCoreApplication::processEvents();
        QQuickItem* t = shown(tool);
        REQUIRE(QTest::qWaitFor([&] { return t->property("shown").toBool(); }, 5000));
        clickScene(sceneRect(t).center());
    }
    void key(Qt::Key k, Qt::KeyboardModifiers m = Qt::NoModifier) const
    {
        if (QGuiApplication::focusWindow() != window())
            ray::test::focusWindow(window());
        QTest::keyClick(window(), k, m);
        QTest::qWait(20);
    }
    void type(const QString& text) const
    {
        for (const QChar c : text)
            QTest::keyClick(window(), c.toLatin1());
        QCoreApplication::processEvents();
    }
    std::string invoke(const std::string& action, const std::string& args) const
    {
        auto job = host->app()->actions().invoke(action, args, api::Caller::User);
        REQUIRE(QTest::qWaitFor([&] { return job.done(); }, kTimeoutMs));
        auto r = job.wait();
        if (!r)
            FAIL(action << " failed: " << r.error().message);
        return r->resultJson;
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
        return d != nullptr && !d->rebuildPending() && host->scene()->ready() && !host->scene()->meshing();
    }
    void shot(const char* step)
    {
        QString dir = qEnvironmentVariable("RAY_T057_SHOTS");
        if (dir.isEmpty())
            dir = QStringLiteral(RAY_SHOTS_DIR);
        QDir().mkpath(dir);
        sendMouse(QEvent::MouseMove, QPointF(window()->width() / 2.0, window()->height() - 4.0), Qt::NoButton,
                  Qt::NoButton);
        QTest::qWait(300);
        const QString name =
            QStringLiteral("t057_%1_%2.png").arg(++shots, 2, 10, QLatin1Char('0')).arg(QLatin1String(step));
        window()->grabWindow().save(QDir(dir).filePath(name), "PNG");
    }
    std::vector<const cam::Operation*> operations() const
    {
        std::vector<const cam::Operation*> out;
        const doc::DocStatePtr s = document()->state();
        if (!s->cam || s->cam->setups.empty())
            return out;
        for (const OperationId& id : s->cam->setups.front().operations)
            out.push_back(&s->cam->operations.at(id));
        return out;
    }
};

// Every button under `root` that shows no text (an icon alone) says what it does: a tooltip and an accessible name.
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

// The part, the setup and the CAM workspace.
void prepare(Window& w)
{
    w.invoke("file.new", "{}");
    const double L = 6 * kIn, W = 4 * kIn, T = 0.75 * kIn;
    const std::string body = uuid('b', 1);
    const auto feature = [&](const char* type, int n, const std::string& props) {
        w.invoke(std::string("feature.") + type + ".add", R"({"id":")" + uuid('f', n) + R"(","props":)" + props + "}");
    };
    Sketch base;
    base.rect(0, 0, L, W);
    feature("sketch", 2, R"({"plane":{"principal":"xy"},"sketch":)" + base.json() + "}");
    feature("extrude", 3,
            R"({"sketch":")" + uuid('f', 2) + R"(","distance":)" + num(T) + R"(,"operation":"new","target":")" + body +
                R"("})");
    feature("datum_plane", 4, R"({"base":{"principal":"xy"},"offset":)" + num(T) + "}");
    const std::string onTop = R"({"datum":")" + uuid('f', 4) + R"("})";
    Sketch tapped;
    for (const auto& [x, y] : {std::pair{0.46, 0.1}, std::pair{0.54, 0.1}, std::pair{0.46, 0.9}, std::pair{0.54, 0.9}})
        tapped.point(L * x, W * y);
    feature("sketch", 5, R"({"plane":)" + onTop + R"(,"sketch":)" + tapped.json() + "}");
    feature("hole", 6,
            R"({"sketch":")" + uuid('f', 5) + R"(","target":")" + body +
                R"(","kind":"tapped","end":"through","thread":{"designation":"1/4-20 UNC","standard":"unc",)"
                R"("nominalDiameter":6.35,"tpi":20,"tapDrill":5.1054}})");
    Sketch cb;
    for (const auto& [x, y] :
         {std::pair{0.08, 0.12}, std::pair{0.92, 0.12}, std::pair{0.08, 0.88}, std::pair{0.92, 0.88}})
        cb.point(L * x, W * y);
    feature("sketch", 7, R"({"plane":)" + onTop + R"(,"sketch":)" + cb.json() + "}");
    feature("hole", 8,
            R"({"sketch":")" + uuid('f', 7) + R"(","target":")" + body +
                R"(","kind":"counterbore",)"
                R"("diameter":)" +
                num(0.266 * kIn) + R"(,"end":"through","counterbore":{"diameter":)" + num(0.44 * kIn) + R"(,"depth":)" +
                num(0.26 * kIn) + "}}");
    REQUIRE(QTest::qWaitFor([&] { return w.settled(); }, kTimeoutMs));
    // Setup 1: the shipped 6061-T6 and the Haas machine.
    lib::LibraryStack* stack = w.host->app()->libraries();
    std::string material, materialId, machine, machineId;
    for (const lib::Material& m : stack->materials()) {
        if (m.name == "6061-T6") {
            material = lib::toJson(m);
            materialId = m.id.value.toString();
        }
    }
    for (const lib::MachineDef& m : stack->machines()) {
        if (m.controlDialect == "haas-ngc") {
            machine = lib::toJson(m);
            machineId = m.id.value.toString();
        }
    }
    REQUIRE_FALSE(material.empty());
    w.invoke("cam.setup.add", R"({"id":")" + uuid('a', 1) + R"(","name":"Op 1 - top","partBodies":[")" + body +
                                  R"("],"machine":")" + machineId + R"(","material":")" + materialId +
                                  R"(","library":{"machines":[)" + machine + R"(],"materials":[)" + material + "]}}");
    w.click(QStringLiteral("workspaceTab.%1").arg(ui::kCamWorkspace));
    REQUIRE(QTest::qWaitFor([&] { return w.host->context()->workspace() == ui::kCamWorkspace; }, 5000));
    w.host->viewport()->fitAll();
    REQUIRE(QTest::qWaitFor([&] { return w.settled() && w.tree()->count() == 1; }, kTimeoutMs));
}

// Selects the list row whose title is `title` (and, for a set, is a set row) with the keyboard: the search field,
// then Down to the row.
void chooseWithKeys(Window& w, const QString& search, const QString& title)
{
    QQuickItem* field = w.shown(QStringLiteral("templatesSearch"));
    field->forceActiveFocus();
    QMetaObject::invokeMethod(field, "selectAll");
    w.key(Qt::Key_Delete);
    w.type(search);
    REQUIRE(QTest::qWaitFor([&] { return w.templates()->search() == search; }, 5000));
    w.key(Qt::Key_Down); // into the list, its first row
    for (int i = 0; i < 40; ++i) {
        const QVariantMap info = w.templates()->selectedInfo();
        if (info.value(QStringLiteral("title")).toString() == title)
            return;
        w.key(Qt::Key_Down);
    }
    FAIL("no row " << title.toStdString());
}

bool previewDone(const Window& w)
{
    const QString s = w.templates()->previewState();
    return s == QLatin1String("ok") || s == QLatin1String("refused") || s == QLatin1String("failed");
}

} // namespace

TEST_CASE("operation templates in the window: browse, preview, apply, refuse, save, rename, delete, re-apply",
          "[cam][templates][flow]")
{
    RAY_REQUIRE_RHI();
    Window w;
    prepare(w);
    w.shot("setup");

    // ---- The setup row's Templates button opens the panel on that setup ----
    w.clickRowTool(QStringLiteral("setupRow.0"), QStringLiteral("camRowTemplates.0"));
    REQUIRE(QTest::qWaitFor([&] { return w.templates()->shown(); }, 5000));
    // The popup itself is no item: its content is (the footer's parent).
    QQuickItem* panel = w.shown(QStringLiteral("templatesFooter"))->parentItem();
    CHECK(w.templates()->mode() == QStringLiteral("apply"));
    CHECK(w.templates()->setupIdText() == QString::fromStdString(uuid('a', 1)));
    // The setup's material (6061-T6) filters the list: its three sets, each with its steps.
    CHECK(w.templates()->templateCount() == 14);
    w.shot("browse");
    CHECK(checkIconButtons(panel, QStringLiteral("templates, browsing")) >= 1);

    // ---- The keyboard: the tapped holes set, and its preview before anything is applied ----
    chooseWithKeys(w, QStringLiteral("tapped"), QStringLiteral("Tapped holes"));
    REQUIRE(QTest::qWaitFor([&] { return previewDone(w); }, kTimeoutMs));
    INFO(w.templates()->previewMessage().toStdString());
    REQUIRE(w.templates()->previewState() == QStringLiteral("ok"));
    const QVariantList ops = w.templates()->previewOps();
    REQUIRE(ops.size() == 3);
    CHECK(ops[0].toMap().value(QStringLiteral("name")).toString() == QStringLiteral("Spot drill"));
    CHECK(ops[0].toMap().value(QStringLiteral("tool")).toString() ==
          QStringLiteral("1/4 in 90 deg carbide spot drill"));
    CHECK(ops[1].toMap().value(QStringLiteral("tool")).toString() ==
          QStringLiteral("#7 (0.201 in) HSS-Co jobber drill"));
    CHECK(ops[2].toMap().value(QStringLiteral("tool")).toString() ==
          QStringLiteral("1/4-20 UNC HSS-E spiral point tap"));
    for (const QVariant& op : ops)
        CHECK(op.toMap().value(QStringLiteral("holes")).toInt() == 4);
    CHECK(w.operations().empty()); // nothing applied yet
    CHECK(w.find(QStringLiteral("templatesPreviewOp.2")) != nullptr);
    // The holes it would apply to: every hole, or one group of alike holes.
    const QVariantList targets = w.templates()->targets();
    REQUIRE(targets.size() == 4); // every hole, the tapped group, the counterbored group, the selection
    QString counterbored;
    bool tappedGroup = false;
    for (const QVariant& t : targets) {
        const QString text = t.toMap().value(QStringLiteral("text")).toString();
        tappedGroup = tappedGroup || text == QStringLiteral("4 x 1/4-20 UNC tapped, through");
        if (text == QStringLiteral("4 x 6.756 mm, through, counterbored 11.176 x 6.604"))
            counterbored = t.toMap().value(QStringLiteral("value")).toString();
    }
    CHECK(tappedGroup);
    REQUIRE_FALSE(counterbored.isEmpty());
    QQuickItem* apply = w.shown(QStringLiteral("templatesApply"));
    CHECK(apply->isEnabled());
    w.shot("preview");

    // ---- Apply (Ctrl+Enter): three ordinary operations, one undo step ----
    w.key(Qt::Key_Return, Qt::ControlModifier);
    REQUIRE(QTest::qWaitFor([&] { return w.operations().size() == 3 && !w.templates()->busy(); }, kTimeoutMs));
    CHECK(w.templates()->status().startsWith(QStringLiteral("Made 3 operations")));
    CHECK(w.document()->undoCommand()->name == "cam.batch");
    REQUIRE(QTest::qWaitFor([&] { return w.tree()->count() == 4; }, kTimeoutMs));
    CHECK(w.tree()->get(1).value(QStringLiteral("rowTemplate")).toString() == QStringLiteral("Spot drill"));
    CHECK(w.tree()->get(3).value(QStringLiteral("rowTemplate")).toString() == QStringLiteral("Tap"));
    w.shot("applied");

    // ---- A set that cannot be applied: no reamer of 0.266 in. The preview says which tool for which holes ----
    chooseWithKeys(w, QStringLiteral("reamed"), QStringLiteral("Reamed holes"));
    // The counterbored holes, chosen in the "Apply to" list with the keyboard.
    w.shown(QStringLiteral("templatesTarget"))->forceActiveFocus();
    for (int i = 0; i < 6 && w.templates()->target() != counterbored; ++i)
        w.key(Qt::Key_Down);
    REQUIRE(w.templates()->target() == counterbored);
    REQUIRE(QTest::qWaitFor([&] { return previewDone(w); }, kTimeoutMs));
    CHECK(w.templates()->previewState() == QStringLiteral("refused"));
    CHECK(w.templates()->previewMessage().contains(QStringLiteral("No tool in the libraries for 'Ream' (reamer")));
    CHECK(w.templates()->previewMessage().contains(QStringLiteral("4 holes")));
    CHECK_FALSE(w.shown(QStringLiteral("templatesApply"))->isEnabled());
    CHECK_FALSE(w.templates()->apply());
    w.shot("refused");

    // ---- Esc closes; the tree shows where each operation came from ----
    w.key(Qt::Key_Escape);
    REQUIRE(QTest::qWaitFor([&] { return !w.templates()->shown(); }, 5000));
    QQuickItem* details = w.shown(QStringLiteral("camRowDetails.2"));
    CHECK(details->property("text").toString() == QStringLiteral("From template Tap drill, chip break"));
    QQuickItem* tree = w.shown(QStringLiteral("camTree"));
    w.sendMouse(QEvent::MouseMove, sceneRect(w.shown(QStringLiteral("operationRow.3"))).center(), Qt::NoButton,
                Qt::NoButton);
    QTest::qWait(100);
    CHECK(checkIconButtons(tree, QStringLiteral("CAM tree, an operation row hovered")) >= 3);
    w.shot("tree");

    // ---- Save the tap as a template, from its row ----
    w.clickRowTool(QStringLiteral("operationRow.3"), QStringLiteral("camRowSaveTemplate.3"));
    REQUIRE(QTest::qWaitFor([&] { return w.templates()->shown() && w.templates()->mode() == "save"; }, 5000));
    QQuickItem* name = w.shown(QStringLiteral("templatesSaveName"));
    name->forceActiveFocus();
    QMetaObject::invokeMethod(name, "selectAll");
    w.key(Qt::Key_Delete);
    w.type(QStringLiteral("Shop tap"));
    CHECK(w.templates()->saveValues().value(QStringLiteral("name")).toString() == QStringLiteral("Shop tap"));
    CHECK(w.templates()->saveValues().value(QStringLiteral("feeds")).toString() == QStringLiteral("calculated"));
    CHECK(checkIconButtons(w.shown(QStringLiteral("templatesFooter"))->parentItem(),
                           QStringLiteral("templates, saving")) >= 1);
    w.shot("save");
    w.click(QStringLiteral("templatesSave"));
    REQUIRE(QTest::qWaitFor([&] { return w.templates()->mode() == "apply" && !w.templates()->busy(); }, kTimeoutMs));
    INFO(w.templates()->status().toStdString());
    CHECK(w.templates()->status().startsWith(QStringLiteral("Saved 'Shop tap'")));
    const QString saved = w.templates()->selected();
    REQUIRE(w.host->app()->libraries()->opTemplate(Uuid::fromString(saved.toStdString())) != nullptr);
    CHECK(w.templates()->selectedInfo().value(QStringLiteral("canRename")).toBool());
    w.shot("saved");

    // ---- Rename it (F2 in the list), then delete it (with a question) ----
    QQuickItem* list = w.shown(QStringLiteral("templatesList"));
    list->forceActiveFocus();
    REQUIRE(QTest::qWaitFor([&] { return list->hasActiveFocus(); }, 5000));
    INFO("the focus: " << (w.window()->activeFocusItem() ? w.window()->activeFocusItem()->objectName().toStdString()
                                                         : std::string("none"))
                       << ", can rename: "
                       << w.templates()->selectedInfo().value(QStringLiteral("canRename")).toBool());
    w.key(Qt::Key_F2);
    QQuickItem* rename = w.shown(QStringLiteral("templatesRenameField"));
    REQUIRE(QTest::qWaitFor([&] { return rename->hasActiveFocus(); }, 5000));
    w.key(Qt::Key_Delete);
    w.type(QStringLiteral("Shop tap rigid"));
    w.key(Qt::Key_Return);
    REQUIRE(QTest::qWaitFor(
        [&] {
            const lib::OpTemplate* t = w.host->app()->libraries()->opTemplate(Uuid::fromString(saved.toStdString()));
            return t != nullptr && t->name == "Shop tap rigid";
        },
        kTimeoutMs));
    w.click(QStringLiteral("templatesDelete"));
    REQUIRE(QTest::qWaitFor([&] { return w.templates()->deleteAsked() == saved; }, 5000));
    w.shown(QStringLiteral("templatesDeleteQuestion"));
    w.shot("delete_question");
    w.click(QStringLiteral("templatesDeleteConfirm"));
    REQUIRE(QTest::qWaitFor(
        [&] { return w.host->app()->libraries()->opTemplate(Uuid::fromString(saved.toStdString())) == nullptr; },
        kTimeoutMs));
    w.key(Qt::Key_Escape);
    REQUIRE(QTest::qWaitFor([&] { return !w.templates()->shown(); }, 5000));

    // ---- Re-apply a template from an operation's row: one undo step ----
    const OperationId drill = w.operations()[1]->id;
    w.invoke("cam.operation.edit", R"({"id":")" + drill.value.toString() + R"(","cutting":{"rpm":1000}})");
    REQUIRE(w.operations()[1]->cutting.spindleRpm == 1000);
    w.clickRowTool(QStringLiteral("operationRow.2"), QStringLiteral("camRowReapply.2"));
    REQUIRE(QTest::qWaitFor([&] { return w.operations()[1]->cutting.spindleRpm != 1000; }, kTimeoutMs));
    CHECK(w.document()->undoCommand()->label.rfind("Re-apply Template", 0) == 0);
    REQUIRE(QTest::qWaitFor(
        [&] {
            return w.find(QStringLiteral("camTreeTemplatesMessage")) != nullptr &&
                   w.find(QStringLiteral("camTreeTemplatesMessage"))->isVisible();
        },
        5000));
    w.shot("reapplied");
    for (const QString& warning : w.host->warnings())
        UNSCOPED_INFO(warning.toStdString());
    CHECK(w.host->warnings().isEmpty());
}

int main(int argc, char** argv)
{
    QTemporaryDir profile;
    qputenv("RAYCAD_USER_DATA_DIR", QDir::toNativeSeparators(profile.path()).toUtf8());
    QGuiApplication app(argc, argv);
    QGuiApplication::setQuitOnLastWindowClosed(false);
    return Catch::Session().run(argc, argv);
}
