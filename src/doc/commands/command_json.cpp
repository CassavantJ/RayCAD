// The JSON form of commands and command logs (document_impl.h), used by the recovery journal (PLAN.md 4.2).
#include "../document_impl.h"
#include "../json.h"

#include <exception>
#include <new>

namespace ray::doc {
namespace {

using json::Member;
using json::Value;

constexpr int kCommandSchema = 1;

tl::unexpected<Error> formatError(const std::string& where, const std::string& what)
{
    return tl::unexpected(Error{ErrorCode::FormatError, "Invalid command: " + where + ": " + what, {}});
}

Result<Value> commandValue(const Command& command)
{
    auto args = json::parse(command.argsJson);
    if (!args || !args->isObject()) {
        return tl::unexpected(Error{ErrorCode::InvalidArgument,
                                    "The arguments of the command '" + command.name + "' are not a JSON object",
                                    {}});
    }
    return Value::object({{"args", std::move(*args)},
                          {"label", Value::string(command.label)},
                          {"name", Value::string(command.name)},
                          {"schema", Value::number(kCommandSchema)}});
}

Result<Command> commandOf(const Value& value, const std::string& where)
{
    if (!value.isObject()) {
        return formatError(where, "must be an object");
    }
    for (const Member& member : value.asObject()) {
        if (member.key != "args" && member.key != "label" && member.key != "name" && member.key != "schema") {
            return formatError(where, "unknown key '" + member.key + "'");
        }
    }
    const Value* schema = value.find("schema");
    if (schema == nullptr || !schema->isNumber()) {
        return formatError(where, "'schema' must be a number");
    }
    if (schema->asNumber() > kCommandSchema) {
        return tl::unexpected(Error{ErrorCode::FormatTooNew,
                                    "This command was written by a newer RayCAD (command format " +
                                        schema->numberText() + "); this version reads format " +
                                        std::to_string(kCommandSchema) + ".",
                                    {}});
    }
    if (schema->asNumber() != kCommandSchema) {
        return formatError(where, "unknown schema");
    }
    const Value* name = value.find("name");
    const Value* label = value.find("label");
    const Value* args = value.find("args");
    if (name == nullptr || !name->isString() || name->asString().empty()) {
        return formatError(where, "'name' must be a non-empty string");
    }
    if (label == nullptr || !label->isString()) {
        return formatError(where, "'label' must be a string");
    }
    if (args == nullptr || !args->isObject()) {
        return formatError(where, "'args' must be an object");
    }
    return Command{name->asString(), json::write(*args), label->asString()};
}

} // namespace

Result<std::string> commandToJson(const Command& command)
{
    try {
        auto value = commandValue(command);
        if (!value) {
            return tl::unexpected(value.error());
        }
        return json::write(*value);
    } catch (...) {
        return tl::unexpected(Error{ErrorCode::Internal, "Out of memory while writing a command", {}});
    }
}

Result<Command> commandFromJson(std::string_view text)
{
    try {
        auto value = json::parse(text);
        if (!value) {
            return formatError("command", value.error().message);
        }
        return commandOf(*value, "command");
    } catch (...) {
        return tl::unexpected(Error{ErrorCode::Internal, "Out of memory while reading a command", {}});
    }
}

Result<std::string> commandLogToJson(const std::vector<Command>& commands)
{
    try {
        Value::Array items;
        items.reserve(commands.size());
        for (const Command& command : commands) {
            auto value = commandValue(command);
            if (!value) {
                return tl::unexpected(value.error());
            }
            items.push_back(std::move(*value));
        }
        return json::write(Value::array(std::move(items)));
    } catch (...) {
        return tl::unexpected(Error{ErrorCode::Internal, "Out of memory while writing a command log", {}});
    }
}

Result<std::vector<Command>> commandLogFromJson(std::string_view text)
{
    try {
        auto value = json::parse(text);
        if (!value) {
            return formatError("command log", value.error().message);
        }
        if (!value->isArray()) {
            return formatError("command log", "must be an array");
        }
        std::vector<Command> out;
        out.reserve(value->asArray().size());
        for (const Value& item : value->asArray()) {
            auto command = commandOf(item, "command log[" + std::to_string(out.size()) + "]");
            if (!command) {
                return tl::unexpected(command.error());
            }
            out.push_back(std::move(*command));
        }
        return out;
    } catch (...) {
        return tl::unexpected(Error{ErrorCode::Internal, "Out of memory while reading a command log", {}});
    }
}

} // namespace ray::doc
