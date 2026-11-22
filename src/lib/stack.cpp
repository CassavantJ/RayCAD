// LibraryStack: several library roots layered by id, later roots winning (libraries.h).
//
// The stack lives on the UI thread. Each root is an ILibrary (thread-safe); the stack keeps the merged copy of their
// items and, for every item, the root it comes from. Watching hands each library a callback that the library's own
// thread posts through the dispatcher after a rescan that changed something; on the UI thread it merges again and
// calls the listeners. The callback holds the stack's state weakly: one posted before setRoots or the stack's
// destruction does nothing when it runs.
#include "tool_query.h"

#include <ray/libraries.h>

#include <algorithm>
#include <map>
#include <tuple>
#include <utility>

namespace ray::lib {

namespace {

// The folder's own name, for a root whose library.json has no name.
std::string folderName(const std::string& dir)
{
    std::string trimmed = dir;
    while (trimmed.size() > 1 && (trimmed.back() == '/' || trimmed.back() == '\\')) {
        trimmed.pop_back();
    }
    const auto slash = trimmed.find_last_of("/\\");
    return slash == std::string::npos ? trimmed : trimmed.substr(slash + 1);
}

} // namespace

struct LibraryStack::Impl {
    // The roots and what was merged from them; replaced whole by setRoots, so a posted watch callback that holds it
    // weakly finds it gone.
    struct Roots {
        std::vector<std::string> dirs;
        std::size_t readOnly = 0;                         // the first `readOnly` roots
        std::vector<std::unique_ptr<ILibrary>> libraries; // parallel to dirs; null for a root that did not open
        std::vector<LibraryIssue> rootIssues;             // parallel to dirs: why it did not open
        template <class Id, class T>
        using Merged = std::map<Id, std::pair<T, std::size_t>>; // the item and the root it comes from
        Merged<ToolId, Tool> tools;
        Merged<HolderId, Holder> holders;
        Merged<MaterialId, Material> materials;
        Merged<MachineId, MachineDef> machines;
        Merged<Uuid, OpTemplate> templates;
    };
    std::shared_ptr<Roots> roots = std::make_shared<Roots>();
    IUiDispatcher* dispatcher = nullptr; // set by watch()
    std::map<int, std::function<void()>> listeners;
    int nextListener = 1;

    void openMissing()
    {
        Roots& r = *roots;
        r.libraries.resize(r.dirs.size());
        r.rootIssues.resize(r.dirs.size());
        for (std::size_t i = 0; i < r.dirs.size(); ++i) {
            if (r.libraries[i]) {
                continue;
            }
            auto opened = ILibrary::open(r.dirs[i]);
            if (opened) {
                r.libraries[i] = std::move(*opened);
                r.rootIssues[i] = {};
                watchOne(i);
            } else {
                r.rootIssues[i] = {r.dirs[i], "The library cannot be opened: " + opened.error().message};
            }
        }
    }

    void merge()
    {
        Roots& r = *roots;
        r.tools.clear();
        r.holders.clear();
        r.materials.clear();
        r.machines.clear();
        r.templates.clear();
        for (std::size_t i = 0; i < r.libraries.size(); ++i) {
            const auto& library = r.libraries[i];
            if (!library) {
                continue;
            }
            for (Tool& t : library->tools()) {
                const ToolId id = t.id;
                r.tools.insert_or_assign(id, std::make_pair(std::move(t), i));
            }
            for (Holder& h : library->holders()) {
                const HolderId id = h.id;
                r.holders.insert_or_assign(id, std::make_pair(std::move(h), i));
            }
            for (Material& m : library->materials()) {
                const MaterialId id = m.id;
                r.materials.insert_or_assign(id, std::make_pair(std::move(m), i));
            }
            for (MachineDef& m : library->machines()) {
                const MachineId id = m.id;
                r.machines.insert_or_assign(id, std::make_pair(std::move(m), i));
            }
            for (OpTemplate& t : library->templates()) {
                const Uuid id = t.id;
                r.templates.insert_or_assign(id, std::make_pair(std::move(t), i));
            }
        }
    }

