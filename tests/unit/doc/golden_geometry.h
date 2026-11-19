// Geometry goldens (PLAN.md section 11, task T-096): a reference part built from a feature list is compared with
// tests/golden/geometry/<name>.json: volume and area within 1e-9 relative, bounding box within 1e-6 mm, face, edge and
// vertex counts exact, and the hash of the element map exact per operating system ("mapHash": {"windows": ...,
// "linux": ..., "macos": ...}; an OS without an entry yet is reported, not failed).
//
// With RAYCAD_GOLDEN_PRODUCED=<folder> the test also writes what it produced, laid out like the suite (the golden with
// this OS's values), so tools/update_golden.py --produced <folder> --suite geometry can show and accept a change.
#pragma once

#include "json.h"
#include "json_number.h"

#include <ray/document.h>
#include <ray/kernel.h>
#include <ray/naming.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

namespace ray::doc::test {

inline const char* goldenPlatform()
{
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

inline void checkGoldenGeometry(const std::string& name, const kernel::Shape& shape, const naming::ElementMap& map)
{
    auto mp = kernel::massProps(shape);
    REQUIRE(mp);
    const Box3 box = kernel::bounds(shape);
    Hasher h;
    h.bytes(map.serialize());
    const std::string mapHash = h.finish().hex();
    const double counts[3] = {static_cast<double>(shape.count(kernel::SubKind::Face)),
                              static_cast<double>(shape.count(kernel::SubKind::Edge)),
                              static_cast<double>(shape.count(kernel::SubKind::Vertex))};

    const std::string path = std::string(RAY_GOLDEN_GEOMETRY_DIR) + "/" + name + ".json";
    std::optional<json::Value> golden;
    {
        std::ifstream in(std::filesystem::path(path), std::ios::binary);
        if (in) {
            std::stringstream text;
            text << in.rdbuf();
            auto parsed = json::parse(text.str());
            if (parsed && parsed->isObject()) {
                golden = std::move(*parsed);
            }
        }
    }
    // What this run produced: the golden's other platforms' hashes kept.
    json::Value::Object hashes;
    if (golden) {
        if (const json::Value* g = golden->find("mapHash"); g != nullptr && g->isObject()) {
            for (const json::Member& m : g->asObject()) {
                if (m.key != goldenPlatform()) {
                    hashes.push_back(m);
                }
            }
        }
    }
    hashes.push_back({goldenPlatform(), json::Value::string(mapHash)});
    const json::Value produced = json::Value::object(
        {{"part", json::Value::string(name)},
         {"volume", json::Value::number(mp->volume)},
         {"area", json::Value::number(mp->area)},
         {"bounds", json::Value::array({json::Value::number(box.min.x), json::Value::number(box.min.y),
                                        json::Value::number(box.min.z), json::Value::number(box.max.x),
                                        json::Value::number(box.max.y), json::Value::number(box.max.z)})},
         {"counts", json::Value::object({{"faces", json::Value::number(counts[0])},
                                         {"edges", json::Value::number(counts[1])},
                                         {"vertices", json::Value::number(counts[2])}})},
         {"mapHash", json::Value::object(std::move(hashes))}});
    if (const char* dir = std::getenv("RAYCAD_GOLDEN_PRODUCED")) {
        std::filesystem::create_directories(std::filesystem::path(dir));
        std::ofstream out(std::filesystem::path(dir) / (name + ".json"), std::ios::binary);
        out << json::write(produced) << "\n";
    }
    INFO("golden " << path);
    REQUIRE(golden.has_value());
    auto number = [&](const json::Value& v, std::string_view key) {
        const json::Value* x = v.find(key);
        REQUIRE(x != nullptr);
        REQUIRE(x->isNumber());
        return x->asNumber();
    };
    const double volume = number(*golden, "volume");
    const double area = number(*golden, "area");
    CHECK(std::abs(mp->volume - volume) <= 1e-9 * std::abs(volume));
    CHECK(std::abs(mp->area - area) <= 1e-9 * std::abs(area));
    const json::Value* b = golden->find("bounds");
    REQUIRE((b != nullptr && b->isArray() && b->asArray().size() == 6));
    const double now[6] = {box.min.x, box.min.y, box.min.z, box.max.x, box.max.y, box.max.z};
    for (std::size_t i = 0; i < 6; ++i) {
        CHECK(std::abs(b->asArray()[i].asNumber() - now[i]) <= 1e-6);
    }
    const json::Value* c = golden->find("counts");
    REQUIRE(c != nullptr);
    CHECK(number(*c, "faces") == counts[0]);
    CHECK(number(*c, "edges") == counts[1]);
    CHECK(number(*c, "vertices") == counts[2]);
    const json::Value* g = golden->find("mapHash");
    const json::Value* mine = g != nullptr ? g->find(goldenPlatform()) : nullptr;
    if (mine == nullptr) {
        std::printf("golden %s: no element-map hash for %s yet (this run: %s)\n", name.c_str(), goldenPlatform(),
                    mapHash.c_str());
    } else {
        CHECK(mine->asString() == mapHash);
    }
}

} // namespace ray::doc::test
