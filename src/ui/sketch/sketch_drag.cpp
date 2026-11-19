#include "sketch_drag.h"

#include "../queued_call.h"

#include <ray_api/sketch_edit.h>

#include <chrono>
#include <exception>
#include <utility>

namespace ray::ui::sketchui {

SketchDrag::SketchDrag(QObject* context, std::uint64_t serial, sketch::Sketch model, SketchEntId point, Sink sink)
    : context_(context), serial_(serial), sink_(std::move(sink))
{
    thread_ = std::thread([this, model = std::move(model), point]() mutable { run(std::move(model), point); });
}

SketchDrag::~SketchDrag()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cancel_.cancel();
    wake_.notify_all();
    if (thread_.joinable())
        thread_.join();
}

void SketchDrag::move(Vec2 target)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        target_ = target; // the newest wins; an older one not yet taken is dropped
    }
    wake_.notify_one();
}

void SketchDrag::finish()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        finish_ = true;
    }
    wake_.notify_one();
}

void SketchDrag::post(Update update)
{
    update.serial = serial_;
    // The sink is copied into the call: the call may run after this object is gone (the context decides).
    invokeQueued(context_, [sink = sink_, update = std::move(update)]() { sink(update); });
}

void SketchDrag::run(sketch::Sketch model, SketchEntId point)
{
    try {
        std::unique_ptr<sketch::ISketchSolver> solver = api::sketch_edit::makeSolver();
        JobContext ctx{cancel_.token(), Progress()};
        auto session = solver ? solver->beginDrag(model, point, ctx)
                              : Result<std::unique_ptr<sketch::ISketchSolver::IDragSession>>(
                                    tl::unexpected(Error{ErrorCode::Internal, "No solver", {}}));
        if (!session || !*session) {
            Update u;
            u.kind = Update::Kind::Failed;
            u.error = session ? "The drag could not start" : session.error().message;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (stop_)
                    return;
            }
            post(std::move(u));
            return;
        }
        {
            Update u;
            u.kind = Update::Kind::Started;
            post(std::move(u));
        }
        for (;;) {
            std::optional<Vec2> target;
            bool finishing = false;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [&] { return stop_ || finish_ || target_.has_value(); });
                if (stop_)
                    return;
                target = std::exchange(target_, std::nullopt);
                finishing = finish_ && !target;
            }
            if (target) {
                const auto t0 = std::chrono::steady_clock::now();
                auto step = (*session)->move(*target);
                const auto t1 = std::chrono::steady_clock::now();
                Update u;
                u.kind = Update::Kind::Step;
                u.stepMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
                if (step) {
                    u.status = step->status;
                    if (step->status != sketch::SolveStatus::Failed)
                        u.sketch = std::move(step->solved);
                    else
                        u.kind = Update::Kind::Step; // geometry stays at the last good step
                } else {
                    u.status = sketch::SolveStatus::Failed;
                }
                if (u.status != sketch::SolveStatus::Failed)
                    post(std::move(u));
                continue;
            }
            if (finishing) {
                Update u;
                u.kind = Update::Kind::Final;
                u.sketch = (*session)->end();
                post(std::move(u));
                return;
            }
        }
    } catch (const std::exception& e) {
        Update u;
        u.kind = Update::Kind::Failed;
        u.error = e.what();
        post(std::move(u));
    } catch (...) {
        Update u;
        u.kind = Update::Kind::Failed;
        u.error = "unexpected failure";
        post(std::move(u));
    }
}

} // namespace ray::ui::sketchui
