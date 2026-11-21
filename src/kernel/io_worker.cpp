// I/O threads, UTF-8 paths and the process-wide mesh lock (task T-005). See io_internal.h.
#include "io_internal.h"

#include <algorithm>
#include <exception>
#include <list>
#include <system_error>
#include <thread>
#include <vector>

namespace ray::kernel::detail {

// ---------- IoTask ----------

void IoTask::report(double fraction01, std::string_view stage) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (fraction01 > fraction_)
        fraction_ = std::min(fraction01, 1.0);
    if (!stage.empty())
        stage_ = stage;
}

IoTask::Snapshot IoTask::snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return Snapshot{fraction_, stage_, done_};
}

bool IoTask::waitDone(std::chrono::milliseconds timeout) const
{
    std::unique_lock<std::mutex> lock(mutex_);
    return doneSignal_.wait_for(lock, timeout, [this] { return done_; });
}

void IoTask::execute() noexcept
{
    run();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        done_ = true;
    }
    doneSignal_.notify_all();
}

namespace {

// Every I/O thread of the process. A thread is joined by the caller that waited for it, or, once abandoned,
// by the next start() after it finished, by waitForIoThreads(), or at the latest by the destructor of the
// function-local static below at process exit (before OCCT's libraries are unloaded).
class IoThreads {
public:
    IoThreads() = default;
    IoThreads(const IoThreads&) = delete;
    IoThreads& operator=(const IoThreads&) = delete;
    ~IoThreads() { joinAll(); }

    void start(const std::shared_ptr<IoTask>& task)
    {
        reapFinished();
        auto finished = std::make_shared<std::atomic<bool>>(false);
        Entry entry;
        entry.task = task.get();
        entry.finished = finished;
        // The thread owns a reference to the task: an abandoned task is destroyed on its own thread.
        entry.thread = std::thread([owned = task, finished]() mutable {
            owned->execute();
            owned.reset();
            finished->store(true, std::memory_order_release);
        });
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.push_back(std::move(entry));
    }

    void join(const IoTask* task)
    {
        std::thread thread;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = std::find_if(entries_.begin(), entries_.end(),
                                         [task](const Entry& entry) { return entry.task == task; });
            if (it == entries_.end())
                return;
            thread = std::move(it->thread);
            entries_.erase(it);
        }
        if (thread.joinable())
            thread.join();
    }

    void joinAll()
    {
        for (;;) {
            std::list<Entry> taken;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                taken.swap(entries_);
            }
            if (taken.empty())
                return;
            for (Entry& entry : taken)
                if (entry.thread.joinable())
                    entry.thread.join();
        }
    }

    int running()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<int>(std::count_if(entries_.begin(), entries_.end(), [](const Entry& entry) {
            return !entry.finished->load(std::memory_order_acquire);
        }));
    }

private:
    struct Entry {
        const IoTask* task = nullptr; // identity only; never dereferenced here
        std::shared_ptr<std::atomic<bool>> finished;
        std::thread thread;
    };

    void reapFinished()
    {
        std::list<Entry> finished;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto it = entries_.begin(); it != entries_.end();) {
                const auto next = std::next(it);
                if (it->finished->load(std::memory_order_acquire))
                    finished.splice(finished.end(), entries_, it);
                it = next;
            }
        }
        for (Entry& entry : finished)
            if (entry.thread.joinable())
                entry.thread.join();
    }

    std::mutex mutex_;
    std::list<Entry> entries_;
};

IoThreads& ioThreads()
{
    // The locks an I/O thread may hold are constructed first, so they are destroyed after the destructor that joins
    // the I/O threads still running at process exit.
    (void)processMeshMutex();
    (void)dataExchangeMutex();
    static IoThreads threads;
    return threads;
}

Error cancelledError(std::string_view where)
{
    return Error{ErrorCode::Cancelled, "Cancelled.", {std::string(where)}};
}

// Strict UTF-8 check: no overlong forms, no surrogates, nothing above U+10FFFF.
bool validUtf8(std::string_view text)
{
    std::size_t i = 0;
    while (i < text.size()) {
        const auto c = static_cast<unsigned char>(text[i]);
        std::size_t extra = 0;
        std::uint32_t code = 0;
        if (c < 0x80) {
            ++i;
            continue;
        }
        if ((c & 0xE0) == 0xC0) {
            extra = 1;
            code = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
            code = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
            code = c & 0x07;
        } else {
            return false;
        }
        if (i + extra >= text.size())
            return false;
        for (std::size_t k = 1; k <= extra; ++k) {
            const auto cc = static_cast<unsigned char>(text[i + k]);
            if ((cc & 0xC0) != 0x80)
                return false;
            code = (code << 6) | (cc & 0x3F);
        }
        static constexpr std::uint32_t kMinimum[4] = {0, 0x80, 0x800, 0x10000};
        if (code < kMinimum[extra] || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF))
            return false;
        i += extra + 1;
    }
    return true;
}

} // namespace