    void notify()
    {
        // A copy: a listener may subscribe or unsubscribe.
        std::vector<std::function<void()>> calls;
        for (const auto& [id, fn] : listeners) {
            calls.push_back(fn);
        }
        for (const auto& fn : calls) {
            try {
                fn();
            } catch (...) {
                log(LogLevel::Warn, "lib", "A library listener threw; ignored");
            }
        }
    }

    // Starts the watch of root i, when the stack is watched.
    void watchOne(std::size_t i)
    {
        if (dispatcher == nullptr || !roots->libraries[i]) {
            return;
        }
        std::weak_ptr<Roots> weak = roots;
        roots->libraries[i]->watch(*dispatcher, [this, weak]() {
            // On the UI thread. The stack's roots may have been replaced (or the stack destroyed: the libraries go
            // with their Roots, and a library's thread stops before it is destroyed, so `this` is alive whenever
            // the roots are).
            if (auto alive = weak.lock()) {
                merge();
                notify();
            }
        });
    }

    template <class Map>
    static auto listOf(const Map& map)
    {
        std::vector<typename Map::mapped_type::first_type> out;
        out.reserve(map.size());
        for (const auto& [id, entry] : map) {
            out.push_back(entry.first);
        }
        return out;
    }

    template <class Map, class Id>
    static const typename Map::mapped_type::first_type* find(const Map& map, const Id& id)
    {
        const auto it = map.find(id);
        return it == map.end() ? nullptr : &it->second.first;
    }
};

LibraryStack::LibraryStack() : impl_(std::make_unique<Impl>()) {}
LibraryStack::~LibraryStack() = default;
LibraryStack::LibraryStack(LibraryStack&&) noexcept = default;
LibraryStack& LibraryStack::operator=(LibraryStack&&) noexcept = default;

void LibraryStack::setRoots(std::vector<std::string> dirsLowestFirst, std::size_t readOnlyRoots)
{
    auto next = std::make_shared<Impl::Roots>();
    next->dirs = std::move(dirsLowestFirst);
    next->readOnly = std::min(readOnlyRoots, next->dirs.size());
    impl_->roots = std::move(next); // the old libraries (and their watch threads) go now
    impl_->openMissing();
    impl_->merge();
    impl_->notify();
}

std::vector<LibraryIssue> LibraryStack::issues() const
{
    const Impl::Roots& r = *impl_->roots;
    std::vector<LibraryIssue> out;
    for (std::size_t i = 0; i < r.dirs.size(); ++i) {
        if (r.libraries[i]) {
            for (LibraryIssue& issue : r.libraries[i]->issues()) {
                out.push_back(std::move(issue));
            }
        } else {
            out.push_back(r.rootIssues[i]);
        }
    }
    return out;
}

bool LibraryStack::refresh()
{
    Impl::Roots& r = *impl_->roots;
    bool changed = false;
    for (auto& library : r.libraries) {
        if (library) {
            changed = library->refresh() || changed;
        }
    }
    const auto openCount = [&r] {
        return std::count_if(r.libraries.begin(), r.libraries.end(), [](const auto& l) { return l != nullptr; });
    };
    const auto before = openCount();
    impl_->openMissing();
    changed = changed || before != openCount();
    impl_->merge();
    if (changed) {
        impl_->notify();
    }
    return changed;
}

std::vector<LibraryStack::RootInfo> LibraryStack::roots() const
{
    const Impl::Roots& r = *impl_->roots;
    std::vector<RootInfo> out;
    for (std::size_t i = 0; i < r.dirs.size(); ++i) {
        RootInfo info;
        info.dir = r.dirs[i];
        info.open = r.libraries[i] != nullptr;
        info.writable = info.open && i >= r.readOnly;
        info.name = info.open ? r.libraries[i]->name() : std::string();
        if (info.name.empty()) {
            info.name = folderName(r.dirs[i]);
        }
        out.push_back(std::move(info));
    }
    return out;
}

ILibrary* LibraryStack::library(std::size_t root)
{
    Impl::Roots& r = *impl_->roots;
    if (root >= r.libraries.size() || root < r.readOnly) {
        return nullptr;
    }
    return r.libraries[root].get();
}

void LibraryStack::remerge()
{
    impl_->merge();
    impl_->notify();
}

std::optional<std::size_t> LibraryStack::rootOf(ItemKind kind, const Uuid& id) const
{
    const Impl::Roots& r = *impl_->roots;
    const auto at = [](const auto& map, const auto& key) -> std::optional<std::size_t> {
        const auto it = map.find(key);
        return it == map.end() ? std::nullopt : std::optional<std::size_t>(it->second.second);
    };
    switch (kind) {
    case ItemKind::Tool:
        return at(r.tools, ToolId{id});
    case ItemKind::Holder:
        return at(r.holders, HolderId{id});
    case ItemKind::Material:
        return at(r.materials, MaterialId{id});
    case ItemKind::Machine:
        return at(r.machines, MachineId{id});
    case ItemKind::Template:
        return at(r.templates, id);
    }
    return std::nullopt;
}

std::vector<Tool> LibraryStack::tools() const
{
    return Impl::listOf(impl_->roots->tools);
}

std::vector<Holder> LibraryStack::holders() const
{
    return Impl::listOf(impl_->roots->holders);
}

std::vector<Material> LibraryStack::materials() const
{
    return Impl::listOf(impl_->roots->materials);
}

std::vector<MachineDef> LibraryStack::machines() const
{
    return Impl::listOf(impl_->roots->machines);
}

std::vector<OpTemplate> LibraryStack::templates() const
{
    return Impl::listOf(impl_->roots->templates);
}

const Tool* LibraryStack::tool(const ToolId& id) const
{
    return Impl::find(impl_->roots->tools, id);
}

const Holder* LibraryStack::holder(const HolderId& id) const
{
    return Impl::find(impl_->roots->holders, id);
}

const Material* LibraryStack::material(const MaterialId& id) const
{
    return Impl::find(impl_->roots->materials, id);
}

const MachineDef* LibraryStack::machine(const MachineId& id) const
{
    return Impl::find(impl_->roots->machines, id);
}

const OpTemplate* LibraryStack::opTemplate(const Uuid& id) const
{
    return Impl::find(impl_->roots->templates, id);
}

Result<std::vector<Tool>> LibraryStack::queryTools(std::string_view toolQueryJson, const QueryVars& vars) const
{
    auto predicate = detail::compileToolQuery(toolQueryJson, vars);
    if (!predicate) {
        return tl::unexpected<Error>(predicate.error());
    }
    std::vector<Tool> out;
    for (const auto& [id, entry] : impl_->roots->tools) {
        if ((*predicate)(entry.first)) {
            out.push_back(entry.first);
        }
    }
    // The map is ordered by id, so a stable sort by diameter gives (diameter, id).
    std::stable_sort(out.begin(), out.end(), [](const Tool& a, const Tool& b) { return a.diameter < b.diameter; });
    return out;
}

std::vector<OpTemplate> LibraryStack::templatesFor(std::string_view featureKind, const MaterialId& material,
                                                   const MachineId& machine) const
{
    std::vector<OpTemplate> out;
    for (const auto& [id, entry] : impl_->roots->templates) {
        const OpTemplate& t = entry.first;
        if (!featureKind.empty() && t.featureKind != featureKind) {
            continue;
        }
        if ((t.material && *t.material != material) || (t.machine && *t.machine != machine)) {
            continue;
        }
        out.push_back(t);
    }
    const auto specificity = [](const OpTemplate& t) {
        return (t.material ? 1 : 0) + (t.machine ? 1 : 0);
    };
    std::sort(out.begin(), out.end(), [&](const OpTemplate& a, const OpTemplate& b) {
        return std::make_tuple(-specificity(a), std::cref(a.name), a.id) <
               std::make_tuple(-specificity(b), std::cref(b.name), b.id);
    });
    return out;
}

void LibraryStack::watch(IUiDispatcher& dispatcher)
{
    impl_->dispatcher = &dispatcher;
    for (std::size_t i = 0; i < impl_->roots->libraries.size(); ++i) {
        impl_->watchOne(i);
    }
}

int LibraryStack::subscribe(std::function<void()> onChanged)
{
    if (!onChanged) {
        return 0;
    }
    const int id = impl_->nextListener++;
    impl_->listeners.emplace(id, std::move(onChanged));
    return id;
}

void LibraryStack::unsubscribe(int id)
{
    impl_->listeners.erase(id);
}

} // namespace ray::lib
