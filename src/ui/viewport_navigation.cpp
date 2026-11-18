#include "viewport_navigation.h"

#include <QtCore/QtGlobal>

#include <algorithm>
#include <cmath>

namespace ray::ui::nav {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDegToRad = kPi / 180.0;

Vec3 add(Vec3 a, Vec3 b)
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
Vec3 sub(Vec3 a, Vec3 b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
Vec3 mul(Vec3 a, double s)
{
    return {a.x * s, a.y * s, a.z * s};
}
double dot(Vec3 a, Vec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
Vec3 cross(Vec3 a, Vec3 b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
double length(Vec3 a)
{
    return std::sqrt(dot(a, a));
}
Vec3 normalized(Vec3 a, Vec3 fallback)
{
    const double len = length(a);
    return len > 1e-12 ? mul(a, 1.0 / len) : fallback;
}
Vec3 rotate(Vec3 v, Vec3 k, double angle)
{
    const double c = std::cos(angle);
    const double s = std::sin(angle);
    return add(add(mul(v, c), mul(cross(k, v), s)), mul(k, dot(k, v) * (1.0 - c)));
}
double lerp(double a, double b, double t)
{
    return a + (b - a) * t;
}

// Unit vectors u to v along the great circle; `hint` gives the axis when they are opposite.
Vec3 slerp(Vec3 u, Vec3 v, double t, Vec3 hint)
{
    const double c = std::clamp(dot(u, v), -1.0, 1.0);
    const double angle = std::acos(c);
    if (angle < 1e-9)
        return v;
    Vec3 axis = cross(u, v);
    if (length(axis) < 1e-9) {
        axis = cross(u, hint);
        if (length(axis) < 1e-9)
            axis = cross(u, std::abs(u.z) < 0.9 ? Vec3{0, 0, 1} : Vec3{1, 0, 0});
    }
    return normalized(rotate(u, normalized(axis, Vec3{0, 0, 1}), angle * t), v);
}

double fovRad(const render::Camera& cam)
{
    return std::clamp(cam.fovDeg, 1.0, 150.0) * kDegToRad;
}

constexpr Qt::KeyboardModifiers kNone = Qt::NoModifier;

} // namespace

const std::vector<Preset>& presets()
{
    static const std::vector<Preset> all = {
        {"raycad",
         QT_TRANSLATE_NOOP("ray::ui::ViewportItem", "RayCAD"),
         QT_TRANSLATE_NOOP("ray::ui::ViewportItem", "Middle drag orbits, right drag pans, Shift+middle pans, "
                                                    "Ctrl+middle zooms; Alt+left orbits on a touchpad"),
         {{Qt::MiddleButton, kNone, Action::Orbit},
          {Qt::RightButton, kNone, Action::Pan},
          {Qt::MiddleButton, Qt::ShiftModifier, Action::Pan},
          {Qt::MiddleButton, Qt::ControlModifier, Action::Zoom},
          {Qt::LeftButton, Qt::AltModifier, Action::Orbit},
          {Qt::LeftButton, Qt::AltModifier | Qt::ShiftModifier, Action::Pan},
          {Qt::LeftButton, Qt::AltModifier | Qt::ControlModifier, Action::Zoom}}},
        {"middle-orbit",
         QT_TRANSLATE_NOOP("ray::ui::ViewportItem", "Middle-button orbit"),
         QT_TRANSLATE_NOOP(
             "ray::ui::ViewportItem",
             "Middle drag orbits, Ctrl+middle pans, Shift+middle zooms; the right button is left for menus"),
         {{Qt::MiddleButton, kNone, Action::Orbit},
          {Qt::MiddleButton, Qt::ControlModifier, Action::Pan},
          {Qt::MiddleButton, Qt::ShiftModifier, Action::Zoom}}},
        {"right-orbit",
         QT_TRANSLATE_NOOP("ray::ui::ViewportItem", "Right-button orbit"),
         QT_TRANSLATE_NOOP("ray::ui::ViewportItem",
                           "Right drag orbits, middle drag pans, Shift+right pans, Ctrl+right zooms"),
         {{Qt::RightButton, kNone, Action::Orbit},
          {Qt::MiddleButton, kNone, Action::Pan},
          {Qt::RightButton, Qt::ShiftModifier, Action::Pan},
          {Qt::RightButton, Qt::ControlModifier, Action::Zoom},
          {Qt::MiddleButton, Qt::ControlModifier, Action::Zoom}}},
    };
    return all;
}

Action actionFor(const Preset& preset, Qt::MouseButton button, Qt::KeyboardModifiers modifiers)
{
    const Qt::KeyboardModifiers mods = modifiers & (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier);
    for (const Binding& b : preset.drags) {
        if (b.button == button && b.modifiers == mods)
            return b.action;
    }
    if (button == Qt::LeftButton && !mods.testFlag(Qt::AltModifier))
        return Action::Select;
    return Action::None;
}

Frame frameOf(const render::Camera& cam)
{
    Frame f{};
    const Vec3 toTarget = sub(cam.target, cam.eye);
    f.distance = length(toTarget);
    f.forward = normalized(toTarget, Vec3{0, 1, 0});
    Vec3 up = normalized(cam.up, Vec3{0, 0, 1});
    f.right = cross(f.forward, up);
    if (length(f.right) < 1e-9) {
        up = std::abs(f.forward.z) < 0.9 ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
        f.right = cross(f.forward, up);
    }
    f.right = normalized(f.right, Vec3{1, 0, 0});
    f.up = cross(f.right, f.forward);
    return f;
}

render::Camera interpolate(const render::Camera& from, const render::Camera& to, double t)
{
    if (t <= 0)
        return from;
    if (t >= 1)
        return to;
    const Frame a = frameOf(from);
    const Frame b = frameOf(to);
    render::Camera out = to;
    const Vec3 target{lerp(from.target.x, to.target.x, t), lerp(from.target.y, to.target.y, t),
                      lerp(from.target.z, to.target.z, t)};
    const double distance = lerp(a.distance, b.distance, t);
    const Vec3 forward = slerp(a.forward, b.forward, t, a.up);
    Vec3 up = slerp(a.up, b.up, t, a.forward);
    up = normalized(sub(up, mul(forward, dot(up, forward))), b.up);
    out.target = target;
    out.eye = sub(target, mul(forward, distance));
    out.up = up;
    out.orthoHeight = lerp(from.orthoHeight, to.orthoHeight, t);
    out.fovDeg = lerp(from.fovDeg, to.fovDeg, t);
    return out;
}

render::Camera withProjection(const render::Camera& cam, bool perspective)
{
    if (cam.perspective == perspective)
        return cam;
    render::Camera out = cam;
    const Frame f = frameOf(cam);
    const double halfTan = std::tan(fovRad(cam) * 0.5);
    if (!perspective) {
        out.orthoHeight = std::max(1e-6, 2.0 * f.distance * halfTan);
    } else {
        const double distance = std::max(1e-6, cam.orthoHeight / (2.0 * halfTan));
        out.eye = sub(cam.target, mul(f.forward, distance));
    }
    out.perspective = perspective;
    return out;
}

render::Camera viewAlong(const render::Camera& cam, Vec3 direction, const Box3& box, double aspect)
{
    render::Camera out = cam;
    const Vec3 dir = normalized(direction, Vec3{0, 1, 0});
    Vec3 up{0, 0, 1};
    if (std::abs(dir.z) > 0.999)
        up = dir.z < 0 ? Vec3{0, 1, 0} : Vec3{0, -1, 0};
    const Vec3 centre = mul(add(box.min, box.max), 0.5);
    const double radius = std::max(length(sub(box.max, box.min)) * 0.5, 1e-6);
    out.target = centre;
    out.eye = sub(centre, mul(dir, 4.0 * radius));
    out.up = up;
    return render::fitCamera(out, box, aspect);
}

std::optional<Vec3> rayPlane(const render::Ray& ray, Vec3 point, Vec3 normal)
{
    const double denom = dot(ray.dir, normal);
    if (std::abs(denom) < 1e-12)
        return std::nullopt;
    const double t = dot(sub(point, ray.origin), normal) / denom;
    if (!(t >= 0) || !std::isfinite(t))
        return std::nullopt;
    return add(ray.origin, mul(ray.dir, t));
}

std::optional<QPointF> project(const render::Camera& cam, Vec3 world, double widthPx, double heightPx)
{
    const Frame f = frameOf(cam);
    const Vec3 d = sub(world, cam.eye);
    const double xc = dot(d, f.right);
    const double yc = dot(d, f.up);
    const double zc = dot(d, f.forward);
    const double w = std::max(1.0, widthPx);
    const double h = std::max(1.0, heightPx);
    const double aspect = w / h;
    double ndcX = 0;
    double ndcY = 0;
    if (cam.perspective) {
        if (zc <= 1e-9)
            return std::nullopt;
        const double t = std::tan(fovRad(cam) * 0.5);
        ndcX = xc / (zc * t * aspect);
        ndcY = yc / (zc * t);
    } else {
        const double halfH = cam.orthoHeight * 0.5;
        ndcX = xc / (halfH * aspect);
        ndcY = yc / halfH;
    }
    return QPointF((ndcX + 1.0) * 0.5 * w, (1.0 - ndcY) * 0.5 * h);
}

} // namespace ray::ui::nav
