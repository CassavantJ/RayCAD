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
// Expressions: numbers, + - * /, unary minus, parentheses and variables such as feature.minWidth or $tool.pitch
// (a leading '$' is ignored). Evaluated in double precision left to right, which is the same on every platform.
// Any other key, a string that names a variable (variables are numbers only) or an expression that fails makes
// the query malformed: InvalidArgument.
#pragma once
#include <ray/libraries.h>

#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace ray::lib::detail {

Result<double> evaluate(std::string_view expression, const std::map<std::string, double>& vars);

using ToolPredicate = std::function<bool(const Tool&)>;
Result<ToolPredicate> compileToolQuery(std::string_view queryJson, const std::map<std::string, double>& vars);

} // namespace ray::lib::detail
