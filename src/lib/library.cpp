// ILibrary: one library folder (libraries.h, PLAN.md 3.6 and 4.3).
//
// Reading. A rescan lists each kind's folder (one directory listing gives every file's size and time stamp) and
// reads a file again unless its size and time stamp are unchanged AND it is "confirmed": two reads one rescan apart
// gave the same bytes. Time stamps on SMB and FAT are as coarse as 2 s, so an edit made within the same tick as an
// earlier read keeps size and stamp; the second read, at least one rescan later, finds it. Every 12th rescan reads
// every file regardless. A bad file becomes a LibraryIssue and is otherwise skipped. A folder that cannot be listed
// (a network share gone away) keeps the items last read, with an issue, instead of emptying the library.
//
// Writing. save() reads the file again and compares its bytes (by hash) with the bytes the library last read; any
// difference, or a file created or deleted by someone else, is ErrorCode::Locked, and the library takes the
// on-disk version. Otherwise the new bytes go to a temporary file that is flushed and renamed over the old one
// (fileio.h). No locks: a writer that changes the file between that check and the rename (a few milliseconds) is
// not detected; the later rename wins whole. Keys the file had that this version does not know are kept.
//
// Threads. Rescans and writes are serialised by ioMutex_; the accessors read the state under dataMutex_, which is
// held only to copy or swap it, so a slow network share never blocks a reader for longer than that.
#include "codec.h"
#include "fileio.h"
#include "library_internal.h"

#include <ray/libraries.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <utility>
#include <variant>

namespace ray::lib {
namespace detail {
namespace {

std::atomic<int> gWatchIntervalMs{5000};

using Item = std::variant<Tool, Holder, Material, MachineDef, OpTemplate>;

Uuid idOfItem(const Item& item)
{
    return std::visit(
        [](const auto& x) -> Uuid {
            if constexpr (std::is_same_v<std::decay_t<decltype(x)>, OpTemplate>) {
                return x.id;
            } else {
                return x.id.value;
            }
        },
        item);
}

Result<Item> decodeItem(Kind kind, const json::Value& value)
{
    const auto wrap = [](auto result) -> Result<Item> {
        if (!result) {
            return tl::unexpected<Error>(result.error());
        }
        return Item(std::move(*result));
    };
    switch (kind) {
    case Kind::Tool:
        return wrap(decodeTool(value));
    case Kind::Holder:
        return wrap(decodeHolder(value));
    case Kind::Material:
        return wrap(decodeMaterial(value));
    case Kind::Machine:
        return wrap(decodeMachine(value));
    case Kind::Template:
        return wrap(decodeTemplate(value));
    }
    return tl::unexpected<Error>(Error{ErrorCode::Internal, "Unknown library item kind", {}});
}

Hash256 hashBytes(std::string_view bytes)
{
    Hasher hasher;
    hasher.bytes(std::as_bytes(std::span<const char>(bytes.data(), bytes.size())));
    return hasher.finish();
}

tl::unexpected<Error> fail(ErrorCode code, std::string message, std::string context = {})
{
    Error e{code, std::move(message), {}};
    if (!context.empty()) {
        e.context.push_back(std::move(context));
    }
    return tl::unexpected<Error>(std::move(e));
}

struct FileRecord {
    fio::Stamp stamp;
    bool stampKnown = false;
    Hash256 hash;
    bool confirmed = false;   // two reads one rescan apart gave these bytes with this stamp
    json::Value raw;          // the parsed file; null if it did not parse
    std::optional<Item> item; // set when the file is a valid item
    std::string issue;        // set when it is not
};

FileRecord loadRecord(Kind kind, std::string_view bytes)
{
    FileRecord rec;
    rec.hash = hashBytes(bytes);
    auto parsed = json::parse(bytes);
    if (!parsed) {
        rec.issue = parsed.error().message;
        return rec;
    }
    rec.raw = std::move(*parsed);
    if (auto st = checkValue(kind, rec.raw); !st) {
        rec.issue = st.error().message;
        return rec;
    }
    auto item = decodeItem(kind, rec.raw);
    if (!item) {
        rec.issue = item.error().message;
        return rec;
    }
    rec.item = std::move(*item);
    return rec;
}

struct KindState {
    std::map<std::string, FileRecord> files; // by file name
    std::map<Uuid, std::string> byId;        // id -> the file that provides it
    std::vector<LibraryIssue> issues;        // duplicates and listing failures
};

struct State {
    std::array<KindState, 5> kinds;
    std::vector<std::string> posts;
    std::optional<LibraryIssue> manifestIssue;
    std::optional<Hash256> manifestHash;
    std::string name; // library.json "name"
};

class FolderLibrary final : public ILibrary {
public:
    explicit FolderLibrary(std::string root) : root_(std::move(root)) {}

