// The viewport item, navigation, selection and model tree (task T-020), on real Qt Quick windows with the
// platform's default scene graph (Direct3D 11 on Windows), so the RHI item really renders.
//
//   - a STEP file opens through the session (Files, T-019), shows in the view and in the tree; a face picked by
//     pixel reads back its SubId and a TopoRef that resolves Exact to the same face; the properties read-out fills in
//   - orbit, pan, zoom-to-cursor (the point under the cursor stays within 2 px), fit, the standard views, the
//     projection toggle, the view cube and the keyboard change the camera as expected
//   - the navigation presets are selectable and map the buttons differently
//   - the selection filters (faces, edges, vertices, bodies), Ctrl/Shift, box select and Esc
//   - tree to view and view to tree selection, and the visibility toggle
//   - a frame that only answers a pick leaves the image as it was
//   - no UI-thread stall over 50 ms while a mid-size corpus file imports; no frame while idle for 1 s, nor for a
//     hover repeated at the same place (Qt Quick's while the cursor rests on the window)
//   - Esc abandons an import at once; errors show as a message; dropping a file opens it
//   - SceneController::syncFromDocument (the T-017/T-019 seam) lists features and bodies
#include "app_context.h"
#include "app_host.h"
#include "app_options.h"
#include "files.h"
#include "scene_controller.h"
#include "selection_model.h"
#include "session.h"
#include "theme_object.h"
#include "viewport_item.h"
#include "viewport_navigation.h"

#include <ray_api/viewer.h>
#include <ray_render/viewport.h>

#include <QtCore/QElapsedTimer>
#include <QtCore/QFileInfo>
#include <QtCore/QMimeData>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtGui/QDragEnterEvent>
#include <QtGui/QDropEvent>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>
#include <QtGui/QMouseEvent>
#include <QtGui/QWheelEvent>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>
#include <QtQuick/QSGRendererInterface>
#include <QtTest/QTest>

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace ray;
using ray::ui::AppContext;
using ray::ui::AppHost;
using ray::ui::AppOptions;
using ray::ui::SceneController;
using ray::ui::SelectionModel;
using ray::ui::ViewportItem;

namespace {

const QString kBoxHole = QStringLiteral(RAY_KERNEL_TEST_DATA_DIR "/box_hole.step");
const QString kAssembly = QStringLiteral(RAY_KERNEL_TEST_DATA_DIR "/assembly_mm.step");
const QString kGarbage = QStringLiteral(RAY_KERNEL_TEST_DATA_DIR "/garbage.step");
// One of the larger committed corpus files (about 0.8 MB).
const QString kMidSize = QStringLiteral(RAY_CORPUS_DIR "/nist-pmi/nist_ctc_04_asme1_rd_geometry.stp");

constexpr int kOpenTimeoutMs = 120000;

// Asked of a real, shown window: QQuickWindow::graphicsApi() only says what would be requested, and where the
// platform has no OpenGL (the offscreen platform on Linux) Qt Quick then falls back to its software backend, so a
// viewport pick would never be answered.
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

#define REQUIRE_RHI()                                                                                                  \
    do {                                                                                                               \
        if (!rhiSceneGraph())                                                                                          \
            SKIP("the scene graph has no QRhi (software backend): the viewport cannot render here");                   \
    } while (false)

// One application window with the viewer, its own cache and user data folders and a session.
struct Window {
    QTemporaryDir cache;
    QTemporaryDir userData;
    std::unique_ptr<AppHost> host;

    explicit Window(const char* theme = "dark", QSize size = QSize(1440, 900))
    {
        AppOptions options;
        options.theme = QString::fromLatin1(theme);
        options.glass = false;
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
        // A fixed tier, so the test does not depend on this machine's GPU (and skips hardware detection).
        scene()->setPreset(render::presetFor(render::Tier::Medium));
        grab();
    }

    QQuickWindow* window() const { return host->window(); }
    ViewportItem* view() const { return host->viewport(); }
    SceneController* scene() const { return host->scene(); }
    SelectionModel* selection() const { return host->selection(); }
    AppContext* context() const { return host->context(); }
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

    void checkNoWarnings() const
    {
        for (const QString& warning : host->warnings())
            UNSCOPED_INFO(warning.toStdString());
        CHECK(host->warnings().isEmpty());
    }

    bool settled() const
    {
        return !host->files()->busy() && scene()->ready() && !scene()->refining() && !view()->animating();
    }

    // Opens a file and waits until it is shown at the tier tolerance.
    void open(const QString& path)
    {
        REQUIRE(host->files()->open(path));
        REQUIRE(QTest::qWaitFor([&] { return settled(); }, kOpenTimeoutMs));
        grab();
    }

    // ---- input, in window (= scene) coordinates ----
    void mouse(QEvent::Type type, QPointF pos, Qt::MouseButton button, Qt::MouseButtons buttons,
               Qt::KeyboardModifiers mods = Qt::NoModifier) const
    {
        QMouseEvent e(type, pos, window()->mapToGlobal(pos), button, buttons, mods);
        QGuiApplication::sendEvent(window(), &e);
    }
    void click(QPointF pos, Qt::KeyboardModifiers mods = Qt::NoModifier) const
    {
        mouse(QEvent::MouseButtonPress, pos, Qt::LeftButton, Qt::LeftButton, mods);
        mouse(QEvent::MouseButtonRelease, pos, Qt::LeftButton, Qt::NoButton, mods);
    }
    // Press, wait for the press pick (the orbit pivot), move in steps, release.
    void drag(QPointF from, QPointF to, Qt::MouseButton button, Qt::KeyboardModifiers mods = Qt::NoModifier) const
    {
        mouse(QEvent::MouseButtonPress, from, button, button, mods);
        QTest::qWait(80);
        constexpr int kSteps = 8;
        for (int i = 1; i <= kSteps; ++i) {
            const QPointF p = from + (to - from) * (double(i) / kSteps);
            mouse(QEvent::MouseMove, p, Qt::NoButton, button, mods);
        }
        mouse(QEvent::MouseButtonRelease, to, button, Qt::NoButton, mods);
    }
    void wheel(QPointF pos, int angleY, Qt::KeyboardModifiers mods = Qt::NoModifier) const
    {
        QWheelEvent e(pos, window()->mapToGlobal(pos), QPoint(), QPoint(0, angleY), Qt::NoButton, mods,
                      Qt::NoScrollPhase, false);
        QGuiApplication::sendEvent(window(), &e);
    }
    void hover(QPointF pos) const { mouse(QEvent::MouseMove, pos, Qt::NoButton, Qt::NoButton); }

