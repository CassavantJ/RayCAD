// The application session (<ray/api.h> IApp, HeadlessApp, makeApp, makeHeadlessApp).
//
// A document is made the way <ray/document.h> describes: an object pool for imported BREP objects, a chain
// (pool, then the .raycad file) the rebuilder reads them through, and a Document that rebuilds on the app's jobs.
// Work that takes time (opening, importing, saving, exporting) runs as a job on a worker; the short steps that touch
// a document (creating it, executing a command, switching its file) hop to the UI thread through UiBridge.
//
// Recovery journal (PLAN.md 4.2, store.h IRecoveryJournal; the recovery side is in recovery.cpp): a document opened
// read-write starts its journal at open; a never-saved one at its first change, into a backing file in
// <user data dir>/unsaved. Every executed command, undo and redo is appended on the UI thread right after it
// succeeded, imported objects before their command; save calls markSaved then markClean; close() discards the
// journal; the app's destructor keeps the journals of documents with unsaved changes. A failed append switches the
// journal off for that document with a warning, and work goes on.
#include "app_impl.h"

#include <algorithm>
#include <exception>
#include <new>
#include <system_error>
#include <thread>

namespace ray::store::detail {
// Defined in src/store/journal.cpp (declared in its private journal.h): where journals live; "" = the default.
void setRecoveryDir(const std::string& utf8Dir);
} // namespace ray::store::detail

namespace ray::api::detail {

// ---------- FileSource ----------
Result<std::vector<std::byte>> FileSource::object(const Hash256& hash) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (detached_) {
        return fail(ErrorCode::Cancelled, "The document was closed");
    }
    if (file_ == nullptr) {
        return fail(ErrorCode::NotFound, "The document has no saved objects yet", hash.hex());
    }
    return file_->get(hash);
}

void FileSource::set(store::IObjectStore* file)
{
    std::lock_guard<std::mutex> lock(mutex_);
    file_ = file;
}

void FileSource::detach()
{
    std::lock_guard<std::mutex> lock(mutex_);
    detached_ = true;
    file_ = nullptr;
}

// ---------- Construction and shutdown ----------
Result<std::unique_ptr<AppImpl>> AppImpl::create(AppOptions options, std::unique_ptr<QueueDispatcher> owned)
{
    try {
        std::unique_ptr<AppImpl> app(new AppImpl());
        if (auto st = app->init(std::move(options), std::move(owned)); !st) {
            return tl::unexpected<Error>(st.error());
        }
        return app;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::Internal, "Out of memory while starting the application");
    } catch (const std::exception& e) {
        return fail(ErrorCode::Internal, std::string("The application could not start: ") + e.what());
    }
}

Status AppImpl::init(AppOptions options, std::unique_ptr<QueueDispatcher> owned)
{
    uiThread_ = std::this_thread::get_id();
    ownedDispatcher_ = std::move(owned);
    dispatcher_ = ownedDispatcher_ ? ownedDispatcher_.get() : options.dispatcher;
    if (dispatcher_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "The application needs a UI dispatcher");
    }
    if (options.jobs != nullptr) {
        jobs_ = options.jobs;
    } else {
        ownedJobs_ = makeJobSystem(*dispatcher_, std::max(0, options.workerThreads));
        jobs_ = ownedJobs_.get();
    }
    if (jobs_ == nullptr) {
        return fail(ErrorCode::Internal, "The job system could not start");
    }
    bridge_ = std::make_unique<UiBridge>(*dispatcher_, uiThread_);
    featureTypes_ = std::make_shared<doc::FeatureRegistry>();
    if (auto st = doc::registerBuiltinFeatureTypes(*featureTypes_); !st) {
        return withContext(st.error(), "registering the built-in feature types");
    }
    commands_ = doc::builtinCommandRegistry();
    if (!commands_) {
        return fail(ErrorCode::Internal, "Out of memory while registering the document commands");
    }
    userDataDir_ = resolveUserDataDir(options.userDataDir);
    recent_ = std::make_unique<RecentFiles>(userDataDir_);
    if (journaling()) {
        // Journals live in <user data dir>/recovery (store.h). The store has no public setting for the folder, so the
        // app names it through the store's own hook; for the platform default it is the store's default anyway.
        store::detail::setRecoveryDir(fromFsPath(toFsPath(userDataDir_) / "recovery"));
    }
    actions_ = makeActionRegistry(*this);
    if (auto st = registerBuiltinActions(*this); !st) {
        return withContext(st.error(), "registering the built-in actions");
    }
    return {};
}

