// Glass panels over the viewport (task T-016, PLAN.md 6.6), in a real Qt Quick window with the platform's default
// scene graph (Direct3D 11 on Windows), so the viewport and its blurred backdrop really render:
//   - a glass panel over a model shows the blurred model behind it, aligned: a high-contrast edge of the model is
//     continuous between the sharp viewport beside a panel and the blurred backdrop inside it (left and right side
//     panels, and again after the window was resized and the right panel moved); this runs at the screen's scale
//     and, through CTest, at 150 % (a fractional device pixel ratio)
//   - exactly one blur per changed frame however many glass panels are open, none when the image did not change,
//     none while idle; switching glass off stops the blur, frees the backdrop and gives flat panels with the same
//     layout
//   - 200 scripted window resizes with glass on: no crash, no warning, no RHI resource leak; and 200 more with every
//     resize hovered (Qt's pointer, moved by the test), so the picking pass is made and kept: no leak either
//   - imported body colours (an assembly file that carries them) are drawn, and a selection stays visible on them
// With RAY_GLASS_SCREENSHOT_DIR set, the windows are saved there as PNG files (for review; nothing is compared).
#include "../scene_graph_probe.h"
#include "app_context.h"
#include "app_host.h"
#include "app_options.h"
#include "files.h"
#include "scene_controller.h"
#include "selection_model.h"
#include "theme_object.h"
#include "viewport_item.h"
#include "viewport_navigation.h"

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
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

using namespace ray;
using ray::ui::AppContext;
using ray::ui::AppHost;
using ray::ui::AppOptions;
using ray::ui::GlassBackdrop;
using ray::ui::ViewportItem;

namespace {

const QString kBoxHole = QStringLiteral(RAY_KERNEL_TEST_DATA_DIR "/box_hole.step");
const QString kAssembly = QStringLiteral(RAY_KERNEL_TEST_DATA_DIR "/assembly_mm.step");
constexpr int kOpenTimeoutMs = 120000;

// Qt warnings of the whole run (the RHI leak check warns when a QRhi is destroyed with resources left).
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

using ray::uitest::rhiSceneGraph;

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

void walk(QQuickItem* item, const std::function<void(QQuickItem*)>& fn)
{
    fn(item);
    for (QQuickItem* child : item->childItems())
        walk(child, fn);
}

struct Window {
    QTemporaryDir cache;
    QTemporaryDir userData;
    std::unique_ptr<AppHost> host;

    explicit Window(const char* theme, bool glass, QSize size = QSize(1440, 900))
    {
        AppOptions options;
        options.theme = QString::fromLatin1(theme);
        options.glass = glass;
        options.size = size;
        host = std::make_unique<AppHost>(options, cache.path());
        REQUIRE(host->optionError().isEmpty());
        REQUIRE(host->load());
        host->show();
        REQUIRE(QTest::qWaitForWindowExposed(host->window()));
        REQUIRE(view() != nullptr);
        REQUIRE(host->startSession(userData.path().toStdString()));
        host->files()->setUseNativeDialogs(false);
        view()->setAnimationsEnabled(false);
        host->scene()->setPreset(render::presetFor(render::Tier::Medium));
        grab();
    }

    QQuickWindow* window() const { return host->window(); }
    ViewportItem* view() const { return host->viewport(); }
    AppContext* context() const { return host->context(); }
    ui::ThemeObject* theme() const { return host->theme(); }
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

    // The glass backdrops that currently sample the viewport.
    std::vector<GlassBackdrop*> sampling() const
    {
        std::vector<GlassBackdrop*> out;
        walk(window()->contentItem(), [&](QQuickItem* item) {
            if (auto* g = qobject_cast<GlassBackdrop*>(item); g && g->sampling())
                out.push_back(g);
        });
        return out;
    }

