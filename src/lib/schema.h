// The JSON Schemas of the library files (schemas/*.schema.json, compiled in) and the small validator that checks
// files against them. Private to src/lib.
//
// The validator implements the part of JSON Schema draft 2020-12 the library schemas use: type (a name or a list),
// enum, const, minimum, maximum, exclusiveMinimum, exclusiveMaximum, minLength, maxLength, pattern (ECMAScript),
// minItems, maxItems, items, required, properties, additionalProperties, anyOf, oneOf and local "#/$defs/..."
// references; $schema, $id, $defs, $comment, title, description, default and examples are annotations. Any other
// keyword is an error when the schema is loaded, so a schema cannot quietly use a keyword nothing checks.
#pragma once
#include "json.h"

#include <ray/core.h>

#include <memory>
#include <string>
#include <string_view>

namespace ray::lib::detail {

class Schema {
public:
    // Parses and checks a schema. FormatError for invalid JSON, an unsupported keyword or a bad pattern.
    static Result<std::shared_ptr<const Schema>> load(std::string_view schemaJson);
    ~Schema();
    Schema(const Schema&) = delete;
    Schema& operator=(const Schema&) = delete;
    // FormatError with a message such as "/presets/0/surfaceSpeed: must be greater than 0" for the first problem.
    Status validate(const json::Value& instance) const;

    struct Impl;

private:
    explicit Schema(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// The compiled-in schema of one file kind: "tool", "holder", "material", "machine", "optemplate". Loaded once;
// an unknown name or a schema that does not load is a programming error (std::terminate).
const Schema& builtinSchema(std::string_view name);
// The text of a compiled-in schema (tests compare it with schemas/ on disk); empty for an unknown name.
std::string_view builtinSchemaText(std::string_view name);

// Structural equality of two JSON values (numbers by value).
bool jsonEqual(const json::Value& a, const json::Value& b);

} // namespace ray::lib::detail
