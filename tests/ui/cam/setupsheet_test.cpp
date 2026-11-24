// Setup sheets (task T-076) in the desktop app:
//
//   - the PDF and the preview from the sheet's HTML (camui::toPdf, toImages): on Letter and A4, the page count the
//     layout gives, the paper size, the text on each page read back out of the PDF (its fonts' ToUnicode maps), each
//     setup of a whole job starting on a page of its own, the embedded picture drawn, nothing loaded from a file
//   - the dialog on the real Qt Quick window with the real application session: opened from the CAM tree's setup row
//     beside Post, from the post dialog and from the command; the picture of the part on the machine (the view turns
//     and comes back), the preview, Save PDF and Save HTML with the default name from the program-file naming rules, an
//     existing file (keep both, replace), a folder that is not there (reported, nothing written), the gate (a stale
//     operation blocks the sheet; Generate all clears it), every setup in one document, Esc
//
// Screenshots go to build/shots/t076_*.png. With RAY_T076_SAMPLES set to a folder, the sample PDF and HTML saved by the
// flow are copied there for review.
#include "../scene_graph_probe.h"
#include "../window_focus.h"
#include "app_context.h"
#include "app_host.h"
#include "app_options.h"
#include "cam/post_tool.h"
#include "cam/setup_tree.h"
#include "cam/setupsheet_print.h"
#include "cam/setupsheet_tool.h"
#include "engine_test_util.h" // the CAM engine tests' test-only operation type (tests/unit/cam)
#include "files.h"
#include "palette_commands.h"
#include "scene_controller.h"
#include "session.h"
#include "theme_object.h"
#include "viewport_item.h"

#include <ray_api/cam_operation.h>
#include <ray_api/cam_setup.h>
#include <ray_api/cam_tools.h>

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
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
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace ray;
using Catch::Matchers::ContainsSubstring;
using ray::ui::AppHost;
using ray::ui::AppOptions;
using ray::ui::PostToolModel;
using ray::ui::SetupSheetModel;
using ray::ui::SetupTreeModel;
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
const std::string kVmc = "b903ede7-5123-4c26-bc15-466e92fba4c6";                // the shipped "Generic VMC"

QByteArray readFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.readAll();
}

// ---------------------------------------------------------------------------------------------------- PDF reading
// Just enough of PDF to read back what QPdfWriter writes: objects, Flate streams, the page tree, the fonts' ToUnicode
// maps and the glyphs shown between BT and ET. Text comes out without the spaces between words (only the glyphs);
// callers compare without whitespace.
struct PdfObject {
    QByteArray dict;   // up to "stream" or "endobj"
    QByteArray stream; // inflated when /FlateDecode
};

QByteArray inflate(const QByteArray& data)
{
    // qUncompress takes zlib data behind a big-endian size hint.
    const quint32 hint = static_cast<quint32>(std::max<qsizetype>(1 << 20, data.size() * 40));
    QByteArray prefixed(4, '\0');
    prefixed[0] = static_cast<char>((hint >> 24) & 0xff);
    prefixed[1] = static_cast<char>((hint >> 16) & 0xff);
    prefixed[2] = static_cast<char>((hint >> 8) & 0xff);
    prefixed[3] = static_cast<char>(hint & 0xff);
    return qUncompress(prefixed + data);
}

std::map<int, PdfObject> pdfObjects(const QByteArray& pdf)
{
    std::map<int, PdfObject> out;
    static const QRegularExpression head(QStringLiteral("(?:^|[\\r\\n])(\\d+) 0 obj"));
    const QString latin = QString::fromLatin1(pdf);
    auto it = head.globalMatch(latin);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const int number = m.captured(1).toInt();
        const qsizetype start = m.capturedEnd(0);
        const qsizetype end = pdf.indexOf("endobj", start);
        if (end < 0)
            break;
        const QByteArray body = pdf.mid(start, end - start);
        PdfObject o;
        const qsizetype s = body.indexOf("stream");
        if (s >= 0 && body.lastIndexOf("endstream") > s) {
            o.dict = body.left(s);
            qsizetype from = s + 6;
            if (from < body.size() && body[from] == '\r')
                ++from;
            if (from < body.size() && body[from] == '\n')
                ++from;
            const QByteArray raw = body.mid(from, body.lastIndexOf("endstream") - from);
            o.stream = o.dict.contains("/FlateDecode") ? inflate(raw) : raw;
        } else {
            o.dict = body;
        }
        out[number] = std::move(o);
    }
    return out;
}

