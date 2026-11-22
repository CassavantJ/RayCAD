// Operation parameters read against the operation type's JSON Schema (task T-035), private to src/cam. Shared by every
// operation type in src/cam/ops (docs/operation_types.md).
//
// The schema is the single source of the parameters: their names, types, ranges and defaults. A type keeps its
// schema text compiled in (embeddedOpSchema, made by src/cam/CMakeLists.txt from schemas/ops/<type>.schema.json),
// parses it once (OpSchema::load) and reads each operation's paramsJson through it (OpSchema::read): every property
// gets the value given or the schema's "default", so a generator never sees a missing key, and the defaults cannot
// drift apart between the form, the API and the generator.
//
// The schema subset read here is what a flat parameter object needs: a top-level "properties" whose members have
// "type" (number, integer, boolean, string) or "enum" (of strings), "minimum", "maximum", "exclusiveMinimum",
// "exclusiveMaximum" and a "default" (required: a parameter without one is a schema error). "title" names the
// parameter in messages. Every other keyword is an annotation for editors (x-unit, x-relevantWhen, ...).
#pragma once

#include "../json.h"

#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace ray::cam::ops {

// The compiled-in schema of operation type `name` ("drill"); empty for an unknown name.
std::string_view embeddedOpSchema(std::string_view name);

class OpParams;

class OpSchema {
public:
    // FormatError naming the problem when the text is not a schema this reader can use.
    static Result<std::shared_ptr<const OpSchema>> load(std::string_view schemaJson);
    // The parameters of one operation: `paramsJson` (canonical JSON of an object; "" reads as {}). InvalidArgument,
    // in words for the operation panel, for a value of the wrong type, out of range or not in its enum, and for a key
    // the schema does not have (a parameter of a newer RayCAD, which this one cannot honour).
    Result<OpParams> read(std::string_view paramsJson) const;

    struct Property {
        std::string key, title;
        enum class Kind { Number, Integer, Boolean, String, Enum } kind = Kind::Number;
        json::Value schema;   // the property's own schema
        json::Value fallback; // its "default"
    };
    const std::map<std::string, Property>& properties() const { return properties_; }

private:
    std::map<std::string, Property> properties_;
};

// Values by key; every key of the schema is present. Reading a key the schema does not have is a programming error
// and gives 0, false or "".
class OpParams {
public:
    double number(std::string_view key) const;
    bool flag(std::string_view key) const;
    const std::string& text(std::string_view key) const;
    // True when the operation's params gave the key (not the default).
    bool given(std::string_view key) const;

private:
    friend class OpSchema;
    struct Entry {
        json::Value value;
        bool given = false;
    };
    std::map<std::string, Entry, std::less<>> values_;
};

} // namespace ray::cam::ops