    ~FolderLibrary() override
    {
        {
            const std::lock_guard<std::mutex> lock(watchMutex_);
            stop_ = true;
        }
        wake_.notify_all();
        if (watcher_.joinable()) {
            watcher_.join();
        }
    }

    FolderLibrary(const FolderLibrary&) = delete;
    FolderLibrary& operator=(const FolderLibrary&) = delete;

    void initialScan()
    {
        for (Kind kind : kAllKinds) {
            fio::removeStaleTemporaries(folderOf(kind));
        }
        refresh();
    }

    std::vector<LibraryIssue> issues() const override
    {
        const std::lock_guard<std::mutex> lock(dataMutex_);
        std::vector<LibraryIssue> out;
        for (Kind kind : kAllKinds) {
            const KindState& ks = state_.kinds[static_cast<std::size_t>(kind)];
            for (const auto& [name, rec] : ks.files) {
                if (!rec.issue.empty()) {
                    out.push_back({pathOf(kind, name), rec.issue});
                }
            }
            out.insert(out.end(), ks.issues.begin(), ks.issues.end());
        }
        if (state_.manifestIssue) {
            out.push_back(*state_.manifestIssue);
        }
        std::sort(out.begin(), out.end(), [](const LibraryIssue& a, const LibraryIssue& b) {
            return a.file != b.file ? a.file < b.file : a.message < b.message;
        });
        return out;
    }

    std::vector<Tool> tools() const override { return items<Tool>(Kind::Tool); }
    std::vector<Holder> holders() const override { return items<Holder>(Kind::Holder); }
    std::vector<Material> materials() const override { return items<Material>(Kind::Material); }
    std::vector<MachineDef> machines() const override { return items<MachineDef>(Kind::Machine); }
    std::vector<OpTemplate> templates() const override { return items<OpTemplate>(Kind::Template); }

    std::vector<std::string> postFiles() const override
    {
        const std::lock_guard<std::mutex> lock(dataMutex_);
        return state_.posts;
    }

    Status save(const Tool& x) override { return saveItem(Kind::Tool, toValue(x)); }
    Status save(const Holder& x) override { return saveItem(Kind::Holder, toValue(x)); }
    Status save(const Material& x) override { return saveItem(Kind::Material, toValue(x)); }
    Status save(const MachineDef& x) override { return saveItem(Kind::Machine, toValue(x)); }
    Status save(const OpTemplate& x) override { return saveItem(Kind::Template, toValue(x)); }

    std::string name() const override
    {
        const std::lock_guard<std::mutex> lock(dataMutex_);
        return state_.name;
    }

    Status remove(ItemKind which, const Uuid& id) override
    {
        const auto kind =
            static_cast<std::size_t>(which) < kAllKinds.size() ? std::optional<Kind>(which) : std::nullopt;
        if (!kind) {
            return fail(ErrorCode::InvalidArgument, "Unknown library item kind");
        }
        const std::string_view kindName = kindInfo(*kind).name;
        try {
            const std::lock_guard<std::mutex> io(ioMutex_);
            const KindState& ks = state_.kinds[static_cast<std::size_t>(*kind)];
            const auto owner = ks.byId.find(id);
            if (owner == ks.byId.end()) {
                return fail(ErrorCode::NotFound, "The library has no " + std::string(kindName) + " with this id",
                            id.toString());
            }
            const std::string name = owner->second;
            const std::string path = pathOf(*kind, name);
            if (auto conflict = checkUnchanged(*kind, name, path); !conflict) {
                return conflict;
            }
            if (auto st = fio::removeFile(path); !st && st.error().code != ErrorCode::NotFound) {
                return st;
            }
            const std::lock_guard<std::mutex> data(dataMutex_);
            KindState& target = state_.kinds[static_cast<std::size_t>(*kind)];
            target.files.erase(name);
            rebuildIds(*kind, target);
            return {};
        } catch (const std::exception& e) {
            return fail(ErrorCode::Internal, std::string("Cannot remove a library item: ") + e.what());
        }
    }