AppImpl::~AppImpl()
{
    try {
        // 1. Stop: UI hops from workers give up, posted notifications do nothing, every app job is cancelled and
        //    waited for (each sees its token cancelled; a job still in an uncancellable kernel stage is waited out).
        if (bridge_) {
            bridge_->shutdown();
        }
        alive_->ok.store(false);
        std::vector<std::function<void()>> cancels;
        std::vector<std::function<void()>> waits;
        {
            std::lock_guard<std::mutex> lock(jobsMutex_);
            for (auto& [id, job] : tracked_) {
                if (job.cancel) {
                    cancels.push_back(job.cancel);
                }
                if (job.wait) {
                    waits.push_back(job.wait);
                }
            }
        }
        for (auto& c : cancels) {
            c();
        }
        for (auto& w : waits) {
            w();
        }
        // 2. Close every document; a rebuild still running reads no file after this. Unsaved changes keep their
        //    journal, so the next start offers them for recovery.
        std::vector<DocEntry*> entries;
        {
            std::lock_guard<std::mutex> lock(docsMutex_);
            for (auto& e : documents_) {
                entries.push_back(e.get());
            }
        }
        for (DocEntry* e : entries) {
            const bool unsaved = e->od.modified || (e->od.document && e->od.document->dirty());
            closeEntry(e, unsaved);
        }
        // 3. The job system (if ours) cancels and waits for the remaining jobs (rebuilds).
        ownedJobs_.reset();
    } catch (...) {
    }
}

// ---------- Documents ----------
OpenDocument* AppImpl::active()
{
    std::lock_guard<std::mutex> lock(docsMutex_);
    return active_ ? &active_->od : nullptr;
}

void AppImpl::setActive(OpenDocument* od)
{
    std::string id;
    {
        std::lock_guard<std::mutex> lock(docsMutex_);
        DocEntry* next = nullptr;
        if (od != nullptr) {
            for (auto& e : documents_) {
                if (&e->od == od) {
                    next = e.get();
                }
            }
            if (next == nullptr) {
                return;
            }
        }
        if (next == active_) {
            return;
        }
        active_ = next;
        id = next ? next->od.id : std::string();
    }
    emit(AppEvent::Kind::ActiveDocumentChanged, id);
}

std::vector<OpenDocument*> AppImpl::documents()
{
    std::lock_guard<std::mutex> lock(docsMutex_);
    std::vector<OpenDocument*> out;
    for (auto& e : documents_) {
        out.push_back(&e->od);
    }
    return out;
}

OpenDocument* AppImpl::findDocument(std::string_view id)
{
    std::lock_guard<std::mutex> lock(docsMutex_);
    for (auto& e : documents_) {
        if (e->od.id == id) {
            return &e->od;
        }
    }
    return nullptr;
}

DocEntry* AppImpl::entryOf(const OpenDocument& od)
{
    std::lock_guard<std::mutex> lock(docsMutex_);
    for (auto& e : documents_) {
        if (&e->od == &od) {
            return e.get();
        }
    }
    return nullptr;
}

Result<DocEntry*> AppImpl::resolve(std::string_view id)
{
    std::lock_guard<std::mutex> lock(docsMutex_);
    if (id.empty()) {
        if (active_ == nullptr) {
            return fail(ErrorCode::NotFound, "No document is open");
        }
        return active_;
    }
    for (auto& e : documents_) {
        if (e->od.id == id) {
            return e.get();
        }
    }
    return fail(ErrorCode::NotFound, "No open document has the id " + std::string(id));
}

Result<BusyGuard> AppImpl::claim(std::string_view id)
{
    auto entry = resolve(id);
    if (!entry) {
        return tl::unexpected<Error>(entry.error());
    }
    return BusyGuard(*entry);
}

std::string AppImpl::untitledName()
{
    return "Untitled " + std::to_string(++untitledCount_);
}

Result<DocEntry*> AppImpl::addEntry(std::unique_ptr<store::IDocumentFile> file, doc::DocStatePtr initial,
                                    std::string path, std::string title, bool readOnly,
                                    std::optional<store::LockInfo> lock)
{
    NewEntry e;
    e.file = std::move(file);
    e.initial = std::move(initial);
    e.path = std::move(path);
    e.title = std::move(title);
    e.readOnly = readOnly;
    e.lock = std::move(lock);
    return addEntry(std::move(e));
}

Result<DocParts> AppImpl::makeParts(store::IObjectStore* file)
{
    DocParts parts;
    parts.pool = doc::makeObjectPool();
    parts.fileSource = std::make_shared<FileSource>(file);
    if (!parts.pool) {
        return fail(ErrorCode::Internal, "Out of memory while creating a document");
    }
    parts.chain = doc::makeObjectChain({parts.pool, parts.fileSource});
    parts.rebuilder = doc::makeRebuilder(*jobs_, featureTypes_, parts.chain);
    if (!parts.chain || !parts.rebuilder) {
        return fail(ErrorCode::Internal, "Out of memory while creating a document");
    }
    return parts;
}

std::unique_ptr<doc::Document> AppImpl::makeDocumentFrom(const DocParts& parts, doc::DocStatePtr initial)
{
    return doc::makeDocument(*dispatcher_, *jobs_, parts.rebuilder, commands_, std::move(initial));
}

