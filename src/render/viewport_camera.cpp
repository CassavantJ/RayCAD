// Camera helpers for navigation (task T-014): orbit, pan, zoom, fit and the standard views. Pure math on doubles,
// no Qt. The world is Z-up.
#include <ray_render/viewport.h>

#include <algorithm>
#include <cmath>

namespace ray::render {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDegToRad = kPi / 180.0;
constexpr double kFitMargin = 1.05; // frames the bounding sphere with 5 % to spare
constexpr double kMinFovDeg = 1.0;
constexpr double kMaxFovDeg = 150.0;

Vec3 operator+(Vec3 a, Vec3 b)
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
Vec3 operator-(Vec3 a, Vec3 b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
Vec3 operator*(Vec3 a, double s)
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
    return len > 1e-12 ? a * (1.0 / len) : fallback;
}

// Rodrigues rotation of v about the unit axis k.
Vec3 rotate(Vec3 v, Vec3 k, double angleRad)
{
    const double c = std::cos(angleRad);
    const double s = std::sin(angleRad);
    return v * c + cross(k, v) * s + k * (dot(k, v) * (1.0 - c));
}

// The camera's orthonormal frame: forward (eye to target), right, up.
struct Frame {
    Vec3 forward, right, up;
    double distance;
};

Frame frameOf(const Camera& cam)
{
    Frame f;
    const Vec3 toTarget = cam.target - cam.eye;
    f.distance = length(toTarget);
    f.forward = normalized(toTarget, Vec3{0, 1, 0});
    Vec3 up = normalized(cam.up, Vec3{0, 0, 1});
    f.right = cross(f.forward, up);
    if (length(f.right) < 1e-9) {
        // up parallel to the view direction: take any perpendicular
        up = std::abs(f.forward.z) < 0.9 ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
        f.right = cross(f.forward, up);
    }
    f.right = normalized(f.right, Vec3{1, 0, 0});
    f.up = cross(f.right, f.forward);
    return f;
}

double fovRad(const Camera& cam)
{
    return std::clamp(cam.fovDeg, kMinFovDeg, kMaxFovDeg) * kDegToRad;
}

// World units per pixel at the target's depth.
double unitsPerPixel(const Camera& cam, int heightPx)
{
    const double h = std::max(1, heightPx);
    if (!cam.perspective)
        return cam.orthoHeight / h;
    return 2.0 * frameOf(cam).distance * std::tan(fovRad(cam) * 0.5) / h;
}

struct Sphere {
    Vec3 centre;
    double radius;
};

Sphere sphereOf(const Box3& box)
{
    const Vec3 centre = (box.min + box.max) * 0.5;
    const double radius = std::max(length(box.max - box.min) * 0.5, 1e-6);
    return {centre, radius};
}

} // namespace

Camera defaultCamera()
{
    Camera cam{};
    cam.target = Vec3{0, 0, 0};
    cam.eye = Vec3{200, -200, 200};
    cam.up = Vec3{0, 0, 1};
    cam.perspective = true;
    cam.fovDeg = 30.0;
    cam.orthoHeight = 200.0;
    return cam;
}

Camera orbitCamera(const Camera& cam, double yawDeg, double pitchDeg, Vec3 pivot)
{
    Camera out = cam;
    const Frame f = frameOf(cam);
    Vec3 up = f.up;
    // Yaw about world Z through the pivot.
    const Vec3 z{0, 0, 1};
    const double yaw = yawDeg * kDegToRad;
    Vec3 eye = rotate(cam.eye - pivot, z, yaw);
    Vec3 target = rotate(cam.target - pivot, z, yaw);
    up = rotate(up, z, yaw);
    Vec3 right = rotate(f.right, z, yaw);
    // Pitch about the camera's right axis through the pivot.
    const double pitch = pitchDeg * kDegToRad;
    eye = rotate(eye, right, pitch);
    target = rotate(target, right, pitch);
    up = rotate(up, right, pitch);
    out.eye = eye + pivot;
    out.target = target + pivot;
    out.up = normalized(up, Vec3{0, 0, 1});
    return out;
}

Camera panCamera(const Camera& cam, double dxPx, double dyPx, int viewportHeightPx)
{
    Camera out = cam;
    const Frame f = frameOf(cam);
    const double s = unitsPerPixel(cam, viewportHeightPx);
    // Dragging right moves the content right, so the camera moves left; screen y grows downwards.
    const Vec3 move = f.right * (-dxPx * s) + f.up * (dyPx * s);
    out.eye = cam.eye + move;
    out.target = cam.target + move;
    return out;
}