    bool refresh() override
    {
        try {
            const std::lock_guard<std::mutex> io(ioMutex_);
            const bool full = (++scans_ % 12) == 0;
            bool changed = false;
            State next;
            for (Kind kind : kAllKinds) {
                next.kinds[static_cast<std::size_t>(kind)] =
                    scanKind(kind, state_.kinds[static_cast<std::size_t>(kind)], full, changed);
            }
            next.posts = scanPosts();
            changed = changed || next.posts != state_.posts;
            scanManifest(next);
            changed = changed || next.manifestHash != state_.manifestHash ||
                      (next.manifestIssue.has_value() != state_.manifestIssue.has_value());
            const std::lock_guard<std::mutex> data(dataMutex_);
            state_ = std::move(next);
            return changed;
        } catch (const std::exception& e) {
            log(LogLevel::Error, "lib", std::string("Library rescan failed: ") + e.what());
            return false;
        }
    }

    void watch(IUiDispatcher& dispatcher, std::function<void()> onChangedUiThread) override
    {
        const std::lock_guard<std::mutex> lock(watchMutex_);
        dispatcher_ = &dispatcher;
        onChanged_ = std::move(onChangedUiThread);
        if (!watcher_.joinable()) {
            watcher_ = std::thread([this] { watchLoop(); });
        }
    }

private:
    std::string folderOf(Kind kind) const { return fio::join(root_, kindInfo(kind).folder); }
    std::string pathOf(Kind kind, const std::string& name) const { return fio::join(folderOf(kind), name); }

    template <class T>
    std::vector<T> items(Kind kind) const
    {
        const std::lock_guard<std::mutex> lock(dataMutex_);
        const KindState& ks = state_.kinds[static_cast<std::size_t>(kind)];
        std::vector<T> out;
        out.reserve(ks.byId.size());
        for (const auto& [id, name] : ks.byId) {
            out.push_back(std::get<T>(*ks.files.at(name).item));
        }
        return out;
    }

    // Which file provides each id: the one named <id><suffix> if there is one, else the first by name. Every other
    // file with the same id is reported.
    void rebuildIds(Kind kind, KindState& ks) const
    {
        ks.byId.clear();
        std::vector<LibraryIssue> keep;
        for (const LibraryIssue& issue : ks.issues) {
            if (issue.message.rfind("Duplicate id", 0) != 0) {
                keep.push_back(issue);
            }
        }
        ks.issues = std::move(keep);
        std::map<Uuid, std::vector<std::string>> claims;
        for (const auto& [name, rec] : ks.files) {
            if (rec.item) {
                claims[idOfItem(*rec.item)].push_back(name);
            }
        }
        for (auto& [id, names] : claims) {
            const std::string preferred = id.toString() + std::string(kindInfo(kind).suffix);
            std::string winner = names.front();
            for (const std::string& name : names) {
                if (name == preferred) {
                    winner = name;
                }
            }
            ks.byId.emplace(id, winner);
            for (const std::string& name : names) {
                if (name != winner) {
                    ks.issues.push_back({pathOf(kind, name), "Duplicate id " + id.toString() + ": " + winner +
                                                                 " has the same id and is used instead"});
                }
            }
        }
    }