Result<DocEntry*> AppImpl::addEntry(NewEntry e)
{
    if (!e.parts) {
        auto parts = makeParts(e.file.get());
        if (!parts) {
            return tl::unexpected<Error>(parts.error());
        }
        e.parts = std::move(*parts);
    }
    if (!e.document) {
        e.document = makeDocumentFrom(*e.parts, std::move(e.initial));
    }
    if (!e.document) {
        return fail(ErrorCode::Internal, "Out of memory while creating a document");
    }
    auto entry = std::make_unique<DocEntry>();
    OpenDocument& od = entry->od;
    od.id = Uuid::generate().toString();
    od.path = std::move(e.path);
    od.title = std::move(e.title);
    od.readOnly = e.readOnly;
    od.lock = std::move(e.lock);
    od.objects = e.parts->pool;
    od.document = std::move(e.document);
    od.file = std::move(e.file);
    od.journal = std::move(e.journal.journal);
    od.modified = e.modified;
    entry->fileSource = e.parts->fileSource;
    entry->objects = e.parts->chain;
    entry->backing = std::move(e.journal.backing);
    entry->backingPath = std::move(e.journal.backingPath);
    entry->key = od.path.empty() ? std::string() : pathKey(od.path);
    DocEntry* raw = entry.get();
    {
        std::lock_guard<std::mutex> guard(docsMutex_);
        documents_.push_back(std::move(entry));
        active_ = raw;
    }
    emit(AppEvent::Kind::DocumentOpened, raw->od.id);
    emit(AppEvent::Kind::ActiveDocumentChanged, raw->od.id);
    if (e.journal.error) {
        raw->journalOff = true;
        warn(*raw, "Crash recovery is off for '" + raw->od.title + "': " + e.journal.error->message);
    }
    for (std::string& w : e.warnings) {
        warn(*raw, std::move(w));
    }
    return raw;
}

Result<OpenDocument*> AppImpl::newDocument(std::string_view templateId)
{
    if (!templateId.empty() && templateId != "default") {
        return fail(ErrorCode::NotFound, "There is no document template named '" + std::string(templateId) + "'");
    }
    try {
        auto entry = addEntry(nullptr, nullptr, {}, untitledName(), false, std::nullopt);
        if (!entry) {
            return tl::unexpected<Error>(entry.error());
        }
        return &(*entry)->od;
    } catch (const std::exception& e) {
        return fail(ErrorCode::Internal, std::string("Cannot create a document: ") + e.what());
    }
}

void AppImpl::closeEntry(DocEntry* entry, bool keepJournal)
{
    if (entry == nullptr) {
        return;
    }
    std::unique_ptr<DocEntry> owned;
    bool activeChanged = false;
    {
        std::lock_guard<std::mutex> lock(docsMutex_);
        auto it = std::find_if(documents_.begin(), documents_.end(), [&](const auto& e) { return e.get() == entry; });
        if (it == documents_.end()) {
            return;
        }
        owned = std::move(*it);
        documents_.erase(it);
        if (active_ == entry) {
            active_ = documents_.empty() ? nullptr : documents_.back().get();
            activeChanged = true;
        }
    }
    const std::string id = owned->od.id;
    owned->fileSource->detach();
    owned->od.document.reset();
    {
        std::lock_guard<std::mutex> journalLock(owned->journalMutex);
        if (owned->od.journal && !keepJournal) {
            if (auto st = owned->od.journal->discard(); !st) {
                log(LogLevel::Warn, "api", "The recovery journal could not be deleted: " + st.error().message);
            }
        }
        owned->od.journal.reset(); // without discard() the journal is kept for the next start
    }
    owned->od.file.reset();
    if (keepJournal) {
        owned->backing.reset();
    } else {
        dropBacking(*owned);
    }
    owned.reset();
    emit(AppEvent::Kind::DocumentClosed, id);
    if (activeChanged) {
        OpenDocument* now = active();
        emit(AppEvent::Kind::ActiveDocumentChanged, now ? now->id : std::string());
    }
}

Status AppImpl::close(OpenDocument& od)
{
    DocEntry* entry = entryOf(od);
    if (entry == nullptr) {
        return fail(ErrorCode::NotFound, "The document is not open");
    }
    if (entry->busy.load() > 0) {
        return fail(ErrorCode::Locked,
                    "'" + od.title + "' is being saved or changed by a running job; try again when it ends");
    }
    closeEntry(entry);
    return {};
}

void AppImpl::setConfirmHook(ConfirmHook hook)
{
    std::lock_guard<std::mutex> lock(hookMutex_);
    hook_ = std::move(hook);
}

ConfirmHook AppImpl::confirmHook() const
{
    std::lock_guard<std::mutex> lock(hookMutex_);
    return hook_;
}

// ---------- Commands ----------
Result<doc::DocStatePtr> AppImpl::execute(DocEntry& entry, const doc::Command& command)
{
    // A never-saved document starts its journal at its first change, from the state before it.
    if (auto error = ensureJournal(entry)) {
        journalFailed(entry, *error);
    }
    auto result = entry.od.document->execute(command);
    if (result) {
        std::lock_guard<std::mutex> lock(entry.journalMutex);
        if (entry.od.journal && !entry.journalOff) {
            // The command as the document executed it (canonical arguments).
            const std::optional<doc::Command> executed = entry.od.document->undoCommand();
            if (auto st = entry.od.journal->append(executed ? *executed : command, *result); !st) {
                journalFailedLocked(entry, st.error());
            }
        }
    }
    return result;
}