    // Top view, orthographic, with the model's X extent from window x `left` to `right` and the near (low Y) edge of
    // its silhouette at window y `edgeY`.
    void placeModel(double left, double right, double edgeY) const
    {
        const auto box = view()->viewport()->sceneBounds();
        REQUIRE(box.has_value());
        const double w = view()->width(), h = view()->height();
        const double s = (right - left) / (box->max.x - box->min.x); // pixels per mm
        render::Camera cam{};
        cam.perspective = false;
        cam.fovDeg = 30;
        cam.orthoHeight = h / s;
        const double tx = box->min.x + (w / 2 - left) / s;
        const double ty = box->min.y + (edgeY - h / 2) / s;
        const double tz = (box->min.z + box->max.z) / 2;
        cam.target = Vec3{tx, ty, tz};
        cam.eye = Vec3{tx, ty, tz + 10 * (box->max.z - box->min.z + 100)};
        cam.up = Vec3{0, 1, 0};
        view()->setCamera(cam);
        const auto p = ui::nav::project(cam, Vec3{box->min.x, box->min.y, box->max.z}, w, h);
        REQUIRE(p.has_value());
        CHECK(std::abs(p->x() - left) < 0.5);
        CHECK(std::abs(p->y() - edgeY) < 0.5);
    }

    void save(const QString& name) const
    {
        const QByteArray dir = qgetenv("RAY_GLASS_SCREENSHOT_DIR");
        if (dir.isEmpty())
            return;
        QDir().mkpath(QString::fromLocal8Bit(dir));
        CHECK(grab().save(QDir(QString::fromLocal8Bit(dir)).filePath(name + QStringLiteral(".png"))));
    }
};

double luminance(const QColor& c)
{
    return 0.2126 * c.red() + 0.7152 * c.green() + 0.0722 * c.blue();
}

// Along the window column x, from y0 to y1 (logical), the logical y where the luminance crosses halfway between its
// level at the top and at the bottom of the range (each the mean of its outer fifth). nullopt if there is no step.
std::optional<double> stepAt(const QImage& img, double x, double y0, double y1, double* contrast = nullptr)
{
    const double dpr = img.devicePixelRatio();
    const int px = int(std::floor(x * dpr));
    const int a = int(std::floor(y0 * dpr)), b = int(std::floor(y1 * dpr));
    std::vector<double> l;
    for (int y = a; y <= b; ++y)
        l.push_back(luminance(img.pixelColor(px, y)));
    const std::size_t fifth = std::max<std::size_t>(1, l.size() / 5);
    double top = 0, bottom = 0;
    for (std::size_t i = 0; i < fifth; ++i) {
        top += l[i];
        bottom += l[l.size() - 1 - i];
    }
    top /= double(fifth);
    bottom /= double(fifth);
    if (contrast)
        *contrast = std::abs(top - bottom);
    if (std::abs(top - bottom) < 3)
        return std::nullopt;
    const double mid = (top + bottom) / 2;
    for (std::size_t i = 0; i + 1 < l.size(); ++i) {
        if ((l[i] - mid) * (l[i + 1] - mid) <= 0 && l[i] != l[i + 1]) {
            const double t = (mid - l[i]) / (l[i + 1] - l[i]);
            return (double(a) + double(i) + t + 0.5) / dpr;
        }
    }
    return std::nullopt;
}

// The edge of the model behind `panel` (inner side `innerX`, `inwards` +1 for the left panel, -1 for the right)
// measured beside the panel in the sharp viewport and inside it in the blurred backdrop: positions in logical px.
struct Continuity {
    double outside = 0, inside = 0, outsideContrast = 0, insideContrast = 0;
};
Continuity continuity(const Window& w, QQuickItem* panel, double innerX, double inwards, double edgeY)
{
    QQuickItem* tint = findItem(panel, "panelGlassTint");
    REQUIRE(tint != nullptr);
    tint->setVisible(false); // the blur alone, for a measurable step
    const QImage img = w.grab();
    tint->setVisible(true);
    Continuity c;
    // The blur reaches about three sigma, sigma being blur.radius / 2 logical px.
    const double reach = 3 * 24.0 / 2 + 12;
    const auto out = stepAt(img, innerX - inwards * 10, edgeY - reach, edgeY + reach, &c.outsideContrast);
    const auto in = stepAt(img, innerX + inwards * 36, edgeY - reach, edgeY + reach, &c.insideContrast);
    REQUIRE(out.has_value());
    REQUIRE(in.has_value());
    c.outside = *out;
    c.inside = *in;
    return c;
}

} // namespace

