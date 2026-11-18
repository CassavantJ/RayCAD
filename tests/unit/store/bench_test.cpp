// Run by hand: loadHead and save on a 500 MB document (PLAN.md 4.1 partial load: under 200 ms for any size).
//   ray_store_tests "[.bench500]"
// Needs about 1.1 GB of free space in the temp folder. The normal run has the 20 MB version (file_test.cpp).
#include "store_test_util.h"

#include <ray/store.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <iostream>

using namespace storetest;
using ray::store::IDocumentFile;
using ray::store::ObjectKind;
using ray::store::OpenMode;

TEST_CASE("loadHead and save on a 500 MB file", "[.bench500]")
{
    TempDir dir;
    const std::string path = dir.path("five hundred.raycad");
    std::mt19937_64 rng(500);
    ray::JobContext ctx;
    ray::doc::DocState state = makeState(rng, 200, 2000);
    const auto t0 = nowMs();
    {
        auto file = IDocumentFile::create(path);
        REQUIRE(file);
        // Ten saves of 50 x 1 MB incompressible cache objects (stand-ins for B-reps and meshes).
        for (int round = 0; round < 10; ++round) {
            IDocumentFile::SaveRequest request;
            state.features[static_cast<std::size_t>(round)].name = "round " + std::to_string(round);
            request.state = std::make_shared<const ray::doc::DocState>(state);
            request.message = "build " + std::to_string(round);
            for (int i = 0; i < 50; ++i) {
                auto blob = randomBytes(rng, 1u << 20);
                request.derivedIndex[hashOf(randomBytes(rng, 8))] = hashOf(blob);
                request.derived.emplace_back(ObjectKind::Brep, std::move(blob));
            }
            REQUIRE((*file)->save(request, ctx));
        }
    }
    const auto built = nowMs();
    const auto size = fileSize(path);
    REQUIRE(size > 500u * 1000 * 1000);

    std::vector<double> opens, loads, saves;
    for (int rep = 0; rep < 5; ++rep) {
        const auto a = nowMs();
        auto file = IDocumentFile::open(path, OpenMode::ReadWrite);
        REQUIRE(file);
        const auto b = nowMs();
        auto head = (*file)->loadHead();
        const auto c = nowMs();
        REQUIRE(head);
        REQUIRE((*head)->stateHash() == state.stateHash());
        opens.push_back(static_cast<double>(b - a));
        loads.push_back(static_cast<double>(c - b));
        // A 50 KB change: one feature's props.
        state.features[7].propsJson = "{\"payload\":\"" + randomHex(rng, 50000) + "\"}";
        IDocumentFile::SaveRequest request;
        request.state = std::make_shared<const ray::doc::DocState>(state);
        request.message = "edit " + std::to_string(rep);
        const auto d = nowMs();
        REQUIRE((*file)->save(request, ctx));
        saves.push_back(static_cast<double>(nowMs() - d));
    }
    // Read-only open with nobody writing: a verified local copy (file copy plus integrity check).
    const auto r0 = nowMs();
    auto readOnly = IDocumentFile::open(path, OpenMode::ReadOnly);
    REQUIRE(readOnly);
    const auto r1 = nowMs();
    REQUIRE((*readOnly)->loadHead());
    readOnly->reset();
    const auto median = [](std::vector<double> v) {
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    std::cout << "[store] 500 MB bench: file " << size / 1'000'000 << " MB built in " << (built - t0) / 1000.0
              << " s; open median " << median(opens) << " ms, loadHead median " << median(loads) << " ms (max "
              << *std::max_element(loads.begin(), loads.end()) << "), 50 KB save median " << median(saves)
              << " ms; read-only open (copy and verify) " << (r1 - r0) << " ms (warm OS file cache)\n";
    CHECK(*std::max_element(loads.begin(), loads.end()) < 200);
}