Status AppImpl::undo(DocEntry& entry, bool redo)
{
    doc::Document& d = *entry.od.document;
    if (redo ? !d.canRedo() : !d.canUndo()) {
        return fail(ErrorCode::InvalidArgument, redo ? "There is nothing to redo" : "There is nothing to undo");
    }
    Status done = redo ? d.redo() : d.undo();
    if (done) {
        std::lock_guard<std::mutex> lock(entry.journalMutex);
        if (entry.od.journal && !entry.journalOff) {
            const doc::DocStatePtr after = d.state();
            auto st = redo ? entry.od.journal->appendRedo(after) : entry.od.journal->appendUndo(after);
            if (!st) {
                journalFailedLocked(entry, st.error());
            }
        }
    }
    return done;
}

// ---------- Open and import ----------
namespace {
bool isImportExtension(const std::string& ext)
{
    return ext == ".step" || ext == ".stp" || ext == ".p21" || ext == ".iges" || ext == ".igs";
}

std::string lockText(const store::LockInfo& lock)
{
    std::string who = lock.user.empty() ? std::string("another user") : lock.user;
    if (!lock.host.empty()) {
        who += " on " + lock.host;
    }
    return who;
}
} // namespace

Result<OpenDocument*> AppImpl::openWork(const std::string& path, JobContext& ctx)
{
    if (path.empty()) {
        return fail(ErrorCode::InvalidArgument, "No file name given");
    }
    const std::string ext = lowerExtension(path);
    if (isImportExtension(ext)) {
        auto id = importWork(std::nullopt, path, ImportOptions{}, ctx);
        if (!id) {
            return tl::unexpected<Error>(id.error());
        }
        return bridge_->call<OpenDocument*>(
            [this, id = *id]() -> Result<OpenDocument*> {
                OpenDocument* od = findDocument(id);
                if (!od) {
                    return fail(ErrorCode::Cancelled, "The document was closed");
                }
                return od;
            },
            ctx.cancel);
    }
    if (ext == ".dxf") {
        return fail(ErrorCode::InvalidArgument, "DXF files open into a sketch, which is not available yet", path);
    }
    if (ext != std::string(store::kFileExtension)) {
        return fail(ErrorCode::InvalidArgument,
                    "RayCAD opens .raycad documents and STEP or IGES files, not '" + fileName(path) + "'", path);
    }
    // A document this app has open already is only made active.
    const std::string key = pathKey(path);
    auto existing = bridge_->call<OpenDocument*>(
        [this, key]() -> Result<OpenDocument*> {
            DocEntry* found = nullptr;
            {
                std::lock_guard<std::mutex> lock(docsMutex_);
                for (auto& e : documents_) {
                    if (!e->key.empty() && e->key == key) {
                        found = e.get();
                    }
                }
            }
            if (found) {
                setActive(&found->od);
                recent_->add(found->od.path);
                emit(AppEvent::Kind::RecentFilesChanged, {});
                return &found->od;
            }
            return static_cast<OpenDocument*>(nullptr);
        },
        ctx.cancel);
    if (!existing) {
        return tl::unexpected<Error>(existing.error());
    }
    if (*existing) {
        return *existing;
    }

    ctx.progress.set(0.0, "Opening");
    store::OpenInfo info{};
    auto file = store::IDocumentFile::open(path, store::OpenMode::ReadWrite, &info);
    if (!file) {
        return withContext(file.error(), "opening " + path);
    }
    if (auto st = ctx.cancel.check(); !st) {
        return tl::unexpected<Error>(st.error());
    }
    ctx.progress.set(0.5, "Reading");
    doc::DocStatePtr head;
    auto loaded = (*file)->loadHead();
    if (loaded) {
        head = *loaded;
    } else if (loaded.error().code != ErrorCode::NotFound) {
        return withContext(loaded.error(), "reading " + path);
    } // NotFound: a file with no saved version yet opens as an empty document.
    const bool readOnly = info.openedReadOnlyBecauseLocked;
    std::optional<store::LockInfo> lock = info.lock;
    if (readOnly && lock) {
        log(LogLevel::Info, "api",
            "'" + path + "' is open in another session (" + lockText(*lock) + "): opened read-only");
    }
    // Changes from a session that ended unexpectedly come first (its lock may still be there, so a read-only open
    // checks too); otherwise a read-write document starts its journal now.
    auto slot = std::make_shared<JournalSlot>();
    if (journaling()) {
        if (auto pending = store::IRecoveryJournal::findPendingFor(**file)) {
            return fail(ErrorCode::Locked,
                        "'" + fileName(path) +
                            "' has unsaved changes from a session that ended unexpectedly; recover or discard them "
                            "first",
                        "recovery journal " + pending->journalPath);
        }
    }
    if (!readOnly && journaling()) {
        auto journal = store::IRecoveryJournal::start(**file, path, head);
        if (journal) {
            slot->journal = std::move(*journal);
        } else {
            slot->error = journal.error();
        }
    }
    // std::function needs a copyable closure: the file travels in a shared holder (closed if the hop is abandoned).
    auto holder = std::make_shared<std::unique_ptr<store::IDocumentFile>>(std::move(*file));
    return bridge_->call<OpenDocument*>(
        [this, holder, slot, head, path, readOnly, lock]() -> Result<OpenDocument*> {
            NewEntry e;
            e.file = std::move(*holder);
            e.initial = head;
            e.path = path;
            e.title = fileStem(path);
            e.readOnly = readOnly;
            e.lock = readOnly ? lock : std::nullopt;
            e.journal = std::move(*slot);
            auto entry = addEntry(std::move(e));
            if (!entry) {
                return tl::unexpected<Error>(entry.error());
            }
            recent_->add(path);
            emit(AppEvent::Kind::RecentFilesChanged, {});
            return &(*entry)->od;
        },
        ctx.cancel);
}

