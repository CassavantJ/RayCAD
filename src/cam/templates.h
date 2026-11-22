// Operation templates (v1 shop knowledge, task T-057), private to src/cam, src/api and their tests.
//
// A template (lib::OpTemplate, a library file templates/<uuid>.optemplate.json, schemas/optemplate.schema.json) is a
// saved operation keyed by the feature it is for. Applying it to recognised holes makes ORDINARY operations
// (cam::Operation) with templateId set: nothing links them to the template afterwards, so a later edit of the template
// changes no existing operation (the API's "re-apply" does that, explicitly).
//
// THE FILE. Besides the members OpTemplate has (opType, featureKind, material, machine, toolQuery, params), a
// template file may have these keys, which src/lib keeps in OpTemplate::extraJson and this file reads
// (schemas/optemplate.schema.json describes each; docs/template_expressions.md is the user's page):
//   "set"       the template set it belongs to ("Tapped holes"): the templates applied together in one action;
//   "sequence"  its place in the set (ascending); templates of one set with the same sequence are ALTERNATIVES: for
//               each hole the first whose featureKind and "when" match is used (peck rules by depth ratio);
//   "when"      conditions on feature variables, all of which must hold: {"feature.depthRatio": {"above": 3,
//               "max": 6}, "feature.through": 1, "feature.threadSpec": "M6x1"}: a number or a text is equality (a
//               boolean is 1 or 0), an object takes "min" and "max" (inclusive) and "above" and "below" (exclusive);
//   "feeds"     {"mode": "calculated", "opClass"?: "drill", "coolant"?, "clockwise"?}: computeFeeds for the tool,
//               the setup's stock material and its machine (as the operation panel's "Use calculated"), or
//               {"mode": "fixed", "rpm", "feedCut", "feedPlunge"?, "feedRamp"?, "coolant"?, "clockwise"?} (mm/min);
//               absent: calculated;
//   "heights"   {"clearance"?, "retract"?, "feedPlane"?, "top"?, "bottom"?}: lengths; the caller's defaults for
//               the others (the operation panel's).
// and in "toolQuery", besides the keys of LibraryStack::queryTools:
//   "prefer"    {"diameter": <expression>}: of the tools that match, the one whose value is nearest (ties: the first
//               in the query's order, the smaller diameter);
//   "id"        "<tool uuid>": that exact tool (the rest of the query must still match it).
//
// EXPRESSIONS. Any numeric value of toolQuery, params (a property of type number or integer in the operation type's
// schema), heights, fixed feeds and "when" may be a string: an expression of the DOCUMENT's expression language
// (doc::evaluate: + - * / ^, units mm in deg rad, min max abs sqrt sin cos tan floor ceil round, the document's
// parameters) over the variables feature.* and tool.* (written $feature.diameter, or feature.diameter). Each variable
// is put into the expression as its value with its unit, "(5.1054 mm)", so the evaluator checks units: a length
// field must come out as a length ("$feature.diameter + 0.2 mm", not "+ 0.2"). Lengths are mm, angles degrees, the
// others plain numbers. A text key (threadSpec, bodyMaterial, an enum parameter) takes a literal or a text variable
// as the whole value ("$feature.threadSpec"). Errors name the template, the field and the expression.
// tool.* exist in params, heights and feeds (after the tool is chosen); in toolQuery and "when" only feature.* do.
//
// FEATURES. v1 recognises holes (recognizeHoles). A hole's kinds are "hole", "hole.tapped" or "hole.plain" (a thread
// spec or not), "hole.through" or "hole.blind", and "hole.counterbored" / "hole.countersunk" when it is; a template
// whose featureKind is empty or one of them applies to it.
//
// Deterministic: no clock, no locale, ordered containers; the expression evaluator is the caller's (the document's).
#pragma once

#include <ray/cam.h>
#include <ray/libraries.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ray::cam::templates {

// ---------- Variables ----------
enum class VarUnit : std::uint8_t { Length, Angle, Plain, Text };
struct Var {
    VarUnit unit = VarUnit::Plain;
    double number = 0; // mm, degrees or a plain number
    std::string text;  // VarUnit::Text
};
using Vars = std::map<std::string, Var, std::less<>>; // "feature.diameter" -> value

// Every variable a template may name, with its unit and what it is (docs/template_expressions.md lists the same; a
// test checks that). `always`: every hole (every tool) has it; otherwise only when the description says so.
struct VarInfo {
    std::string_view name;
    VarUnit unit;
    bool always;
    std::string_view description;
};
std::span<const VarInfo> featureVariables();
std::span<const VarInfo> toolVariables();

Vars featureVars(const RecognizedHole& hole);
Vars toolVars(const lib::Tool& tool);

// ---------- Expressions ----------
// The kind of value a field takes (doc::Parameter::Kind): the evaluator returns Length in mm, Angle in degrees,
// Count (a whole number) and Unitless as plain numbers.
enum class FieldKind : std::uint8_t { Length, Angle, Count, Unitless };
// The document's expression evaluator (api::cam_operation::evaluate over the document's parameters).
using EvaluateFn = std::function<Result<double>(std::string_view expression, FieldKind kind)>;

// `expression` with each feature.* / tool.* variable replaced by its value and unit in parentheses ("(6.35 mm)",
// "(118 deg)", "(20)"), so the document's evaluator reads it. InvalidArgument for a variable `vars` lacks (an unknown
// name, or one this hole or tool does not have) or a text variable, naming it.
Result<std::string> substitute(std::string_view expression, const Vars& vars);
// A numeric field: a JSON number as it is, or an expression string (substitute, then `evaluate`). Errors (the
// variable's or the evaluator's) say "Template '<name>', <field>: '<expression>': <what>".
Result<double> evaluateField(const lib::OpTemplate& t, std::string_view field, std::string_view expression,
                             const Vars& vars, FieldKind kind, const EvaluateFn& evaluate);

