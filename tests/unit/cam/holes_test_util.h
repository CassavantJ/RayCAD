// Helpers of the hole recognition tests (task T-034): corpus parts imported as a user's file is, frames, and a
// readable dump of what recognizeHoles found.
#pragma once

#include <ray/cam.h>
#include <ray/kernel.h>
#include <ray/naming.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <sstream>
#include <string>

namespace ray::cam::test {

// The single solid of a corpus file (tests/corpus/files/<id>), imported with the default options (as the document's
// import does: full healing, sewing).
inline kernel::Shape importCorpus(const std::string& id)
{
    const std::string path = std::string(RAY_CORPUS_DIR) + "/" + id;
    JobContext ctx;
    const bool iges = path.size() > 4 && (path.ends_with(".igs") || path.ends_with(".iges"));
    auto imported = iges ? kernel::importIges(path, kernel::ImportOptions{}, ctx)
                         : kernel::importStep(path, kernel::ImportOptions{}, ctx);
    if (!imported) {
        FAIL("import of " << id << " failed: " << imported.error().message);
    }
    REQUIRE(imported->bodies.size() == 1);
    return imported->bodies[0].shape;
}

inline Xform identityFrame()
{
    return Xform::identity();
}

// Seen from below: WCS X = model X, Y = -model Y, Z = -model Z (a half turn about X).
inline Xform fromBelow()
{
    return Xform{{1, 0, 0, 0, 0, -1, 0, 0, 0, 0, -1, 0}};
}

// The frame whose Z is the model axis named by `name`: "z+" (the identity), "z-" (fromBelow), "y+", "y-", "x+", "x-";
// right-handed, no translation.
inline Xform axisFrame(const std::string& name)
{
    if (name == "z-") {
        return fromBelow();
    }
    if (name == "y+") {
        return Xform{{1, 0, 0, 0, 0, 0, -1, 0, 0, 1, 0, 0}};
    }
    if (name == "y-") {
        return Xform{{1, 0, 0, 0, 0, 0, 1, 0, 0, -1, 0, 0}};
    }
    if (name == "x+") {
        return Xform{{0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0}};
    }
    if (name == "x-") {
        return Xform{{0, -1, 0, 0, 0, 0, 1, 0, -1, 0, 0, 0}};
    }
    REQUIRE(name == "z+");
    return identityFrame();
}

inline Vec3 unitOf(Vec3 v)
{
    const double len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return Vec3{v.x / len, v.y / len, v.z / len};
}

// The rotation by `angle` (radians) about the unit axis u, as a 3x4 matrix with translation t.
inline Xform rotation(Vec3 u, double angle, Vec3 t)
{
    const double c = std::cos(angle);
    const double s = std::sin(angle);
    const double k = 1 - c;
    return Xform{{c + u.x * u.x * k, u.x * u.y * k - u.z * s, u.x * u.z * k + u.y * s, t.x, //
                  u.y * u.x * k + u.z * s, c + u.y * u.y * k, u.y * u.z * k - u.x * s, t.y, //
                  u.z * u.x * k - u.y * s, u.z * u.y * k + u.x * s, c + u.z * u.z * k, t.z}};
}

// The inverse of a rigid move.
inline Xform inverse(const Xform& a)
{
    const double* m = a.m;
    const double tx = -(m[0] * m[3] + m[4] * m[7] + m[8] * m[11]);
    const double ty = -(m[1] * m[3] + m[5] * m[7] + m[9] * m[11]);
    const double tz = -(m[2] * m[3] + m[6] * m[7] + m[10] * m[11]);
    return Xform{{m[0], m[4], m[8], tx, m[1], m[5], m[9], ty, m[2], m[6], m[10], tz}};
}

inline const char* bottomWord(HoleBottom b)
{
    switch (b) {
    case HoleBottom::Through:
        return "through";
    case HoleBottom::Flat:
        return "flat";
    case HoleBottom::Conical:
        return "conical";
    }
    return "?";
}

inline std::string describe(const RecognizedHole& h)
{
    std::ostringstream s;
    s.precision(10);
    s << "hole at (" << toMm(h.x) << ", " << toMm(h.y) << ") top " << toMm(h.zTop) << " bottom " << toMm(h.zBottom)
      << " " << bottomWord(h.bottom) << (h.counterbored ? " counterbored" : "") << (h.countersunk ? " countersunk" : "")
      << (h.chamfered ? " chamfered" : "") << " bore " << h.bore << " faces " << h.faces.size();
    if (!h.threadSpec.empty()) {
        s << " thread " << h.threadSpec << " pitch " << h.threadPitch;
    }
    for (const HoleSegment& g : h.segments) {
        s << "\n    " << (g.conical ? "cone " : "cyl ") << g.diameter << " -> " << g.bottomDiameter << " depth "
          << g.depth << " z " << toMm(g.zTop) << ".." << toMm(g.zBottom);
        if (g.conical) {
            s << " angle " << g.coneAngleDeg;
        }
    }
    return s.str();
}

} // namespace ray::cam::test
