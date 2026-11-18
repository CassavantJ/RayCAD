// Section view and draft analysis in the application (task T-023), in a real Qt Quick window whose viewport renders
// (Direct3D 11 on Windows by default):
//   - the commands view.section.toggle, view.section.fromFace, view.draft.toggle and view.draft.fromFace are in the
//     catalog, found by the palette's search and offered by the context menu (the face ones for a selected face)
//   - the Section panel opens with the section, drives the viewport's plane, and dragging its offset slider redraws
//     without uploading a mesh again; an idle window draws nothing
//   - the cap checkbox is disabled with a reason on a tier whose preset has no section caps, and the renderer then
//     caps nothing; section and draft analysis survive theme and tier switches
//   - "from the selected planar face" orients the plane and the pull direction along the face's normal
//   - with glass panels on, a section change blurs the backdrop again
// With RAY_SECTION_SCREENSHOT_DIR set, windows are saved there (dark and light: cap on, cap off, draft analysis) for
// review; RAY_SECTION_SCREENSHOT_PART names the part shown in them (default: the kernel tests' box_hole.step).
#include "app_context.h"
#include "app_host.h"
#include "app_options.h"
#include "files.h"
#include "palette_commands.h"
#include "scene_controller.h"
#include "section_model.h"
#include "selection_model.h"
#include "theme_object.h"
#include "viewport_item.h"

#include <ray_render/viewport.h>

#include <QtCore/QDir>
#include <QtCore/QTemporaryDir>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>
#include <QtGui/QMouseEvent>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>
#include <QtQuick/QSGRendererInterface>
#include <QtTest/QTest>

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

using namespace ray;
using ray::ui::AppHost;
using ray::ui::AppOptions;
using ray::ui::SectionModel;
using ray::ui::ViewportItem;

namespace {

const QString kBoxHole = QStringLiteral(RAY_KERNEL_TEST_DATA_DIR "/box_hole.step");
constexpr int kOpenTimeoutMs = 120000;

std::mutex g_warningsMutex;
QStringList g_warnings;
QtMessageHandler g_previous = nullptr;
void recordMessage(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    if (type == QtWarningMsg || type == QtCriticalMsg) {
        std::lock_guard lock(g_warningsMutex);
        g_warnings << message;
    }
    if (g_previous)
        g_previous(type, context, message);
}
QStringList takeWarnings()
{
    std::lock_guard lock(g_warningsMutex);
    return std::exchange(g_warnings, {});
}

bool rhiSceneGraph()
{
    static const bool rhi = [] {
        QQuickWindow probe;
        probe.resize(64, 64);
        probe.show();
        if (!QTest::qWaitForWindowExposed(&probe))
            return false;
        (void)probe.grabWindow();
        const QSGRendererInterface* renderer = probe.rendererInterface();
        return renderer != nullptr && QSGRendererInterface::isApiRhiBased(renderer->graphicsApi());
    }();
    return rhi;
}

#define REQUIRE_RHI()                                                                                                  \
    do {                                                                                                               \
        if (!rhiSceneGraph())                                                                                          \
            SKIP("the scene graph has no QRhi (software backend): the viewport cannot render here");                   \
    } while (false)

QQuickItem* findItem(QQuickItem* item, const char* objectName)
{
    if (item->objectName() == QLatin1String(objectName))
        return item;
    for (QQuickItem* child : item->childItems()) {
        if (QQuickItem* found = findItem(child, objectName))
            return found;
    }
    return nullptr;
}

QStringList ids(const QVariantList& entries)
{
    QStringList out;
    for (const QVariant& v : entries) {
        const QVariantMap m = v.toMap();
        if (m.value(QStringLiteral("kind")).toString() == QLatin1String("menu"))
            out << ids(m.value(QStringLiteral("items")).toList());
        else
            out << m.value(QStringLiteral("id")).toString();
    }
    return out;
}

struct Window {
    QTemporaryDir cache;
    QTemporaryDir userData;
    std::unique_ptr<AppHost> host;

    explicit Window(const char* theme, bool glass, render::Tier tier = render::Tier::Medium)
    {
        AppOptions options;
        options.theme = QString::fromLatin1(theme);
        options.glass = glass;
        options.size = QSize(1440, 900);
        host = std::make_unique<AppHost>(options, cache.path());
        REQUIRE(host->optionError().isEmpty());
        REQUIRE(host->load());
        host->show();
        REQUIRE(QTest::qWaitForWindowExposed(host->window()));
        REQUIRE(view() != nullptr);
        REQUIRE(host->startSession(userData.path().toStdString()));
        host->files()->setUseNativeDialogs(false);
        view()->setAnimationsEnabled(false);
        host->scene()->setPreset(render::presetFor(tier));
        grab();
    }