    // ---- picking through the viewport (synchronous for the test) ----
    std::optional<render::PickHit> pick(QPointF pos, std::uint8_t mask) const
    {
        bool done = false;
        std::optional<render::PickHit> out;
        const QPointF local = view()->mapFromScene(pos);
        view()->viewport()->pick(int(std::floor(local.x())), int(std::floor(local.y())), mask,
                                 [&](std::optional<render::PickHit> hit) {
                                     out = hit;
                                     done = true;
                                 });
        REQUIRE(QTest::qWaitFor([&] { return done; }, 5000));
        return out;
    }

    // Window position of a world point under the current camera.
    std::optional<QPointF> project(const Vec3& world) const
    {
        const auto p = ui::nav::project(view()->camera(), world, view()->width(), view()->height());
        if (!p)
            return std::nullopt;
        return view()->mapToScene(*p);
    }

    Box3 bounds() const
    {
        const auto box = view()->viewport()->sceneBounds();
        REQUIRE(box.has_value());
        return *box;
    }

    // The middle of the free viewport area (no panel over it).
    QPointF frameCentre() const
    {
        QQuickItem* area = find("viewportArea");
        REQUIRE(area != nullptr);
        return area->mapToScene(QPointF(area->width() / 2, area->height() / 2));
    }

    bool waitSelection(int count) const
    {
        return QTest::qWaitFor(
            [&] { return selection()->count() == count && !selection()->pending() && !selection()->infoBusy(); },
            10000);
    }
};

Vec3 centre(const Box3& b)
{
    return Vec3{(b.min.x + b.max.x) / 2, (b.min.y + b.max.y) / 2, (b.min.z + b.max.z) / 2};
}

double dist(Vec3 a, Vec3 b)
{
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}

double dist(QPointF a, QPointF b)
{
    return std::hypot(a.x() - b.x(), a.y() - b.y());
}

QColor pixel(const QImage& image, QPointF scenePoint)
{
    const qreal dpr = image.devicePixelRatio();
    return image.pixelColor(int(std::floor(scenePoint.x() * dpr)), int(std::floor(scenePoint.y() * dpr)));
}

bool near(const QColor& a, const QColor& b, int tolerance)
{
    return std::abs(a.red() - b.red()) <= tolerance && std::abs(a.green() - b.green()) <= tolerance &&
           std::abs(a.blue() - b.blue()) <= tolerance;
}

// A colour of the viewport background gradient (between its top and bottom tokens).
bool isBackground(const ray::ui::ThemeObject& theme, const QColor& c)
{
    const QColor top = theme.viewportBgTop();
    const QColor bottom = theme.viewportBgBottom();
    for (int i = 0; i <= 20; ++i) {
        const double t = i / 20.0;
        const QColor mix = QColor::fromRgbF(float(top.redF() + (bottom.redF() - top.redF()) * t),
                                            float(top.greenF() + (bottom.greenF() - top.greenF()) * t),
                                            float(top.blueF() + (bottom.blueF() - top.blueF()) * t));
        if (near(mix, c, 6))
            return true;
    }
    return false;
}

// The window point at the middle of the top face of the box_hole part, away from the hole.
QPointF topFacePoint(const Window& w)
{
    const Box3 b = w.bounds();
    const Vec3 p{b.min.x + (b.max.x - b.min.x) * 0.2, b.min.y + (b.max.y - b.min.y) * 0.5, b.max.z};
    const auto at = w.project(p);
    REQUIRE(at.has_value());
    return *at;
}

bool sameDirection(QVector3D a, Vec3 b, double tol = 1e-3)
{
    const double len = std::sqrt(b.x * b.x + b.y * b.y + b.z * b.z);
    return std::abs(a.x() - b.x / len) < tol && std::abs(a.y() - b.y / len) < tol && std::abs(a.z() - b.z / len) < tol;
}

void requireBoxInFrame(const Window& w)
{
    const Box3 b = w.bounds();
    QQuickItem* area = w.find("viewportArea");
    const QRectF frame = area->mapRectToScene(QRectF(0, 0, area->width(), area->height())).adjusted(-2, -2, 2, 2);
    double minX = 1e9, maxX = -1e9, minY = 1e9, maxY = -1e9;
    for (int i = 0; i < 8; ++i) {
        const Vec3 c{(i & 1) ? b.max.x : b.min.x, (i & 2) ? b.max.y : b.min.y, (i & 4) ? b.max.z : b.min.z};
        const auto p = w.project(c);
        REQUIRE(p.has_value());
        INFO("corner " << i << " at " << p->x() << ", " << p->y());
        CHECK(frame.contains(*p));
        minX = std::min(minX, p->x());
        maxX = std::max(maxX, p->x());
        minY = std::min(minY, p->y());
        maxY = std::max(maxY, p->y());
    }
    // ...and uses a good part of it.
    CHECK(std::max((maxX - minX) / frame.width(), (maxY - minY) / frame.height()) > 0.4);
}

// Every pick-able edge end or edge middle of body 0 that a pick with `mask` answers as `kind`.
std::optional<std::pair<QPointF, render::PickHit>> findPickable(const Window& w, std::uint8_t mask,
                                                                kernel::SubKind kind, bool ends)
{
    const auto mesh = w.scene()->meshOf(0);
    REQUIRE(mesh != nullptr);
    QQuickItem* area = w.find("viewportArea");
    const QRectF frame = area->mapRectToScene(QRectF(0, 0, area->width(), area->height())).adjusted(20, 60, -20, -20);
    for (const kernel::EdgeRange& edge : mesh->edges) {
        if (edge.vertexCount < 2)
            continue;
        const std::size_t first = edge.firstVertex;
        const std::size_t at = ends ? first : first + edge.vertexCount / 2;
        Vec3 p{mesh->edgePositions[at * 3], mesh->edgePositions[at * 3 + 1], mesh->edgePositions[at * 3 + 2]};
        if (!ends && edge.vertexCount == 2) {
            const std::size_t b = first + 1;
            p = Vec3{(p.x + mesh->edgePositions[b * 3]) / 2, (p.y + mesh->edgePositions[b * 3 + 1]) / 2,
                     (p.z + mesh->edgePositions[b * 3 + 2]) / 2};
        }
        const auto px = w.project(p);
        if (!px || !frame.contains(*px))
            continue;
        const auto hit = w.pick(*px, mask);
        if (hit && hit->kind == kind)
            return std::make_pair(*px, *hit);
    }
    return std::nullopt;
}

} // namespace

