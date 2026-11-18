// Private to src/kernel and its tests (task T-005): the machinery behind importStep/importIges and the
// exporters. Includes no OCCT header, so tests/unit/kernel may include it.
//
// HOW CANCEL REACHES THE CALLER WITHIN 100 ms (PLAN.md section 7, spike S3)
//
// Part of a STEP read cannot be interrupted (OCCT's model-load stage, about 30 ms per MiB, under a global
// OCCT mutex), and healing and meshing can go a second or more between two cancel polls. The job system does
// not abandon running jobs, so the kernel does it itself: every import and export runs its OCCT work on a
// kernel-owned I/O thread (runOnIoThread below), and the calling thread only waits, forwarding progress and
// polling the CancelToken every 10 ms. On cancel the calling thread asks the I/O thread to stop and returns
// Error{Cancelled} at once; the I/O thread finishes the OCCT call it is in (aborting at OCCT's next poll),
// throws its result away and exits. Everything it touches is owned by the shared IoTask: copies of the path,
// the options and the input Shapes (immutable, shared_ptr), its own file handles and its own progress state.
// It never touches the caller's JobContext, Progress or listener, so the caller's stack may be gone.
//
// For T-010 / T-019 (callers): submit the call as an ordinary job and do nothing special:
//
//     auto job = jobs.submit<kernel::ImportResult>({"Import"}, [path, opts](JobContext& ctx) {
//         return kernel::importStep(path, opts, ctx);
//     });
//     ...
//     job.cancel();   // the job's result is Error{Cancelled} within 100 ms; its worker is free again
//
// An abandoned read still holds one core and 7 to 8 MB per MiB of file until OCCT returns, and a new STEP
// import started meanwhile waits for it inside OCCT (global read mutex) - on the I/O thread, so the new
// import's caller can still be cancelled at once. IGES reads are serialised the same way by the kernel
// (io_import.cpp: OCCT's IGES parser is not reentrant). I/O threads that are still running at process exit are
// joined by a static destructor before OCCT is unloaded; waitForIoThreads() joins them earlier (tests).
#pragma once
#include <ray/kernel.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ray::kernel::detail {

// ---------- Background I/O tasks ----------

// State shared between the calling thread and the I/O thread of one import or export.
class IoTask {
public:
    explicit IoTask(CancelToken cancel) : cancel_(std::move(cancel)) {}
    virtual ~IoTask() = default;
    IoTask(const IoTask&) = delete;
    IoTask& operator=(const IoTask&) = delete;

    // I/O thread side. True once the caller cancelled or abandoned the task; long loops and OCCT progress
    // indicators poll this.
    bool stopRequested() const noexcept { return stop_.load(std::memory_order_acquire) || cancel_.cancelled(); }
    // Progress of the whole task in [0, 1] and the stage name (a string literal). Thread-safe; cheap.
    void report(double fraction01, std::string_view stage) noexcept;

    // Caller side.
    void requestStop() noexcept { stop_.store(true, std::memory_order_release); }
    struct Snapshot {
        double fraction;
        std::string_view stage;
        bool done;
    };
    Snapshot snapshot() const;
    // Waits until done or `timeout` passed; returns done.
    bool waitDone(std::chrono::milliseconds timeout) const;

    // Runs the work on the I/O thread, then marks the task done. Never throws.
    void execute() noexcept;

protected:
    virtual void run() noexcept = 0; // stores the result and releases every input before it returns

private:
    CancelToken cancel_;
    std::atomic<bool> stop_{false};
    mutable std::mutex mutex_;
    mutable std::condition_variable doneSignal_;
    double fraction_ = 0.0;  // guarded by mutex_
    std::string_view stage_; // guarded by mutex_; always a string literal
    bool done_ = false;      // guarded by mutex_
};

// Starts `task` on a new I/O thread and waits for it as the header comment describes: forwards progress to
// ctx.progress, and returns Error{Cancelled} within about 10 ms of ctx.cancel being cancelled (the task is then
// abandoned). Returns success once the task is done; its result is then in the task.
Status runOnIoThread(const std::shared_ptr<IoTask>& task, JobContext& ctx, std::string_view where);

// Joins every I/O thread, including abandoned ones. For tests and orderly shutdown.
void waitForIoThreads();
// Number of I/O threads that have not finished yet (abandoned ones included). For tests.
int runningIoThreads();

template <class T, class Work>
class IoTaskOf final : public IoTask {
public:
    IoTaskOf(CancelToken cancel, Work work) : IoTask(std::move(cancel)), work_(std::move(work)) {}
    std::optional<Result<T>> result;

protected:
    void run() noexcept override
    {
        try {
            result.emplace((*work_)(static_cast<IoTask&>(*this)));
        } catch (...) {
            // `work` runs its OCCT code under guarded(); this only catches a failure to store the result.
            try {
                result.emplace(tl::unexpected(Error{ErrorCode::Internal, "The import or export failed.", {}}));
            } catch (...) {
            }
        }
        work_.reset(); // inputs (Shapes, strings) are released on this thread, before the task is done
    }

private:
    std::optional<Work> work_;
};

// Runs work(IoTask&) -> Result<T> on an I/O thread. `work` must own everything it uses (capture by value).
template <class T, class Work>
Result<T> runAbandonable(JobContext& ctx, std::string_view where, Work work)
{
    auto task = std::make_shared<IoTaskOf<T, Work>>(ctx.cancel, std::move(work));
    if (auto waited = runOnIoThread(task, ctx, where); !waited)
        return tl::unexpected(std::move(waited.error()));
    if (!task->result.has_value())
        return tl::unexpected(
            Error{ErrorCode::Internal, "The import or export returned no result.", {std::string(where)}});
    return std::move(*task->result);
}

// ---------- Files ----------

// A UTF-8 path as a filesystem path (UTF-16 on Windows). InvalidArgument if it is empty or not valid UTF-8.
Result<std::filesystem::path> pathFromUtf8(const std::string& utf8, std::string_view where);
// The path as UTF-8, for messages.
std::string utf8Of(const std::filesystem::path& path);
// Opens a file through the platform's wide-character API on Windows. nullptr on failure.
std::FILE* openFile(const std::filesystem::path& path, bool write);

// Checks that `path` names a readable regular file: NotFound if nothing is there, IoError for a folder or a
// file that cannot be opened.
Status checkReadable(const std::filesystem::path& path, std::string_view where);

// ---------- Meshing ----------
// One BRepMesh call at a time in the process (PLAN.md section 7 rule 3, spike S3). exportStl takes it.
// TODO(T-006): tessellate and triangulateExact must take this same lock; move it into the mesh code then.
std::mutex& processMeshMutex();

} // namespace ray::kernel::detail
