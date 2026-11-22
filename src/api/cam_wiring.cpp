// CAM, libraries and posts of the app (api.h IApp::operationTypes, libraries, cam, posts).
//
// One operation registry, one CAM engine, one library stack and one post engine per app. The engine is
// cam::makeCamEngine over doc::camSnapshot and the app's job system, wrapped so every generation is an app job: the
// destructor cancels and waits for it like any other, and closing a document cancels the generations of its
// operations and makes the engine forget them (unless another open document has the same operation ids: a second copy
// of the same file). The library stack is made from AppOptions::libraryRoots, or (when they are empty) from the
// built-in library, the shared folders and the user library of the settings, made again on the UI thread when those
// settings change, and watched (LibraryStack::watch) through the app's dispatcher. Only the built-in library (local,
// read-only) is read on the UI thread; the shared folders and the user library are read on the app's workers
// (LibraryStack::setRootsInBackground) and join the stack when read, so a slow or unreachable network share neither
// delays start-up by more than kLibraryWait nor hangs it: it becomes an issue when its read fails.
#include "app_impl.h"

#include <algorithm>
#include <set>
#include <system_error>

namespace ray::api::detail {

namespace {

using ToolpathPtr = std::shared_ptr<const cam::Toolpath>;

// How long making the default library stack waits for the roots read in the background: local folders are read by
// then (the stack is complete when the app starts); a slow network share arrives later, through the listeners.
constexpr std::chrono::milliseconds kLibraryWait{500};

// The engine as the app hands it out: the real one, with each generation adopted as an app job and remembered per
// operation until it ends, so a closing document can cancel its own.
class AppCamEngine final : public cam::ICamEngine {
public:
    AppCamEngine(AppImpl& app, std::unique_ptr<cam::ICamEngine> inner) : app_(app), inner_(std::move(inner)) {}

    std::map<OperationId, cam::OpState> status(doc::DocStatePtr state, doc::BuildResultPtr build) const override
    {
        return inner_->status(std::move(state), std::move(build));
    }

    JobHandle<ToolpathPtr> generate(doc::DocStatePtr state, doc::BuildResultPtr build, OperationId id) override
    {
        JobHandle<ToolpathPtr> job = inner_->generate(std::move(state), std::move(build), id);
        app_.adoptJob(job, "Toolpath");
        std::lock_guard<std::mutex> lock(mutex_);
        auto& jobs = running_[id];
        std::erase_if(jobs, [](const JobHandle<ToolpathPtr>& j) { return j.done(); });
        jobs.push_back(job);
        return job;
    }

    void generateAllStale(doc::DocStatePtr state, doc::BuildResultPtr build) override
    {
        // Through generate() above, so every job is the app's.
        for (const auto& [id, st] : status(state, build)) {
            if (st.status == cam::OpStatus::Stale) {
                (void)generate(state, build, id);
            }
        }
    }

    void forget(std::span<const OperationId> ops) override { inner_->forget(ops); }

    void cancel(const std::set<OperationId>& ops)
    {
        std::vector<JobHandle<ToolpathPtr>> jobs;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const OperationId& id : ops) {
                if (auto it = running_.find(id); it != running_.end()) {
                    jobs.insert(jobs.end(), it->second.begin(), it->second.end());
                    running_.erase(it);
                }
            }
        }
        for (auto& job : jobs) {
            (void)job.cancel();
        }
    }

private:
    AppImpl& app_;
    std::unique_ptr<cam::ICamEngine> inner_;
    std::mutex mutex_;
    std::map<OperationId, std::vector<JobHandle<ToolpathPtr>>> running_;
};

bool isFolder(const std::string& utf8)
{
    std::error_code ec;
    return !utf8.empty() && std::filesystem::is_directory(toFsPath(utf8), ec);
}

// The shipped library next to the program: <exe dir>/resources/library (Windows and the build tree, where
// src/api/CMakeLists.txt copies it), <exe dir>/../share/raycad/library (Linux), <exe dir>/../Resources/library (a
// macOS bundle). The first that exists; else the first candidate, so the stack reports it as missing.
std::string locateBuiltinLibrary()
{
    const std::string dir = programDir();
    if (dir.empty()) {
        return {};
    }
    const std::filesystem::path exe = toFsPath(dir);
    const std::filesystem::path candidates[] = {exe / "resources" / "library",
                                                exe.parent_path() / "share" / "raycad" / "library",
                                                exe.parent_path() / "Resources" / "library"};
    for (const auto& c : candidates) {
        if (isFolder(fromFsPath(c))) {
            return fromFsPath(c.lexically_normal());
        }
    }
    return fromFsPath(candidates[0]);
}

} // namespace

