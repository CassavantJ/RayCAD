# Writing an operation type

How an operation type of `src/cam` is built, tested and reviewed. The drilling type (`drill`, task T-035) is the
first one and the example throughout; milling types (T-050 onwards) follow the same pattern. The contract is
`IOperationType` in `include/ray/cam.h`.

## Files

| File | What it holds |
|-|-|
| `schemas/ops/<type>.schema.json` | The parameters: names, types, ranges, defaults, titles, descriptions, editor annotations. The single source of all of them. |
| `src/cam/ops/<type>.h` | Private to src/cam and its tests: the rules in words (depths, levels, feeds, checks), the plan struct, the pure helpers tests call directly (`pointLengthNm`, `tapFeedMicronPerMin`). |
| `src/cam/ops/<type>_plan.cpp` | Reads the params, checks everything, recognises and orders the geometry, works out every level: `plan<Type>(GenerateInput, JobContext) -> Result<Plan>`. |
| `src/cam/ops/<type>.cpp` | The `IOperationType` and `<type>Toolpath(const Plan&) -> Toolpath`: moves from the plan, no kernel calls. |
| `src/cam/ops/builtin_ops.cpp` | One line per type in `registerBuiltinOperationTypes`, which the app calls (`src/api/cam_wiring.cpp`). |
| `src/cam/CMakeLists.txt` | The sources, and the type's name in `RAY_CAM_OP_SCHEMAS` so its schema is compiled in. |
| `tests/unit/cam/<type>_*_test.cpp` | Rules against hand-computed values, each rejection, goldens, end to end (below). |
| `tests/golden/toolpaths/<type>_*.rctp` and `.txt` | The goldens and their readable dumps. |

## Parameters

The schema file is compiled into the program (`src/cam/CMakeLists.txt` writes it into a generated source, as
`src/lib` does with its schemas). The type returns that text from `paramsSchemaJson()` (the operation panel, T-045,
builds its form from it and the API validates against it), and reads its parameters through the same text:

```cpp
static const std::shared_ptr<const OpSchema> schema = *OpSchema::load(embeddedOpSchema("drill"));
auto p = schema->read(op.paramsJson);   // InvalidArgument naming the parameter by its title
double peck = p->number("peck");        // the value given, or the schema's default
```

`OpSchema` (`src/cam/ops/op_params.h`) gives every property of the schema its value or its `default`, so the
generator never sees a missing key and the defaults cannot differ between the form, the API and the generator. It
checks `type` (number, integer, boolean, string) or `enum` (of strings) and the numeric limits, refuses a key the
schema does not have (a parameter of a newer RayCAD this one cannot honour), and requires a `default` on every
property. Keep parameters flat: one object of scalars.

Every property has a `title`, a `description` and a `default`. The operation panel (T-045, `src/ui/cam/operation_form.h`)
builds its form from the schema alone, with no code of its own for any type, so the schema is the whole contract
between a type and its editor.

### The schema subset the form edits

Top level: `type: "object"`, `properties`, `additionalProperties` (false), `required`, `title`, `description`,
`$schema`, `$id`, `$comment` and the `x-` annotations below. Any other top-level keyword (`allOf`, `anyOf`, `oneOf`,
`if`/`then`/`else`, `patternProperties`, `dependentRequired`, ...) is shown as a row saying the form cannot edit it.

A property is one of:

| Kind | Schema | Field |
|-|-|-|
| number | `"type": "number"`, optional `minimum`, `maximum`, `exclusiveMinimum`, `exclusiveMaximum` | a text field that takes expressions (below) |
| integer | `"type": "integer"`, the same limits | the same; the value must be whole |
| boolean | `"type": "boolean"` | a check box |
| choice | `"enum"` of strings (optionally with `"type": "string"`) | a list |
| text | `"type": "string"`, optional `minLength`, `maxLength`, `pattern` | a text field |

