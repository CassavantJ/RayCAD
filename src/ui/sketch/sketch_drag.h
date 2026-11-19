// The sketch drag worker (task T-092, PLAN.md 7: "one live PlaneGCS system per drag session; always solves for the
// newest pointer position and drops stale ones; the UI thread never waits for it").
//
// One SketchDrag is one drag of one point. Its own thread runs ISketchSolver::beginDrag (up to 0.6 s at 200 curves)
// and then a loop: take the NEWEST target the UI gave (older ones are dropped), IDragSession::move to it, hand the
// result back. move() solves from the drag-start state, so skipped positions cannot change the final geometry
// (sketch.h). finish() makes the loop solve the newest target, call end() and hand back the final sketch.
//
// Results reach the UI thread through ray::ui::invokeQueued on `context` (a QObject of the UI thread that outlives
// this object), so the sink always runs there. The UI only ever stores a target and notifies: it never blocks on a
// solve. The destructor cancels (beginDrag checks the token) and joins; by then the thread only has to notice.
#pragma once

#include <ray/api.h>

#include <QtCore/QObject>

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace ray::ui::sketchui {

class SketchDrag {
public:
    struct Update {
        std::uint64_t serial = 0;
        enum class Kind { Started, Step, Final, Failed } kind = Kind::Step;
        sketch::Sketch sketch; // Step: the solved geometry; Final: what end() returned
        sketch::SolveStatus status = sketch::SolveStatus::UnderConstrained;
        std::string error;     // Failed
        double stepMs = 0;     // Step: how long move() took
    };
    using Sink = std::function<void(const Update&)>; // runs on the thread of `context`

    SketchDrag(QObject* context, std::uint64_t serial, sketch::Sketch model, SketchEntId point, Sink sink);
    ~SketchDrag();
    SketchDrag(const SketchDrag&) = delete;
    SketchDrag& operator=(const SketchDrag&) = delete;

    std::uint64_t serial() const { return serial_; }
    // UI thread: the newest pointer position (mm on the sketch plane). Never blocks on a solve.
    void move(Vec2 target);
    // UI thread: solve the newest position, end the session and hand back Final.
    void finish();

private:
    void run(sketch::Sketch model, SketchEntId point);
    void post(Update update);

    QObject* context_;
    std::uint64_t serial_;
    Sink sink_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::optional<Vec2> target_;
    bool finish_ = false;
    bool stop_ = false;
    CancelSource cancel_;
    std::thread thread_;
};

} // namespace ray::ui::sketchui