TEST_CASE("a STEP file opens, shows, and a face picked by pixel has a SubId and an exact TopoRef", "[e2e]")
{
    REQUIRE_RHI();
    Window w("dark");
    CHECK(w.find("startScreen")->isVisible());
    CHECK(w.find("openButton")->isEnabled());
    w.open(kBoxHole);
    CHECK_FALSE(w.find("startScreen")->isVisible());
    REQUIRE(w.scene()->bodyCount() == 1);
    CHECK(w.scene()->meshIsFine(0));

    // The model is visible where its top face projects.
    const QPointF top = topFacePoint(w);
    const QImage frame = w.grab();
    CHECK_FALSE(isBackground(*w.host->theme(), pixel(frame, top)));
    // ... and the background shows outside it.
    const QPointF corner = w.find("viewportArea")->mapToScene(QPointF(8, w.find("viewportArea")->height() - 8));
    CHECK(isBackground(*w.host->theme(), pixel(frame, corner)));

    // The tree lists the body.
    QQuickItem* row = w.find("bodyRow.0");
    REQUIRE(row != nullptr);
    CHECK(row->property("text").toString() != QString());
    CHECK_FALSE(row->property("highlighted").toBool());

    // Select the top face by pixel.
    const auto direct = w.pick(top, std::uint8_t(render::PickFilter::Faces));
    REQUIRE(direct.has_value());
    CHECK(w.scene()->bodyIndexForItem(direct->item) == 0);
    w.click(top);
    REQUIRE(w.waitSelection(1));
    const ray::ui::SelectionEntry& e = w.selection()->entries().front();
    CHECK(e.body == 0);
    CHECK(e.hit.kind == kernel::SubKind::Face);
    REQUIRE(e.sub.has_value());
    CHECK(e.sub->kind == kernel::SubKind::Face);
    CHECK(e.sub->index == e.hit.subIndex);
    REQUIRE(e.ref.has_value());
    CHECK_FALSE(e.ref->names.empty());
    CHECK(e.ref->kind == kernel::SubKind::Face);
    const naming::Resolved resolved = w.scene()->snapshot()->resolver->resolve(*e.ref);
    CHECK(resolved.state == naming::ResolveState::Exact);
    REQUIRE(resolved.elements.size() == 1);
    CHECK(resolved.elements.front() == *e.sub);

    // It is the planar top face (kernel query through the API, as the read-out does).
    JobContext ctx;
    const auto info = api::viewer::describeElement(w.scene()->body(0)->shape, *e.sub, ctx);
    REQUIRE(info.has_value());
    REQUIRE(info->surface.has_value());
    CHECK(info->surface->type == kernel::SurfaceType::Plane);
    CHECK(std::abs(std::abs(info->surface->axis.z) - 1.0) < 1e-6);

    // The properties panel shows it; the tree highlights the body.
    CHECK(w.selection()->infoTitle() == QStringLiteral("Face"));
    bool plane = false;
    for (const QVariant& r : w.selection()->info()) {
        const QVariantMap m = r.toMap();
        if (m.value("label").toString() == QStringLiteral("Surface"))
            plane = m.value("value").toString() == QStringLiteral("Plane");
    }
    CHECK(plane);
    CHECK(w.find("selectionInfoDetails")->isVisible());
    CHECK(row->property("highlighted").toBool());

    // The face is highlighted in the view.
    const QImage selected = w.grab();
    CHECK_FALSE(near(pixel(selected, top), pixel(frame, top), 6));

    // Esc clears.
    w.view()->forceActiveFocus();
    QTest::keyClick(w.window(), Qt::Key_Escape);
    CHECK(w.selection()->count() == 0);
    CHECK_FALSE(row->property("highlighted").toBool());
    w.checkNoWarnings();
}