Result<std::string> AppImpl::importWork(std::optional<std::string> documentId, const std::string& path,
                                        const ImportOptions& options, JobContext& ctx)
{
    // Claim the target first, so it cannot close while the file is read.
    auto target = bridge_->call<std::shared_ptr<BusyGuard>>(
        [this, documentId]() -> Result<std::shared_ptr<BusyGuard>> {
            if (!documentId) {
                return std::make_shared<BusyGuard>();
            }
            if (documentId->empty() && active() == nullptr) {
                return std::make_shared<BusyGuard>();
            }
            auto claimed = claim(*documentId);
            if (!claimed) {
                return tl::unexpected<Error>(claimed.error());
            }
            return std::make_shared<BusyGuard>(std::move(*claimed));
        },
        ctx.cancel);
    if (!target) {
        return tl::unexpected<Error>(target.error());
    }
    std::shared_ptr<BusyGuard> guard = *target;

    doc::ImportRequest request;
    request.feature = FeatureId{Uuid::generate()};
    request.name = options.name;
    request.path = path;
    if (options.heal) {
        request.options.heal = *options.heal;
    }
    request.role = options.role;
    auto prepared = doc::prepareImport(request, ctx);
    if (!prepared) {
        return withContext(prepared.error(), "importing " + path);
    }
    auto shared = std::make_shared<doc::PreparedImport>(std::move(*prepared));

    // The imported objects go into the recovery journal before the import command, here on the worker (they can be
    // large). An import into an open document holds its save lock until the command is journalled, so a save never
    // runs between the objects and the command (markSaved would otherwise drop the objects from the journal).
    auto slot = std::make_shared<JournalSlot>();
    std::optional<Error> journalError;
    std::unique_lock<std::mutex> saving;
    const auto appendObjects = [&shared](store::IRecoveryJournal& journal) -> std::optional<Error> {
        for (const auto& [hash, bytes] : shared->objects) {
            if (auto st = journal.appendObject(hash, bytes); !st) {
                return st.error();
            }
        }
        return std::nullopt;
    };
    if (DocEntry* existing = guard->get()) {
        saving = std::unique_lock<std::mutex>(existing->saveMutex);
        journalError = ensureJournal(*existing);
        if (!journalError) {
            std::lock_guard<std::mutex> lock(existing->journalMutex);
            if (existing->od.journal && !existing->journalOff) {
                journalError = appendObjects(*existing->od.journal);
            }
        }
    } else if (journaling()) {
        *slot = startUnsavedJournal(nullptr);
        if (slot->journal) {
            if (auto error = appendObjects(*slot->journal)) {
                (void)slot->journal->discard();
                slot->journal.reset();
                slot->backing.reset();
                removeDocumentFiles(slot->backingPath);
                slot->backingPath.clear();
                slot->error = *error;
            }
        }
    }
    return bridge_->call<std::string>(
        [this, guard, shared, path, slot, journalError]() -> Result<std::string> {
            DocEntry* entry = guard->get();
            if (entry == nullptr) {
                NewEntry e;
                e.title = fileStem(path);
                e.journal = std::move(*slot);
                auto created = addEntry(std::move(e));
                if (!created) {
                    return tl::unexpected<Error>(created.error());
                }
                entry = *created;
            } else if (journalError) {
                journalFailed(*entry, *journalError);
            }
            for (const auto& [hash, bytes] : shared->objects) {
                auto put = entry->od.objects->put(bytes);
                if (!put) {
                    return withContext(put.error(), "keeping the imported geometry");
                }
            }
            auto executed = execute(*entry, shared->command);
            if (!executed) {
                return tl::unexpected<Error>(executed.error());
            }
            return entry->od.id;
        },
        ctx.cancel);
}

JobHandle<OpenDocument*> AppImpl::open(const std::string& path)
{
    return runJob<OpenDocument*>(JobInfo{"Open " + fileName(path), JobPriority::Normal, true},
                                 [this, path](JobContext& ctx) { return openWork(path, ctx); });
}

JobHandle<std::monostate> AppImpl::importFile(OpenDocument& od, const std::string& path)
{
    const std::string id = od.id;
    return runJob<std::monostate>(JobInfo{"Import " + fileName(path), JobPriority::Normal, true},
                                  [this, id, path](JobContext& ctx) -> Result<std::monostate> {
                                      auto done = importWork(id, path, ImportOptions{}, ctx);
                                      if (!done) {
                                          return tl::unexpected<Error>(done.error());
                                      }
                                      return std::monostate{};
                                  });
}

