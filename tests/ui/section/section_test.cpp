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
#include "measure_model.h"
#include "palette_commands.h"
#include "scene_controller.h"
#include "section_model.h"
#include "selection_model.h"
#include "settings.h"
#include "test_screen.h"
#include "theme_object.h"
#include "units.h"
#include "viewport_item.h"

#include <ray_render/viewport.h>

#include <QtCore/QDir>
#include <QtCore/QTemporaryDir>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>
#include <QtGui/QMouseEvent>
#include <QtQml/QQmlApplicationEngine>
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
        ray::test::screen::placeBeforeShow(probe); // RAY_TEST_SCREEN: on that screen
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
    // The part is a closed solid and Medium has caps (its closedness is worked out on a worker first).
    CHECK(QTest::qWaitFor([&] { return w.info().sectionCaps == 1; }, 10000));

    // Drag the slider's handle across its groove: every move that moves the plane is drawn, nothing is uploaded
    // again, and the last frame shows where the drag ended.
    QQuickItem* slider = w.find("sectionOffsetSlider");
    REQUIRE(slider != nullptr);
    REQUIRE(slider->isEnabled());
    const double middle = s->offset();
    const render::ViewportInfo before = w.info();
    const QPointF from = slider->mapToScene(QPointF(slider->width() / 2, slider->height() / 2));
    const QPointF to = slider->mapToScene(QPointF(slider->width() * 0.9, slider->height() / 2));
    constexpr int kMoves = 20;
    struct Drag {
        std::vector<double> offsets; // after each move
        int moved = 0;               // moves that moved the plane
        int drawn = 0;               // ...and were drawn
        bool interrupted = false;    // the window's activation changed during it
        std::string trace;
    };
    // A drag ends when the window loses the activation: Qt Quick takes the grab from the item that has it. The test
    // harness keeps other programs (other tests' windows open all the time on a busy machine) from taking it
    // (ui/foreign_input_listener.cpp); should it change all the same, the slider rightly ignores the rest of the moves,
    // and the drag is done again, from the middle.
    const auto drag = [&] {
        Drag d;
        const auto watch = QObject::connect(w.window(), &QWindow::activeChanged, w.window(), [&] {
            d.interrupted = true;
            d.trace +=
                std::string("\n  the window's activation changed: active ") + (w.window()->isActive() ? "yes" : "no");
        });
        QTest::mousePress(w.window(), Qt::LeftButton, Qt::NoModifier, from.toPoint());
        double last = s->offset();
        for (int i = 1; i <= kMoves; ++i) {
            const QPointF p = from + (to - from) * (double(i) / kMoves);
            QMouseEvent move(QEvent::MouseMove, p, w.window()->mapToGlobal(p), Qt::NoButton, Qt::LeftButton,
                             Qt::NoModifier);
            const std::uint64_t frames = w.info().framesRendered;
            QGuiApplication::sendEvent(w.window(), &move);
            d.offsets.push_back(s->offset());
            const bool moved = s->offset() != last;
            last = s->offset();
            // A move that moved the plane is drawn: the next frame comes for it (however slowly on a loaded machine;
            // without the wait, the next move would be drawn in the same frame).
            const bool drew = moved && QTest::qWaitFor([&] { return w.info().framesRendered > frames; }, 10000);
            d.moved += moved ? 1 : 0;
            d.drawn += drew ? 1 : 0;
            d.trace += "\n  move " + std::to_string(i) + ": offset " + std::to_string(s->offset()) +
                       (moved ? drew ? ", drawn" : ", NOT drawn" : ", unchanged");
        }
        QTest::mouseRelease(w.window(), Qt::LeftButton, Qt::NoModifier, to.toPoint());
        QObject::disconnect(watch);
        return d;
    };
    Drag d;
    int attempts = 0;
    for (; attempts < 5; ++attempts) {
        if (attempts > 0) {
            s->setOffset(middle);
            w.settle();
        }
        d = drag();
        if (!d.interrupted)
            break;
        WARN("drag " << attempts + 1 << " lost the window's activation, done again:" << d.trace);
    }
    const int tries = d.interrupted ? attempts : attempts + 1;
    w.settle();
    const render::ViewportInfo after = w.info();
    INFO("drag " << tries << ": " << d.moved << " of " << kMoves << " moves moved the plane, " << d.drawn
                 << " were drawn; frames " << before.framesRendered << " -> " << after.framesRendered << ", uploads "
                 << before.meshUploads << " -> " << after.meshUploads << d.trace);
    REQUIRE_FALSE(d.interrupted); // every one of five drags lost the activation: nothing can be told from them
    CHECK(after.meshUploads == before.meshUploads);
    CHECK(d.moved >= kMoves - 2); // the handle follows the pointer (the first moves may be within the drag threshold)
    CHECK(d.drawn == d.moved);
    const std::vector<double>& offsets = d.offsets;
    CHECK(offsets.back() > middle);
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
    // Not this window's doing: the pipeline cache is missing on a first run, and Qt's Windows clipboard retries when
    // another process (a test copying a measurement next to this one, or the user) has the system clipboard open.
    warnings.removeIf([](const QString& m) {
        return m.contains(QLatin1String("pipeline cache")) || m.contains(QLatin1String("Retrying to obtain clipboard"));
    });
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
    CHECK(QTest::qWaitFor([&] { return w.info().sectionCaps == 1; }, 10000));
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
    CHECK(QTest::qWaitFor([&] { return w.info().sectionCaps == 1; }, 10000));
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