TEST_CASE("orbit, pan, zoom to cursor, fit, standard views, projection, view cube and keyboard", "[navigation]")
{
    REQUIRE_RHI();
    Window w("light");
    w.open(kBoxHole);
    requireBoxInFrame(w);
    ViewportItem* view = w.view();
    REQUIRE(view->navigationPreset() == 0);

    SECTION("orbit turns about the model point under the cursor")
    {
        const QPointF from = topFacePoint(w);
        const auto hit = w.pick(from, std::uint8_t(render::PickFilter::Faces));
        REQUIRE(hit.has_value());
        const render::Camera before = view->camera();
        w.drag(from, from + QPointF(120, 40), Qt::MiddleButton); // RayCAD preset: middle drag orbits
        const render::Camera after = view->camera();
        CHECK(dist(before.eye, after.eye) > 1.0);
        CHECK(std::abs(dist(before.eye, hit->point) - dist(after.eye, hit->point)) <
              1e-6 * dist(before.eye, hit->point) + 1e-6);
        // The pivot keeps its place on screen.
        const auto p = w.project(hit->point);
        REQUIRE(p.has_value());
        CHECK(dist(*p, from) < 1.0);
    }

    SECTION("pan moves the content with the cursor and keeps the view direction")
    {
        const render::Camera before = view->camera();
        const auto t0 = w.project(before.target);
        REQUIRE(t0.has_value());
        const QPointF from = w.frameCentre();
        w.drag(from, from + QPointF(80, -30), Qt::RightButton); // RayCAD preset: right drag pans
        const render::Camera after = view->camera();
        const Vec3 d0{before.target.x - before.eye.x, before.target.y - before.eye.y, before.target.z - before.eye.z};
        const Vec3 d1{after.target.x - after.eye.x, after.target.y - after.eye.y, after.target.z - after.eye.z};
        CHECK(dist(d0, d1) < 1e-9 * (1 + dist(Vec3{}, d0)));
        const auto t1 = w.project(before.target);
        REQUIRE(t1.has_value());
        CHECK(dist(*t1, *t0 + QPointF(80, -30)) < 1.0);
    }

    SECTION("the wheel zooms about the point under the cursor, which stays within 2 px")
    {
        for (const bool perspective : {true, false}) {
            INFO((perspective ? "perspective" : "orthographic"));
            view->setPerspective(perspective);
            const QPointF at = topFacePoint(w) + QPointF(-30, 10);
            const auto hit = w.pick(at, std::uint8_t(render::PickFilter::Faces));
            REQUIRE(hit.has_value());
            const render::Camera before = view->camera();
            w.hover(at); // the hover pick gives the zoom its depth
            QTest::qWait(100);
            for (int i = 0; i < 3; ++i)
                w.wheel(at, 120);
            const render::Camera after = view->camera();
            if (perspective)
                CHECK(dist(after.eye, hit->point) < dist(before.eye, hit->point) * 0.8);
            else
                CHECK(after.orthoHeight < before.orthoHeight * 0.8);
            const auto p = w.project(hit->point);
            REQUIRE(p.has_value());
            CHECK(dist(*p, at) <= 2.0);
            // And out again, the other way.
            w.wheel(at, -240);
            const auto q = w.project(hit->point);
            REQUIRE(q.has_value());
            CHECK(dist(*q, at) <= 2.0);
        }
        view->setPerspective(true);
    }

    SECTION("fit frames everything in the free area; fit selection frames the selection")
    {
        view->panBy(400, 300);
        view->zoomAt(6.0, 100, 100);
        view->fitAll();
        requireBoxInFrame(w);
        // Select a face, fit to it: the face fills more of the view than the whole part did.
        w.click(topFacePoint(w));
        REQUIRE(w.waitSelection(1));
        const double before = view->camera().perspective ? dist(view->camera().eye, view->camera().target) : 0;
        view->fitSelection();
        const double after = dist(view->camera().eye, view->camera().target);
        CHECK(after <= before + 1e-6);
    }

    SECTION("standard views look along the expected axes and frame the part")
    {
        const std::vector<std::pair<const char*, Vec3>> views = {
            {"front", {0, 1, 0}}, {"back", {0, -1, 0}},  {"left", {1, 0, 0}}, {"right", {-1, 0, 0}},
            {"top", {0, 0, -1}},  {"bottom", {0, 0, 1}}, {"iso", {-1, 1, -1}}};
        for (const auto& [name, dir] : views) {
            INFO("view " << name);
            REQUIRE(view->setStandardView(QString::fromLatin1(name)));
            CHECK(sameDirection(view->viewForward(), dir));
            requireBoxInFrame(w);
        }
        CHECK_FALSE(view->setStandardView(QStringLiteral("sideways")));
    }

    SECTION("perspective and orthographic keep the scale at the target")
    {
        REQUIRE(view->perspective());
        const render::Camera p = view->camera();
        view->toggleProjection();
        REQUIRE_FALSE(view->perspective());
        const render::Camera o = view->camera();
        const double expected = 2.0 * dist(p.eye, p.target) * std::tan(p.fovDeg * 3.14159265358979323846 / 360.0);
        CHECK(std::abs(o.orthoHeight - expected) < 1e-6 * expected);
        QQuickItem* button = w.find("projectionButton");
        REQUIRE(button != nullptr);
        REQUIRE(QMetaObject::invokeMethod(button, "clicked"));
        CHECK(view->perspective());
    }

    SECTION("transitions glide and end on the target camera")
    {
        view->setAnimationsEnabled(true);
        view->setStandardView(QStringLiteral("top"));
        CHECK(view->animating());
        REQUIRE(QTest::qWaitFor([&] { return !view->animating(); }, 2000));
        CHECK(sameDirection(view->viewForward(), Vec3{0, 0, -1}));
        view->setAnimationsEnabled(false);
    }

    SECTION("the view cube: a face click gives that view, a corner a diagonal view")
    {
        view->setStandardView(QStringLiteral("iso"));
        w.grab();
        QQuickItem* face = w.find("viewCubeFace.top");
        REQUIRE(face != nullptr);
        REQUIRE(face->isVisible());
        const QVariantList pts = face->property("pts").toList();
        REQUIRE(pts.size() == 5);
        QPointF mid;
        for (int i = 0; i < 4; ++i)
            mid += pts[i].toPointF() / 4.0;
        w.click(face->mapToScene(mid));
        CHECK(sameDirection(view->viewForward(), Vec3{0, 0, -1}));

        view->setStandardView(QStringLiteral("front"));
        w.grab();
        QQuickItem* right = w.find("viewCubeFace.right");
        REQUIRE(right != nullptr);
        CHECK_FALSE(right->isVisible()); // seen edge-on from the front

        view->setStandardView(QStringLiteral("iso"));
        w.grab();
        bool clicked = false;
        for (int i = 0; i < 8 && !clicked; ++i) {
            QQuickItem* corner = w.find(("viewCubeCorner." + std::to_string(i)).c_str());
            REQUIRE(corner != nullptr);
            if (!corner->isVisible())
                continue;
            const QPointF c = corner->mapToScene(QPointF(corner->width() / 2, corner->height() / 2));
            const QVector3D v = corner->property("modelData").value<QVector3D>();
            w.click(c);
            CHECK(sameDirection(view->viewForward(), Vec3{-v.x(), -v.y(), -v.z()}));
            clicked = true;
        }
        CHECK(clicked);
    }

    SECTION("the keyboard: 7 top, 1 front, 3 right, 0 iso, P projection, F fit")
    {
        view->forceActiveFocus();
        QTest::keyClick(w.window(), Qt::Key_7);
        CHECK(sameDirection(view->viewForward(), Vec3{0, 0, -1}));
        QTest::keyClick(w.window(), Qt::Key_1);
        CHECK(sameDirection(view->viewForward(), Vec3{0, 1, 0}));
        QTest::keyClick(w.window(), Qt::Key_3);
        CHECK(sameDirection(view->viewForward(), Vec3{-1, 0, 0}));
        QTest::keyClick(w.window(), Qt::Key_7, Qt::ControlModifier);
        CHECK(sameDirection(view->viewForward(), Vec3{0, 0, 1}));
        QTest::keyClick(w.window(), Qt::Key_0);
        CHECK(sameDirection(view->viewForward(), Vec3{-1, 1, -1}));
        QTest::keyClick(w.window(), Qt::Key_P);
        CHECK_FALSE(view->perspective());
        QTest::keyClick(w.window(), Qt::Key_P);
        CHECK(view->perspective());
        const render::Camera before = view->camera();
        QTest::keyClick(w.window(), Qt::Key_Left);
        CHECK(dist(before.eye, view->camera().eye) > 1.0);
        view->panBy(300, 0);
        QTest::keyClick(w.window(), Qt::Key_F);
        requireBoxInFrame(w);
    }
    w.checkNoWarnings();
}