// ---------- Save ----------
Result<store::Commit> AppImpl::saveWork(DocEntry& entry, std::optional<std::string> saveAsPath, bool overwrite,
                                        JobContext& ctx)
{
    std::lock_guard<std::mutex> saving(entry.saveMutex);
    if (auto st = ctx.cancel.check(); !st) {
        return tl::unexpected<Error>(st.error());
    }
    OpenDocument& od = entry.od;
    // The path, the file and the read-only flag change only in this function, under saveMutex.
    const doc::DocStatePtr state = od.document->state();
    std::string path = saveAsPath ? *saveAsPath : od.path;
    if (path.empty()) {
        return fail(ErrorCode::InvalidArgument, "'" + od.title + "' has not been saved yet: give a file name");
    }
    if (lowerExtension(path).empty()) {
        path += std::string(store::kFileExtension);
    }
    if (lowerExtension(path) != std::string(store::kFileExtension)) {
        return fail(ErrorCode::InvalidArgument, "Documents are saved as .raycad files; use export for other formats",
                    path);
    }
    const bool asNew = od.file == nullptr || pathKey(path) != entry.key;
    if (!asNew && od.readOnly) {
        std::string who = od.lock ? lockText(*od.lock) : std::string("another session");
        return fail(ErrorCode::Locked,
                    "'" + od.title + "' is open read-only because " + who + " has it open; save it under another name");
    }

    std::unique_ptr<store::IDocumentFile> created;
    store::IDocumentFile* target = od.file.get();
    if (asNew) {
        ctx.progress.set(0.0, "Creating the file");
        if (fileExists(path)) {
            if (!overwrite) {
                return fail(ErrorCode::InvalidArgument, "A file named '" + fileName(path) + "' already exists", path);
            }
            // Only a RayCAD document nobody has open is replaced.
            store::OpenInfo info{};
            auto probe = store::IDocumentFile::open(path, store::OpenMode::ReadWrite, &info);
            if (!probe) {
                return withContext(probe.error(), "checking the file to replace, " + path);
            }
            const bool lockedElsewhere = info.openedReadOnlyBecauseLocked;
            probe->reset();
            if (lockedElsewhere) {
                return fail(ErrorCode::Locked,
                            "'" + fileName(path) + "' is open " +
                                (info.lock ? "by " + lockText(*info.lock) : std::string("elsewhere")) +
                                " and cannot be replaced",
                            path);
            }
            std::error_code ec;
            std::filesystem::remove(toFsPath(path), ec);
            if (ec) {
                return fail(ErrorCode::IoError, "Cannot replace '" + fileName(path) + "': " + ec.message(), path);
            }
        }
        auto made = store::IDocumentFile::create(path);
        if (!made) {
            return withContext(made.error(), "creating " + path);
        }
        created = std::move(*made);
        target = created.get();
    }

    // Every imported BREP object the state names, from the pool or the file the document came from.
    const std::vector<Hash256> needed = doc::referencedObjects(*state);
    Progress objectsProgress = ctx.progress.sub(0.6);
    for (std::size_t i = 0; i < needed.size(); ++i) {
        if (auto st = ctx.cancel.check(); !st) {
            return tl::unexpected<Error>(st.error());
        }
        if (!target->has(needed[i])) {
            auto bytes = entry.objects->object(needed[i]);
            if (!bytes) {
                return withContext(bytes.error(), "reading imported geometry " + needed[i].hex());
            }
            auto put = target->put(store::ObjectKind::Brep, *bytes);
            if (!put) {
                return withContext(put.error(), "writing imported geometry");
            }
        }
        objectsProgress.set(static_cast<double>(i + 1) / static_cast<double>(needed.size()), "Saving geometry");
    }
    store::IDocumentFile::SaveRequest request;
    request.state = state;
    request.message = "Save";
    JobContext saveCtx{ctx.cancel, ctx.progress.sub(0.4)};
    auto commit = target->save(request, saveCtx);
    if (!commit) {
        return withContext(commit.error(), "saving " + path);
    }

    // A new file gets a new journal, from the saved state (changes made while a save-as ran are in neither).
    auto slot = std::make_shared<JournalSlot>();
    if (asNew && journaling()) {
        auto journal = store::IRecoveryJournal::start(*created, path, state);
        if (journal) {
            slot->journal = std::move(*journal);
        } else {
            slot->error = journal.error();
        }
    }
    auto holder = std::make_shared<std::unique_ptr<store::IDocumentFile>>(std::move(created));
    const store::Commit done = *commit;
    // The new file, the journal and markClean are applied on the UI thread. If that hop is abandoned (the app shuts
    // down), the file was still written completely.
    auto applied = bridge_->call<std::monostate>(
        [this, &entry, holder, slot, path, state, asNew, done]() -> Result<std::monostate> {
            OpenDocument& o = entry.od;
            if (asNew) {
                entry.fileSource->set(holder->get());
                std::unique_ptr<store::IDocumentFile> old = std::move(o.file);
                o.file = std::move(*holder);
                {
                    // The old file's journal (or the never-saved document's backing file) is done with.
                    std::lock_guard<std::mutex> lock(entry.journalMutex);
                    if (o.journal) {
                        (void)o.journal->discard();
                    }
                    o.journal = std::move(slot->journal);
                    entry.journalOff = slot->error.has_value();
                }
                old.reset();
                dropBacking(entry);
                o.path = path;
                o.title = fileStem(path);
                o.readOnly = false;
                o.lock.reset();
                entry.key = pathKey(path);
                if (slot->error) {
                    warn(entry, "Crash recovery is off for '" + o.title + "': " + slot->error->message);
                }
            } else {
                std::lock_guard<std::mutex> lock(entry.journalMutex);
                if (o.journal && !entry.journalOff) {
                    if (auto st = o.journal->markSaved(done); !st) {
                        journalFailedLocked(entry, st.error());
                    }
                }
            }
            // Edits made while the save ran are not in the file: only an unchanged document becomes clean.
            if (o.document->state() == state) {
                o.document->markClean();
                o.modified = false;
            }
            recent_->add(path);
            emit(AppEvent::Kind::DocumentSaved, o.id);
            emit(AppEvent::Kind::RecentFilesChanged, {});
            return std::monostate{};
        },
        ctx.cancel);
    if (!applied) {
        return tl::unexpected<Error>(applied.error());
    }
    return done;
}