Status runOnIoThread(const std::shared_ptr<IoTask>& task, JobContext& ctx, std::string_view where)
{
    if (ctx.cancel.cancelled())
        return tl::unexpected(cancelledError(where));
    try {
        ioThreads().start(task);
    } catch (const std::exception& failure) {
        return tl::unexpected(Error{ErrorCode::Internal,
                                    std::string("Could not start a worker thread: ") + failure.what(),
                                    {std::string(where)}});
    }
    // Poll every 10 ms: cancel reaches the caller within one poll plus the cost of Progress::set.
    constexpr std::chrono::milliseconds kPoll{10};
    double lastFraction = -1.0;
    std::string_view lastStage;
    for (;;) {
        const bool done = task->waitDone(kPoll);
        const IoTask::Snapshot now = task->snapshot();
        if (!now.stage.empty() && (now.fraction != lastFraction || now.stage != lastStage)) {
            lastFraction = now.fraction;
            lastStage = now.stage;
            ctx.progress.set(now.fraction, now.stage);
        }
        if (done)
            break;
        // Abandoned: the thread stops at OCCT's next poll and cleans up itself. If it is already committing its
        // result (an export renaming its file into place), it is waited for instead and its result returned, so a
        // Cancelled export never leaves a file behind and a file that appeared is never reported as Cancelled.
        if (ctx.cancel.cancelled() && task->requestStop())
            return tl::unexpected(cancelledError(where));
    }
    ioThreads().join(task.get());
    return std::monostate{};
}

void waitForIoThreads()
{
    ioThreads().joinAll();
}

int runningIoThreads()
{
    return ioThreads().running();
}

// ---------- Files ----------

Result<std::filesystem::path> pathFromUtf8(const std::string& utf8, std::string_view where)
{
    if (utf8.empty())
        return tl::unexpected(Error{ErrorCode::InvalidArgument, "No file name was given.", {std::string(where)}});
    if (!validUtf8(utf8) || utf8.find('\0') != std::string::npos)
        return tl::unexpected(
            Error{ErrorCode::InvalidArgument, "The file name is not valid UTF-8 text.", {std::string(where)}});
    try {
        const std::u8string text(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size());
        return std::filesystem::path(text);
    } catch (const std::exception&) {
        return tl::unexpected(
            Error{ErrorCode::InvalidArgument, "The file name cannot be used on this system.", {std::string(where)}});
    }
}

std::string utf8Of(const std::filesystem::path& path)
{
    try {
        const std::u8string text = path.u8string();
        return std::string(reinterpret_cast<const char*>(text.data()), text.size());
    } catch (const std::exception&) {
        return std::string("(unprintable file name)");
    }
}

std::FILE* openFile(const std::filesystem::path& path, bool write)
{
#if defined(_WIN32)
    return _wfopen(path.c_str(), write ? L"wb" : L"rb");
#else
    return std::fopen(path.c_str(), write ? "wb" : "rb");
#endif
}

Status checkReadable(const std::filesystem::path& path, std::string_view where)
{
    std::error_code ec;
    const std::filesystem::file_status status = std::filesystem::status(path, ec);
    if (status.type() == std::filesystem::file_type::not_found)
        return tl::unexpected(
            Error{ErrorCode::NotFound, "The file does not exist: " + utf8Of(path), {std::string(where)}});
    if (ec)
        return tl::unexpected(Error{ErrorCode::IoError,
                                    "The file cannot be read: " + utf8Of(path) + " (" + ec.message() + ")",
                                    {std::string(where)}});
    if (status.type() == std::filesystem::file_type::directory)
        return tl::unexpected(
            Error{ErrorCode::IoError, "This is a folder, not a file: " + utf8Of(path), {std::string(where)}});
    std::FILE* file = openFile(path, false);
    if (file == nullptr)
        return tl::unexpected(
            Error{ErrorCode::IoError, "The file cannot be opened: " + utf8Of(path), {std::string(where)}});
    std::fclose(file);
    return std::monostate{};
}

std::mutex& processMeshMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::mutex& dataExchangeMutex()
{
    static std::mutex mutex;
    return mutex;
}

bool lockPolling(std::unique_lock<std::mutex>& lock, IoTask& task)
{
    while (!lock.try_lock()) {
        if (task.stopRequested())
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

} // namespace ray::kernel::detail