TEST_CASE("glass panels show the blurred model behind them, aligned", "[glass][align]")
{
    RAY_REQUIRE_RHI();
    for (const char* themeName : {"dark", "light"}) {
        INFO("theme " << themeName);
        Window w(themeName, true);
        w.open(kBoxHole);
        QQuickItem* left = w.find("leftPanel");
        QQuickItem* right = w.find("rightPanel");
        REQUIRE(left != nullptr);
        REQUIRE(right != nullptr);
        const QRectF l = left->mapRectToScene(QRectF(0, 0, left->width(), left->height()));
        const QRectF r = right->mapRectToScene(QRectF(0, 0, right->width(), right->height()));
        const double edgeY = l.top() + l.height() * 0.72;
        w.placeModel(l.right() - 160, r.left() + 160, edgeY);
        w.grab();
        CHECK(w.view()->backdropUsers() > 0);
        CHECK(w.info().backdropBlurs > 0);
        w.save(QStringLiteral("glass_align_%1").arg(QLatin1String(themeName)));

        const auto check = [&](QQuickItem* panel, double innerX, double inwards, const char* what) {
            const Continuity c = continuity(w, panel, innerX, inwards, edgeY);
            INFO(what << ": edge at y " << c.outside << " beside the panel (step " << c.outsideContrast << "), "
                      << c.inside << " in the blur inside it (step " << c.insideContrast << ")");
            CHECK(c.outsideContrast > 40);
            CHECK(c.insideContrast > 15); // blurred, but clearly the model
            CHECK(std::abs(c.inside - c.outside) <= 2.5);
            CHECK(std::abs(c.outside - edgeY) <= 1.5);
        };
        check(left, l.right(), -1, "left panel");
        check(right, r.left(), +1, "right panel");

        // Under the tint the model still shows: the panel is lighter or darker above the edge than below it.
        const QImage tinted = w.grab();
        const double dpr = tinted.devicePixelRatio();
        const auto at = [&](double x, double y) {
            return luminance(tinted.pixelColor(int(x * dpr), int(y * dpr)));
        };
        const double x = l.right() - 40;
        const double through = std::abs(at(x, edgeY - 40) - at(x, edgeY + 40));
        INFO("tinted panel: luminance step " << through);
        CHECK(through > 3);

        // The window grows: the right panel moves, the viewport is resized, and the blur follows both.
        w.window()->resize(1600, 960);
        REQUIRE(QTest::qWaitFor([&] { return std::abs(w.view()->width() - 1600) < 1; }, 5000));
        const QRectF l2 = left->mapRectToScene(QRectF(0, 0, left->width(), left->height()));
        const QRectF r2 = right->mapRectToScene(QRectF(0, 0, right->width(), right->height()));
        CHECK(r2.left() > r.left() + 100);
        const double edgeY2 = l2.top() + l2.height() * 0.72;
        w.placeModel(l2.right() - 160, r2.left() + 160, edgeY2);
        w.grab();
        const Continuity c2 = continuity(w, right, r2.left(), +1, edgeY2);
        INFO("after the resize: " << c2.outside << " beside, " << c2.inside << " inside");
        CHECK(std::abs(c2.inside - c2.outside) <= 2.5);
        // Only the right panel changes (made wider, as its splitter does): without a new frame of the viewport, its
        // blur still lines up.
        const std::uint64_t frames = w.info().framesRendered;
        QQuickItem* shell = w.find("shell");
        REQUIRE(shell != nullptr);
        shell->setProperty("rightPanelWidth", right->width() + 50);
        w.grab();
        const QRectF r3 = right->mapRectToScene(QRectF(0, 0, right->width(), right->height()));
        CHECK(r3.left() < r2.left() - 40);
        const Continuity c3 = continuity(w, right, r3.left(), +1, edgeY2);
        INFO("panel resized: " << c3.outside << " beside, " << c3.inside << " inside");
        CHECK(std::abs(c3.inside - c3.outside) <= 2.5);
        // Only moved (its margin changes, its size does not): the backdrop item itself keeps its place in the panel,
        // so it learns of the move from the ancestor (ItemTransformHasChanged).
        QObject* anchors = right->property("anchors").value<QObject*>();
        REQUIRE(anchors != nullptr);
        REQUIRE(anchors->setProperty("rightMargin", anchors->property("rightMargin").toDouble() + 40));
        w.grab();
        const QRectF r4 = right->mapRectToScene(QRectF(0, 0, right->width(), right->height()));
        CHECK(std::abs(r4.left() - (r3.left() - 40)) < 0.5);
        CHECK(std::abs(r4.width() - r3.width()) < 0.5);
        const Continuity c4 = continuity(w, right, r4.left(), +1, edgeY2);
        INFO("panel moved: " << c4.outside << " beside, " << c4.inside << " inside");
        CHECK(std::abs(c4.inside - c4.outside) <= 2.5);
        CHECK(w.info().framesRendered - frames <= 2); // the panel moved, not the viewport's picture
        w.save(QStringLiteral("glass_moved_%1").arg(QLatin1String(themeName)));
        w.context()->setGlass(false);
        w.save(QStringLiteral("flat_moved_%1").arg(QLatin1String(themeName)));
        CHECK(w.host->warnings().isEmpty());
    }
}

