// Tool queries of LibraryStack::queryTools and operation templates (PLAN.md 3.6). Private to src/lib.
//
// A query is a JSON object; a tool matches when every key matches:
//   "type"          a tool type name ("flatEndMill", or the enum spelling "FlatEndMill") or a list of them;
//   numeric fields  "diameter", "shankDiameter", "fluteLength", "shoulderLength", "overallLength", "cornerRadius",
//                   "tipAngle", "tipDiameter", "threadPitch", "flutes": a number or expression (equal within
//                   1e-6 mm, relative above 1 mm) or {"min": ..., "max": ...} (inclusive, either may be left out);
//   "threadSpec"    equal ignoring case and spaces ("1/4-20 UNC" = "1/4-20unc");
//   "bodyMaterial"  equal ignoring case;
//   "material", "opClass"  the tool has a preset for that material id and/or op class (one preset for both).
// Variables (QueryVars, keys spelled without '$') are numbers or strings. The canonical spelling names them with a
// '$': "$feature.minWidth", "$feature.threadSpec" (PLAN.md 3.6).
//   Expressions (numeric fields): numbers, + - * /, unary minus, parentheses and number variables, written
//   $feature.minWidth or, accepted as well, feature.minWidth. Evaluated in double precision left to right, which is
//   the same on every platform.
//   String keys ("threadSpec", "bodyMaterial", "material", "opClass"): a literal, or "$name" as the whole value for
//   a string variable. Without the '$' the value is a literal, so "feature.threadSpec" matches a tool whose thread
//   designation is that text.
// Any other key, an unknown variable, a variable of the wrong type or an expression that fails makes the query
// malformed: InvalidArgument naming the key.
#pragma once
#include <ray/libraries.h>

#include <functional>
#include <string>
#include <string_view>

namespace ray::lib::detail {

Result<double> evaluate(std::string_view expression, const QueryVars& vars);

using ToolPredicate = std::function<bool(const Tool&)>;
Result<ToolPredicate> compileToolQuery(std::string_view queryJson, const QueryVars& vars);

} // namespace ray::lib::detail
