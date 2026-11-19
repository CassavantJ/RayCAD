// Helpers of the parametric feature tests (task T-096): sketch JSON builders, the typed feature commands, and
// queries on built bodies.
#pragma once

#include "rebuild_test_util.h"

#include "features/feature_props.h"

#include <ray/document.h>
#include <ray/kernel.h>
#include <ray/naming.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace ray::doc::test {

inline constexpr double kPi = 3.14159265358979323846;

// Entity ids of the test sketches: uuid('e', n); constraint ids: uuid('c', n).
inline std::string ent(std::uint64_t n)
{
    return uuid('e', n);
}

inline std::string num(double v)
{
    std::ostringstream s;
    s.precision(17);
    s << v;
    return s.str();
}

// Builds the JSON of a sketch model.
class SketchJson {
public:
    SketchJson& point(std::uint64_t id, double x, double y, bool construction = false)
    {
        add(R"({"id":")" + ent(id) + R"(","type":"point","x":)" + num(x) + R"(,"y":)" + num(y) +
            (construction ? R"(,"construction":true)" : "") + "}");
        return *this;
    }
    SketchJson& line(std::uint64_t id, std::uint64_t a, std::uint64_t b, bool construction = false)
    {
        add(R"({"id":")" + ent(id) + R"(","type":"line","a":")" + ent(a) + R"(","b":")" + ent(b) + "\"" +
            (construction ? R"(,"construction":true)" : "") + "}");
        return *this;
    }
    SketchJson& circle(std::uint64_t id, std::uint64_t center, double r)
    {
        add(R"({"id":")" + ent(id) + R"(","type":"circle","center":")" + ent(center) + R"(","radius":)" + num(r) + "}");
        return *this;
    }
    SketchJson& arc(std::uint64_t id, std::uint64_t center, std::uint64_t start, std::uint64_t end, bool ccw = true)
    {
        add(R"({"id":")" + ent(id) + R"(","type":"arc","center":")" + ent(center) + R"(","start":")" + ent(start) +
            R"(","end":")" + ent(end) + R"(","ccw":)" + (ccw ? "true" : "false") + "}");
        return *this;
    }
    SketchJson& spline(std::uint64_t id, const std::vector<std::uint64_t>& points, int degree = 3)
    {
        std::string p;
        for (std::uint64_t point : points) {
            p += (p.empty() ? "\"" : ",\"") + ent(point) + "\"";
        }
        add(R"({"id":")" + ent(id) + R"(","type":"spline","points":[)" + p + R"(],"degree":)" + std::to_string(degree) +
            "}");
        return *this;
    }
    SketchJson& slot(std::uint64_t id, std::uint64_t a, std::uint64_t b, double width)
    {
        add(R"({"id":")" + ent(id) + R"(","type":"slot","a":")" + ent(a) + R"(","b":")" + ent(b) + R"(","width":)" +
            num(width) + "}");
        return *this;
    }
    // A rectangle: points base..base+3 counter-clockwise from (x0, y0), lines base+4 (bottom), +5 (right), +6 (top),
    // +7 (left).
    SketchJson& rect(std::uint64_t base, double x0, double y0, double x1, double y1)
    {
        point(base, x0, y0).point(base + 1, x1, y0).point(base + 2, x1, y1).point(base + 3, x0, y1);
        return line(base + 4, base, base + 1)
            .line(base + 5, base + 1, base + 2)
            .line(base + 6, base + 2, base + 3)
            .line(base + 7, base + 3, base);
    }
    // A circle: centre point base, circle base+1.
    SketchJson& circleAt(std::uint64_t base, double cx, double cy, double r)
    {
        return point(base, cx, cy).circle(base + 1, base, r);
    }
    SketchJson& constraint(std::uint64_t id, const std::string& type, const std::vector<std::uint64_t>& refs,
                           const std::string& expression = {}, std::optional<double> value = std::nullopt)
    {
        std::string r;
        for (std::uint64_t ref : refs) {
            r += (r.empty() ? "\"" : ",\"") + ent(ref) + "\"";
        }
        std::string c = R"({"id":")" + uuid('c', id) + R"(","type":")" + type + R"(","refs":[)" + r + "]";
        if (!expression.empty()) {
            c += R"(,"expression":")" + expression + "\"";
        }
        if (value) {
            c += R"(,"value":)" + num(*value);
        }
        constraints_.push_back(c + "}");
        return *this;
    }
    std::string json() const
    {
        std::string e, c;
        for (const std::string& x : entities_) {
            e += (e.empty() ? "" : ",") + x;
        }
        for (const std::string& x : constraints_) {
            c += (c.empty() ? "" : ",") + x;
        }
        return R"({"schema":1,"entities":[)" + e + R"(],"constraints":[)" + c + "]}";
    }