    KindState scanKind(Kind kind, const KindState& old, bool full, bool& changed) const
    {
        KindState out;
        const std::string folder = folderOf(kind);
        auto listing = fio::listFolder(folder);
        if (!listing) {
            if (listing.error().code == ErrorCode::NotFound) {
                changed = changed || !old.files.empty();
                return out; // the kind's folder is optional
            }
            out = old; // keep what was read before: a share that went away does not empty the library
            std::erase_if(out.issues, [&folder](const LibraryIssue& issue) { return issue.file == folder; });
            out.issues.push_back({folder, listing.error().message});
            return out;
        }
        const std::string_view suffix = kindInfo(kind).suffix;
        for (const fio::DirEntry& entry : *listing) {
            const std::string& name = entry.name;
            if (name.empty() || name.front() == '.' || name.size() <= suffix.size() ||
                name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
                continue;
            }
            const auto before = old.files.find(name);
            if (before != old.files.end() && before->second.stampKnown && before->second.stamp == entry.stamp &&
                before->second.confirmed && !full) {
                out.files.emplace(name, before->second);
                continue;
            }
            auto bytes = fio::readFile(fio::join(folder, name));
            if (!bytes) {
                if (bytes.error().code == ErrorCode::NotFound) {
                    continue; // deleted between the listing and the read
                }
                FileRecord rec = before != old.files.end() ? before->second : FileRecord{};
                rec.confirmed = false;
                if (!rec.item) {
                    rec.issue = bytes.error().message;
                }
                out.files.emplace(name, std::move(rec));
                continue;
            }
            const Hash256 hash = hashBytes(*bytes);
            if (before != old.files.end() && before->second.hash == hash) {
                FileRecord rec = before->second;
                rec.confirmed = rec.stampKnown && rec.stamp == entry.stamp;
                rec.stamp = entry.stamp;
                rec.stampKnown = true;
                out.files.emplace(name, std::move(rec));
                continue;
            }
            FileRecord rec = loadRecord(kind, *bytes);
            rec.stamp = entry.stamp;
            rec.stampKnown = true;
            out.files.emplace(name, std::move(rec));
            changed = true;
        }
        for (const auto& [name, rec] : old.files) {
            if (out.files.count(name) == 0) {
                changed = true;
            }
        }
        rebuildIds(kind, out);
        return out;
    }

    std::vector<std::string> scanPosts() const
    {
        std::vector<std::string> out;
        const std::string folder = fio::join(root_, "posts");
        auto listing = fio::listFolder(folder);
        if (!listing) {
            return out;
        }
        for (const fio::DirEntry& entry : *listing) {
            const std::string_view suffix = ".post.js";
            if (!entry.name.empty() && entry.name.front() != '.' && entry.name.size() > suffix.size() &&
                entry.name.compare(entry.name.size() - suffix.size(), suffix.size(), suffix) == 0) {
                out.push_back(fio::join(folder, entry.name));
            }
        }
        return out;
    }

    // library.json is optional: { "schema": 1, "name": "...", ... }.
    void scanManifest(State& next) const
    {
        const std::string path = fio::join(root_, "library.json");
        auto bytes = fio::readFile(path);
        if (!bytes) {
            if (bytes.error().code != ErrorCode::NotFound) {
                next.manifestIssue = LibraryIssue{path, bytes.error().message};
            }
            return;
        }
        next.manifestHash = hashBytes(*bytes);
        auto value = json::parse(*bytes);
        if (!value) {
            next.manifestIssue = LibraryIssue{path, value.error().message};
            return;
        }
        const json::Value* version = value->find("schema");
        const json::Value* name = value->find("name");
        if (!value->isObject() || !version || !version->isNumber() || !name || !name->isString()) {
            next.manifestIssue =
                LibraryIssue{path, "/: library.json needs \"schema\" (a number) and \"name\" (a string)"};
        } else if (version->asNumber() > kFormatVersion) {
            next.manifestIssue = LibraryIssue{path, "/schema: this library was written by a newer RayCAD"};
        }
        if (name != nullptr && name->isString()) {
            next.name = name->asString();
        }
    }

    // ErrorCode::Locked (and the on-disk version taken into the state) if the file is not what was last read.
    Status checkUnchanged(Kind kind, const std::string& name, const std::string& path)
    {
        auto current = fio::readFile(path);
        if (!current && current.error().code != ErrorCode::NotFound) {
            return tl::unexpected<Error>(current.error());
        }
        const KindState& ks = state_.kinds[static_cast<std::size_t>(kind)];
        const auto known = ks.files.find(name);
        std::string what;
        if (known == ks.files.end()) {
            if (!current) {
                return {};
            }
            what = "was created by someone else";
        } else if (!current) {
            what = "was deleted by someone else";
        } else if (hashBytes(*current) != known->second.hash) {
            what = "was changed by someone else";
        } else {
            return {};
        }
        {
            const std::lock_guard<std::mutex> data(dataMutex_);
            KindState& target = state_.kinds[static_cast<std::size_t>(kind)];
            if (current) {
                target.files[name] = loadRecord(kind, *current);
            } else {
                target.files.erase(name);
            }
            rebuildIds(kind, target);
        }
        return fail(ErrorCode::Locked,
                    "The " + std::string(kindInfo(kind).name) + " file " + what +
                        " since it was read; the library now holds the version on disk",
                    path);
    }

