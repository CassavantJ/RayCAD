// The five reference scenes of the viewport tests (task T-014). Meshes come from ray::kernel.
#pragma once

#include "viewport_testing.h"

#include <shape_testing.h>

#include <string>
#include <vector>

namespace ray::render::test {

struct Scene {
    std::string name;
    Tier preset;
};

inline const std::vector<Scene>& scenes()
{
    static const std::vector<Scene> list = {
        {"box", Tier::Low},           {"cylinder", Tier::Low},    {"filleted_body", Tier::Medium},
        {"two_bodies", Tier::Medium}, {"edges_only", Tier::High},
    };
    return list;
}

inline Xform translation(double x, double y, double z)
{
    Xform xf = Xform::identity();
    xf.m[3] = x;
    xf.m[7] = y;
    xf.m[11] = z;
    return xf;
}

// Fills an empty viewport with scene `name` and points the camera at it. aspect = width / height.
inline void buildScene(const std::string& name, IViewport& vp, double aspect)
{
    const MeshStyle model{Token::ModelFace, Token::ModelEdge, 1.0f, true, false};
    Box3 bounds{};
    bool perspective = true;
    if (name == "box") {
        vp.addMesh(meshOf(kernel::makeBox(Vec3{0, 0, 0}, Vec3{40, 30, 20})), Xform::identity(), model);
        bounds = {Vec3{0, 0, 0}, Vec3{40, 30, 20}};
    } else if (name == "cylinder") {
        vp.addMesh(meshOf(kernel::makeCylinder(Vec3{0, 0, 0}, Vec3{0, 0, 1}, 15, 40)), Xform::identity(), model);
        bounds = {Vec3{-15, -15, 0}, Vec3{15, 15, 40}};
        perspective = false;
    } else if (name == "filleted_body") {
        vp.addMesh(meshOf(kernel::testing::makeFilletedBody()), Xform::identity(), model);
        bounds = {Vec3{0, 0, 0}, Vec3{80, 50, 20}};
    } else if (name == "two_bodies") {
        vp.addMesh(meshOf(kernel::makeBox(Vec3{0, 0, 0}, Vec3{60, 40, 10})), Xform::identity(), model);
        // The second body is placed by a transform and drawn see-through in another style.
        vp.addMesh(meshOf(kernel::makeCylinder(Vec3{0, 0, 0}, Vec3{0, 0, 1}, 10, 25)), translation(30, 20, 5),
                   MeshStyle{Token::StockFace, Token::ModelSilhouette, 0.5f, true, false});
        bounds = {Vec3{0, 0, 0}, Vec3{60, 40, 30}};
    } else if (name == "edges_only") {
        // The filleted body as a wireframe (no triangles), plus axis lines: solid, dashed and thin.
        kernel::Mesh wire = *meshOf(kernel::testing::makeFilletedBody());
        wire.indices.clear();
        wire.faces.clear();
        vp.addMesh(std::make_shared<const kernel::Mesh>(std::move(wire)), Xform::identity(), model);
        const std::vector<Vec3> x{{0, 0, 0}, {90, 0, 0}};
        const std::vector<Vec3> y{{0, 0, 0}, {0, 60, 0}};
        const std::vector<Vec3> z{{0, 0, 0}, {0, 0, 40}};
        vp.addLines(x, Token::AxisX, 3.0f, false);
        vp.addLines(y, Token::AxisY, 3.0f, true);
        vp.addLines(z, Token::AxisZ, 1.0f, false);
        bounds = {Vec3{0, 0, 0}, Vec3{90, 60, 40}};
    } else {
        FAIL("unknown scene " << name);
    }
    vp.setCamera(standardViewCamera(StandardView::Iso, bounds, aspect, perspective));
}

} // namespace ray::render::test