TEST_CASE("navigation presets are selectable and map the buttons differently", "[navigation][presets]")
{
    REQUIRE_RHI();
    Window w("dark");
    w.open(kBoxHole);
    ViewportItem* view = w.view();
    const QStringList names = view->navigationPresetNames();
    REQUIRE(names.size() == 3);
    CHECK(names[0] == QStringLiteral("RayCAD"));
    CHECK(names[1] == QStringLiteral("Middle-button orbit"));
    CHECK(names[2] == QStringLiteral("Right-button orbit"));
    CHECK(view->navigationPresetDescriptions().size() == 3);

    const QPointF from = w.frameCentre();
    auto direction = [&] {
        const render::Camera c = view->camera();
        return Vec3{c.target.x - c.eye.x, c.target.y - c.eye.y, c.target.z - c.eye.z};
    };
    auto rightDragTurns = [&] {
        view->setStandardView(QStringLiteral("iso"));
        const Vec3 d0 = direction();
        w.drag(from, from + QPointF(100, 0), Qt::RightButton);
        return dist(d0, direction()) > 1e-6 * (1 + dist(Vec3{}, d0));
    };
    auto middleDragTurns = [&] {
        view->setStandardView(QStringLiteral("iso"));
        const Vec3 d0 = direction();
        w.drag(from, from + QPointF(100, 0), Qt::MiddleButton);
        return dist(d0, direction()) > 1e-6 * (1 + dist(Vec3{}, d0));
    };

    // RayCAD: middle orbits, right pans.
    CHECK(middleDragTurns());
    CHECK_FALSE(rightDragTurns());

    // Picked from the view options menu.
    QQuickItem* shell = w.find("shell");
    REQUIRE(QMetaObject::invokeMethod(shell, "openViewMenu"));
    QQuickItem* item = nullptr;
    REQUIRE(QTest::qWaitFor(
        [&] {
            item = w.find("navigationPreset.2");
            return item != nullptr;
        },
        2000));
    REQUIRE(QMetaObject::invokeMethod(item, "triggered"));
    CHECK(view->navigationPreset() == 2);
    // Right-button orbit: right orbits, middle pans.
    CHECK(rightDragTurns());
    CHECK_FALSE(middleDragTurns());

    view->setNavigationPreset(1);
    // Middle-button orbit: middle orbits; Ctrl+middle pans; right does nothing to the camera.
    CHECK(middleDragTurns());
    CHECK_FALSE(rightDragTurns());
    {
        view->setStandardView(QStringLiteral("iso"));
        const Vec3 d0 = direction();
        const render::Camera c0 = view->camera();
        w.drag(from, from + QPointF(100, 0), Qt::MiddleButton, Qt::ControlModifier);
        CHECK(dist(d0, direction()) < 1e-6 * (1 + dist(Vec3{}, d0)));
        CHECK(dist(c0.eye, view->camera().eye) > 1e-3);
    }
    // Every preset selects with the left button.
    for (int p = 0; p < 3; ++p) {
        view->setNavigationPreset(p);
        view->setStandardView(QStringLiteral("iso"));
        w.selection()->clear();
        w.click(topFacePoint(w));
        CHECK(w.waitSelection(1));
    }
    w.checkNoWarnings();
}