    QQuickWindow* window() const { return host->window(); }
    ViewportItem* view() const { return host->viewport(); }
    SectionModel* section() const { return host->section(); }
    ui::CommandCatalog* commands() const { return host->commands(); }
    QImage grab() const { return host->window()->grabWindow(); }
    QQuickItem* find(const char* name) const { return findItem(window()->contentItem(), name); }
    render::ViewportInfo info() const { return view()->viewport()->info(); }

    void open(const QString& path)
    {
        REQUIRE(host->files()->open(path));
        REQUIRE(QTest::qWaitFor(
            [&] {
                return !host->files()->busy() && host->scene()->ready() && !host->scene()->refining() &&
                       !view()->animating();
            },
            kOpenTimeoutMs));
        grab();
    }

    // Waits until a frame was drawn after the call (the window renders on demand).
    void settle()
    {
        const std::uint64_t before = info().framesRendered;
        static_cast<void>(QTest::qWaitFor([&] { return info().framesRendered > before; }, 2000));
        grab();
    }

    void save(const QString& name) const
    {
        const QByteArray dir = qgetenv("RAY_SECTION_SCREENSHOT_DIR");
        if (dir.isEmpty())
            return;
        QDir().mkpath(QString::fromLocal8Bit(dir));
        CHECK(grab().save(QDir(QString::fromLocal8Bit(dir)).filePath(name + QStringLiteral(".png"))));
    }
};

// A planar face of body 0 whose outward normal is closest to `towards`, as a selection hit.
std::optional<render::PickHit> planarFaceHit(const Window& w, Vec3 towards)
{
    const auto mesh = w.host->scene()->meshOf(0);
    if (!mesh)
        return std::nullopt;
    std::optional<render::PickHit> best;
    double bestDot = 0.99;
    for (const kernel::FaceRange& f : mesh->faces) {
        bool planar = f.indexCount > 0;
        Vec3 n{};
        for (std::uint32_t i = 0; i < f.indexCount && planar; ++i) {
            const std::uint32_t v = mesh->indices[f.firstIndex + i];
            const Vec3 vn{mesh->normals[v * 3], mesh->normals[v * 3 + 1], mesh->normals[v * 3 + 2]};
            if (i == 0)
                n = vn;
            planar = std::abs(vn.x - n.x) < 1e-4 && std::abs(vn.y - n.y) < 1e-4 && std::abs(vn.z - n.z) < 1e-4;
        }
        const double d = n.x * towards.x + n.y * towards.y + n.z * towards.z;
        if (planar && d > bestDot) {
            const std::uint32_t v = mesh->indices[f.firstIndex];
            bestDot = d;
            best = render::PickHit{w.host->scene()->itemForBody(0), kernel::SubKind::Face, f.face.index,
                                   Vec3{mesh->positions[v * 3], mesh->positions[v * 3 + 1], mesh->positions[v * 3 + 2]},
                                   0};
        }
    }
    return best;
}

} // namespace