with `title`, `description` (the field's hint), `default`, `$comment` and the property annotations. Anything else
(`object`, `array`, `$ref`, `oneOf`, a type list, `const`, an enum of numbers, an `x-unit` the form does not know) gives
the property a row "cannot edit this parameter here" naming the reason; its stored value is kept unchanged. A property
without a `default` cannot be reset (OpSchema refuses such a schema anyway).

### Property annotations

- `x-unit`: the quantity of a number or integer. Stored values are always in these units, whatever the document's:
  - `length`: mm. The field shows the document's display unit ("10 mm", "0.3937 in") and takes what the feature panels
    take: a bare number in the display unit, a number with a unit (`mm`, `in`), an expression, parameter names
    (`doc::evaluate`, kind Length). Limits are mm and shown converted.
  - `angle`: degrees. A bare number is degrees; `deg` and `rad` and expressions work (kind Angle).
  - `time`: seconds. A bare number is seconds; a trailing `s`, `ms` or `min` is read; the rest is a plain expression.
  - `count`: a plain number or expression without units (kind Unitless); also the meaning of no `x-unit`.
- `x-enumTitles`: the labels of a choice's `enum` values, an array of strings of the same length and order. A list of
  another length is a schema problem: the form shows the values themselves.
- `x-relevantWhen`: `{ "<property>": [values...], ... }`. The property matters only when every named property is
  relevant itself (so relevance is transitive) and has one of the listed values. Each named property must be a choice
  (values are its enum strings) or a boolean (values are `true` / `false`) of the same schema; a name that is not, or a
  value not in its enum, is a schema problem the form reports. A property that is not relevant is hidden and not
  validated by the form; its value stays as it was (or its default) and is stored, so the type must give it no
  meaning in that case. (The validators of the repository have no `if`/`then`, so this is an annotation.)

### Top-level annotations

- `x-propertyOrder`: the order of the form, an array of property names; properties it does not list follow in key
  order (canonical JSON sorts keys), names that are not properties are ignored.
- `x-heightsUsed`: the members of `Operation::heights` the type reads, a subset of `clearance`, `retract`,
  `feedPlane`, `top`, `bottom`; absent means all five. Only these are shown. The commands still store all five,
  ordered clearance >= retract >= feedPlane >= top >= bottom, so the panel gives an unused height the value of the
  nearest used one before it in that order, else of the nearest used one after it (drill: `feedPlane` = `retract`,
  `bottom` = `top`), and the type must not read them.
- `x-cuttingUsed`: the members of `Operation::cutting` the type reads, a subset of `rpm`, `clockwise`, `feedCut`,
  `feedPlunge`, `feedRamp`, `feedLeadIn`, `feedLeadOut`, `coolant`; absent means all. Only these are shown; `rpm` and
  `feedCut` are always required by the commands, and an unused feed is stored as the cut feed.
- `x-geometry`: what the geometry picker selects, `{ "kind", "count", "description" }`:
  - `kind`: `hole` (a face of a hole recognised along the setup's Z, `recognizeHoles`; a click on any face of the hole
    picks it and the operation stores the hole's bore wall, one reference per hole), `face`, `edge`, or `none` (the
    type takes no geometry);
  - `count`: `one`, `oneOrMore` or `any` (zero or more); OK needs at least one for `one` and `oneOrMore`;
  - `description`: the prompt shown above the list.
  Absent means `{ "kind": "none" }`.
- `x-toolTypes`: the tool types (the library's `type` names, `flatEndMill`, `drill`, `tap`, ...) the tool picker
  offers first: an array, or `{ "property": "<choice>", "values": { "<value>": [types], ... }, "default": [types] }`
  when they depend on a choice (drill: by `cycle`). Other tools are listed after a "Show every tool" switch; the type's
  `validate` stays the judge. Absent: every tool.

Lengths are stored in mm, times in seconds; the operation's own `Heights` and `CuttingData` are not params. Say in
the schema's top `description` what the type reads from them and what it ignores (drill ignores `feedPlane` and
`bottom`).

### Feeds the panel offers

"Use calculated" fills `rpm` and `feedCut` from `computeFeeds` for the tool, the setup's material and its machine.
`computeFeeds` has no rule for plunge, ramp and lead feeds, so the panel offers fractions of the cut feed as editable
defaults and says so: plunge 50 %, ramp 75 %, lead-in and lead-out 100 %. What it wrote is kept with the operation
(`Operation::calculated`, a record in no input hash), so the panel tells calculated values from typed ones.

## validate and generate

`validate` is cheap and runs before every generation; `generate` runs on a worker. Both call the same plan function,
so they cannot disagree:

```cpp
Status validate(const GenerateInput& in) const override {
    JobContext ctx;                       // validate has no job
    auto plan = planDrill(in, ctx);
    return plan ? Status{} : tl::unexpected(plan.error());
}
Result<Toolpath> generate(const GenerateInput& in, JobContext& ctx) const override {
    auto plan = planDrill(in, ctx);       // the same checks, then the moves
    ...
    return drillToolpath(*plan);
}
```

The plan does every check and every piece of geometry work once, and returns the first problem in words a machinist
reads ("The 7 mm drill is larger than the 6.6 mm hole at X15 Y15"; "At the hole at X15 Y15 the tool goes 22.803 mm
deep, but its flutes are 20 mm long"). Name places by WCS coordinates, tools by size and kind, parameters by their
title. Codes: `InvalidArgument` for what the user changes in the operation, `ToolpathFailed` for geometry or a
machine that cannot do it, `Internal` for a bug or out of memory.

Errors and warnings: `Status` cannot carry warnings, so a problem is an error when the toolpath would be wrong or
unsafe (a tool larger than the hole, a tap whose thread does not match the hole's, flutes or stick-out too short for
the depth, heights out of order, a clearance below the stock, a hole not reachable, a spindle speed beyond the
machine), and a warning when the toolpath is right but the user should know (a hole without a thread spec, a drill
point left in a flat floor, a point going below the modelled one, no rigid tapping). The plan collects warnings in a
fixed order and `generate` puts them in `Toolpath::warnings`, which the engine keeps with the toolpath and the UI shows
with it; they are not part of the RCTP bytes.

Rules for the generator:

- read only `GenerateInput`: the reduced tool, holder and machine (`generatorTool` and friends: only what the input
  hash covers; if a type needs another field, add it there and to the hash, with a test that the hash changes), the
  operation (never its name or template), and the resolved geometry. No clock, tier, thread count or locale;
- geometry: match `geom.selected` (with `selectedShapeIndex`) against what the type recognises in `geom.partShapes`
  (holes: `recognizeHoles(shape, geom.wcsFromModel, ctx, holeOptionsFor(geom, part))`, matching a selected face
  against `RecognizedHole::faces`); a selected element that is not usable is an error saying where it is;
- integer nanometres for every coordinate as early as possible; `dmath` for trigonometry; round with a stated rule
  (`toNm`, half to even) once, and say in the header which value is rounded where;
- poll `ctx.cancel` in loops over geometry.

## Tests and goldens

For each type:

1. **Rules against hand-computed values**: every formula of the header with the arithmetic written out in the test,
   to the tolerance the acceptance asks (drill: point length and breakthrough to 1 um, the tapping feed exactly).
2. **Each rejection on its own**: one case per check, asserting the code and the words, and that `generate` refuses
   with the same message (`drill_validate_test.cpp`).
3. **The invariant checker on every toolpath**: `tests/unit/cam/invariants/toolpath_invariants.h` (`checkToolpath`:
   the canned-cycle rules plus rapids over the stock, the floor, machine travel and feeds). Extend it with general
   rules your type needs, written from `cam.h`, never from `src/cam`.
4. **Goldens**: a small kernel-built part and at least one corpus part, one golden per mode. Each golden is
   `tests/golden/toolpaths/<name>.rctp` plus `<name>.txt` (`tests/unit/cam/toolpath_dump.h`: every move on one line,
   lengths exact in mm). The test checks that a fresh toolpath gives both files byte for byte and that the `.txt`
   says what the `.rctp` holds. To make them again after a reviewed change, run the golden cases with
   `RAY_UPDATE_TOOLPATH_GOLDENS=1` and review the `.txt` diff; never edit a golden by hand.
5. **End to end**: a part built by features, the operation added by commands, generated by the engine with
   `registerBuiltinOperationTypes`; an edit makes it Stale and it regenerates; whatever the type reads beyond the
   geometry (drill: the hole's thread) makes it stale when it changes.
6. **The chain** once per type family: the toolpath through `sim::buildBackplot` and the post host with the test post
   (`tests/unit/post/posts/iso-test.post.js`) against a committed G-code golden.