TEST_CASE("selection filters, modifiers, box select", "[selection]")
{
    REQUIRE_RHI();
    Window w("dark");
    w.open(kBoxHole);
    SelectionModel& sel = *w.selection();
    const auto faces = std::uint8_t(render::PickFilter::Faces);
    const auto edges = std::uint8_t(render::PickFilter::Edges);
    const auto vertices = std::uint8_t(render::PickFilter::Vertices);
    const auto bodies = std::uint8_t(render::PickFilter::Bodies);
    const QPointF top = topFacePoint(w);

    SECTION("edges only: a face interior picks nothing, an edge picks the edge")
    {
        sel.setFilterMask(edges);
        CHECK(sel.edges());
        CHECK_FALSE(sel.faces());
        w.click(top);
        QTest::qWait(150);
        CHECK(sel.count() == 0);
        const auto edge = findPickable(w, edges, kernel::SubKind::Edge, false);
        REQUIRE(edge.has_value());
        w.click(edge->first);
        REQUIRE(w.waitSelection(1));
        const ray::ui::SelectionEntry& e = sel.entries().front();
        CHECK(e.hit.kind == kernel::SubKind::Edge);
        REQUIRE(e.sub.has_value());
        CHECK(*e.sub == (kernel::SubId{kernel::SubKind::Edge, edge->second.subIndex}));
        REQUIRE(e.ref.has_value());
        const naming::Resolved r = w.scene()->snapshot()->resolver->resolve(*e.ref);
        CHECK(r.state == naming::ResolveState::Exact);
        CHECK(r.elements == std::vector<kernel::SubId>{*e.sub});
        bool length = false;
        for (const QVariant& row : sel.info())
            length = length || row.toMap().value("label").toString() == QStringLiteral("Length");
        CHECK(length);
    }

    SECTION("vertices only: an edge end picks the kernel vertex at that point")
    {
        sel.setFilterMask(vertices);
        const auto vertex = findPickable(w, vertices, kernel::SubKind::Vertex, true);
        REQUIRE(vertex.has_value());
        w.click(vertex->first);
        REQUIRE(w.waitSelection(1));
        const ray::ui::SelectionEntry& e = sel.entries().front();
        CHECK(e.hit.kind == kernel::SubKind::Vertex);
        REQUIRE(e.sub.has_value());
        CHECK(e.sub->kind == kernel::SubKind::Vertex);
        JobContext ctx;
        const auto info = api::viewer::describeElement(w.scene()->body(0)->shape, *e.sub, ctx);
        REQUIRE(info.has_value());
        REQUIRE(info->point.has_value());
        CHECK(dist(*info->point, e.hit.point) < 1e-3);
        REQUIRE(e.ref.has_value());
        const naming::Resolved r = w.scene()->snapshot()->resolver->resolve(*e.ref);
        CHECK(r.state == naming::ResolveState::Exact);
        CHECK(r.elements == std::vector<kernel::SubId>{*e.sub});
    }

    SECTION("bodies only: a click picks the whole body")
    {
        sel.setFilterMask(bodies);
        w.click(top);
        REQUIRE(w.waitSelection(1));
        CHECK(sel.entries().front().hit.kind == kernel::SubKind::Solid);
        CHECK_FALSE(sel.entries().front().ref.has_value()); // a body is held by its id
        bool volume = false;
        for (const QVariant& row : sel.info())
            volume = volume || row.toMap().value("label").toString() == QStringLiteral("Volume");
        CHECK(volume);
    }

    SECTION("faces only: near an edge still picks the face")
    {
        sel.setFilterMask(faces);
        w.click(top);
        REQUIRE(w.waitSelection(1));
        CHECK(sel.entries().front().hit.kind == kernel::SubKind::Face);
    }

    SECTION("the filter bar toggles the filter")
    {
        QQuickItem* bar = w.find("selectionFilter");
        REQUIRE(bar != nullptr);
        CHECK(bar->isVisible());
        QQuickItem* faceToggle = w.find("filterFaces");
        REQUIRE(faceToggle != nullptr);
        CHECK(faceToggle->property("checked").toBool());
        REQUIRE(QMetaObject::invokeMethod(faceToggle, "toggle"));
        REQUIRE(QMetaObject::invokeMethod(faceToggle, "toggled"));
        CHECK_FALSE(sel.faces());
        w.find("filterBodies")->setProperty("checked", true);
        REQUIRE(QMetaObject::invokeMethod(w.find("filterBodies"), "toggled"));
        CHECK(sel.bodies());
    }

    SECTION("Ctrl toggles, Shift adds, a click on nothing clears, a box selects many")
    {
        sel.setFilterMask(faces);
        w.click(top);
        REQUIRE(w.waitSelection(1));
        // Another visible face: the hole wall, from a pick around the model.
        const Box3 b = w.bounds();
        const auto side = w.project(Vec3{b.max.x, (b.min.y + b.max.y) / 2, (b.min.z + b.max.z) / 2});
        REQUIRE(side.has_value());
        const QPointF sidePoint = *side + (w.frameCentre() - *side) * 0.05;
        const auto sideHit = w.pick(sidePoint, faces);
        REQUIRE(sideHit.has_value());
        w.click(sidePoint, Qt::ShiftModifier);
        REQUIRE(w.waitSelection(2));
        w.click(sidePoint, Qt::ControlModifier);
        REQUIRE(w.waitSelection(1));
        w.click(sidePoint, Qt::ControlModifier);
        REQUIRE(w.waitSelection(2));
        // A click on empty background clears.
        const QPointF empty = w.find("viewportArea")->mapToScene(QPointF(10, w.find("viewportArea")->height() - 10));
        REQUIRE_FALSE(w.pick(empty, faces).has_value());
        w.click(empty);
        REQUIRE(w.waitSelection(0));
        // A box over the whole part selects every visible face.
        QQuickItem* area = w.find("viewportArea");
        const QPointF a = area->mapToScene(QPointF(30, 70));
        const QPointF c = area->mapToScene(QPointF(area->width() - 30, area->height() - 30));
        w.drag(a, c, Qt::LeftButton);
        REQUIRE(QTest::qWaitFor([&] { return sel.count() >= 3 && !sel.pending(); }, 5000));
        CHECK_FALSE(w.view()->boxActive());
        // Esc (in the view) clears.
        w.view()->forceActiveFocus();
        QTest::keyClick(w.window(), Qt::Key_Escape);
        CHECK(sel.count() == 0);
    }
    w.checkNoWarnings();
}

TEST_CASE("tree to view and view to tree; visibility", "[tree]")
{
    REQUIRE_RHI();
    Window w("light");
    w.open(kAssembly);
    REQUIRE(w.scene()->bodyCount() >= 2);
    QQuickItem* row0 = w.find("bodyRow.0");
    QQuickItem* row1 = w.find("bodyRow.1");
    REQUIRE(row0 != nullptr);
    REQUIRE(row1 != nullptr);

    // Tree to view: clicking a body row selects that body and highlights it in the view.
    const QImage before = w.grab();
    REQUIRE(QMetaObject::invokeMethod(row1, "clicked"));
    REQUIRE(w.waitSelection(1));
    CHECK(w.selection()->entries().front().body == 1);
    CHECK(w.selection()->entries().front().hit.kind == kernel::SubKind::Solid);
    CHECK(row1->property("highlighted").toBool());
    CHECK_FALSE(row0->property("highlighted").toBool());
    const std::vector<render::PickHit> hits = w.selection()->hits();
    const auto box = w.scene()->boundsOf(hits);
    REQUIRE(box.has_value());
    const auto at = w.project(centre(*box));
    REQUIRE(at.has_value());
    const QImage after = w.grab();
    CHECK(before != after);
    w.selection()->selectBody(0, int(Qt::ControlModifier));
    REQUIRE(w.waitSelection(2));
    CHECK(row0->property("highlighted").toBool());

    // View to tree: a pick in the view highlights its body's row.
    w.selection()->clear();
    CHECK_FALSE(row0->property("highlighted").toBool());
    const std::vector<render::PickHit> body0 = {
        render::PickHit{w.scene()->itemForBody(0), kernel::SubKind::Solid, 0, Vec3{}, 0}};
    const auto box0 = w.scene()->boundsOf(body0);
    REQUIRE(box0.has_value());
    // Look for a pixel of body 0 near its centre.
    bool picked = false;
    const auto c0 = w.project(centre(*box0));
    REQUIRE(c0.has_value());
    for (int dy = -40; dy <= 40 && !picked; dy += 8) {
        for (int dx = -40; dx <= 40 && !picked; dx += 8) {
            const QPointF p = *c0 + QPointF(dx, dy);
            // The selection's own filter: near an edge of another body, that edge would win.
            const auto hit = w.pick(p, w.selection()->filterMask());
            if (hit && w.scene()->bodyIndexForItem(hit->item) == 0) {
                w.click(p);
                picked = true;
            }
        }
    }
    REQUIRE(picked);
    REQUIRE(w.waitSelection(1));
    CHECK(w.selection()->entries().front().body == 0);
    CHECK(row0->property("highlighted").toBool());
    CHECK_FALSE(row1->property("highlighted").toBool());

    // The eye hides the body in the view.
    w.selection()->clear();
    QQuickItem* eye = w.find("visibilityToggle.0");
    REQUIRE(eye != nullptr);
    const QImage shown = w.grab();
    w.click(eye->mapToScene(QPointF(eye->width() / 2, eye->height() / 2)));
    CHECK_FALSE(w.scene()->bodyVisible(0));
    CHECK(w.selection()->count() == 0); // the eye does not select the row
    const QImage hidden = w.grab();
    CHECK(shown != hidden);
    w.scene()->setBodyVisible(0, true);
    CHECK(w.scene()->bodyVisible(0));
    w.checkNoWarnings();
}