TEST_CASE("section and draft commands are in the catalog, the palette and the context menu", "[ui][section]")
{
    REQUIRE_RHI();
    Window w("dark", false);
    QStringList all;
    for (const ui::Command& c : w.commands()->commands())
        all << c.id;
    for (const char* id : {"view.section.toggle", "view.section.fromFace", "view.draft.toggle", "view.draft.fromFace"})
        CHECK(all.contains(QLatin1String(id)));
    // Nothing shown yet: they say why.
    CHECK_FALSE(w.commands()->info(QStringLiteral("view.section.toggle")).value(QStringLiteral("enabled")).toBool());

    w.open(kBoxHole);
    const auto found = w.commands()->search(QStringLiteral("section"));
    REQUIRE_FALSE(found.empty());
    CHECK(std::any_of(found.begin(), found.end(), [](const ui::Command& c) { return c.id == "view.section.toggle"; }));
    const auto draft = w.commands()->search(QStringLiteral("draft"));
    CHECK(std::any_of(draft.begin(), draft.end(), [](const ui::Command& c) { return c.id == "view.draft.toggle"; }));

    const QStringList menu = ids(w.commands()->contextEntries());
    CHECK(menu.contains(QStringLiteral("view.section.toggle")));
    CHECK(menu.contains(QStringLiteral("view.draft.toggle")));
    CHECK(w.commands()->info(QStringLiteral("view.section.toggle")).value(QStringLiteral("enabled")).toBool());
    CHECK(w.commands()->info(QStringLiteral("view.section.fromFace")).value(QStringLiteral("reason")).toString() ==
          w.section()->faceReason());

    // A planar face selected: the face entries are offered and enabled.
    const auto top = planarFaceHit(w, Vec3{0, 0, 1});
    REQUIRE(top.has_value());
    w.host->selection()->applyClick(*top, Qt::NoModifier);
    REQUIRE(QTest::qWaitFor([&] { return !w.host->selection()->pending(); }, 10000));
    const QStringList faceMenu = ids(w.commands()->contextEntries());
    CHECK(faceMenu.contains(QStringLiteral("view.section.fromFace")));
    CHECK(faceMenu.contains(QStringLiteral("view.draft.fromFace")));
    CHECK(w.section()->faceAvailable());
    CHECK(w.commands()->info(QStringLiteral("view.section.fromFace")).value(QStringLiteral("enabled")).toBool());
    CHECK(w.host->warnings().isEmpty());
}

TEST_CASE("the section panel drives the viewport's plane; dragging redraws without re-meshing", "[ui][section]")
{
    REQUIRE_RHI();
    Window w("dark", false);
    w.open(kBoxHole);
    SectionModel* s = w.section();
    QQuickItem* panel = w.find("sectionPanel");
    REQUIRE(panel != nullptr);
    CHECK_FALSE(panel->isVisible());

    REQUIRE(w.commands()->run(QStringLiteral("view.section.toggle")));
    CHECK(s->active());
    CHECK(s->panelOpen());
    REQUIRE(QTest::qWaitFor([&] { return panel->isVisible(); }, 2000));
    CHECK(w.commands()->info(QStringLiteral("view.section.toggle")).value(QStringLiteral("marked")).toBool());
    w.settle();
    const auto plane = w.view()->viewport()->section();
    REQUIRE(plane.has_value());
    CHECK(plane->normal.z == 1.0);
    const auto box = w.view()->viewport()->sceneBounds();
    REQUIRE(box.has_value());
    CHECK(std::abs(s->offsetMin() - box->min.z) < 1e-9);
    CHECK(std::abs(s->offsetMax() - box->max.z) < 1e-9);
    CHECK(std::abs(plane->origin.z - 0.5 * (box->min.z + box->max.z)) < 1e-9); // the middle at first
    CHECK(w.info().sectionCaps == 1); // the part is a closed solid and Medium has caps

    // Drag the slider's handle across its groove: every move redraws, nothing is uploaded again.
    QQuickItem* slider = w.find("sectionOffsetSlider");
    REQUIRE(slider != nullptr);
    REQUIRE(slider->isEnabled());
    const render::ViewportInfo before = w.info();
    const QPointF from = slider->mapToScene(QPointF(slider->width() / 2, slider->height() / 2));
    const QPointF to = slider->mapToScene(QPointF(slider->width() * 0.9, slider->height() / 2));
    QTest::mousePress(w.window(), Qt::LeftButton, Qt::NoModifier, from.toPoint());
    int moves = 0;
    std::vector<double> offsets;
    for (int i = 1; i <= 20; ++i) {
        const QPointF p = from + (to - from) * (double(i) / 20);
        QMouseEvent move(QEvent::MouseMove, p, w.window()->mapToGlobal(p), Qt::NoButton, Qt::LeftButton,
                         Qt::NoModifier);
        QGuiApplication::sendEvent(w.window(), &move);
        const std::uint64_t frames = w.info().framesRendered;
        static_cast<void>(QTest::qWaitFor([&] { return w.info().framesRendered > frames; }, 1000));
        offsets.push_back(s->offset());
        ++moves;
    }
    QTest::mouseRelease(w.window(), Qt::LeftButton, Qt::NoModifier, to.toPoint());
    w.settle();
    const render::ViewportInfo after = w.info();
    INFO("frames " << before.framesRendered << " -> " << after.framesRendered << ", uploads " << before.meshUploads
                   << " -> " << after.meshUploads);
    CHECK(after.meshUploads == before.meshUploads);
    CHECK(after.framesRendered >= before.framesRendered + std::uint64_t(moves) / 2);
    CHECK(offsets.back() > offsets.front());
    CHECK(std::is_sorted(offsets.begin(), offsets.end()));
    CHECK(std::abs(w.view()->viewport()->section()->origin.z - s->offset()) < 1e-9);
    // The numeric field follows the slider, in document units.
    QQuickItem* field = w.find("sectionOffsetField");
    REQUIRE(field != nullptr);
    CHECK(std::abs(field->property("value").toDouble() - s->toUnits(s->offset())) < 1e-9);
    CHECK(field->property("unit").toString() == s->unit());

    // Idle: no frames at all.
    QTest::qWait(300);
    const std::uint64_t idle = w.info().framesRendered;
    QTest::qWait(1000);
    CHECK(w.info().framesRendered == idle);

    // Flip keeps the plane's place and cuts the other side away.
    s->setFlipped(true);
    CHECK(w.view()->viewport()->section()->normal.z == -1.0);
    CHECK(std::abs(w.view()->viewport()->section()->origin.z - s->offset()) < 1e-9);
    // Off again.
    REQUIRE(w.commands()->run(QStringLiteral("view.section.toggle")));
    CHECK_FALSE(w.view()->viewport()->section().has_value());
    CHECK(w.host->warnings().isEmpty());
    QStringList warnings = takeWarnings();
    warnings.removeIf([](const QString& m) { return m.contains(QLatin1String("pipeline cache")); }); // first run
    INFO(warnings.join(QLatin1Char('\n')).toStdString());
    CHECK(warnings.isEmpty());
}