Status AppImpl::initCam(const AppOptions& options)
{
    operationTypes_ = std::make_shared<cam::OperationRegistry>();
    cam::registerBuiltinOperationTypes(*operationTypes_); // "drill" (T-035)
    auto engine = cam::makeCamEngine(*jobs_, operationTypes_, &doc::camSnapshot);
    if (!engine) {
        return fail(ErrorCode::Internal, "The CAM engine could not start");
    }
    cam_ = std::make_unique<AppCamEngine>(*this, std::move(engine));
    posts_ = post::makeQuickJsPostEngine();
    if (!posts_) {
        return fail(ErrorCode::Internal, "The post engine could not start");
    }

    explicitLibraryRoots_ = options.libraryRoots;
    builtinLibraryDir_ = options.builtinLibraryDir;
    libraries_ = std::make_unique<lib::LibraryStack>();
    libraries_->watch(*dispatcher_);
    if (!explicitLibraryRoots_.empty()) {
        libraries_->setRoots(explicitLibraryRoots_);
    } else {
        configureLibraries();
        // A change of the library settings makes the stack again, on the UI thread (the store calls listeners on
        // the thread that changed it).
        std::shared_ptr<Alive> alive = alive_;
        librarySettingsListener_ =
            settings_->subscribe([this, alive](const UserSettings&, const std::vector<std::string>& changed) {
                const bool library = std::any_of(changed.begin(), changed.end(),
                                                 [](const std::string& key) { return key.rfind("library.", 0) == 0; });
                if (!library || !alive->ok.load()) {
                    return;
                }
                try {
                    dispatcher_->post([this, alive] {
                        if (alive->ok.load() && libraries_) {
                            configureLibraries();
                        }
                    });
                } catch (...) {
                }
            });
    }
    return {};
}

std::pair<std::vector<std::string>, std::size_t> AppImpl::defaultLibraryRoots() const
{
    std::vector<std::string> roots;
    std::size_t readOnly = 0;
    const std::string builtin = builtinLibraryDir_ ? *builtinLibraryDir_ : locateBuiltinLibrary();
    if (!builtin.empty()) {
        roots.push_back(builtin);
        readOnly = 1;
    }
    const UserSettings user = settings_->get();
    for (const std::string& shared : user.sharedLibraryFolders) {
        roots.push_back(shared);
    }
    if (!user.userLibraryFolder.empty()) {
        roots.push_back(user.userLibraryFolder);
    } else if (!userDataDir_.empty()) {
        roots.push_back(fromFsPath(toFsPath(userDataDir_) / "library"));
    }
    return {std::move(roots), readOnly};
}

void AppImpl::configureLibraries()
{
    if (!libraries_ || !explicitLibraryRoots_.empty()) {
        return;
    }
    auto [roots, readOnly] = defaultLibraryRoots();
    // The default user library is the app's own folder: made when missing (a folder the user named is not).
    if (settings_->get().userLibraryFolder.empty() && !userDataDir_.empty() && !roots.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(toFsPath(roots.back()), ec);
        if (ec) {
            log(LogLevel::Warn, "api", "The user library folder could not be made: " + ec.message());
        }
    }
    libraries_->setRootsInBackground(std::move(roots), readOnly, *jobs_, readOnly, kLibraryWait);
}

void AppImpl::cancelGenerations(const doc::DocStatePtr& state)
{
    auto* engine = dynamic_cast<AppCamEngine*>(cam_.get());
    if (engine == nullptr || !state || !state->cam) {
        return;
    }
    std::set<OperationId> ops;
    for (const auto& [id, op] : state->cam->operations) {
        ops.insert(id);
    }
    // The engine keys what it holds by operation id, so two open copies of one file share it: operations that a
    // document still open also has are left to it (the closing one is already out of documents_).
    {
        std::lock_guard<std::mutex> lock(docsMutex_);
        for (const auto& e : documents_) {
            const doc::DocStatePtr other = e->od.document ? e->od.document->state() : nullptr;
            if (other && other->cam) {
                for (const auto& [id, op] : other->cam->operations) {
                    ops.erase(id);
                }
            }
        }
    }
    engine->cancel(ops);
    // Then the engine drops their latest toolpaths; a generation that ends after this is cached by input hash only,
    // so reopening the file finds its toolpaths again while they are in the cache (cam.h ICamEngine::forget).
    const std::vector<OperationId> closed(ops.begin(), ops.end());
    engine->forget(closed);
}

void AppImpl::shutdownCam()
{
    if (librarySettingsListener_ != 0 && settings_) {
        settings_->unsubscribe(librarySettingsListener_);
        librarySettingsListener_ = 0;
    }
    cam_.reset();
    libraries_.reset(); // stops the watch threads; callbacks already posted find the stack gone
}

} // namespace ray::api::detail
