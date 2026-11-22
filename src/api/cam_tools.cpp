// The tool library manager's helpers (ray_api/cam_tools.h, task T-046).
#include <ray_api/cam_tools.h>

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

namespace ray::api::cam_tools {

namespace {

constexpr std::array<std::pair<lib::ToolType, std::string_view>, 12> kTypes = {{
    {lib::ToolType::FlatEndMill, "flatEndMill"},
    {lib::ToolType::BallEndMill, "ballEndMill"},
    {lib::ToolType::BullEndMill, "bullEndMill"},
    {lib::ToolType::ChamferMill, "chamferMill"},
    {lib::ToolType::FaceMill, "faceMill"},
    {lib::ToolType::SpotDrill, "spotDrill"},
    {lib::ToolType::Drill, "drill"},
    {lib::ToolType::Tap, "tap"},
    {lib::ToolType::Reamer, "reamer"},
    {lib::ToolType::BoringBar, "boringBar"},
    {lib::ToolType::ThreadMill, "threadMill"},
    {lib::ToolType::EngraveCutter, "engraveCutter"},
}};

Hash256 hashText(std::string_view text)
{
    Hasher h;
    h.bytes(std::as_bytes(std::span<const char>(text.data(), text.size())));
    return h.finish();
}

tl::unexpected<Error> conflict(std::string message)
{
    return tl::unexpected<Error>(Error{ErrorCode::Locked, std::move(message), {}});
}

// What the library holds for the id now, as a content hash; nullopt when it has none.
template <class T, class Id>
std::optional<Hash256> currentHash(const std::vector<T>& items, const Id& id)
{
    const auto it = std::find_if(items.begin(), items.end(), [&](const T& x) { return x.id == id; });
    if (it == items.end()) {
        return std::nullopt;
    }
    return contentHash(*it);
}

// The check before a save or remove: the library still holds `base` (nullopt: nothing) for the item.
Status unchanged(const std::optional<Hash256>& now, const std::optional<Hash256>& base, const char* what)
{
    if (now == base) {
        return {};
    }
    const std::string kind(what);
    if (!base) {
        return conflict("The " + kind + " was created in the library by someone else since you started it");
    }
    if (!now) {
        return conflict("The " + kind + " was deleted from the library by someone else since you opened it");
    }
    return conflict("The " + kind + " was changed in the library by someone else since you opened it");
}

} // namespace

std::string toolJson(const lib::Tool& tool)
{
    return lib::toJson(tool);
}

std::string holderJson(const lib::Holder& holder)
{
    return lib::toJson(holder);
}

Result<lib::Tool> toolFromJson(std::string_view json)
{
    return lib::toolFromJson(json);
}

Result<lib::Holder> holderFromJson(std::string_view json)
{
    return lib::holderFromJson(json);
}

std::string_view toolTypeName(lib::ToolType type)
{
    for (const auto& [t, name] : kTypes) {
        if (t == type) {
            return name;
        }
    }
    return "flatEndMill";
}

std::optional<lib::ToolType> toolTypeFromName(std::string_view name)
{
    for (const auto& [t, n] : kTypes) {
        if (n == name) {
            return t;
        }
    }
    return std::nullopt;
}

Hash256 contentHash(const lib::Tool& tool)
{
    return tool.contentHash();
}

Hash256 contentHash(const lib::Holder& holder)
{
    return hashText(lib::toJson(holder));
}

Status saveTool(lib::ILibrary& library, const lib::Tool& tool, const std::optional<Hash256>& base)
{
    if (auto st = unchanged(currentHash(library.tools(), tool.id), base, "tool"); !st) {
        return st;
    }
    return library.save(tool);
}

Status saveHolder(lib::ILibrary& library, const lib::Holder& holder, const std::optional<Hash256>& base)
{
    if (auto st = unchanged(currentHash(library.holders(), holder.id), base, "holder"); !st) {
        return st;
    }
    return library.save(holder);
}

Status removeTool(lib::ILibrary& library, const ToolId& id, const Hash256& base)
{
    if (auto st = unchanged(currentHash(library.tools(), id), base, "tool"); !st) {
        return st;
    }
    return library.remove(lib::ItemKind::Tool, id.value);
}

Status removeHolder(lib::ILibrary& library, const HolderId& id, const Hash256& base)
{
    if (auto st = unchanged(currentHash(library.holders(), id), base, "holder"); !st) {
        return st;
    }
    return library.remove(lib::ItemKind::Holder, id.value);
}

std::string templateJson(const lib::OpTemplate& t)
{
    return lib::toJson(t);
}

Result<lib::OpTemplate> templateFromJson(std::string_view json)
{
    return lib::templateFromJson(json);
}

Hash256 contentHash(const lib::OpTemplate& t)
{
    return hashText(lib::toJson(t));
}

Status saveTemplate(lib::ILibrary& library, const lib::OpTemplate& t, const std::optional<Hash256>& base)
{
    if (auto st = unchanged(currentHash(library.templates(), t.id), base, "template"); !st) {
        return st;
    }
    return library.save(t);
}

Status removeTemplate(lib::ILibrary& library, const Uuid& id, const Hash256& base)
{
    if (auto st = unchanged(currentHash(library.templates(), id), base, "template"); !st) {
        return st;
    }
    return library.remove(lib::ItemKind::Template, id);
}

} // namespace ray::api::cam_tools