JobHandle<std::monostate> AppImpl::save(OpenDocument& od, std::optional<std::string> saveAsPath, bool overwrite)
{
    DocEntry* entry = entryOf(od);
    auto guard = std::make_shared<BusyGuard>(entry);
    const std::string title = "Save " + (saveAsPath ? fileName(*saveAsPath) : od.title);
    return runJob<std::monostate>(JobInfo{title, JobPriority::Normal, true},
                                  [this, guard, saveAsPath, overwrite](JobContext& ctx) -> Result<std::monostate> {
                                      if (guard->get() == nullptr) {
                                          return fail(ErrorCode::NotFound, "The document is not open");
                                      }
                                      auto commit = saveWork(*guard->get(), saveAsPath, overwrite, ctx);
                                      if (!commit) {
                                          return tl::unexpected<Error>(commit.error());
                                      }
                                      return std::monostate{};
                                  });
}

// ---------- Export ----------
Status AppImpl::exportWork(DocEntry& entry, const std::string& path, ExportFormat format, double chordTolerance,
                           const std::vector<BodyId>* bodies, JobContext& ctx)
{
    doc::Document& d = *entry.od.document;
    // Export what the current state builds to: wait for its rebuild.
    ctx.progress.set(0.0, "Waiting for the rebuild");
    for (;;) {
        if (auto st = ctx.cancel.check(); !st) {
            return st;
        }
        const doc::DocStatePtr state = d.state();
        if (!d.rebuildPending()) {
            if (d.builtState() == state && d.build()) {
                break;
            }
            if (auto error = d.buildError()) {
                return withContext(*error, "rebuilding before the export");
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const doc::DocStatePtr state = d.builtState();
    const doc::BuildResultPtr build = d.build();
    std::vector<kernel::Shape> shapes;
    std::vector<BodyId> order;
    for (const doc::Body& b : state->bodies) {
        order.push_back(b.id);
    }
    for (const auto& [id, built] : build->bodies) {
        if (std::find(order.begin(), order.end(), id) == order.end()) {
            order.push_back(id);
        }
    }
    for (const BodyId& id : order) {
        if (bodies && std::find(bodies->begin(), bodies->end(), id) == bodies->end()) {
            continue;
        }
        auto it = build->bodies.find(id);
        if (it != build->bodies.end() && !it->second.shape.isNull()) {
            shapes.push_back(it->second.shape);
        }
    }
    if (bodies) {
        for (const BodyId& id : *bodies) {
            auto it = build->bodies.find(id);
            if (it == build->bodies.end() || it->second.shape.isNull()) {
                return fail(ErrorCode::NotFound, "The document has no built body " + id.value.toString());
            }
        }
    }
    if (shapes.empty()) {
        return fail(ErrorCode::InvalidArgument, "There is nothing to export: the document has no built bodies");
    }
    JobContext kernelCtx{ctx.cancel, ctx.progress.sub(1.0)};
    Status st;
    switch (format) {
    case ExportFormat::Step:
        st = kernel::exportStep(path, shapes, kernelCtx);
        break;
    case ExportFormat::Iges:
        st = kernel::exportIges(path, shapes, kernelCtx);
        break;
    case ExportFormat::Stl:
        st = kernel::exportStl(path, shapes, chordTolerance, kernelCtx);
        break;
    }
    if (!st) {
        return withContext(st.error(), "exporting " + path);
    }
    return {};
}

// ---------- Jobs and events ----------
std::vector<JobStatus> AppImpl::activeJobs() const
{
    std::lock_guard<std::mutex> lock(jobsMutex_);
    std::vector<JobStatus> out;
    for (const auto& [id, job] : tracked_) {
        if (job.visible) {
            out.push_back(JobStatus{id, job.title, job.progress, job.stage});
        }
    }
    return out;
}

void AppImpl::cancelJob(std::uint64_t id)
{
    std::function<void()> cancel;
    {
        std::lock_guard<std::mutex> lock(jobsMutex_);
        if (auto it = tracked_.find(id); it != tracked_.end()) {
            cancel = it->second.cancel;
        }
    }
    if (cancel) {
        cancel();
    }
}

void AppImpl::jobProgress(std::uint64_t id, double fraction, std::string_view stage)
{
    {
        std::lock_guard<std::mutex> lock(jobsMutex_);
        auto it = tracked_.find(id);
        if (it == tracked_.end()) {
            return;
        }
        it->second.progress = fraction;
        if (!stage.empty()) {
            it->second.stage = std::string(stage);
        }
    }
    emit(AppEvent::Kind::JobsChanged, {});
}

void AppImpl::jobEnded(std::uint64_t id)
{
    bool visible = false;
    {
        std::lock_guard<std::mutex> lock(jobsMutex_);
        auto it = tracked_.find(id);
        if (it == tracked_.end()) {
            return;
        }
        visible = it->second.visible;
        tracked_.erase(it);
    }
    if (visible) {
        emit(AppEvent::Kind::JobsChanged, {});
    }
}

int AppImpl::subscribe(AppListener listener)
{
    if (!listener) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(listenersMutex_);
    const int id = nextListener_++;
    listeners_[id] = std::move(listener);
    return id;
}

void AppImpl::unsubscribe(int id)
{
    std::lock_guard<std::mutex> lock(listenersMutex_);
    listeners_.erase(id);
}

void AppImpl::emit(AppEvent::Kind kind, std::string documentId, std::string message)
{
    std::shared_ptr<Alive> alive = alive_;
    try {
        dispatcher_->post([this, alive, kind, documentId = std::move(documentId), message = std::move(message)]() {
            if (!alive->ok.load()) {
                return;
            }
            std::vector<AppListener> listeners;
            {
                std::lock_guard<std::mutex> lock(listenersMutex_);
                for (auto& [id, l] : listeners_) {
                    listeners.push_back(l);
                }
            }
            const AppEvent event{kind, documentId, message};
            for (auto& l : listeners) {
                try {
                    l(event);
                } catch (...) {
                    log(LogLevel::Warn, "api", "An app listener threw; ignored");
                }
            }
        });
    } catch (...) {
    }
}

// ---------- Headless ----------
bool AppImpl::runUntil(const std::function<bool()>& done, std::chrono::milliseconds timeout)
{
    if (!ownedDispatcher_) {
        return done();
    }
    return ownedDispatcher_->runUntil(done, timeout);
}

std::size_t AppImpl::runPending()
{
    return ownedDispatcher_ ? ownedDispatcher_->runPending() : 0;
}

Status AppImpl::waitForBuild(OpenDocument& od, std::chrono::milliseconds timeout)
{
    doc::Document* d = od.document.get();
    if (d == nullptr) {
        return fail(ErrorCode::InvalidArgument, "The document is not open");
    }
    if (!runUntil([d] { return !d->rebuildPending(); }, timeout)) {
        return fail(ErrorCode::Internal, "Timed out waiting for the rebuild of '" + od.title + "'");
    }
    if (auto error = d->buildError()) {
        return tl::unexpected<Error>(*error);
    }
    return {};
}

} // namespace ray::api::detail

// ---------- Factories ----------
namespace ray::api {

std::unique_ptr<IApp> makeApp(AppOptions options)
{
    if (options.dispatcher == nullptr) {
        log(LogLevel::Error, "api", "makeApp needs AppOptions::dispatcher; use makeHeadlessApp without one");
        return nullptr;
    }
    auto app = detail::AppImpl::create(std::move(options), nullptr);
    if (!app) {
        log(LogLevel::Error, "api", app.error().message);
        return nullptr;
    }
    return std::move(*app);
}

std::unique_ptr<HeadlessApp> makeHeadlessApp(AppOptions options)
{
    if (options.dispatcher != nullptr) {
        log(LogLevel::Error, "api", "makeHeadlessApp makes its own dispatcher; use makeApp with one");
        return nullptr;
    }
    std::unique_ptr<detail::QueueDispatcher> queue;
    try {
        queue = std::make_unique<detail::QueueDispatcher>();
    } catch (...) {
        return nullptr;
    }
    auto app = detail::AppImpl::create(std::move(options), std::move(queue));
    if (!app) {
        log(LogLevel::Error, "api", app.error().message);
        return nullptr;
    }
    return std::move(*app);
}

std::unique_ptr<HeadlessApp> makeHeadlessApp(std::vector<std::string> libraryRoots, int workerThreads)
{
    AppOptions options;
    options.libraryRoots = std::move(libraryRoots);
    options.workerThreads = workerThreads;
    return makeHeadlessApp(std::move(options));
}

} // namespace ray::api