TEST_CASE("a frame that only answers a pick leaves the image as it was", "[pick]")
{
    REQUIRE_RHI();
    Window w("dark");
    w.open(kBoxHole);
    QTest::qWait(200);
    const QImage a = w.grab();

    // A programmatic pick: no highlight change, so the renderer only reads back the ID buffer.
    const std::uint64_t picksBefore = w.view()->viewport()->info().pickBufferRenders;
    REQUIRE(w.pick(topFacePoint(w), std::uint8_t(render::PickFilter::Faces)).has_value());
    QTest::qWait(100);
    const QImage b = w.grab();
    CHECK(a == b);
    CHECK(w.view()->viewport()->info().pickBufferRenders >= picksBefore);

    // A hover over empty background: a pick-only frame too.
    const QPointF empty = w.find("viewportArea")->mapToScene(QPointF(12, w.find("viewportArea")->height() - 12));
    w.hover(empty);
    QTest::qWait(150);
    CHECK_FALSE(w.selection()->hover().has_value());
    const QImage c = w.grab();
    CHECK(a == c);

    // A hover over the part highlights it; away from the part the image is unchanged.
    w.hover(topFacePoint(w));
    REQUIRE(QTest::qWaitFor([&] { return w.selection()->hover().has_value(); }, 3000));
    QTest::qWait(100);
    const QImage d = w.grab();
    CHECK(a != d);
    const Box3 box = w.bounds();
    QRectF part;
    for (int i = 0; i < 8; ++i) {
        const Vec3 corner{(i & 1) ? box.max.x : box.min.x, (i & 2) ? box.max.y : box.min.y,
                          (i & 4) ? box.max.z : box.min.z};
        const auto p = w.project(corner);
        REQUIRE(p.has_value());
        part = part.isNull() ? QRectF(*p, QSizeF(1, 1)) : part.united(QRectF(*p, QSizeF(1, 1)));
    }
    part.adjust(-8, -8, 8, 8);
    QQuickItem* area = w.find("viewportArea");
    const QRectF free = area->mapRectToScene(QRectF(0, 0, area->width(), area->height()));
    const qreal dpr = d.devicePixelRatio();
    int compared = 0;
    int differing = 0;
    for (int y = int(free.top()) + 4; y < int(free.bottom()) - 4; y += 6) {
        for (int x = int(free.left()) + 4; x < int(free.right()) - 4; x += 6) {
            if (part.contains(QPointF(x, y)))
                continue;
            // The view cube, its buttons, the filter bar and the triad are not the viewport.
            bool overlay = false;
            for (const char* name : {"viewCubeColumn", "selectionFilter", "axisTriad"}) {
                QQuickItem* o = w.find(name);
                if (o && o->mapRectToScene(QRectF(0, 0, o->width(), o->height()))
                             .adjusted(-4, -4, 4, 4)
                             .contains(QPointF(x, y)))
                    overlay = true;
            }
            if (overlay)
                continue;
            ++compared;
            if (a.pixel(int(x * dpr), int(y * dpr)) != d.pixel(int(x * dpr), int(y * dpr)))
                ++differing;
        }
    }
    CHECK(compared > 1000);
    CHECK(differing == 0);
    w.checkNoWarnings();
}

TEST_CASE("no UI-thread stall over 50 ms while a mid-size corpus file imports; idle draws nothing", "[stall][idle]")
{
    REQUIRE_RHI();
    Window w("dark");
    REQUIRE(QFileInfo::exists(kMidSize));

    // Watchdog: a 2 ms timer on the UI thread; a gap between ticks is time the event loop was blocked.
    QElapsedTimer clock;
    qint64 last = 0;
    qint64 worst = 0;
    QTimer tick;
    tick.setTimerType(Qt::PreciseTimer);
    tick.setInterval(2);
    QObject::connect(&tick, &QTimer::timeout, [&] {
        const qint64 now = clock.elapsed();
        worst = std::max(worst, now - last);
        last = now;
    });
    clock.start();
    tick.start();
    REQUIRE(w.host->files()->open(kMidSize));
    REQUIRE(QTest::qWaitFor([&] { return w.settled(); }, kOpenTimeoutMs));
    // A few frames after the meshes arrived (uploads happen on the render thread).
    QTest::qWait(300);
    tick.stop();
    INFO("worst UI-thread gap " << worst << " ms");
    CHECK(worst < 50);
    CHECK(w.scene()->bodyCount() >= 1);

    // Idle: no frame for a second.
    w.grab();
    QTest::qWait(300);
    int swapped = 0;
    QObject::connect(w.window(), &QQuickWindow::frameSwapped, w.window(), [&] { ++swapped; }, Qt::DirectConnection);
    const std::uint64_t frames = w.view()->viewport()->info().framesRendered;
    QTest::qWait(1000);
    CHECK(w.view()->viewport()->info().framesRendered == frames);
    CHECK(swapped == 0);

    // While the cursor rests on the window, Qt Quick sends the hover again, at the same place, after frames. The
    // view picks a place once: the same hover again neither picks nor draws (picking would ask for a frame, and so
    // for the next such hover, for ever), until the picture under the cursor changes.
    const QPointF centre = w.frameCentre();
    w.hover(centre); // Qt Quick's own repeated hovers now come here too, not where the real cursor is
    const QPointF at = w.view()->mapFromScene(centre);
    const auto hoverAgain = [&] {
        QHoverEvent e(QEvent::HoverMove, at, w.view()->mapToGlobal(at), at);
        QCoreApplication::sendEvent(w.view(), &e);
    };
    hoverAgain();
    QTest::qWait(300);
    const render::ViewportInfo rested = w.view()->viewport()->info();
    swapped = 0;
    for (int i = 0; i < 5; ++i) {
        hoverAgain();
        QTest::qWait(100);
    }
    CHECK(w.view()->viewport()->info().framesRendered == rested.framesRendered);
    CHECK(swapped == 0);
    // After a camera change the same place is picked again (the picking ID buffer is drawn anew for it).
    w.view()->orbitBy(10, 0);
    hoverAgain();
    CHECK(QTest::qWaitFor([&] { return w.view()->viewport()->info().pickBufferRenders > rested.pickBufferRenders; },
                          3000));
}