Camera zoomCamera(const Camera& cam, double factor, std::optional<Vec3> towards)
{
    if (!(factor > 0) || !std::isfinite(factor))
        return cam;
    Camera out = cam;
    const Vec3 fixed = towards.value_or(cam.target);
    if (cam.perspective) {
        // Scale eye and target about the fixed point: the point keeps its place on screen.
        out.eye = fixed + (cam.eye - fixed) * (1.0 / factor);
        out.target = fixed + (cam.target - fixed) * (1.0 / factor);
        if (!towards)
            out.target = cam.target;
    } else {
        out.orthoHeight = cam.orthoHeight / factor;
        if (towards) {
            // Shift sideways only; moving along the view direction changes nothing in an orthographic view.
            const Frame f = frameOf(cam);
            const Vec3 d = cam.target - fixed;
            const Vec3 lateral = d - f.forward * dot(d, f.forward);
            const Vec3 shift = lateral * (1.0 / factor) - lateral;
            out.eye = cam.eye + shift;
            out.target = cam.target + shift;
        }
    }
    return out;
}

Camera fitCamera(const Camera& cam, const Box3& box, double aspect)
{
    Camera out = cam;
    const Frame f = frameOf(cam);
    const Sphere sphere = sphereOf(box);
    aspect = aspect > 0 && std::isfinite(aspect) ? aspect : 1.0;
    const double r = sphere.radius * kFitMargin;
    double distance = 0;
    if (cam.perspective) {
        const double halfV = fovRad(cam) * 0.5;
        const double halfH = std::atan(std::tan(halfV) * aspect);
        distance = r / std::sin(std::min(halfV, halfH));
    } else {
        out.orthoHeight = 2.0 * r * std::max(1.0, 1.0 / aspect);
        distance = 3.0 * r; // only for the clip planes
    }
    out.target = sphere.centre;
    out.eye = sphere.centre - f.forward * distance;
    out.up = f.up;
    return out;
}

Camera standardViewCamera(StandardView view, const Box3& box, double aspect, bool perspective, double fovDeg)
{
    Vec3 dir{0, 1, 0}; // eye to target
    Vec3 up{0, 0, 1};
    switch (view) {
    case StandardView::Front:
        dir = {0, 1, 0};
        break;
    case StandardView::Back:
        dir = {0, -1, 0};
        break;
    case StandardView::Left:
        dir = {1, 0, 0};
        break;
    case StandardView::Right:
        dir = {-1, 0, 0};
        break;
    case StandardView::Top:
        dir = {0, 0, -1};
        up = {0, 1, 0};
        break;
    case StandardView::Bottom:
        dir = {0, 0, 1};
        up = {0, -1, 0};
        break;
    case StandardView::Iso:
        dir = normalized(Vec3{-1, 1, -1}, Vec3{0, 1, 0});
        break;
    }
    const Sphere sphere = sphereOf(box);
    Camera cam{};
    cam.perspective = perspective;
    cam.fovDeg = fovDeg;
    cam.orthoHeight = 2.0 * sphere.radius;
    cam.target = sphere.centre;
    cam.eye = sphere.centre - dir * (4.0 * sphere.radius);
    cam.up = up;
    return fitCamera(cam, box, aspect);
}

Ray cameraRay(const Camera& cam, double xPx, double yPx, int widthPx, int heightPx)
{
    const Frame f = frameOf(cam);
    const double w = std::max(1, widthPx);
    const double h = std::max(1, heightPx);
    const double ndcX = (xPx / w) * 2.0 - 1.0;
    const double ndcY = 1.0 - (yPx / h) * 2.0;
    const double aspect = w / h;
    if (cam.perspective) {
        const double t = std::tan(fovRad(cam) * 0.5);
        const Vec3 dir = f.forward + f.right * (ndcX * t * aspect) + f.up * (ndcY * t);
        return Ray{cam.eye, normalized(dir, f.forward)};
    }
    const double halfH = cam.orthoHeight * 0.5;
    const Vec3 origin = cam.eye + f.right * (ndcX * halfH * aspect) + f.up * (ndcY * halfH);
    return Ray{origin, f.forward};
}

} // namespace ray::render