private:
    void add(std::string e) { entities_.push_back(std::move(e)); }
    std::vector<std::string> entities_;
    std::vector<std::string> constraints_;
};

inline std::string ids(const std::vector<std::uint64_t>& entities)
{
    std::string out = "[";
    for (std::size_t i = 0; i < entities.size(); ++i) {
        out += (i ? ",\"" : "\"") + ent(entities[i]) + "\"";
    }
    return out + "]";
}

// feature.<type>.add with the given id ('f', n) and props.
inline Command addFeature(const std::string& type, std::uint64_t n, const std::string& props,
                          const std::string& extra = {})
{
    return cmd("feature." + type + ".add", R"({"id":")" + uuid('f', n) + R"(","props":)" + props + extra + "}",
               "Add " + type);
}

inline Command editFeature(const std::string& type, std::uint64_t n, const std::string& props)
{
    return cmd("feature." + type + ".edit", R"({"id":")" + uuid('f', n) + R"(","props":)" + props + "}",
               "Edit " + type);
}

inline std::string sketchProps(const std::string& plane, const SketchJson& sketch)
{
    return R"({"plane":)" + plane + R"(,"sketch":)" + sketch.json() + "}";
}

inline std::string extrudeProps(std::uint64_t sketch, const std::string& distance, const std::string& operation,
                                char body, const std::string& extra = {})
{
    return R"({"sketch":")" + uuid('f', sketch) + R"(","distance":)" + distance + R"(,"operation":")" + operation +
           R"(","target":")" + uuid(body, 1) + "\"" + extra + "}";
}

// The built body, or fails the test.
inline const BuiltBody& bodyOf(const BuildResult& build, char body)
{
    const auto it = build.bodies.find(bodyId(body));
    REQUIRE(it != build.bodies.end());
    return it->second;
}

inline double volumeOf(const BuildResult& build, char body)
{
    auto mp = kernel::massProps(bodyOf(build, body).shape);
    REQUIRE(mp);
    return mp->volume;
}

// The first element of `kind` for which `pred` holds, or nullopt.
inline std::optional<kernel::SubId> findFace(const kernel::Shape& shape,
                                             const std::function<bool(const kernel::SurfaceInfo&)>& pred)
{
    for (std::uint32_t i = 0; i < shape.count(kernel::SubKind::Face); ++i) {
        auto info = kernel::surfaceInfo(shape, kernel::SubId{kernel::SubKind::Face, i});
        if (info && pred(*info)) {
            return kernel::SubId{kernel::SubKind::Face, i};
        }
    }
    return std::nullopt;
}

inline std::optional<kernel::SubId> findEdge(const kernel::Shape& shape,
                                             const std::function<bool(const kernel::CurveInfo&)>& pred)
{
    for (std::uint32_t i = 0; i < shape.count(kernel::SubKind::Edge); ++i) {
        auto info = kernel::curveInfo(shape, kernel::SubId{kernel::SubKind::Edge, i});
        if (info && pred(*info)) {
            return kernel::SubId{kernel::SubKind::Edge, i};
        }
    }
    return std::nullopt;
}

inline bool near(double a, double b, double tol = 1e-6)
{
    return std::abs(a - b) <= tol;
}

// A TopoRef to an element of a built body, as props JSON.
inline naming::TopoRef refTo(const BuildResult& build, char body, kernel::SubId sub)
{
    const BuiltBody& b = bodyOf(build, body);
    auto resolver = naming::makeResolver({naming::ResolverBody{bodyId(body), b.map, b.shape}});
    return resolver->makeRef(bodyId(body), sub);
}

inline std::string refJson(const naming::TopoRef& ref)
{
    return json::write(features::topoRefToJson(ref));
}

inline naming::Resolved resolveOn(const BuildResult& build, char body, const naming::TopoRef& ref)
{
    const BuiltBody& b = bodyOf(build, body);
    auto resolver = naming::makeResolver({naming::ResolverBody{bodyId(body), b.map, b.shape}});
    return resolver->resolve(ref);
}

inline const FeatureReport& report(const BuildResult& build, std::uint64_t n)
{
    return reportOf(build, featureId(n));
}

} // namespace ray::doc::test