TEST_CASE("the section offset shows and takes the display units and follows the unit toggle", "[ui][section][units]")
{
    REQUIRE_RHI();
    Window w("dark", false);
    w.open(kBoxHole);
    SectionModel* s = w.section();
    ui::UnitsObject* units = w.host->units();
    s->setPanelOpen(true);
    s->setActive(true);
    w.settle();
    QQuickItem* field = w.find("sectionOffsetField");
    QQuickItem* slider = w.find("sectionOffsetSlider");
    REQUIRE(field != nullptr);
    REQUIRE(slider != nullptr);
    const auto check = [&](const char* when) {
        INFO(when << ": unit " << s->unit().toStdString() << ", field "
                  << field->property("text").toString().toStdString());
        const double scale = units->system() == UnitSystem::Inch ? 25.4 : 1.0;
        CHECK(s->unit() == units->unitLabel());
        CHECK(s->unitDecimals() == units->decimals());
        CHECK(field->property("unit").toString() == units->unitLabel());
        CHECK(field->property("decimals").toInt() == units->decimals());
        CHECK(std::abs(field->property("value").toDouble() - s->offset() / scale) < 1e-9);
        CHECK(field->property("text").toString() == QString::number(s->offset() / scale, 'f', units->decimals()));
        CHECK(std::abs(slider->property("to").toDouble() - s->offsetMax() / scale) < 1e-9);
        CHECK(std::abs(slider->property("value").toDouble() - s->offset() / scale) < 1e-9);
        CHECK(s->offsetText() == units->formatLength(s->offset()));
    };
    s->setOffset(s->offsetMin() + 0.3 * (s->offsetMax() - s->offsetMin()));
    QTest::qWait(50);
    check("as opened");
    const ray::UnitSystem before = units->system();
    units->toggle(); // the status bar's unit button
    REQUIRE(units->system() != before);
    QTest::qWait(50);
    check("after the toggle");
    // Typed in either unit; the plane moves in mm.
    const double mm = s->offsetMin() + 0.6 * (s->offsetMax() - s->offsetMin());
    const auto type = [&](const QString& text) {
        field->forceActiveFocus();
        field->setProperty("text", text);
        QMetaObject::invokeMethod(field, "commit");
        w.settle();
    };
    type(QStringLiteral("%1 mm").arg(mm, 0, 'f', 4));
    CHECK(std::abs(s->offset() - mm) < 1e-3);
    CHECK(std::abs(w.view()->viewport()->section()->origin.z - mm) < 1e-3);
    type(QStringLiteral("%1 in").arg(mm / 25.4, 0, 'f', 6));
    CHECK(std::abs(s->offset() - mm) < 1e-3);
    units->toggle();
    w.view()->forceActiveFocus(); // the field shows the formatted value again once it loses the focus
    QTest::qWait(50);
    check("toggled back");
    CHECK(w.host->warnings().isEmpty());
}

TEST_CASE("caps follow the effective preset: the settings override and the tier, live", "[ui][section][settings]")
{
    REQUIRE_RHI();
    Window w("dark", false);
    w.open(kBoxHole);
    SectionModel* s = w.section();
    ui::SettingsObject* settings = w.host->settings();
    REQUIRE(settings->set(QStringLiteral("graphics.tier"), QStringLiteral("high")));
    s->setPanelOpen(true);
    s->setActive(true);
    REQUIRE(QTest::qWaitFor([&] { return w.info().sectionCaps == 1; }, 10000));
    QQuickItem* cap = w.find("sectionCap");
    QQuickItem* reason = w.find("sectionCapReason");
    REQUIRE(cap != nullptr);
    REQUIRE(reason != nullptr);
    CHECK(cap->isEnabled());

    // The override turns caps off on a tier that has them: disabled with a reason that names the setting.
    REQUIRE(settings->set(QStringLiteral("graphics.sectionCaps"), false));
    CHECK_FALSE(s->capAvailable());
    CHECK(s->capReason().contains(QStringLiteral("Settings")));
    CHECK_FALSE(cap->isEnabled());
    CHECK(reason->isVisible());
    REQUIRE(QTest::qWaitFor([&] { return w.info().sectionCaps == 0; }, 10000));
    // ...and on, even on Low.
    REQUIRE(settings->set(QStringLiteral("graphics.tier"), QStringLiteral("low")));
    REQUIRE(settings->set(QStringLiteral("graphics.sectionCaps"), true));
    CHECK(s->capAvailable());
    CHECK(cap->isEnabled());
    REQUIRE(QTest::qWaitFor([&] { return w.info().sectionCaps == 1; }, 10000));
    // Unset: the tier decides (Low has none, and the reason names the tier).
    REQUIRE(settings->reset(QStringLiteral("graphics.sectionCaps")));
    CHECK_FALSE(s->capAvailable());
    CHECK(s->capReason().contains(QStringLiteral("tier")));
    REQUIRE(QTest::qWaitFor([&] { return w.info().sectionCaps == 0; }, 10000));
    REQUIRE(settings->set(QStringLiteral("graphics.tier"), QStringLiteral("medium")));
    CHECK(s->capAvailable());
    REQUIRE(QTest::qWaitFor([&] { return w.info().sectionCaps == 1; }, 10000));
    CHECK(s->active()); // the section itself never went away
    CHECK(w.host->warnings().isEmpty());
}

