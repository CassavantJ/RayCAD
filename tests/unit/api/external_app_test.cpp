// makeApp with a dispatcher and a job system owned by the caller, the way the GUI builds the app (task T-017).
#include "api_test_util.h"
#include "test_dispatcher.h"

#include <catch2/catch_test_macros.hpp>

using ray::api::Caller;
using ray::test::kWait;
using ray::test::TempDir;

TEST_CASE("makeApp runs on the caller's dispatcher and job system", "[api][external]")
{
    TempDir dir;
    ray::test::TestDispatcher ui;
    auto jobs = ray::makeJobSystem(ui, 2);
    REQUIRE(jobs);
    {
        ray::api::AppOptions options;
        options.dispatcher = &ui;
        options.jobs = jobs.get();
        options.userDataDir = dir.file("user");
        auto app = ray::api::makeApp(options);
        REQUIRE(app);
        CHECK(&app->jobs() == jobs.get());
        CHECK(&app->dispatcher() == &ui);
        CHECK(app->userDataDir() == dir.file("user"));
        // CAM, the library stack and the post engine (M2), on the app's own dispatcher and job system.
        CHECK(app->cam() != nullptr);
        CHECK(app->posts() != nullptr);
        CHECK(app->operationTypes() != nullptr);
        CHECK(app->libraries() != nullptr);

        int opened = 0;
        app->subscribe([&](const ray::api::AppEvent& e) {
            if (e.kind == ray::api::AppEvent::Kind::DocumentOpened) {
                ++opened;
            }
        });
        auto job = app->open(ray::test::kernelData("box_hole.step"));
        double lastProgress = -1;
        job.onProgress([&](double f, std::string) { lastProgress = f; });
        REQUIRE(ui.runUntil([&] { return job.done(); }, kWait));
        auto od = job.wait();
        REQUIRE(od);
        REQUIRE(ui.runUntil([&] { return !(*od)->document->rebuildPending(); }, kWait));
        REQUIRE((*od)->document->build());
        CHECK_FALSE((*od)->document->build()->bodies.empty());
        ui.runPending();
        CHECK(opened == 1);
        CHECK(lastProgress > 0);

        auto saved = app->save(**od, dir.file("gui.raycad"));
        REQUIRE(ui.runUntil([&] { return saved.done(); }, kWait));
        REQUIRE(saved.wait());

        auto undo = app->actions().invoke("edit.undo", "", Caller::User);
        REQUIRE(ui.runUntil([&] { return undo.done(); }, kWait));
        REQUIRE(undo.wait());
        CHECK((*od)->document->state()->features.empty());

        // The app goes away with a job running on a job system it does not own.
        (void)app->open(ray::test::corpusFile("nist-pmi/nist_ctc_04_asme1_rd_geometry.stp"));
        ui.runUntil([] { return false; }, std::chrono::milliseconds(30));
    }
    // Whatever the app posted after it was destroyed does nothing.
    ui.runPending();
    jobs.reset();
    ui.runPending();
    CHECK_FALSE(ray::api::makeApp(ray::api::AppOptions{}));
}