TEST_CASE("one blur per changed frame with any number of glass panels; none when off", "[glass][counter]")
{
    RAY_REQUIRE_RHI();
    Window w("dark", true);
    w.open(kBoxHole);
    const std::size_t panels = w.sampling().size();
    INFO(panels << " glass panels sample the backdrop");
    CHECK(panels >= 4); // top bar, model, properties, status bar, selection filter, view cube buttons
    CHECK(w.view()->backdropUsers() == int(panels));

    const auto orbit = [&](int steps) {
        for (int i = 0; i < steps; ++i) {
            w.view()->orbitBy(4, 0);
            w.grab();
        }
    };
    const auto measure = [&](int steps) {
        const render::ViewportInfo a = w.info();
        orbit(steps);
        const render::ViewportInfo b = w.info();
        return std::pair{b.backdropBlurs - a.backdropBlurs, b.framesRendered - a.framesRendered};
    };
    const auto [blurs, frames] = measure(10);
    CHECK(blurs == 10);
    CHECK(frames >= 10);

    // One more glass panel (the message banner): still one blur per frame.
    QQuickItem* banner = w.find("messageBanner");
    REQUIRE(banner != nullptr);
    w.context()->showMessage(QStringLiteral("A message on glass"), false);
    w.grab();
    CHECK(w.sampling().size() > panels);
    const auto [blurs2, frames2] = measure(10);
    CHECK(blurs2 == 10);
    CHECK(frames2 >= 10);
    w.context()->dismissMessage();

    // Frames that only answer a pick, or change nothing in the picture, blur nothing; idle renders nothing.
    w.grab();
    const render::ViewportInfo before = w.info();
    bool answered = false;
    w.view()->viewport()->pick(10, 10, std::uint8_t(render::PickFilter::Faces),
                               [&](std::optional<render::PickHit>) { answered = true; });
    REQUIRE(QTest::qWaitFor([&] { return answered; }, 5000));
    w.grab();
    QTest::qWait(1000);
    const render::ViewportInfo idle = w.info();
    CHECK(idle.backdropBlurs == before.backdropBlurs);
    const std::uint64_t idleFrames = idle.framesRendered;
    QTest::qWait(1000);
    CHECK(w.info().framesRendered == idleFrames);

    // Glass off: no backdrop user, no blur, the backdrop is gone; panels are flat PanelBg with the same geometry.
    const auto layout = [&] {
        std::vector<std::pair<std::string, QRectF>> rects;
        walk(w.window()->contentItem(), [&](QQuickItem* i) {
            rects.emplace_back(i->objectName().toStdString() + " (" + i->metaObject()->className() + ")",
                               i->mapRectToScene(QRectF(0, 0, i->width(), i->height())));
        });
        return rects;
    };
    const auto glassRects = layout();
    w.context()->setGlass(false);
    w.grab();
    CHECK(w.sampling().empty());
    CHECK(w.view()->backdropUsers() == 0);
    const auto flatRects = layout();
    const auto [blurs3, frames3] = measure(5);
    CHECK(blurs3 == 0);
    CHECK(frames3 >= 5);
    CHECK(w.view()->viewport()->blurredBackdropTexture().width == 0);
    REQUIRE(flatRects.size() == glassRects.size());
    for (std::size_t i = 0; i < flatRects.size(); ++i) {
        INFO("item " << i << ": " << glassRects[i].first << " / " << flatRects[i].first);
        CHECK(flatRects[i].first == glassRects[i].first);
        CHECK(flatRects[i].second == glassRects[i].second);
    }
    QQuickItem* left = w.find("leftPanel");
    const QImage flat = w.grab();
    const QPointF inside = left->mapToScene(QPointF(left->width() / 2, left->height() * 0.8));
    const QColor c =
        flat.pixelColor(int(inside.x() * flat.devicePixelRatio()), int(inside.y() * flat.devicePixelRatio()));
    CHECK(c == w.theme()->panelBg());
    // ...and back on: one blur of the image already there, then one per changed frame again.
    w.context()->setGlass(true);
    w.grab();
    CHECK(w.view()->backdropUsers() == int(panels));
    const auto [blurs4, frames4] = measure(5);
    CHECK(blurs4 == 5);
    CHECK(w.host->warnings().isEmpty());
}

