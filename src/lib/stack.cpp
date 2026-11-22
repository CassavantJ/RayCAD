// LibraryStack: several library roots layered by id, later roots winning (libraries.h).
#include "tool_query.h"

#include <ray/libraries.h>

#include <algorithm>
#include <map>
#include <tuple>
#include <utility>

namespace ray::lib {

struct LibraryStack::Impl {
    std::vector<std::string> roots;
    std::vector<std::unique_ptr<ILibrary>> libraries; // parallel to roots; null for a root that did not open
    std::vector<LibraryIssue> rootIssues;             // parallel to roots: why it did not open
    std::map<ToolId, Tool> tools;
    std::map<HolderId, Holder> holders;
    std::map<MaterialId, Material> materials;
    std::map<MachineId, MachineDef> machines;
    std::map<Uuid, OpTemplate> templates;

    void openMissing()
    {
        libraries.resize(roots.size());
        rootIssues.resize(roots.size());
        for (std::size_t i = 0; i < roots.size(); ++i) {
            if (libraries[i]) {
                continue;
            }
            auto opened = ILibrary::open(roots[i]);
            if (opened) {
                libraries[i] = std::move(*opened);
                rootIssues[i] = {};
            } else {
                rootIssues[i] = {roots[i], "The library cannot be opened: " + opened.error().message};
            }
        }
    }

    void merge()
    {
        tools.clear();
        holders.clear();
        materials.clear();
        machines.clear();
        templates.clear();
        for (const auto& library : libraries) {
            if (!library) {
                continue;
            }
            for (Tool& t : library->tools()) {
                tools.insert_or_assign(t.id, std::move(t));
            }
            for (Holder& h : library->holders()) {
                holders.insert_or_assign(h.id, std::move(h));
            }
            for (Material& m : library->materials()) {
                materials.insert_or_assign(m.id, std::move(m));
            }
            for (MachineDef& m : library->machines()) {
                machines.insert_or_assign(m.id, std::move(m));
            }
            for (OpTemplate& t : library->templates()) {
                templates.insert_or_assign(t.id, std::move(t));
            }
        }
    }
};

LibraryStack::LibraryStack() : impl_(std::make_unique<Impl>()) {}
LibraryStack::~LibraryStack() = default;
LibraryStack::LibraryStack(LibraryStack&&) noexcept = default;
LibraryStack& LibraryStack::operator=(LibraryStack&&) noexcept = default;

void LibraryStack::setRoots(std::vector<std::string> dirsLowestFirst)
{
    impl_ = std::make_unique<Impl>();
    impl_->roots = std::move(dirsLowestFirst);
    impl_->openMissing();
    impl_->merge();
}

std::vector<LibraryIssue> LibraryStack::issues() const
{
    std::vector<LibraryIssue> out;
    for (std::size_t i = 0; i < impl_->roots.size(); ++i) {
        if (impl_->libraries[i]) {
            for (LibraryIssue& issue : impl_->libraries[i]->issues()) {
                out.push_back(std::move(issue));
            }
        } else {
            out.push_back(impl_->rootIssues[i]);
        }
    }
    return out;
}

bool LibraryStack::refresh()
{
    bool changed = false;
    for (auto& library : impl_->libraries) {
        if (library) {
            changed = library->refresh() || changed;
        }
    }
    const std::size_t before = static_cast<std::size_t>(
        std::count_if(impl_->libraries.begin(), impl_->libraries.end(), [](const auto& l) { return l != nullptr; }));
    impl_->openMissing();
    const std::size_t after = static_cast<std::size_t>(
        std::count_if(impl_->libraries.begin(), impl_->libraries.end(), [](const auto& l) { return l != nullptr; }));
    changed = changed || before != after;
    impl_->merge();
    return changed;
}

const Tool* LibraryStack::tool(const ToolId& id) const
{
    const auto it = impl_->tools.find(id);
    return it == impl_->tools.end() ? nullptr : &it->second;
}

const Holder* LibraryStack::holder(const HolderId& id) const
{
    const auto it = impl_->holders.find(id);
    return it == impl_->holders.end() ? nullptr : &it->second;
}

const Material* LibraryStack::material(const MaterialId& id) const
{
    const auto it = impl_->materials.find(id);
    return it == impl_->materials.end() ? nullptr : &it->second;
}

const MachineDef* LibraryStack::machine(const MachineId& id) const
{
    const auto it = impl_->machines.find(id);
    return it == impl_->machines.end() ? nullptr : &it->second;
}

std::vector<Tool> LibraryStack::queryTools(std::string_view toolQueryJson,
                                           const std::map<std::string, double>& vars) const
{
    auto predicate = detail::compileToolQuery(toolQueryJson, vars);
    if (!predicate) {
        log(LogLevel::Warn, "lib", "Tool query matches nothing: " + predicate.error().message);
        return {};
    }
    std::vector<Tool> out;
    for (const auto& [id, t] : impl_->tools) {
        if ((*predicate)(t)) {
            out.push_back(t);
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
    for (const auto& [id, t] : impl_->templates) {
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

} // namespace ray::lib