    Status saveItem(Kind kind, json::Value value)
    {
        try {
            if (auto st = checkValue(kind, value); !st) {
                return fail(ErrorCode::InvalidArgument,
                            "Not a valid " + std::string(kindInfo(kind).name) + ": " + st.error().message);
            }
            auto item = decodeItem(kind, value);
            if (!item) {
                return fail(ErrorCode::InvalidArgument,
                            "Not a valid " + std::string(kindInfo(kind).name) + ": " + item.error().message);
            }
            const Uuid id = idOfItem(*item);
            const std::lock_guard<std::mutex> io(ioMutex_);
            const KindState& ks = state_.kinds[static_cast<std::size_t>(kind)];
            const auto owner = ks.byId.find(id);
            const std::string name =
                owner != ks.byId.end() ? owner->second : id.toString() + std::string(kindInfo(kind).suffix);
            const std::string path = pathOf(kind, name);
            if (auto conflict = checkUnchanged(kind, name, path); !conflict) {
                return conflict;
            }
            const auto known = ks.files.find(name);
            json::Value merged =
                known != ks.files.end() ? mergeUnknownKeys(known->second.raw, std::move(value)) : std::move(value);
            const std::string bytes = json::writePretty(merged);
            const Hash256 hash = hashBytes(bytes);
            if (known == ks.files.end() || known->second.hash != hash) {
                if (auto st = fio::replaceAtomically(path, bytes); !st) {
                    return st;
                }
            }
            FileRecord rec;
            rec.hash = hash;
            rec.raw = std::move(merged);
            rec.item = std::move(*item);
            const std::lock_guard<std::mutex> data(dataMutex_);
            KindState& target = state_.kinds[static_cast<std::size_t>(kind)];
            target.files[name] = std::move(rec);
            rebuildIds(kind, target);
            return {};
        } catch (const std::exception& e) {
            return fail(ErrorCode::Internal, std::string("Cannot save a library item: ") + e.what());
        }
    }

    void watchLoop()
    {
        std::unique_lock<std::mutex> lock(watchMutex_);
        while (!stop_) {
            wake_.wait_for(lock, std::chrono::milliseconds(gWatchIntervalMs.load()), [this] { return stop_; });
            if (stop_) {
                break;
            }
            lock.unlock();
            const bool changed = refresh();
            lock.lock();
            if (changed && !stop_ && dispatcher_ && onChanged_) {
                dispatcher_->post(onChanged_);
            }
        }
    }

    const std::string root_;
    mutable std::mutex ioMutex_;
    mutable std::mutex dataMutex_;
    State state_;
    std::uint64_t scans_ = 0;

    std::mutex watchMutex_;
    std::condition_variable wake_;
    bool stop_ = false;
    IUiDispatcher* dispatcher_ = nullptr;
    std::function<void()> onChanged_;
    std::thread watcher_;
};

} // namespace

void setWatchIntervalMs(int ms)
{
    gWatchIntervalMs.store(ms > 0 ? ms : 1);
}

int watchIntervalMs()
{
    return gWatchIntervalMs.load();
}

} // namespace detail

Result<std::unique_ptr<ILibrary>> ILibrary::open(const std::string& rootDir)
{
    try {
        if (!detail::fio::isFolder(rootDir)) {
            return tl::unexpected<Error>(Error{ErrorCode::NotFound, "The library folder does not exist", {rootDir}});
        }
        auto library = std::make_unique<detail::FolderLibrary>(rootDir);
        library->initialScan();
        return std::unique_ptr<ILibrary>(std::move(library));
    } catch (const std::exception& e) {
        return tl::unexpected<Error>(
            Error{ErrorCode::Internal, std::string("Cannot open a library: ") + e.what(), {rootDir}});
    }
}

} // namespace ray::lib