// ---------- Feature kinds and groups ----------
std::vector<std::string> featureKindsOf(const RecognizedHole& hole); // "hole", "hole.tapped", "hole.through", ...
bool kindMatches(std::string_view templateKind, const RecognizedHole& hole);

// Holes alike enough to take the same operations: the same kind (tapped or plain), thread spec (tapped) or bore
// diameter (plain), through or blind, and counterbore / countersink sizes. Groups in the order of their first hole.
struct HoleGroup {
    std::string key;   // stable text, e.g. "tapped|1/4-20 UNC|through" or "plain|6.6|through|cb11:6.5"
    std::string kind;  // "hole.tapped" | "hole.plain"
    std::string threadSpec;
    double diameter = 0; // the bore, mm
    double depth = 0;    // the full-diameter depth, mm (the first hole's)
    bool through = false;
    double counterboreDiameter = 0, counterboreDepth = 0, countersinkDiameter = 0; // 0: none
    std::string label;   // "4 x 1/4-20 UNC tapped, through" style text in mm (the UI makes its own in its units)
    std::vector<std::size_t> holes; // indices into the holes given
};
std::vector<HoleGroup> groupHoles(std::span<const RecognizedHole> holes);
// A short description of one hole for messages: "1/4-20 UNC at X 12.7 Y 10.16" (mm, the setup's WCS).
std::string describeHole(const RecognizedHole& hole);

// ---------- Reading a template ----------
struct Condition {
    std::string variable; // "feature.depthRatio"
    std::optional<double> equals, min, max, above, below;
    std::optional<std::string> text;
};
struct FeedsRule {
    bool calculated = true;
    std::string opClass;                           // calculated: the preset's class; "" lets computeFeeds choose
    std::map<std::string, std::string> fixed;      // fixed: rpm, feedCut, feedPlunge, feedRamp as JSON text
    std::optional<CuttingData::Coolant> coolant;   // absent: flood
    std::optional<bool> clockwise;                 // absent: true
};
struct TemplateRules {
    std::string set;
    int sequence = 0;
    std::vector<Condition> when;
    FeedsRule feeds;
    std::map<std::string, std::string> heights;    // key -> JSON text of a number or an expression string
    std::string toolQueryJson;                     // the queryTools part (canonical JSON, "prefer" and "id" removed)
    std::map<std::string, std::string> prefer;     // field -> JSON text
    std::optional<ToolId> exactTool;
};
// The rules of a template (its extraJson and toolQuery read). FormatError naming the template and the key.
Result<TemplateRules> rulesOf(const lib::OpTemplate& t);

// The extension keys a template file may have (above); src/api writes them into extraJson when it saves one.
std::string extraJsonOf(const TemplateRules& rules, std::string_view otherExtraJson);

// ---------- Planning an application ----------
struct PlanHole {
    RecognizedHole hole;
    naming::TopoRef ref; // the bore wall (what a drilling operation's geometry names)
};
struct PlanContext {
    // The libraries' tools for a query (LibraryStack::queryTools with no variables: every expression is evaluated
    // here first). Called on the caller's thread.
    std::function<Result<std::vector<lib::Tool>>(std::string_view queryJson)> queryTools;
    EvaluateFn evaluate;
    const OperationRegistry* types = nullptr; // the operation types (schemas)
    const lib::Material* material = nullptr;  // the setup's stock material; null: none (calculated feeds then fail)
    lib::MachineDef machine;                  // the setup's machine (0 = unknown limits)
    Heights defaultHeights;                   // for the heights a template does not give
};
struct PlannedOperation {
    std::size_t templateIndex = 0; // into the templates given
    Operation op;                  // id empty; tool.tool set, the rest of ToolRef for the caller to fill
    lib::Tool tool;                // the library's tool
    std::vector<std::size_t> holes; // into the holes given, in their order
};
struct MissingTool {
    std::size_t templateIndex = 0;
    std::string rule;               // the query as evaluated, in words: "drill, diameter 5.06 to 5.16 mm"
    std::vector<std::size_t> holes;
};
struct Plan {
    std::vector<PlannedOperation> operations; // in order: sequence, then template, then first hole
    std::vector<MissingTool> missing;         // non-empty: the application must create nothing
    std::vector<std::size_t> untouched;       // holes no template applied to
};
// What applying `templates` to `holes` makes. Templates are taken by (sequence, the order given); for each sequence
// each hole takes the first template of that sequence whose featureKind and "when" match it. Holes whose template,
// chosen tool, params, heights and cutting data come out the same share one operation. Errors: a template that does
// not read (FormatError), an operation type that is not registered, an expression or parameter that fails
// (InvalidArgument naming template, field and expression), feeds that cannot be calculated (naming the tool and why).
// A tool that cannot be found is not an error: it is listed in Plan::missing.
Result<Plan> planTemplates(std::span<const lib::OpTemplate> templates, std::span<const PlanHole> holes,
                           const PlanContext& context);
// "Nothing was created: no tool in the libraries for 'Tap drill' (drill, diameter 5.06 to 5.16 mm) for 4 holes
// (1/4-20 UNC at X 12.7 Y 10.16, ...); ..." for a plan with missing tools.
std::string missingToolsMessage(const Plan& plan, std::span<const lib::OpTemplate> templates,
                                std::span<const PlanHole> holes);

} // namespace ray::cam::templates
