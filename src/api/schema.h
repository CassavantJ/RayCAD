// JSON Schema validation of action arguments, private to src/api.
//
// The subset of JSON Schema 2020-12 that RayCAD's schemas use: type (a name or a list of names, with "integer"),
// enum, const, properties, required, additionalProperties (false or a schema), minProperties, maxProperties, items
// (one schema), minItems, maxItems, minimum, maximum, exclusiveMinimum, exclusiveMaximum, minLength, maxLength
// (in code points), pattern (ECMAScript, unanchored), anyOf, oneOf, allOf and not. Annotation keywords ($schema,
// title, description, default, examples) are ignored, and so is any other keyword.
#pragma once

#include "json.h"

#include <string>

namespace ray::api::schema {

// Ok, or InvalidArgument "<path>: <what is wrong>" where <path> is the JSON Pointer of the first bad value ("" is
// the whole document, "/path", "/bodies/2/id"). The first problem in document order is reported.
Status validate(const json::Value& schema, const json::Value& instance);

// Adds the "default" of every top-level property the instance lacks (instance must be an object).
void fillDefaults(const json::Value& schema, json::Value& instance);

// A JSON Pointer with '~' and '/' escaped.
std::string pointerAppend(const std::string& base, std::string_view token);

} // namespace ray::api::schema
