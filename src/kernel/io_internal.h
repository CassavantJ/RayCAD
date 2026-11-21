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
// throws its result away and exits. The one exception is an export that is already renaming its finished file
// into place (IoTask::beginCommit): the caller then waits for that rename and returns the export's own result, so
// the caller's answer and the file on disk always agree. Everything the I/O thread touches is owned by the shared
// IoTask: copies of the path, the options and the input Shapes (immutable, shared_ptr), its own file handles and its
// own progress state. It never touches the caller's JobContext, Progress or listener, so the caller's stack may be
// gone.
//
// For T-010 / T-019 (callers): submit the call as an ordinary job and do nothing special:
//
//     auto job = jobs.submit<kernel::ImportResult>({"Import"}, [path, opts](JobContext& ctx) {
//         return kernel::importStep(path, opts, ctx);
//     });
//     ...
//     job.cancel();   // the job's result is Error{Cancelled} within 100 ms; its worker is free again
//
// An abandoned read still holds one core and 7 to 8 MB per MiB of file until OCCT returns, and a new STEP or IGES
// import or export started meanwhile waits for it (the data exchange lock below; OCCT's data exchange is not
// reentrant) - on the I/O thread, polling its stop request, so the new call's caller can still be cancelled at once.
// I/O threads that are still running at process exit are joined by a static destructor before OCCT is unloaded;
// waitForIoThreads() joins them earlier (tests).
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
    bool stopRequested() const noexcept
    {
        return phase_.load(std::memory_order_acquire) == kStopped || cancel_.cancelled();
    }
    // I/O thread side, just before an export makes its result visible (renames its file into place). True if the
    // task may go on: it has not been cancelled, and from now on it cannot be abandoned (requestStop() returns false
    // and the caller waits for the task instead). False if it was cancelled or abandoned: the export must then
    // leave nothing behind and report Cancelled. Without this a cancel arriving after the last stopRequested() poll
    // made the caller report Cancelled while the abandoned thread still wrote the file.
    bool beginCommit() noexcept
    {
        if (cancel_.cancelled())
            return false;
        int expected = kRunning;
        return phase_.compare_exchange_strong(expected, kCommitting, std::memory_order_acq_rel);
    }
    // Progress of the whole task in [0, 1] and the stage name (a string literal). Thread-safe; cheap.
    void report(double fraction01, std::string_view stage) noexcept;

    // Caller side. Abandons the task; false if it can no longer be abandoned because it is committing its result
    // (beginCommit() won): the caller must then wait for it, which takes no longer than renaming a file.
    bool requestStop() noexcept
    {
        int expected = kRunning;
        if (phase_.compare_exchange_strong(expected, kStopped, std::memory_order_acq_rel))
            return true;
        return expected == kStopped;
    }
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
    static constexpr int kRunning = 0;
    static constexpr int kStopped = 1;    // abandoned by the caller
    static constexpr int kCommitting = 2; // making its result visible; can no longer be abandoned
    CancelToken cancel_;
    std::atomic<int> phase_{kRunning};
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

// ---------- Data exchange ----------
// One STEP or IGES reader or writer session at a time in the process: an import holds it while its reader is built,
// reads and transfers (not while healing); a STEP or IGES export while its writer transfers and writes. OCCT 8.0.1's
// data exchange keeps state in plain globals that every session writes (ThreadSanitizer, with OCCT itself built with
// -fsanitize=thread, on concurrent STEP imports): IFSelect_WorkSession's `errhand` (whether a selection is evaluated
// under try/catch), Interface_CheckTool's `errh`, STEPControl_ActorRead's non-manifold flag NM_DETECTED (set and read
// back within one transfer, so another import running at the same time could change how this file's compounds are
// built), and the static buffer of localtime() behind OSD_Process::SystemDate. Take it OUTSIDE guarded(), where a
// structured exception cannot skip its release. It also covers IGES, whose parser keeps its state in C globals.
std::mutex& dataExchangeMutex();

// Locks the deferred `lock`, polling the task's stop request every 5 ms while another holder has it. False if the
// task was stopped first (the lock is then not held).
bool lockPolling(std::unique_lock<std::mutex>& lock, IoTask& task);

// The thread team for one BRepMesh call at `chordTol` (mesh.cpp MEMORY CAP) on OCCT's default pool, set for the life
// of the object and put back to the previous value when it is destroyed. Construct it OUTSIDE guarded(), after taking
// the mesh lock: a structured exception skips the destructors of objects inside guarded(), and the setting is
// process-wide (only mesh calls use the pool, PLAN.md section 7 rule 4). Never throws; if the pool cannot be read,
// nothing is set and team() is 1.
class MeshTeamScope {
public:
    explicit MeshTeamScope(double chordTol) noexcept;
    ~MeshTeamScope();
    MeshTeamScope(const MeshTeamScope&) = delete;
    MeshTeamScope& operator=(const MeshTeamScope&) = delete;
    int team() const noexcept { return team_; }

private:
    int team_ = 1;
    int previous_ = -1; // -1: nothing was set
};

} // namespace ray::kernel::detail
