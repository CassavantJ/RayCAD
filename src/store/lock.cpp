// The lock file: content format, staleness rules, the owned lock with its heartbeat (PLAN.md 4.3).
#include "lock.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace ray::store::detail {
namespace {

std::atomic<std::int64_t> gHeartbeatMs{LockTiming{}.heartbeatMs};
std::atomic<std::int64_t> gStaleMs{LockTiming{}.staleMs};

// A lock file whose content cannot be parsed and whose owner is gone (it died between creating and writing it) is
// treated as stale after this long. On Windows the owner's handle exists from the moment the file does, so "owner
// gone" is certain at once; on POSIX the creator takes its flock a moment after creating the file.
#ifdef _WIN32
constexpr std::int64_t kUnreadableGraceMs = 0;
#else
constexpr std::int64_t kUnreadableGraceMs = 2'000;
#endif
constexpr std::size_t kMaxNameBytes = 120;
constexpr std::string_view kMagic = "RAYCAD-LOCK 1\n";

// Keeps a name on one line and inside its field, cut at a UTF-8 character boundary.
std::string cleanName(std::string_view name)
{
    std::string out;
    for (const char c : name) {
        const auto u = static_cast<unsigned char>(c);
        out.push_back(u < 0x20 || u == 0x7f ? '?' : c);
    }
    if (out.size() > kMaxNameBytes) {
        std::size_t cut = kMaxNameBytes;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) {
            --cut;
        }
        out.resize(cut);
    }
    return out.empty() ? std::string("?") : out;
}

std::string checksum(std::string_view body)
{
    Hasher hasher;
    hasher.str(body);
    const Hash256 hash = hasher.finish();
    return hash.hex().substr(0, 16);
}

bool parseInt(std::string_view text, std::int64_t& out)
{
    if (text.empty() || text.size() > 18) { // no overflow possible
        return false;
    }
    std::int64_t value = 0;
    bool negative = false;
    std::size_t i = 0;
    if (text[0] == '-') {
        negative = true;
        i = 1;
        if (text.size() == 1) {
            return false;
        }
    }
    for (; i < text.size(); ++i) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
        value = value * 10 + (text[i] - '0');
    }
    out = negative ? -value : value;
    return true;
}

void sleepMs(int ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

std::string describeOwner(const LockFields& f)
{
    return f.user + "@" + f.host + " (process " + std::to_string(f.pid) + ")";
}

} // namespace

void setLockTiming(LockTiming timing) noexcept
{
    gHeartbeatMs = timing.heartbeatMs;
    gStaleMs = timing.staleMs;
}

LockTiming lockTiming() noexcept
{
    return {gHeartbeatMs.load(), gStaleMs.load()};
}

std::string lockPathOf(const std::string& docPath)
{
    return docPath + ".lock";
}

std::string encodeLock(const LockFields& f)
{
    std::string body(kMagic);
    body += "user=" + cleanName(f.user) + "\n";
    body += "host=" + cleanName(f.host) + "\n";
    body += "pid=" + std::to_string(f.pid) + "\n";
    body += "since=" + std::to_string(f.sinceUnixMs) + "\n";
    body += "heartbeat=" + std::to_string(f.heartbeatUnixMs) + "\n";
    body += "beat=" + std::to_string(f.beat) + "\n";
    body += std::string("state=") + (f.saving ? "saving" : "idle") + "\n";
    body += "saves=" + std::to_string(f.saveCount) + "\n";
    std::string out = body + "check=" + checksum(body) + "\n";
    out.resize(kLockFileBytes, ' ');
    return out;
}

std::optional<LockFields> decodeLock(std::string_view bytes)
{
    if (bytes.substr(0, kMagic.size()) != kMagic) {
        return std::nullopt;
    }
    const std::size_t checkAt = bytes.find("\ncheck=");
    if (checkAt == std::string_view::npos || checkAt + 7 + 16 > bytes.size()) {
        return std::nullopt;
    }
    const std::string_view body = bytes.substr(0, checkAt + 1);
    if (bytes.substr(checkAt + 7, 16) != checksum(body)) {
        return std::nullopt;
    }
    LockFields f;
    int seen = 0;
    std::size_t pos = kMagic.size();
    while (pos < body.size()) {
        const std::size_t end = body.find('\n', pos);
        const std::string_view line = body.substr(pos, end - pos);
        pos = end + 1;
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            return std::nullopt;
        }
        const std::string_view key = line.substr(0, eq);
        const std::string_view value = line.substr(eq + 1);
        bool ok = true;
        if (key == "user") {
            f.user = std::string(value);
        } else if (key == "host") {
            f.host = std::string(value);
        } else if (key == "pid") {
            ok = parseInt(value, f.pid);
        } else if (key == "since") {
            ok = parseInt(value, f.sinceUnixMs);
        } else if (key == "heartbeat") {
            ok = parseInt(value, f.heartbeatUnixMs);
        } else if (key == "beat") {
            ok = parseInt(value, f.beat);
        } else if (key == "state") {
            ok = value == "saving" || value == "idle";
            f.saving = value == "saving";
        } else if (key == "saves") {
            ok = parseInt(value, f.saveCount);
        } else {
            continue; // a later version may add keys
        }
        if (!ok) {
            return std::nullopt;
        }
        ++seen;
    }
    if (seen != 8) {
        return std::nullopt;
    }
    return f;
}

std::string currentUserName()
{
    return cleanName(os::userName());
}

std::string currentHostName()
{
    return cleanName(os::hostName());
}

std::int64_t currentProcessId() noexcept
{
    return os::processId();
}

