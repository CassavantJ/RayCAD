// Navigation for the viewport item (task T-020): the mouse mappings users can choose between, and the camera math
// on top of render's helpers (orbitCamera, panCamera, zoomCamera, fitCamera, standardViewCamera): view
// transitions, the projection toggle and views along any direction. No QObject here.
//
// Presets map a mouse button plus modifiers to a drag action. The left button without Alt always selects (click)
// or box-selects (drag); Ctrl and Shift change how a selection combines. The wheel zooms about the point under the
// cursor in every preset; touchpads scroll to pan, pinch (or Ctrl+wheel) to zoom, Shift+scroll to orbit.
#pragma once

#include <ray_render/viewport.h>

#include <QtCore/QPointF>
#include <QtCore/QString>
#include <QtCore/qnamespace.h>

#include <optional>
#include <vector>

namespace ray::ui::nav {

enum class Action { None, Select, Orbit, Pan, Zoom };

struct Binding {
    Qt::MouseButton button;
    Qt::KeyboardModifiers modifiers; // exact set of Ctrl, Shift and Alt
    Action action;
};

struct Preset {
    const char* id;          // stable key for settings: "raycad", "middle-orbit", "right-orbit"
    const char* title;       // English, translated by the UI
    const char* description; // one line, English
    std::vector<Binding> drags;
};

// The presets, default first.
const std::vector<Preset>& presets();
// What pressing `button` with `modifiers` starts.
Action actionFor(const Preset& preset, Qt::MouseButton button, Qt::KeyboardModifiers modifiers);

// ---------- camera math ----------
struct Frame {
    Vec3 forward, right, up;
    double distance; // eye to target
};
Frame frameOf(const render::Camera& camera);
// Smooth step between two cameras: target and distance linear, view direction and up along the great circle,
// projection and field of view from `to`. t in [0, 1]; eased by the caller.
render::Camera interpolate(const render::Camera& from, const render::Camera& to, double t);
// Switches projection keeping the scale at the target: orthographic height = perspective view height there.
render::Camera withProjection(const render::Camera& camera, bool perspective);
// Looks along `direction` (eye to target) and frames `box`; up is world Z unless the view is along Z.
render::Camera viewAlong(const render::Camera& camera, Vec3 direction, const Box3& box, double aspect);
// Where the ray meets the plane through `point` with normal `normal`; nullopt if parallel or behind the origin.
std::optional<Vec3> rayPlane(const render::Ray& ray, Vec3 point, Vec3 normal);
// Screen position (logical pixels, origin top-left) of a world point; nullopt behind a perspective camera.
std::optional<QPointF> project(const render::Camera& camera, Vec3 world, double widthPx, double heightPx);

} // namespace ray::ui::nav
