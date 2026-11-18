// Reading command arguments (see args.h).
#include "args.h"

#include <algorithm>

namespace ray::doc::commands {

tl::unexpected<Error> invalidArgument(std::string message)
{
    return tl::unexpected(Error{ErrorCode::InvalidArgument, std::move(message), {}});
}

tl::unexpected<Error> Args::error(std::string_view key, const std::string& what) const
{
    return invalidArgument(command_ + ": '" + std::string(key) + "' " + what);
}

tl::unexpected<Error> Args::error(const std::string& what) const
{
    return invalidArgument(command_ + ": " + what);
}

Result<Args> Args::parse(std::string_view command, std::string_view argsJson,
                         std::initializer_list<std::string_view> allowed)
{
    auto parsed = json::parse(argsJson);
    if (!parsed) {
        return invalidArgument(std::string(command) + ": the arguments are not valid JSON: " + parsed.error().message);
    }
    if (!parsed->isObject()) {
        return invalidArgument(std::string(command) + ": the arguments must be a JSON object");
    }
    for (const json::Member& member : parsed->asObject()) {
        if (std::find(allowed.begin(), allowed.end(), member.key) == allowed.end()) {
            return invalidArgument(std::string(command) + ": '" + member.key + "' is not an argument of this command");
        }
    }
    return Args(std::string(command), std::move(*parsed));
}

Result<const json::Value*> Args::optValue(std::string_view key) const
{
    return root_.find(key);
}

Result<Uuid> Args::id(std::string_view key) const
{
    const json::Value* value = root_.find(key);
    if (value == nullptr) {
        return error(key, "is required");
    }
    if (!value->isString()) {
        return error(key, "must be a string holding a lower-case UUID");
    }
    const Uuid id = Uuid::fromString(value->asString());
    if (id.toString() != value->asString()) {
        return error(key, "must be a lower-case UUID (8-4-4-4-12 hex digits)");
    }
    if (id == Uuid{}) {
        return error(key, "must not be the nil UUID");
    }
    return id;
}

Result<std::optional<std::string>> Args::optString(std::string_view key) const
{
    const json::Value* value = root_.find(key);
    if (value == nullptr) {
        return std::optional<std::string>();
    }
    if (!value->isString()) {
        return error(key, "must be a string");
    }
    return std::optional<std::string>(value->asString());
}

Result<std::string> Args::string(std::string_view key) const
{
    auto value = optString(key);
    if (!value) {
        return tl::unexpected(value.error());
    }
    if (!*value) {
        return error(key, "is required");
    }
    return std::move(**value);
}

Result<std::optional<std::string>> Args::optText(std::string_view key) const
{
    auto value = optString(key);
    if (value && *value && (*value)->empty()) {
        return error(key, "must not be empty");
    }
    return value;
}

Result<std::string> Args::text(std::string_view key) const
{
    auto value = optText(key);
    if (!value) {
        return tl::unexpected(value.error());
    }
    if (!*value) {
        return error(key, "is required");
    }
    return std::move(**value);
}

Result<std::optional<bool>> Args::optBool(std::string_view key) const
{
    const json::Value* value = root_.find(key);
    if (value == nullptr) {
        return std::optional<bool>();
    }
    if (!value->isBool()) {
        return error(key, "must be true or false");
    }
    return std::optional<bool>(value->asBool());
}

Result<bool> Args::boolean(std::string_view key) const
{
    auto value = optBool(key);
    if (!value) {
        return tl::unexpected(value.error());
    }
    if (!*value) {
        return error(key, "is required");
    }
    return **value;
}

Result<std::optional<std::size_t>> Args::optIndex(std::string_view key, std::size_t max) const
{
    const json::Value* value = root_.find(key);
    if (value == nullptr) {
        return std::optional<std::size_t>();
    }
    const std::string range = "must be a whole number from 0 to " + std::to_string(max);
    if (!value->isNumber()) {
        return error(key, range);
    }
    const double d = value->asNumber();
    // Every size_t below 2^53 is exact as a double; feature lists are far shorter.
    if (!(d >= 0.0 && d <= static_cast<double>(max)) || static_cast<double>(static_cast<std::size_t>(d)) != d) {
        return error(key, range);
    }
    return std::optional<std::size_t>(static_cast<std::size_t>(d));
}

Result<std::optional<std::string>> Args::optObject(std::string_view key) const
{
    const json::Value* value = root_.find(key);
    if (value == nullptr) {
        return std::optional<std::string>();
    }
    if (!value->isObject()) {
        return error(key, "must be a JSON object");
    }
    return std::optional<std::string>(json::write(*value));
}

} // namespace ray::doc::commands