TEST_CASE("caps follow the tier; section and draft survive theme and tier switches", "[ui][section][draft]")
{
    REQUIRE_RHI();
    Window w("dark", false, render::Tier::Low);
    w.open(kBoxHole);
    SectionModel* s = w.section();
    s->setPanelOpen(true);
    s->setActive(true);
    s->setDraft(true);
    s->setDraftThreshold(2.5);
    w.settle();
    // Low: no caps; the checkbox is disabled and says why.
    CHECK_FALSE(s->capAvailable());
    CHECK_FALSE(s->capReason().isEmpty());
    QQuickItem* cap = w.find("sectionCap");
    QQuickItem* reason = w.find("sectionCapReason");
    REQUIRE(cap != nullptr);
    REQUIRE(reason != nullptr);
    CHECK_FALSE(cap->isEnabled());
    CHECK(reason->isVisible());
    CHECK(w.info().sectionCaps == 0);

    w.host->scene()->setPreset(render::presetFor(render::Tier::Medium));
    w.settle();
    CHECK(s->capAvailable());
    CHECK(cap->isEnabled());
    CHECK_FALSE(reason->isVisible());
    CHECK(w.info().sectionCaps == 1);
    const auto plane = w.view()->viewport()->section();
    const auto draft = w.view()->viewport()->draftAnalysis();
    REQUIRE(plane.has_value());
    REQUIRE(draft.has_value());
    CHECK(draft->thresholdDeg == 2.5);
    CHECK(draft->pull.z == 1.0);

    w.host->theme()->setMode(QStringLiteral("light"));
    w.settle();
    w.host->scene()->setPreset(render::presetFor(render::Tier::High));
    w.settle();
    w.host->theme()->setMode(QStringLiteral("dark"));
    w.settle();
    const auto plane2 = w.view()->viewport()->section();
    const auto draft2 = w.view()->viewport()->draftAnalysis();
    REQUIRE(plane2.has_value());
    REQUIRE(draft2.has_value());
    CHECK(plane2->origin.z == plane->origin.z);
    CHECK(plane2->cap == plane->cap);
    CHECK(draft2->thresholdDeg == 2.5);
    CHECK(s->active());
    CHECK(s->draft());
    CHECK(w.info().sectionCaps == 1);
    CHECK(w.host->warnings().isEmpty());
}

