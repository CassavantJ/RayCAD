// Reading command arguments, private to src/doc/commands. Every error is ErrorCode::InvalidArgument with a message
// that names the command and the field, e.g. "feature.add: 'id' must be a lower-case UUID".
#pragma once

#include "../json.h"

#include <ray/document.h>

#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

namespace ray::doc::commands {

class Args {
public:
    // The arguments must be a JSON object whose keys are all in `allowed`.
    static Result<Args> parse(std::string_view command, std::string_view argsJson,
                              std::initializer_list<std::string_view> allowed);

    bool has(std::string_view key) const { return root_.find(key) != nullptr; }

    // A lower-case 8-4-4-4-12 UUID other than the nil one.
    Result<Uuid> id(std::string_view key) const;
    Result<std::string> string(std::string_view key) const;
    Result<std::optional<std::string>> optString(std::string_view key) const;
    // Non-empty strings.
    Result<std::string> text(std::string_view key) const;
    Result<std::optional<std::string>> optText(std::string_view key) const;
    Result<std::optional<bool>> optBool(std::string_view key) const;
    Result<bool> boolean(std::string_view key) const;
    // A whole number from 0 to `max`.
    Result<std::optional<std::size_t>> optIndex(std::string_view key, std::size_t max) const;
    // A JSON object (returned canonical).
    Result<std::optional<std::string>> optObject(std::string_view key) const;
    Result<const json::Value*> optValue(std::string_view key) const; // null if absent

    tl::unexpected<Error> error(std::string_view key, const std::string& what) const;
    tl::unexpected<Error> error(const std::string& what) const; // about the arguments as a whole

    const std::string& command() const { return command_; }

private:
    Args(std::string command, json::Value root) : command_(std::move(command)), root_(std::move(root)) {}
    std::string command_;
    json::Value root_;
};

tl::unexpected<Error> invalidArgument(std::string message);

} // namespace ray::doc::commands