int refAfter(const QByteArray& dict, const char* key)
{
    const QRegularExpression re(QStringLiteral("%1\\s+(\\d+)\\s+0\\s+R").arg(QLatin1String(key)));
    const QRegularExpressionMatch m = re.match(QString::fromLatin1(dict));
    return m.hasMatch() ? m.captured(1).toInt() : -1;
}

QString utf16Hex(const QString& hex)
{
    QString out;
    for (qsizetype i = 0; i + 4 <= hex.size(); i += 4)
        out += QChar(static_cast<char16_t>(hex.mid(i, 4).toUInt(nullptr, 16)));
    return out;
}

std::map<uint, QString> toUnicodeMap(const QByteArray& cmap)
{
    std::map<uint, QString> out;
    const QString text = QString::fromLatin1(cmap);
    static const QRegularExpression charBlock(QStringLiteral("beginbfchar(.*?)endbfchar"),
                                              QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression pair(QStringLiteral("<([0-9A-Fa-f]+)>\\s*<([0-9A-Fa-f]+)>"));
    for (auto b = charBlock.globalMatch(text); b.hasNext();) {
        const QString block = b.next().captured(1);
        for (auto p = pair.globalMatch(block); p.hasNext();) {
            const QRegularExpressionMatch m = p.next();
            out[m.captured(1).toUInt(nullptr, 16)] = utf16Hex(m.captured(2));
        }
    }
    static const QRegularExpression rangeBlock(QStringLiteral("beginbfrange(.*?)endbfrange"),
                                               QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression range(
        QStringLiteral("<([0-9A-Fa-f]+)>\\s*<([0-9A-Fa-f]+)>\\s*(<[0-9A-Fa-f]+>|\\[[^\\]]*\\])"));
    static const QRegularExpression hexItem(QStringLiteral("<([0-9A-Fa-f]+)>"));
    for (auto b = rangeBlock.globalMatch(text); b.hasNext();) {
        const QString block = b.next().captured(1);
        for (auto r = range.globalMatch(block); r.hasNext();) {
            const QRegularExpressionMatch m = r.next();
            const uint lo = m.captured(1).toUInt(nullptr, 16), hi = m.captured(2).toUInt(nullptr, 16);
            const QString dst = m.captured(3);
            if (dst.startsWith(QLatin1Char('<'))) {
                const QString base = utf16Hex(dst.mid(1, dst.size() - 2));
                for (uint c = lo; c <= hi && !base.isEmpty(); ++c) {
                    QString s = base;
                    s[s.size() - 1] = QChar(static_cast<char16_t>(base.back().unicode() + (c - lo)));
                    out[c] = s;
                }
            } else {
                uint c = lo;
                for (auto h = hexItem.globalMatch(dst); h.hasNext() && c <= hi; ++c)
                    out[c] = utf16Hex(h.next().captured(1));
            }
        }
    }
    return out;
}

struct Pdf {
    int pages = 0;
    std::vector<QString> text; // per page, the glyphs shown, in order (no spaces)
    std::vector<QString> mediaBoxes;
};

Pdf readPdf(const QByteArray& bytes)
{
    Pdf out;
    const std::map<int, PdfObject> objects = pdfObjects(bytes);
    // Fonts by resource name: /F<n> <obj> 0 R inside a /Font dictionary; each font's ToUnicode map.
    std::map<QString, std::map<uint, QString>> fonts;
    static const QRegularExpression fontDict(QStringLiteral("/Font\\s*<<(.*?)>>"),
                                             QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression fontRef(QStringLiteral("/(\\w+)\\s+(\\d+)\\s+0\\s+R"));
    for (const auto& [n, o] : objects) {
        for (auto f = fontDict.globalMatch(QString::fromLatin1(o.dict)); f.hasNext();) {
            const QString inner = f.next().captured(1);
            for (auto r = fontRef.globalMatch(inner); r.hasNext();) {
                const QRegularExpressionMatch m = r.next();
                const auto font = objects.find(m.captured(2).toInt());
                if (font == objects.end())
                    continue;
                const auto cmap = objects.find(refAfter(font->second.dict, "/ToUnicode"));
                if (cmap != objects.end())
                    fonts[m.captured(1)] = toUnicodeMap(cmap->second.stream);
            }
        }
    }
    // Pages in the order of the page tree's Kids.
    std::vector<int> pageObjects;
    static const QRegularExpression kids(QStringLiteral("/Kids\\s*\\[([^\\]]*)\\]"));
    static const QRegularExpression ref(QStringLiteral("(\\d+)\\s+0\\s+R"));
    for (const auto& [n, o] : objects) {
        if (!o.dict.contains("/Type /Pages"))
            continue;
        const QRegularExpressionMatch k = kids.match(QString::fromLatin1(o.dict));
        for (auto r = ref.globalMatch(k.captured(1)); r.hasNext();)
            pageObjects.push_back(r.next().captured(1).toInt());
    }
    out.pages = static_cast<int>(pageObjects.size());
    static const QRegularExpression media(QStringLiteral("/MediaBox\\s*\\[([^\\]]*)\\]"));
    static const QRegularExpression token(QStringLiteral("(BT|ET)\\b|/(\\w+)\\s+[-\\d.]+\\s+Tf|<([0-9A-Fa-f]*)>"));
    for (int pageNumber : pageObjects) {
        const PdfObject& page = objects.at(pageNumber);
        out.mediaBoxes.push_back(media.match(QString::fromLatin1(page.dict)).captured(1).simplified());
        QString text;
        const auto contents = objects.find(refAfter(page.dict, "/Contents"));
        if (contents != objects.end()) {
            bool inText = false;
            const std::map<uint, QString>* font = nullptr;
            for (auto t = token.globalMatch(QString::fromLatin1(contents->second.stream)); t.hasNext();) {
                const QRegularExpressionMatch m = t.next();
                if (m.captured(1) == QLatin1String("BT"))
                    inText = true;
                else if (m.captured(1) == QLatin1String("ET"))
                    inText = false;
                else if (!m.captured(2).isEmpty()) {
                    const auto f = fonts.find(m.captured(2));
                    font = f == fonts.end() ? nullptr : &f->second;
                } else if (inText && font != nullptr) {
                    const QString hex = m.captured(3);
                    for (qsizetype i = 0; i + 4 <= hex.size(); i += 4) {
                        const auto g = font->find(hex.mid(i, 4).toUInt(nullptr, 16));
                        if (g != font->end())
                            text += g->second;
                    }
                }
            }
        }
        text.remove(QRegularExpression(QStringLiteral("\\s")));
        out.text.push_back(text);
    }
    return out;
}

QString squeezed(QString s)
{
    s.remove(QRegularExpression(QStringLiteral("\\s")));
    return s;
}

bool pageHas(const Pdf& pdf, int page, const QString& what)
{
    return page < static_cast<int>(pdf.text.size()) &&
           pdf.text[static_cast<std::size_t>(page)].contains(squeezed(what));
}

// ---------------------------------------------------------------------------------------------------- the window
struct Window {
    QTemporaryDir cache;
    QTemporaryDir userData;
    // In the build tree, not the user's temporary folder: the folder shows in the screenshots.
    QTemporaryDir out{outTemplate()};
    static QString outTemplate()
    {
        QDir().mkpath(QStringLiteral(RAY_SHOTS_DIR));
        return QStringLiteral(RAY_SHOTS_DIR "/../t076_out_XXXXXX");
    }
    std::unique_ptr<AppHost> host;

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
        REQUIRE(ray::test::focusWindow(host->window()));
        REQUIRE(host->viewport() != nullptr);
        REQUIRE(host->startSession(userData.path().toStdString()));
        host->app()->setConfirmHook([](const api::ActionInfo&, std::string_view, std::string_view) { return true; });
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
    SetupSheetModel* sheet() const { return singleton<SetupSheetModel>("CamSheet"); }
    PostToolModel* post() const { return singleton<PostToolModel>("CamPost"); }
    SetupTreeModel* tree() const { return singleton<SetupTreeModel>("CamTreeModel"); }
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
    void click(QQuickItem* item) const
    {
        REQUIRE(item != nullptr);
        grab();
        clickScene(item->mapToScene(QPointF(item->width() / 2, item->height() / 2)));
    }
    void click(const QString& objectName) const { click(shown(objectName)); }
    void typeInto(const QString& objectName, const QString& text) const
    {
        QQuickItem* field = shown(objectName);
        if (QGuiApplication::focusWindow() != window())
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
        const bool ok = QTest::qWaitFor([&] { return host->session()->hasDocument() && settled(); }, kTimeoutMs);
        doc::Document* d = host->app()->active() ? host->app()->active()->document.get() : nullptr;
        INFO("document " << (d != nullptr) << ", rebuild pending " << (d != nullptr && d->rebuildPending())
                         << ", files busy " << host->files()->busy() << ", scene ready " << host->scene()->ready()
                         << ", meshing " << host->scene()->meshing() << ", animating " << view()->animating());
        REQUIRE(ok);
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
    // The sheet reaches `state` (the gate, the pictures, the HTML and the pages are asynchronous).
    void sheetState(const char* state) const
    {
        const bool ok = QTest::qWaitFor([&] { return sheet()->state() == QLatin1String(state); }, kTimeoutMs * 2);
        INFO("sheet state '" << sheet()->state().toStdString() << "', status '" << sheet()->statusText().toStdString()
                             << "'");
        for (const QVariant& m : sheet()->messages())
            UNSCOPED_INFO(m.toMap().value(QStringLiteral("text")).toString().toStdString());
        REQUIRE(ok);
    }
    void shot(const QString& name) const
    {
        const QString dir = QStringLiteral(RAY_SHOTS_DIR);
        QDir().mkpath(dir);
        sendMouse(QEvent::MouseMove, QPointF(window()->width() / 2.0, window()->height() - 4.0), Qt::NoButton,
                  Qt::NoButton);
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

// Setup n on the plate with the shipped Generic VMC (embedded from the library).
void addSetup(const Window& w, int n)
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

QJsonArray holeRefs(const Window& w, int setup, std::size_t count)
{
    JobContext ctx;
    auto holes =
        api::cam_operation::holesOf(w.document()->state(), w.document()->build(),
                                    SetupId{Uuid::fromString(ct::id('a', static_cast<std::uint64_t>(setup)))}, ctx);
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

// Drill operation c<n> ("Drill <n>") in setup a<setup> on the first `holes` holes with the 6 mm drill, peck drilling.
void addDrill(const Window& w, std::uint64_t n, int setup, std::size_t holes)
{
    const lib::Tool* t = w.host->app()->libraries()->tool(ToolId{Uuid::fromString(kDrill6.toStdString())});
    REQUIRE(t != nullptr);
    QJsonObject args{{QStringLiteral("id"), QString::fromStdString(ct::id('c', n))},
                     {QStringLiteral("setup"), QString::fromStdString(ct::id('a', static_cast<std::uint64_t>(setup)))},
                     {QStringLiteral("type"), QStringLiteral("drill")},
                     {QStringLiteral("name"), QStringLiteral("Drill %1").arg(n)},
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
                     {QStringLiteral("geometry"), holeRefs(w, setup, holes)},
                     {QStringLiteral("params"), QJsonObject{{QStringLiteral("cycle"), QStringLiteral("peck")}}}};
    args.insert(
        QStringLiteral("library"),
        QJsonObject{
            {QStringLiteral("tools"),
             QJsonArray{QJsonDocument::fromJson(QByteArray::fromStdString(api::cam_tools::toolJson(*t))).object()}}});
    (void)w.invokeResult("cam.operation.add", jsonText(args));
}

void selectSetupRow(const Window& w)
{
    QQuickItem* row = w.shown(QStringLiteral("setupRow.0"));
    w.grab();
    w.clickScene(row->mapToScene(QPointF(row->width() * 0.22, std::min(12.0, row->height() / 2))));
    REQUIRE(QTest::qWaitFor([&] { return w.tree()->currentIndex() == 0; }, 5000));
}

bool sameCamera(const render::Camera& a, const render::Camera& b)
{
    const auto near = [](Vec3 p, Vec3 q) {
        return std::abs(p.x - q.x) < 1e-6 && std::abs(p.y - q.y) < 1e-6 && std::abs(p.z - q.z) < 1e-6;
    };
    return near(a.eye, b.eye) && near(a.target, b.target) && near(a.up, b.up) && a.perspective == b.perspective &&
           std::abs(a.orthoHeight - b.orthoHeight) < 1e-6;
}

QString goldenSheet(const char* name)
{
    return QString::fromUtf8(readFile(QStringLiteral(RAY_GOLDEN_SETUPSHEET_DIR "/") + QLatin1String(name)));
}

} // namespace

// ====================================================================================================== on paper
TEST_CASE("the PDF of a setup sheet: pages, paper, text read back, each setup on its own pages", "[sheet][pdf]")
{
    const QString job = goldenSheet("job_inch.html"); // post::renderSetupSheetsHtml's own output: two setups
    REQUIRE_FALSE(job.isEmpty());
    for (const camui::Paper paper : {camui::Paper::Letter, camui::Paper::A4}) {
        INFO("paper " << camui::paperKey(paper).toStdString());
        QString error;
        const QByteArray bytes = camui::toPdf(job, paper, QStringLiteral("bracket: setup sheets"), &error);
        INFO(error.toStdString());
        REQUIRE(bytes.startsWith("%PDF-"));
        const Pdf pdf = readPdf(bytes);
        const int pages = camui::pageCount(job, paper);
        CHECK(pages >= 2);
        CHECK(pdf.pages == pages);
        REQUIRE(pdf.text.size() == static_cast<std::size_t>(pages));
        for (const QString& box : pdf.mediaBoxes) {
            INFO("MediaBox " << box.toStdString());
            const QStringList v = box.split(QLatin1Char(' '));
            REQUIRE(v.size() == 4);
            // Points: Letter 612 x 792, A4 595.28 x 841.89 (QPageSize rounds A4 to whole points).
            CHECK(std::lround(v[2].toDouble()) == (paper == camui::Paper::Letter ? 612 : 595));
            CHECK(std::lround(v[3].toDouble()) == (paper == camui::Paper::Letter ? 792 : 842));
        }
        // The text is there, black text in the PDF's fonts: the first setup's title on page 1, the second setup's on a
        // later page that starts with it and holds nothing of the first.
        CHECK(pageHas(pdf, 0, QStringLiteral("Setup sheet: Op 1 - top")));
        CHECK(pageHas(pdf, 0, QStringLiteral("This post has not been proven on a real machine")));
        CHECK(pageHas(pdf, 0, QStringLiteral("Top front left corner of the stock")));
        int second = -1;
        for (int p = 0; p < pages; ++p) {
            if (pageHas(pdf, p, QStringLiteral("Setup sheet: Op 2 - bottom")))
                second = p;
        }
        REQUIRE(second > 0);
        CHECK_FALSE(pageHas(pdf, second, QStringLiteral("Setup sheet: Op 1 - top")));
        CHECK(pdf.text[static_cast<std::size_t>(second)].indexOf(
                  squeezed(QStringLiteral("Setup sheet: Op 2 - bottom"))) == 0);
        CHECK(pageHas(pdf, second, QStringLiteral("Round bar")));
        CHECK(pageHas(pdf, second, QStringLiteral("G55")));
        // Every page carries its footer.
        for (int p = 0; p < pages; ++p)
            CHECK(pageHas(pdf, p, QStringLiteral("Page %1 of %2").arg(p + 1).arg(pages)));
        bool estimate = false;
        for (int p = 0; p < pages; ++p)
            estimate = estimate || pageHas(pdf, p, QStringLiteral("Estimated cycle time"));
        CHECK(estimate);
        // The preview is the same pages.
        const std::vector<QImage> images = camui::toImages(job, paper, QStringLiteral("bracket"), 96);
        CHECK(static_cast<int>(images.size()) == pages);
    }
    // Letter is the default where the locale measures in US units, A4 elsewhere.
    CHECK(camui::defaultPaper(QLocale(QLocale::English, QLocale::UnitedStates)) == camui::Paper::Letter);
    CHECK(camui::defaultPaper(QLocale(QLocale::German, QLocale::Germany)) == camui::Paper::A4);
    CHECK(camui::defaultPaper(QLocale(QLocale::English, QLocale::UnitedKingdom)) == camui::Paper::A4);
}

TEST_CASE("the preview pages show the embedded picture and load nothing from a file", "[sheet][pdf]")
{
    // A picture of one colour, embedded as the sheet embeds it, and the same picture named by a file URL.
    QTemporaryDir dir;
    QImage red(30, 20, QImage::Format_RGB32);
    red.fill(QColor(Qt::red));
    const QString file = dir.filePath(QStringLiteral("red.png"));
    REQUIRE(red.save(file, "PNG"));
    const QByteArray png = readFile(file);
    const auto redPixels = [](const QImage& page) {
        int n = 0;
        for (int y = 0; y < page.height(); y += 2) {
            for (int x = 0; x < page.width(); x += 2) {
                const QColor c = page.pixelColor(x, y);
                n += c.red() > 200 && c.green() < 60 && c.blue() < 60 ? 1 : 0;
            }
        }
        return n;
    };
    const QString embedded = QStringLiteral("<html><body><p>Picture</p><img src=\"data:image/png;base64,%1\" "
                                            "width=\"300\" height=\"200\"></body></html>")
                                 .arg(QString::fromLatin1(png.toBase64()));
    const std::vector<QImage> shown = camui::toImages(embedded, camui::Paper::A4, QString(), 96);
    REQUIRE(shown.size() == 1);
    CHECK(redPixels(shown.front()) > 1000);
    const QString linked = QStringLiteral("<html><body><p>Picture</p><img src=\"%1\" width=\"300\" height=\"200\">"
                                          "</body></html>")
                               .arg(QUrl::fromLocalFile(file).toString());
    const std::vector<QImage> notShown = camui::toImages(linked, camui::Paper::A4, QString(), 96);
    REQUIRE(notShown.size() == 1);
    CHECK(redPixels(notShown.front()) == 0);
    // Black on white: the page is white paper wherever nothing is printed.
    CHECK(shown.front().pixelColor(2, 2) == QColor(Qt::white));
}

// ===================================================================================================== the dialog
TEST_CASE("a setup sheet from the CAM tree: picture, preview, PDF and HTML, existing files, errors",
          "[cam][sheet][e2e]")
{
    RAY_REQUIRE_RHI();
    Window w;
    w.open(kPlate);
    w.toWorkspace(ui::kCamWorkspace);
    addSetup(w, 1);
    addDrill(w, 1, 1, 6);
    w.generate(1);
    selectSetupRow(w);
    const render::Camera before = w.view()->camera();

    // Opened from the setup row, beside Post.
    w.click(w.shown(QStringLiteral("camRowSheet.0")));
    SetupSheetModel* s = w.sheet();
    REQUIRE(QTest::qWaitFor([&] { return s->active(); }, 5000));
    w.shown(QStringLiteral("sheetDialog"));
    w.sheetState("ready");
    // The picture of the part on the machine, and the view turned back as it was.
    const SetupId setup{Uuid::fromString(ct::id('a', 1))};
    REQUIRE(s->pictures().count(setup) == 1);
    // In the print palette whatever the app's theme: white paper at the corners, dark edges, the part, its stock
    // outline and the axes drawn (not one flat colour). Taken in the dark theme, then again in the light one.
    const auto checkPicture = [&](const char* theme) {
        INFO("app theme " << theme);
        const std::vector<std::byte>& png = s->pictures().at(setup);
        QImage picture;
        REQUIRE(picture.loadFromData(reinterpret_cast<const uchar*>(png.data()), static_cast<int>(png.size()), "PNG"));
        CHECK(picture.size() == QSize(1296, 864));
        for (const QPoint corner : {QPoint(0, 0), QPoint(picture.width() - 1, 0), QPoint(0, picture.height() - 1),
                                    QPoint(picture.width() - 1, picture.height() - 1)}) {
            const QColor c = picture.pixelColor(corner);
            INFO("corner " << corner.x() << "," << corner.y() << ": " << c.name().toStdString());
            CHECK(std::min({c.red(), c.green(), c.blue()}) >= 250);
        }
        std::set<QRgb> colours;
        int dark = 0;
        for (int y = 0; y < picture.height(); y += 4) {
            for (int x = 0; x < picture.width(); x += 4) {
                const QColor c = picture.pixelColor(x, y);
                colours.insert(c.rgb());
                dark += std::max({c.red(), c.green(), c.blue()}) < 90 ? 1 : 0;
            }
        }
        CHECK(colours.size() > 20);
        CHECK(dark > 50);
    };
    checkPicture("dark");
    w.host->theme()->setProperty("mode", QStringLiteral("light"));
    s->retakePictures();
    w.sheetState("ready");
    checkPicture("light");
    w.host->theme()->setProperty("mode", QStringLiteral("dark"));
    CHECK(sameCamera(w.view()->camera(), before));
    // The sheet: this setup's program, embedded picture, the drill and its holes, as the post would write it.
    const std::string& html = s->html();
    CHECK_THAT(html, ContainsSubstring("Setup sheet: Op 1 - top"));
    CHECK_THAT(html, ContainsSubstring("Program <b>1001</b>"));
    CHECK_THAT(html, ContainsSubstring("<img src=\"data:image/png;base64,"));
    CHECK_THAT(html, ContainsSubstring("Peck drilling (full retract), 6 holes"));
    CHECK_THAT(html, ContainsSubstring("This post has not been proven on a real machine"));
    CHECK_THAT(html, ContainsSubstring("Estimated cycle time"));
    CHECK_THAT(html, ContainsSubstring("plate_thru_holes_1"));
    // The preview: its pages in the dialog.
    REQUIRE(s->pageCount() >= 1);
    QQuickItem* page0 = w.shown(QStringLiteral("sheetPage.0"));
    REQUIRE(QTest::qWaitFor([&] { return page0->property("status").toInt() == 1; }, 10000)); // Image.Ready
    CHECK(page0->height() > page0->width());
    // The default name: the program file's (pattern {doc}_{setup}).
    CHECK(s->pdfName() == QStringLiteral("plate_thru_holes_1_Op_1_-_top.pdf"));
    CHECK(s->htmlName() == QStringLiteral("plate_thru_holes_1_Op_1_-_top.html"));
    CHECK_FALSE(s->canSave());
    CHECK(s->saveBlockedReason() == QStringLiteral("Choose the folder to save the setup sheet in"));
    w.shotsBothThemes(QStringLiteral("t076_sheet_dialog"));

    // Save PDF into a folder typed in, by the keyboard (Enter in a field saves the PDF).
    const QString dir = QDir::toNativeSeparators(w.out.path());
    w.typeInto(QStringLiteral("sheetInput.directory"), dir);
    REQUIRE(QTest::qWaitFor([&] { return s->canSave(); }, 5000));
    QTest::keyClick(w.window(), Qt::Key_Return);
    w.sheetState("done");
    const QString pdfPath = QDir(w.out.path()).filePath(s->pdfName());
    CHECK(QDir::toNativeSeparators(s->resultPath()) == QDir::toNativeSeparators(pdfPath));
    const QByteArray pdfBytes = readFile(pdfPath);
    REQUIRE(pdfBytes.startsWith("%PDF-"));
    const Pdf pdf = readPdf(pdfBytes);
    CHECK(pdf.pages == s->pageCount());
    CHECK(pageHas(pdf, 0, QStringLiteral("Setup sheet: Op 1 - top")));
    CHECK(pageHas(pdf, 0, QStringLiteral("Drill 1")));
    w.shot(QStringLiteral("t076_sheet_saved"));
    // Save HTML: the sheet itself, byte for byte.
    w.click(QStringLiteral("sheetSaveHtml"));
    w.sheetState("done");
    const QString htmlPath = QDir(w.out.path()).filePath(s->htmlName());
    CHECK(readFile(htmlPath) == QByteArray::fromStdString(s->html()));
    if (const QString samples = qEnvironmentVariable("RAY_T076_SAMPLES"); !samples.isEmpty()) {
        QDir().mkpath(samples);
        QFile::remove(QDir(samples).filePath(QStringLiteral("sample_setup_sheet.pdf")));
        QFile::remove(QDir(samples).filePath(QStringLiteral("sample_setup_sheet.html")));
        CHECK(QFile::copy(pdfPath, QDir(samples).filePath(QStringLiteral("sample_setup_sheet.pdf"))));
        CHECK(QFile::copy(htmlPath, QDir(samples).filePath(QStringLiteral("sample_setup_sheet.html"))));
    }

    // The PDF again: it exists, so the dialog asks. Keep both writes the next numbered name; Replace replaces.
    w.click(QStringLiteral("sheetSavePdf"));
    w.sheetState("asking");
    CHECK(s->existingName() == s->pdfName());
    w.shown(QStringLiteral("sheetReplace"));
    w.click(QStringLiteral("sheetNewName"));
    w.sheetState("done");
    CHECK(QFileInfo::exists(QDir(w.out.path()).filePath(QStringLiteral("plate_thru_holes_1_Op_1_-_top_2.pdf"))));
    w.click(QStringLiteral("sheetSavePdf"));
    w.sheetState("asking");
    w.click(QStringLiteral("sheetReplace"));
    w.sheetState("done");
    CHECK(s->statusText().contains(QStringLiteral("replacing")));
    w.click(QStringLiteral("sheetSavePdf"));
    w.sheetState("asking");
    QTest::keyClick(w.window(), Qt::Key_Escape); // Esc answers "keep the old file"
    w.sheetState("cancelled");
    CHECK(QDir(w.out.path()).entryList(QDir::Files).size() == 3);

    // A folder that is not there: reported in full, as the post dialog does; nothing written anywhere.
    const QString missing = QDir::toNativeSeparators(w.out.filePath(QStringLiteral("no such folder")));
    s->setValue(QStringLiteral("directory"), missing);
    w.click(QStringLiteral("sheetSavePdf"));
    w.sheetState("failed");
    REQUIRE_FALSE(s->messages().isEmpty());
    const QString error = s->messages().front().toMap().value(QStringLiteral("text")).toString();
    CHECK(error.contains(QStringLiteral("does not exist")));
    CHECK_FALSE(error.contains(QStringLiteral("program")));
    CHECK(s->statusText().startsWith(QStringLiteral("Nothing was written")));
    CHECK(w.shown(QStringLiteral("sheetMessageText.0"))->property("text").toString() == error);
    CHECK_FALSE(QFileInfo::exists(missing));
    w.shotsBothThemes(QStringLiteral("t076_sheet_error"));
    CHECK(s->saves() == 4);

    // Esc closes the dialog.
    QTest::keyClick(w.window(), Qt::Key_Escape);
    REQUIRE(QTest::qWaitFor([&] { return !s->active(); }, 5000));
    w.checkNoWarnings();
}

TEST_CASE("the setup sheet is blocked like posting; every setup in one document; from the post dialog and the command",
          "[cam][sheet][e2e]")
{
    RAY_REQUIRE_RHI();
    Window w;
    w.open(kPlate);
    w.toWorkspace(ui::kCamWorkspace);
    addSetup(w, 1);
    addDrill(w, 1, 1, 3);
    w.generate(1);
    addSetup(w, 2);
    (void)w.invokeResult("cam.setup.edit", R"({"id":")" + ct::id('a', 2) + R"(","name":"Op 2 - <b>other</b> side"})");
    addDrill(w, 2, 2, 2);
    w.generate(2);
    selectSetupRow(w);

    // From the command (Ctrl+Shift+P's command) on the tree's current setup.
    REQUIRE(w.host->commands()->trigger(QStringLiteral("cam.setupSheet.open")));
    SetupSheetModel* s = w.sheet();
    REQUIRE(QTest::qWaitFor([&] { return s->active(); }, 5000));
    w.sheetState("ready");

    // A stale operation blocks the sheet, named with its reason; Generate all clears it.
    (void)w.invokeResult("cam.operation.edit", R"({"id":")" + ct::id('c', 1) + R"(","cutting":{"rpm":1400}})");
    w.sheetState("blocked");
    CHECK(s->html().empty());
    CHECK(s->pageCount() == 0);
    CHECK_FALSE(s->canSave());
    REQUIRE(s->blockers().size() == 1);
    CHECK(s->blockers().front().toMap().value(QStringLiteral("name")).toString() == QStringLiteral("Drill 1"));
    CHECK(s->blockers().front().toMap().value(QStringLiteral("reasonText")).toString() == QStringLiteral("Stale"));
    w.shown(QStringLiteral("sheetBlocker.0"));
    w.shot(QStringLiteral("t076_sheet_blocked"));
    w.click(QStringLiteral("sheetGenerateAll"));
    w.sheetState("ready");
    CHECK_THAT(s->html(), ContainsSubstring("1400 rpm"));

    // Every setup in one document: each on its own pages, the second setup's name escaped, its picture taken.
    s->setChoice(QStringLiteral("all"));
    w.sheetState("ready");
    CHECK(s->chosenSetups().size() == 2);
    CHECK(s->pictures().size() == 2);
    CHECK(s->pdfName() == QStringLiteral("plate_thru_holes_1_setups.pdf"));
    CHECK_THAT(s->html(), ContainsSubstring("Setup sheet: Op 1 - top"));
    CHECK_THAT(s->html(), ContainsSubstring("Setup sheet: Op 2 - &lt;b&gt;other&lt;/b&gt; side"));
    CHECK_THAT(s->html(), ContainsSubstring("page-break-before: always"));
    CHECK(s->pageCount() >= 2);
    const QByteArray pdfBytes =
        camui::toPdf(QString::fromStdString(s->html()), camui::paperFromKey(s->paper()), QStringLiteral("all"));
    const Pdf pdf = readPdf(pdfBytes);
    CHECK(pdf.pages == s->pageCount());
    w.shot(QStringLiteral("t076_sheet_all_setups"));
    if (const QString samples = qEnvironmentVariable("RAY_T076_SAMPLES"); !samples.isEmpty()) {
        QDir().mkpath(samples);
        QFile pdfFile(QDir(samples).filePath(QStringLiteral("sample_all_setups.pdf")));
        CHECK(pdfFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
        pdfFile.write(pdfBytes);
        QFile htmlFile(QDir(samples).filePath(QStringLiteral("sample_all_setups.html")));
        CHECK(htmlFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
        htmlFile.write(QByteArray::fromStdString(s->html()));
    }
    // The paper: the preview is laid out again.
    const int revision = s->revision();
    s->setPaper(s->paper() == QLatin1String("a4") ? QStringLiteral("letter") : QStringLiteral("a4"));
    REQUIRE(
        QTest::qWaitFor([&] { return s->revision() != revision && s->state() == QLatin1String("ready"); }, kTimeoutMs));
    s->close();

    // From the post dialog: its Setup sheet button closes it and opens the sheet of the same setup.
    REQUIRE(w.post()->open(QString::fromStdString(ct::id('a', 2))));
    w.click(QStringLiteral("postSetupSheet"));
    REQUIRE(QTest::qWaitFor([&] { return s->active() && !w.post()->active(); }, 5000));
    CHECK(s->choice() == QString::fromStdString(ct::id('a', 2)));
    w.sheetState("ready");
    s->close();
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