TEST_CASE("plane and pull direction from a selected planar face", "[ui][section][draft]")
{
    REQUIRE_RHI();
    Window w("dark", false);
    w.open(kBoxHole);
    SectionModel* s = w.section();
    CHECK_FALSE(s->planeFromSelectedFace()); // nothing selected
    CHECK_FALSE(w.commands()->run(QStringLiteral("view.section.fromFace")));

    const auto side = planarFaceHit(w, Vec3{1, 0, 0});
    REQUIRE(side.has_value());
    w.host->selection()->applyClick(*side, Qt::NoModifier);
    REQUIRE(QTest::qWaitFor([&] { return !w.host->selection()->pending(); }, 10000));
    REQUIRE(s->faceAvailable());
    REQUIRE(w.commands()->run(QStringLiteral("view.section.fromFace")));
    CHECK(s->active());
    CHECK(s->plane() == QStringLiteral("face"));
    const auto plane = w.view()->viewport()->section();
    REQUIRE(plane.has_value());
    CHECK(plane->normal.x > 0.999);
    const auto box = w.view()->viewport()->sceneBounds();
    REQUIRE(box.has_value());
    CHECK(std::abs(plane->origin.x - 0.5 * (box->min.x + box->max.x)) < 1e-6);

    REQUIRE(w.commands()->run(QStringLiteral("view.draft.fromFace")));
    CHECK(s->draft());
    CHECK(s->pull() == QStringLiteral("face"));
    const auto draft = w.view()->viewport()->draftAnalysis();
    REQUIRE(draft.has_value());
    CHECK(draft->pull.x > 0.999);

    // An edge selected: no face to take.
    w.host->selection()->clear();
    CHECK_FALSE(s->faceAvailable());
    CHECK_FALSE(s->faceReason().isEmpty());
    CHECK(w.host->warnings().isEmpty());
}

TEST_CASE("with glass panels a section change blurs the backdrop again", "[ui][section][glass]")
{
    REQUIRE_RHI();
    Window w("dark", true);
    w.open(kBoxHole);
    w.section()->setPanelOpen(true); // one more glass panel over the model
    w.settle();
    QTest::qWait(200);
    const std::uint64_t blurs = w.info().backdropBlurs;
    w.section()->setActive(true);
    REQUIRE(QTest::qWaitFor([&] { return w.info().backdropBlurs > blurs; }, 5000));
    const std::uint64_t blurs2 = w.info().backdropBlurs;
    w.section()->setOffset(w.section()->offsetMin() + 0.25 * (w.section()->offsetMax() - w.section()->offsetMin()));
    REQUIRE(QTest::qWaitFor([&] { return w.info().backdropBlurs > blurs2; }, 5000));
    CHECK(w.host->warnings().isEmpty());
}

TEST_CASE("screenshots of a sectioned part, cap on and off, and draft analysis", "[ui][section][screenshots]")
{
    REQUIRE_RHI();
    if (qEnvironmentVariableIsEmpty("RAY_SECTION_SCREENSHOT_DIR"))
        SKIP("set RAY_SECTION_SCREENSHOT_DIR to save the screenshots");
    const QString part = qEnvironmentVariableIsEmpty("RAY_SECTION_SCREENSHOT_PART")
                             ? kBoxHole
                             : qEnvironmentVariable("RAY_SECTION_SCREENSHOT_PART");
    for (const char* themeName : {"dark", "light"}) {
        Window w(themeName, true);
        w.open(part);
        w.view()->setStandardView(QStringLiteral("iso"));
        SectionModel* s = w.section();
        s->setPanelOpen(true);
        s->setPlane(QStringLiteral("zx"));
        s->setFlipped(true); // the iso view looks from the -Y side: cut that half away to look into the part
        s->setActive(true);
        s->setCap(true);
        w.settle();
        QTest::qWait(300);
        w.save(QStringLiteral("section_cap_%1").arg(QLatin1String(themeName)));
        s->setCap(false);
        w.settle();
        QTest::qWait(300);
        w.save(QStringLiteral("section_nocap_%1").arg(QLatin1String(themeName)));
        s->setActive(false);
        s->setDraft(true);
        s->setDraftThreshold(1.0);
        w.settle();
        QTest::qWait(300);
        w.save(QStringLiteral("draft_%1").arg(QLatin1String(themeName)));
        CHECK(w.host->warnings().isEmpty());
    }
}

int main(int argc, char** argv)
{
    if (!qEnvironmentVariableIsSet("QT_RHI_LEAK_CHECK"))
        qputenv("QT_RHI_LEAK_CHECK", "1");
    QGuiApplication app(argc, argv);
    g_previous = qInstallMessageHandler(recordMessage);
    return Catch::Session().run(argc, argv);
}