TEST_CASE("200 window resizes with glass on: no crash, no RHI resource leak", "[glass][resize]")
{
    RAY_REQUIRE_RHI();
    static_cast<void>(takeWarnings());
    {
        Window w("dark", true, QSize(1200, 800));
        w.open(kBoxHole);
        w.grab();
        const std::int64_t baseline = w.info().liveRhiResources;
        const std::uint64_t picks = w.info().pickBufferRenders;
        CHECK(w.info().backdropBlurs > 0);
        // 200 resizes, the pointer (Qt's, moved by the test) over the view at a new place each time when `hovering`;
        // the most resources alive at once. Ends at 1200x800 with glass on, the replaced backdrop freed.
        const auto resizes = [&](bool hovering) {
            std::int64_t most = w.info().liveRhiResources;
            for (int i = 0; i < 200; ++i) {
                const int width = 700 + (i * 37) % 900;
                const int height = 500 + (i * 53) % 500;
                w.window()->resize(width, height);
                if (hovering)
                    QTest::mouseMove(w.window(),
                                     QPoint(width / 2 + (i * 7) % 60 - 30, height / 2 + (i * 11) % 60 - 30));
                if (i % 25 == 7)
                    w.context()->setGlass(i % 2 == 0); // panels come and go as well
                if (i % 2 == 0)
                    w.grab(); // a frame at this size
                else
                    QCoreApplication::processEvents();
                most = std::max(most, w.info().liveRhiResources);
            }
            w.context()->setGlass(true);
            w.window()->resize(1200, 800);
            REQUIRE(QTest::qWaitFor([&] { return std::abs(w.view()->width() - 1200) < 1; }, 5000));
            w.grab();
            // The backdrop texture replaced by the last resize is freed at the viewport's next frame (viewport_rhi.h).
            w.view()->viewport()->requestFrame();
            w.grab();
            return most;
        };
        const std::int64_t most = resizes(false);
        INFO("RHI resources: " << baseline << " at the start, " << w.info().liveRhiResources << " at the end, at most "
                               << most << "; hover picks " << w.info().pickBufferRenders - picks);
        CHECK(w.info().liveRhiResources == baseline);
        CHECK(most <= baseline + 8);
        // Nothing hovered, so nothing was picked: the real pointer over the window is ignored
        // (ui/foreign_input_listener.cpp), and a pick would make the picking pass's resources.
        CHECK(w.info().pickBufferRenders == picks);
        CHECK(w.view()->viewport()->blurredBackdropTexture().width > 0);

        // Again with the pointer over the view, every hover picked: the picking pass's target and pipelines are made
        // at the first pick and then kept, replaced at a new size, never added to.
        QTest::mouseMove(w.window(), QPoint(600, 400));
        REQUIRE(QTest::qWaitFor([&] { return w.info().pickBufferRenders > picks; }, 5000));
        static_cast<void>(resizes(true)); // every size once with the picking pass
        const std::int64_t picking = w.info().liveRhiResources;
        const std::uint64_t picksBefore = w.info().pickBufferRenders;
        const std::int64_t mostPicking = resizes(true);
        INFO("with hover picks: " << picking << " before 200 more resizes, " << w.info().liveRhiResources
                                  << " after, at most " << mostPicking << "; hover picks "
                                  << w.info().pickBufferRenders - picksBefore);
        CHECK(w.info().pickBufferRenders > picksBefore + 50);
        CHECK(w.info().liveRhiResources == picking);
        CHECK(mostPicking <= picking + 8);
        CHECK(w.host->warnings().isEmpty());
    }
    // The window and its QRhi are gone: QRhi's leak check warns about anything left over.
    for (const QString& warning : takeWarnings()) {
        INFO("Qt warning: " << warning.toStdString());
        CHECK_FALSE(warning.contains(QLatin1String("unreleased"), Qt::CaseInsensitive));
        CHECK_FALSE(warning.contains(QLatin1String("leak"), Qt::CaseInsensitive));
    }
}

