// Crash recovery in the app session (<ray/api.h> pendingRecoveries, recover, discardRecovery; PLAN.md 4.2), and the
// journal of never-saved documents: a backing .raycad file in <user data dir>/unsaved/ that holds no version and
// exists so the store's journal, which is keyed by a file id, can protect work that has no file yet.
#include "app_impl.h"

#include <chrono>
#include <system_error>
#include <thread>

namespace ray::api::detail {

void removeDocumentFiles(const std::string& path)
{
    if (path.empty()) {
        return;
    }
    std::error_code ec;
    for (const char* suffix : {"", "-journal", ".lock"}) {
        std::filesystem::remove(toFsPath(path + suffix), ec);
    }
}

std::string AppImpl::unsavedDir() const
{
    return journaling() ? fromFsPath(toFsPath(userDataDir_) / "unsaved") : std::string();
}

JournalSlot AppImpl::startUnsavedJournal(doc::DocStatePtr base)
{
    JournalSlot slot;
    if (!journaling()) {
        return slot;
    }
    std::error_code ec;
    std::filesystem::create_directories(toFsPath(unsavedDir()), ec);
    const std::string path = fromFsPath(toFsPath(unsavedDir()) / (Uuid::generate().toString() + ".raycad"));
    auto file = store::IDocumentFile::create(path);
    if (!file) {
        slot.error = file.error();
        return slot;
    }
    auto journal = store::IRecoveryJournal::start(**file, path, std::move(base));
    if (!journal) {
        file->reset();
        removeDocumentFiles(path);
        slot.error = journal.error();
        return slot;
    }
    slot.journal = std::move(*journal);
    slot.backing = std::move(*file);
    slot.backingPath = path;
    return slot;
}

std::optional<Error> AppImpl::ensureJournal(DocEntry& entry)
{
    std::lock_guard<std::mutex> lock(entry.journalMutex);
    if (!journaling() || entry.journalOff || entry.od.journal || entry.od.readOnly || entry.od.file) {
        return std::nullopt;
    }
    JournalSlot slot = startUnsavedJournal(entry.od.document->state());
    if (slot.error) {
        entry.journalOff = true;
        return slot.error;
    }
    entry.od.journal = std::move(slot.journal);
    entry.backing = std::move(slot.backing);
    entry.backingPath = std::move(slot.backingPath);
    return std::nullopt;
}

void AppImpl::journalFailed(DocEntry& entry, const Error& error)
{
    std::lock_guard<std::mutex> lock(entry.journalMutex);
    journalFailedLocked(entry, error);
}

void AppImpl::journalFailedLocked(DocEntry& entry, const Error& error)
{
    if (entry.od.journal) {
        (void)entry.od.journal->discard();
        entry.od.journal.reset();
    }
    entry.journalOff = true;
    dropBacking(entry);
    warn(entry, "Crash recovery is off for '" + entry.od.title + "' (" + error.message +
                    "); save the document to keep your changes safe");
}

void AppImpl::warn(DocEntry& entry, std::string message)
{
    log(LogLevel::Warn, "api", message);
    entry.od.warnings.push_back(message);
    emit(AppEvent::Kind::Warning, entry.od.id, std::move(message));
}

void AppImpl::dropBacking(DocEntry& entry)
{
    entry.backing.reset();
    removeDocumentFiles(entry.backingPath);
    entry.backingPath.clear();
}

namespace {
bool isUnder(const std::string& path, const std::string& dir)
{
    if (dir.empty()) {
        return false;
    }
    const std::string folder = pathKey(dir);
    const std::string key = pathKey(path);
    return key.size() > folder.size() && key.compare(0, folder.size(), folder) == 0 &&
           (key[folder.size()] == '/' || key[folder.size()] == '\\');
}
} // namespace

std::vector<RecoveryInfo> AppImpl::pendingRecoveries() const
{
    std::vector<RecoveryInfo> out;
    if (!journaling()) {
        return out;
    }
    const std::string unsaved = unsavedDir();
    for (const store::IRecoveryJournal::Pending& p : store::IRecoveryJournal::findPending()) {
        RecoveryInfo info;
        info.id = p.journalPath;
        info.filePath = p.filePath;
        info.unsaved = isUnder(p.filePath, unsaved);
        info.title = info.unsaved ? std::string("Untitled") : fileStem(p.filePath);
        info.steps = p.records.size();
        info.lastWriteMs = p.lastWriteMs;
        info.damagedTail = p.damagedTail;
        out.push_back(std::move(info));
    }
    return out;
}

namespace {
Result<store::IRecoveryJournal::Pending> findJournal(const RecoveryInfo& info)
{
    for (store::IRecoveryJournal::Pending& p : store::IRecoveryJournal::findPending()) {
        if (p.journalPath == info.id) {
            return std::move(p);
        }
    }
    return fail(ErrorCode::NotFound, "There are no changes to recover for '" + info.title + "' any more", info.id);
}
} // namespace

Status AppImpl::discardRecovery(const RecoveryInfo& info)
{
    if (!journaling()) {
        return fail(ErrorCode::NotFound, "Nothing is kept for recovery in this session");
    }
    auto pending = findJournal(info);
    if (!pending) {
        return tl::unexpected<Error>(pending.error());
    }
    if (auto st = store::IRecoveryJournal::discard(*pending); !st) {
        return st;
    }
    if (isUnder(pending->filePath, unsavedDir())) {
        removeDocumentFiles(pending->filePath);
    }
    return {};
}

JobHandle<OpenDocument*> AppImpl::recover(const RecoveryInfo& info)
{
    return runJob<OpenDocument*>(JobInfo{"Recover " + info.title, JobPriority::Normal, true},
                                 [this, info](JobContext& ctx) { return recoverWork(info, ctx); });
}

Result<OpenDocument*> AppImpl::recoverWork(const RecoveryInfo& info, JobContext& ctx)
{
    if (!journaling()) {
        return fail(ErrorCode::NotFound, "Nothing is kept for recovery in this session");
    }
    ctx.progress.set(0.0, "Reading the recovery journal");
    auto pending = findJournal(info);
    if (!pending) {
        return tl::unexpected<Error>(pending.error());
    }
    const std::string path = pending->filePath;
    const bool unsaved = isUnder(path, unsavedDir());
    const std::string key = pathKey(path);
    auto openHere = bridge_->call<bool>(
        [this, key]() -> Result<bool> {
            std::lock_guard<std::mutex> lock(docsMutex_);
            for (auto& e : documents_) {
                if (!e->key.empty() && e->key == key) {
                    return true;
                }
            }
            return false;
        },
        ctx.cancel);
    if (!openHere) {
        return tl::unexpected<Error>(openHere.error());
    }
    if (*openHere) {
        return fail(ErrorCode::Locked, "'" + fileName(path) + "' is open in this window: close it first", path);
    }

    // Read-write; the crashed session's lock is broken when its owner is provably dead or it has gone stale.
    ctx.progress.set(0.1, "Opening");
    std::unique_ptr<store::IDocumentFile> file;
    for (int attempt = 0;; ++attempt) {
        store::OpenInfo opened{};
        auto f = store::IDocumentFile::open(path, store::OpenMode::ReadWrite, &opened);
        if (!f) {
            return withContext(f.error(), "opening " + path + " to recover it");
        }
        if (!opened.openedReadOnlyBecauseLocked) {
            file = std::move(*f);
            break;
        }
        f->reset();
        const bool breakable = opened.lock && (opened.lock->stale || opened.lock->ownerProvablyDead);
        if (!breakable || attempt >= 30) {
            std::string who = "another session";
            if (opened.lock) {
                who = opened.lock->user + (opened.lock->host.empty() ? "" : " on " + opened.lock->host);
            }
            return fail(ErrorCode::Locked, "'" + fileName(path) + "' is open by " + who + "; recover it there", path);
        }
        if (auto st = store::breakStaleLock(path); !st && st.error().code != ErrorCode::Locked) {
            return withContext(st.error(), "breaking the lock of the session that ended");
        }
        if (auto st = ctx.cancel.check(); !st) {
            return tl::unexpected<Error>(st.error());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    ctx.progress.set(0.3, "Replaying changes");
    auto parts = makeParts(file.get());
    if (!parts) {
        return tl::unexpected<Error>(parts.error());
    }
    for (const auto& [hash, bytes] : pending->objects) {
        if (auto put = parts->pool->put(bytes); !put) {
            return withContext(put.error(), "restoring imported geometry");
        }
    }
    const DocParts made = *parts;
    auto recovered = store::IRecoveryJournal::recover(
        *pending, *file, [this, made](doc::DocStatePtr initial) { return makeDocumentFrom(made, std::move(initial)); });
    if (!recovered) {
        return withContext(recovered.error(), "recovering " + path);
    }
    std::vector<std::string> warnings;
    const std::size_t total = pending->records.size();
    if (recovered->stopped) {
        warnings.push_back(std::to_string(recovered->applied) + " of " + std::to_string(total) +
                           " changes recovered: " + recovered->stopped->message);
    }
    if (pending->damagedTail) {
        warnings.push_back("The last change before the session ended was incomplete and could not be recovered");
    }

    auto shared = std::make_shared<store::IRecoveryJournal::Recovered>(std::move(*recovered));
    auto holder = std::make_shared<std::unique_ptr<store::IDocumentFile>>(std::move(file));
    return bridge_->call<OpenDocument*>(
        [this, shared, holder, made, path, unsaved, warnings, &ctx]() -> Result<OpenDocument*> {
            if (!pointOfNoReturn(ctx.cancel)) {
                return fail(ErrorCode::Cancelled, "Cancelled"); // the journal is kept: it can be recovered again
            }
            NewEntry e;
            if (unsaved) {
                e.title = untitledName() + " (recovered)";
                e.journal.backing = std::move(*holder);
                e.journal.backingPath = path;
            } else {
                e.file = std::move(*holder);
                e.path = path;
                e.title = fileStem(path);
            }
            e.journal.journal = std::move(shared->journal);
            e.parts = made;
            e.document = std::move(shared->document);
            e.modified = shared->modified;
            e.warnings = warnings;
            auto entry = addEntry(std::move(e));
            if (!entry) {
                return tl::unexpected<Error>(entry.error());
            }
            if (!unsaved) {
                recent_->add(path);
                emit(AppEvent::Kind::RecentFilesChanged, {});
            }
            return &(*entry)->od;
        },
        ctx.cancel);
}

} // namespace ray::api::detail