TEST_CASE("Esc abandons an import at once; a failed open shows a message; a dropped file opens", "[open]")
{
    REQUIRE_RHI();
    Window w("dark");
    AppContext& context = *w.context();

    // Esc while importing: the job slot clears at once and the result is dropped.
    REQUIRE(w.host->files()->open(kMidSize));
    CHECK(w.host->session()->jobActive());
    QElapsedTimer t;
    t.start();
    w.view()->forceActiveFocus();
    QTest::keyClick(w.window(), Qt::Key_Escape);
    CHECK(t.elapsed() < 100);
    CHECK_FALSE(w.host->session()->jobActive());
    CHECK_FALSE(w.host->files()->busy());
    QTest::qWait(3000);
    CHECK(w.scene()->bodyCount() == 0);
    CHECK(w.find("startScreen")->isVisible());

    // A file that is not STEP: a message, no crash.
    REQUIRE(w.host->files()->open(kGarbage));
    REQUIRE(QTest::qWaitFor([&] { return !w.host->files()->busy(); }, 30000));
    CHECK_FALSE(context.message().isEmpty());
    CHECK(context.messageIsError());
    CHECK(w.find("messageBanner")->isVisible());
    REQUIRE(QMetaObject::invokeMethod(w.find("messageClose"), "clicked"));
    CHECK(context.message().isEmpty());
    // A file that is not there: the open starts, and the API's error shows in the banner.
    REQUIRE(w.host->files()->open(QStringLiteral(RAY_KERNEL_TEST_DATA_DIR "/no_such_file.step")));
    REQUIRE(QTest::qWaitFor([&] { return !w.host->files()->busy(); }, 30000));
    CHECK_FALSE(context.message().isEmpty());
    CHECK_FALSE(w.host->files()->open(QStringLiteral(RAY_CORPUS_DIR "/ezdxf/Minimal_DXF_AC1009.dxf")));
    context.dismissMessage();

    // Drag and drop onto the window.
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(kBoxHole)});
    const QPoint at = w.frameCentre().toPoint();
    QDragEnterEvent enter(QPointF(at), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QGuiApplication::sendEvent(w.window(), &enter);
    CHECK(enter.isAccepted());
    QDropEvent drop(QPointF(at), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QGuiApplication::sendEvent(w.window(), &drop);
    REQUIRE(QTest::qWaitFor([&] { return w.settled() && w.scene()->bodyCount() == 1; }, kOpenTimeoutMs));
    CHECK(w.scene()->sourceName() == QStringLiteral("box_hole"));
    w.checkNoWarnings();
}

TEST_CASE("syncFromDocument lists the document's features and bodies (the T-017/T-019 seam)", "[document]")
{
    REQUIRE_RHI();
    Window w("dark");
    JobContext ctx;
    auto model = api::viewer::importModel(kBoxHole.toStdString(), ctx);
    REQUIRE(model.has_value());
    REQUIRE(model->bodies.size() == 1);

    auto state = std::make_shared<doc::DocState>();
    doc::Feature feature;
    feature.id = FeatureId{Uuid::generate()};
    feature.type = "import";
    feature.name = "Import 1";
    feature.propsJson = "{}";
    state->features.push_back(feature);
    doc::Body body;
    body.id = model->bodies[0].id;
    body.name = "Plate";
    body.role = doc::Body::Role::Part;
    body.color = {0.7f, 0.7f, 0.7f, 1.0f};
    state->bodies.push_back(body);
    auto build = std::make_shared<doc::BuildResult>();
    build->bodies[body.id] = doc::BuiltBody{model->bodies[0].shape, *model->bodies[0].map};
    build->reports.push_back(doc::FeatureReport{feature.id, doc::FeatureStatus::Ok, {}, 0.1, false});

    w.scene()->syncFromDocument(state, build);
    CHECK(w.scene()->hasDocument());
    REQUIRE(w.scene()->bodyCount() == 1);
    CHECK(w.scene()->bodyName(0) == QStringLiteral("Plate"));
    REQUIRE(QTest::qWaitFor([&] { return w.settled(); }, kOpenTimeoutMs));
    QAbstractItemModel* tree = w.scene()->tree();
    REQUIRE(tree->rowCount() == 4); // Features, Import 1, Bodies, Plate
    const QHash<int, QByteArray> roles = tree->roleNames();
    auto role = [&](const char* name) {
        return roles.key(QByteArray(name));
    };
    CHECK(tree->data(tree->index(0, 0), role("rowKind")).toString() == QStringLiteral("header"));
    CHECK(tree->data(tree->index(1, 0), role("rowName")).toString() == QStringLiteral("Import 1"));
    CHECK(tree->data(tree->index(3, 0), role("rowName")).toString() == QStringLiteral("Plate"));
    // Collapsing a section hides its rows.
    qobject_cast<ray::ui::SceneTreeModel*>(tree)->toggleSection(QStringLiteral("features"));
    CHECK(tree->rowCount() == 3);
    // A pick in the view makes a TopoRef through the document's element map.
    w.click(topFacePoint(w));
    REQUIRE(w.waitSelection(1));
    REQUIRE(w.selection()->entries().front().ref.has_value());
    CHECK(w.selection()->entries().front().ref->body == body.id);
    w.checkNoWarnings();
}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    return Catch::Session().run(argc, argv);
}