TEST_CASE("imported body colours are drawn and a selection stays visible on them", "[glass][bodycolor]")
{
    RAY_REQUIRE_RHI();
    for (const char* themeName : {"dark", "light"}) {
        INFO("theme " << themeName);
        Window w(themeName, true);
        w.open(kAssembly);
        const QImage img = w.grab();
        w.save(QStringLiteral("assembly_colours_%1").arg(QLatin1String(themeName)));
        // The assembly's bodies are red (0.8, 0.1, 0.1), blue (0.1, 0.2, 0.9) and green (0.1, 0.7, 0.2): count
        // clearly reddish, bluish and greenish pixels in the free viewport area.
        QQuickItem* area = w.find("viewportArea");
        const QRectF free = area->mapRectToScene(QRectF(0, 0, area->width(), area->height()));
        const double dpr = img.devicePixelRatio();
        const auto isRed = [](const QColor& c) {
            return c.red() > 2 * c.green() + 20 && c.red() > 2 * c.blue() + 20;
        };
        int red = 0, green = 0, blue = 0;
        std::optional<QPointF> redPoint; // inside a red face, far from its edges (an edge would be picked first)
        const int margin = int(16 * dpr);
        for (int y = int(free.top() * dpr); y < int(free.bottom() * dpr); y += 2) {
            for (int x = int(free.left() * dpr); x < int(free.right() * dpr); x += 2) {
                const QColor c = img.pixelColor(x, y);
                if (isRed(c)) {
                    ++red;
                    if (!redPoint && x - margin >= 0 && y - margin >= 0 && x + margin < img.width() &&
                        y + margin < img.height()) {
                        bool inside = true;
                        for (int dy = -margin; dy <= margin && inside; dy += 4) {
                            for (int dx = -margin; dx <= margin && inside; dx += 4)
                                inside = isRed(img.pixelColor(x + dx, y + dy)) &&
                                         img.pixelColor(x + dx, y + dy) == img.pixelColor(x, y);
                        }
                        if (inside)
                            redPoint = QPointF((x + 0.5) / dpr, (y + 0.5) / dpr);
                    }
                }
                green += c.green() > c.red() + 40 && c.green() > c.blue() + 40 ? 1 : 0;
                blue += c.blue() > c.red() + 60 && c.blue() > c.green() + 40 ? 1 : 0;
            }
        }
        INFO("reddish " << red << ", greenish " << green << ", bluish " << blue);
        CHECK(red > 500);
        CHECK(green > 500);
        CHECK(blue > 100);
        // Edges and highlights keep theme tokens; a selected red face changes clearly.
        REQUIRE(redPoint.has_value());
        QMouseEvent press(QEvent::MouseButtonPress, *redPoint, w.window()->mapToGlobal(*redPoint), Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, *redPoint, w.window()->mapToGlobal(*redPoint), Qt::LeftButton,
                            Qt::NoButton, Qt::NoModifier);
        QGuiApplication::sendEvent(w.window(), &press);
        QGuiApplication::sendEvent(w.window(), &release);
        REQUIRE(QTest::qWaitFor([&] { return w.host->selection()->count() == 1; }, 10000));
        // The hover highlight follows the mouse; move it away so only the selection shows.
        QMouseEvent away(QEvent::MouseMove, QPointF(2, 2), w.window()->mapToGlobal(QPointF(2, 2)), Qt::NoButton,
                         Qt::NoButton, Qt::NoModifier);
        QGuiApplication::sendEvent(w.window(), &away);
        QTest::qWait(200);
        const QImage selected = w.grab();
        w.save(QStringLiteral("assembly_selected_%1").arg(QLatin1String(themeName)));
        const QColor a = img.pixelColor(int(redPoint->x() * dpr), int(redPoint->y() * dpr));
        const QColor b = selected.pixelColor(int(redPoint->x() * dpr), int(redPoint->y() * dpr));
        const int change =
            std::max({std::abs(a.red() - b.red()), std::abs(a.green() - b.green()), std::abs(a.blue() - b.blue())});
        INFO("selected face pixel " << a.name().toStdString() << " -> " << b.name().toStdString());
        CHECK(change >= 24);
        CHECK(w.host->warnings().isEmpty());
    }
}

int main(int argc, char** argv)
{
    // QRhi then warns about resources still alive when it is destroyed.
    if (!qEnvironmentVariableIsSet("QT_RHI_LEAK_CHECK"))
        qputenv("QT_RHI_LEAK_CHECK", "1");
    QGuiApplication app(argc, argv);
    g_previous = qInstallMessageHandler(recordMessage);
    return Catch::Session().run(argc, argv);
}