TEST_CASE("the section panel fits small windows and a face seen through the cut can be measured",
          "[ui][section][layout][measure]")
{
    REQUIRE_RHI();
    Window w("dark", false);
    // The window's minimum height is 560 (Main.qml); asking for less only makes Qt warn and clamp.
    w.window()->resize(900, 560);
    REQUIRE(QTest::qWaitFor([&] { return std::abs(w.window()->width() - 900) < 1; }, 5000));
    w.open(kBoxHole);
    SectionModel* s = w.section();
    s->setPanelOpen(true);
    s->setActive(true);
    s->setCap(false);
    w.settle();
    QQuickItem* panel = w.find("sectionPanel");
    QQuickItem* area = w.find("viewportArea");
    QQuickItem* filter = w.find("selectionFilter");
    REQUIRE(panel != nullptr);
    REQUIRE(area != nullptr);
    REQUIRE(panel->isVisible());
    const QRectF p = panel->mapRectToScene(QRectF(0, 0, panel->width(), panel->height()));
    const QRectF a = area->mapRectToScene(QRectF(0, 0, area->width(), area->height()));
    INFO("panel " << p.x() << "," << p.y() << " " << p.width() << "x" << p.height() << ", view area " << a.x() << ","
                  << a.y() << " " << a.width() << "x" << a.height());
    CHECK(a.adjusted(-0.5, -0.5, 0.5, 0.5).contains(p)); // never past the view area: it scrolls instead
    if (filter != nullptr && filter->isVisible()) {
        const QRectF f = filter->mapRectToScene(QRectF(0, 0, filter->width(), filter->height()));
        CHECK_FALSE(p.intersects(f));
    }
    QQuickItem* scroller = w.find("sectionPanelScroller");
    REQUIRE(scroller != nullptr);
    // Whatever does not fit scrolls.
    CHECK((scroller->property("contentHeight").toDouble() <= scroller->height() + 0.5 ||
           scroller->property("interactive").toBool()));

    // Look straight down into the cut (the top half is cut away, no cap): what a click finds lies below the plane
    // and is never a face wholly in the cut-away half; it can be measured.
    s->setPanelOpen(false);
    REQUIRE(w.view()->setStandardView(QStringLiteral("top")));
    w.settle();
    const double planeZ = s->offset();
    const auto mesh = w.host->scene()->meshOf(0);
    REQUIRE(mesh != nullptr);
    auto* measure = w.host->engine()->singletonInstance<ui::MeasureModel*>(QStringLiteral("RayCad.Ui"),
                                                                           QStringLiteral("MeasureTool"));
    REQUIRE(measure != nullptr);
    int picked = 0;
    for (double fx = 0.3; fx <= 0.71 && picked < 3; fx += 0.1) {
        for (double fy = 0.3; fy <= 0.71 && picked < 3; fy += 0.2) {
            w.host->selection()->clear();
            const QPointF at = w.view()->mapFromItem(area, QPointF(a.width() * fx, a.height() * fy));
            w.view()->selectAt(at.x(), at.y());
            if (!QTest::qWaitFor([&] { return w.host->selection()->count() == 1; }, 1500))
                continue;
            const render::PickHit hit = w.host->selection()->entries().front().hit;
            if (hit.kind != kernel::SubKind::Face)
                continue;
            ++picked;
            INFO("pick at " << fx << "," << fy << ": face " << hit.subIndex << " at z " << hit.point.z);
            CHECK(hit.point.z <= planeZ + 1e-3);
            for (const kernel::FaceRange& f : mesh->faces) {
                if (f.face.index != hit.subIndex)
                    continue;
                float lowest = 1e30f;
                for (std::uint32_t i = 0; i < f.indexCount; ++i)
                    lowest = std::min(lowest, mesh->positions[std::size_t(mesh->indices[f.firstIndex + i]) * 3 + 2]);
                CHECK(lowest <= planeZ + 1e-3);
            }
            REQUIRE(w.commands()->run(QStringLiteral("measure.tool")));
            CHECK(QTest::qWaitFor([&] { return !measure->busy() && measure->hasResult(); }, 20000));
        }
    }
    CHECK(picked > 0);
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