Result<LockState> readLock(const std::string& docPath)
{
    const std::string path = lockPathOf(docPath);
    const LockTiming timing = lockTiming();
    LockState state;
    // A torn read (the owner rewriting the block at that moment) fails the checksum: read again.
    for (int attempt = 0; attempt < 20; ++attempt) {
        auto view = os::readLockFile(path);
        if (!view) {
            return tl::unexpected<Error>(view.error());
        }
        const std::int64_t now = os::serverNowMs(path);
        const std::int64_t ageMs = now > view->mtimeMs ? now - view->mtimeMs : 0;
        state.ageSec = static_cast<double>(ageMs) / 1000.0;
        state.ownerDead = view->ownerDead;
        if (auto fields = decodeLock(view->bytes)) {
            state.fields = std::move(*fields);
            state.readable = true;
            state.stale = state.ownerDead || ageMs > timing.staleMs;
            return state;
        }
        state.readable = false;
        state.stale = (state.ownerDead && ageMs >= kUnreadableGraceMs) || ageMs > timing.staleMs;
        if (state.stale) {
            break;
        }
        sleepMs(5);
    }
    state.fields.user = "?";
    state.fields.host = "?";
    // A file that never becomes readable but is not stale either is reported as held by an unknown owner.
    return state;
}

LockInfo toLockInfo(const LockState& s)
{
    return LockInfo{s.fields.user, s.fields.host, s.fields.pid, s.fields.sinceUnixMs, s.ageSec, s.stale, s.ownerDead};
}

// ---------- OwnedLock ----------

OwnedLock::OwnedLock(HeldPtr file, LockFields fields) : file_(std::move(file)), fields_(std::move(fields))
{
    heartbeat_ = std::thread([this] { heartbeatLoop(); });
}

Result<std::unique_ptr<OwnedLock>> OwnedLock::acquire(const std::string& docPath)
{
    const std::string path = lockPathOf(docPath);
    for (int attempt = 0; attempt < 50; ++attempt) {
        HeldPtr held;
        auto outcome = os::createLockFile(path, held);
        if (!outcome) {
            return tl::unexpected<Error>(outcome.error());
        }
        if (*outcome == os::CreateOutcome::Exists) {
            return fail(ErrorCode::Locked, "The document is open for editing elsewhere", "lock file exists");
        }
        if (*outcome == os::CreateOutcome::Retry) {
            sleepMs(10);
            continue;
        }
        LockFields fields;
        fields.user = currentUserName();
        fields.host = currentHostName();
        fields.pid = currentProcessId();
        fields.sinceUnixMs = unixNowMs();
        fields.heartbeatUnixMs = fields.sinceUnixMs;
        // "saving" until the open has finished: opening may roll back a hot journal, which changes the file.
        fields.saving = true;
        if (auto st = os::writeHeld(*held, encodeLock(fields)); !st) {
            os::deleteHeld(std::move(held));
            return tl::unexpected<Error>(st.error());
        }
        return std::unique_ptr<OwnedLock>(new OwnedLock(std::move(held), std::move(fields)));
    }
    return fail(ErrorCode::Locked, "The document is being closed elsewhere; try again", "lock file delete pending");
}

OwnedLock::~OwnedLock()
{
    {
        std::lock_guard<std::mutex> guard(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (heartbeat_.joinable()) {
        heartbeat_.join();
    }
    os::deleteHeld(std::move(file_));
}

void OwnedLock::write()
{
    if (auto st = os::writeHeld(*file_, encodeLock(fields_)); !st) {
        ray::log(LogLevel::Warn, "store", "Could not update the lock file: " + st.error().message);
    }
}

void OwnedLock::beginWrite()
{
    std::lock_guard<std::mutex> guard(mutex_);
    fields_.saving = true;
    write();
}

void OwnedLock::endWrite()
{
    std::lock_guard<std::mutex> guard(mutex_);
    fields_.saving = false;
    ++fields_.saveCount;
    write();
}

LockFields OwnedLock::fields() const
{
    std::lock_guard<std::mutex> guard(mutex_);
    return fields_;
}

void OwnedLock::heartbeatLoop()
{
    std::unique_lock<std::mutex> guard(mutex_);
    for (;;) {
        const auto interval = std::chrono::milliseconds(lockTiming().heartbeatMs);
        if (wake_.wait_for(guard, interval, [this] { return stop_; })) {
            return;
        }
        fields_.heartbeatUnixMs = unixNowMs();
        ++fields_.beat;
        write();
    }
}

// ---------- Breaking a stale lock ----------

Status breakLock(const std::string& docPath)
{
    const std::string path = lockPathOf(docPath);
    const LockTiming timing = lockTiming();
    return os::breakLockFile(path, [&](const os::LockFileView& view) -> Status {
        const std::int64_t now = os::serverNowMs(path);
        const std::int64_t ageMs = now > view.mtimeMs ? now - view.mtimeMs : 0;
        const auto fields = decodeLock(view.bytes);
        if (fields) {
            if (view.ownerDead || ageMs > timing.staleMs) {
                return {};
            }
            return fail(ErrorCode::Locked, "The document is open for editing by " + describeOwner(*fields),
                        "lock is live; heartbeat " + std::to_string(ageMs / 1000) + " s ago");
        }
        if ((view.ownerDead && ageMs >= kUnreadableGraceMs) || ageMs > timing.staleMs) {
            return {};
        }
        return fail(ErrorCode::Locked, "The document is being opened elsewhere", "lock file is being written");
    });
}

} // namespace ray::store::detail

namespace ray::store {

Status breakStaleLock(const std::string& path)
{
    try {
        return detail::breakLock(path);
    } catch (const std::exception& e) {
        return detail::fail(ErrorCode::Internal, "Could not break the lock", e.what());
    }
}

} // namespace ray::store
