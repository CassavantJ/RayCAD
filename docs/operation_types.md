# Writing an operation type

How an operation type of `src/cam` is built, tested and reviewed. The drilling type (`drill`, task T-035) was the
first one and is the example throughout; the milling types (T-052 to T-056) follow the same pattern. The contract is
`IOperationType` in `include/ray/cam.h`.

## The types

Nine types are registered (`registerBuiltinOperationTypes`, `src/cam/ops/builtin_ops.cpp`), each with its schema in
`schemas/ops/<type>.schema.json` (whose top `description` says what the type reads) and its rules in the header
`src/cam/ops/<type>.h`:

| Type | Task | Geometry it picks | What it does |
|-|-|-|-|
| `drill` | T-035 | holes | Spot drilling, drilling, peck drilling, chip breaking, tapping, boring and reaming as canned cycles |
| `face` | T-052 | none | Faces the whole top of the stock flat with parallel strokes, zigzag or one way, at one or more depths |
| `contour2d` | T-053 | edges and faces | Runs beside a chain of edges at one or more depths, with leads, roughing, finish and spring passes, tabs and cutter compensation |
| `pocket2d` | T-054 | faces (floors) | Clears the area above each floor with offset rings, islands left standing, helix, ramp or pre-drilled entry |
| `adaptive2d` | T-061 | faces (floors) | Roughs the area above each floor with constant-engagement passes (the engine of `src/cam/ops/adaptive2d`), helix, ramp, pre-drilled or open-edge entry, reduced feed where the load rises |
| `slot` | T-055 | faces (floors) | Cuts each slot along its centre line with an end mill as wide as the slot, ramping into each depth |
| `chamfer2d` | T-055 | edges | Chamfers or deburrs the top edges of walls with the flank of a chamfer mill or a spot drill |
| `engrave` | T-056 | edges (or none) | Engraves along edges projected onto a plane and text of one or more lines in the RayCAD Single Line font |
| `threadmill` | T-056 | faces | Mills internal and external threads with a helix, single-form or multi-form tool, in one or more radial passes |

Known limits of the types as they are: `pocket2d` takes the walls as vertical (the footprint at the floor);
`chamfer2d` and `engrave` take edges only (their generators refuse faces); geometry is picked on model faces and
edges, never on a sketch; and no type knows the material earlier
operations removed (linking is told nothing of it, `src/cam/link.h`), so a later operation may cut air.

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
| points | `"type": "string"` with `"x-format": "pointsXY"` | a list of points (below) |

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
  - `percent` (added after M3): a number in percent, stored as the number of percent (45 for 45 %). The field shows
    "45 %" and takes a bare number, a number with `%`, or a plain expression; its limits (`minimum`, `maximum`, ...)
    are checked and shown in percent ("Must be at least 5 %"). Say in the description what it is a percentage of
    (pocket2d: "of the tool diameter"); the title needs no "(%)".
- `x-enumTitles`: the labels of a choice's `enum` values, an array of strings of the same length and order. A list of
  another length is a schema problem: the form shows the values themselves. Keep them short (a word or two): the
  detail goes in `x-enumDescriptions`.
- `x-enumDescriptions`: optional, a detail line for each of a choice's `enum` values (such as the G codes a cycle is
  written as), an array of strings of the same length and order; `""` for a value without one. The form shows it
  under the title in the open list and in the field's tooltip, never in the field itself. A list of another length is
  ignored.
- `x-format` (added after M3): what a text holds. `pointsXY`: a list of points on the setup's XY plane (work
  coordinates), stored in mm whatever the document's unit as `x y` pairs separated by `;` (`"20 15; 60 15"`; a comma
  between x and y and a leading `+` also read; numbers are fixed-point, no exponent). The form shows it as a list: one
  line per point with X and Y fields that show and take lengths as length fields do (the display unit, units,
  expressions), a remove button per point, an add button (a point at 0, 0 to type over), and a button that makes
  clicks in the view add points (the panel's selection filter takes faces and vertices: a face of a hole recognised
  along the setup's Z gives the hole's centre, a vertex itself, any other face the point clicked; picking geometry
  stops meanwhile). The points of every relevant points parameter are drawn in the view while the panel is open (a
  cross at the stock's top, a dashed line down through it). A text not edited is stored back exactly as it was; once
  a point is edited, added or removed the text is written again as `x y; x y`, the points not edited keeping their
  numbers as written, a point typed or added written with at most six decimals (a nanometre). A stored text that does not read as points
  (a document typed by hand) is shown as a text field with the reason until it does. The type parses the text itself
  (pocket2d: `parsePointList`); the form reads the same grammar. A format the form does not know is edited as plain
  text.
- `x-multiline` (added after M3): `true` on a text makes its field a text area of several lines: Enter is a line
  break (stored as `\n`), Ctrl+Enter accepts the panel as Enter does in other fields, Esc cancels. `maxLength` and
  `pattern` apply to the whole text, line breaks included. The type says in its description what a line break means
  (engrave: the next line of text).
- `x-relevantWhen`: `{ "<property>": [values...], ... }`. The property matters only when every named property is
  relevant itself (so relevance is transitive) and has one of the listed values. Each named property must be a choice
  (values are its enum strings) or a boolean (values are `true` / `false`) of the same schema; a name that is not, or a
  value not in its enum, is a schema problem the form reports. A property that is not relevant is hidden and not
  validated by the form; its value stays as it was (or its default) and is stored, so the type must give it no
  meaning in that case. (The validators of the repository have no `if`/`then`, so this is an annotation.)

### Top-level annotations

- `x-propertyOrder`: the order of the form, an array of property names; properties it does not list follow in key
  order (canonical JSON sorts keys), names that are not properties are ignored.
- `x-heightsUsed`: the members of `Operation::heights` the type reads, each with the reference it is measured from:
  an object `{ "<height>": "<reference>", ... }` whose keys are a subset of `clearance`, `retract`, `feedPlane`, `top`,
  `bottom` (the older form, an array of those names, still reads, with no reference known). Absent means all five,
  references unknown. Only these are shown, each with its reference as a hint under the field. The references:
  - `stockTop`: the top of the setup's stock box ("from stock top");
  - `partTop`: the highest point of the setup's part bodies ("from part top");
  - `workZero`: absolute WCS Z, the work offset's zero ("from work zero");
  - `eachPick`: each picked element's own height: a hole's top, a chain of edges' height, a face's height ("from each
    hole's top", "from the picked edges", "from the picked faces", "from the picked edges and faces", by
    `x-geometry`'s kind);
  - `highestPick`: the highest of the picked elements ("from the highest hole top", "from the highest pick").
  The commands store all five; the panel gives an unused height the value of the nearest used one before it in the
  order clearance, retract, feedPlane, top, bottom, else of the nearest used one after it (drill: `feedPlane` =
  `retract`, `bottom` = `top`), and the type must not read them.
  Order: the document's commands check no order (the document has no operation registry). The API (and the panel
  before it) refuses, for a registered type, only an order every operation of the type would refuse whatever its
  geometry: of two heights the type uses, the one earlier in that order below the later one when both are measured
  from the same reference, or the earlier from `highestPick` and the later from `eachPick` (the highest pick has both).
  Every other order (heights from different references, such as face's `bottom` from the part top against its `top`
  from the stock top, and the absolute levels against the stock or the part) is the type's own validation's to judge,
  with a message naming the references; every type must make that check (docs above: "heights out of order").
- `x-cuttingUsed`: the members of `Operation::cutting` the type reads, a subset of `rpm`, `clockwise`, `feedCut`,
  `feedPlunge`, `feedRamp`, `feedLeadIn`, `feedLeadOut`, `coolant`; absent means all. Only these are shown; `rpm` and
  `feedCut` are always required by the commands, and an unused feed is stored as the cut feed.
- `x-geometry`: what the geometry picker selects, `{ "kind", "count", "description" }`:
  - `kind`: `hole` (a face of a hole recognised along the setup's Z, `recognizeHoles`; a click on any face of the hole
    picks it and the operation stores the hole's bore wall, one reference per hole), `face`, `edge`, or `none` (the
    type takes no geometry); or a list of kinds when the type takes several (added after M3): `["edge", "face"]`
    (contour2d: chains of edges, or the loops of planar faces). The selection filter then takes every kind listed,
    the picks are stored in the order they were made, and the panel lists them grouped by kind, each kind under a
    heading, with the count by kind ("2 edges, 1 face"). `hole` stands alone (a click on a face of a hole would be
    ambiguous): in a list with other kinds the panel drops it and says so in the geometry prompt, as it does for a
    name it does not know. Declare a kind only when the generator handles it; the generator still names a pick it
    cannot use;
  - `count`: `one`, `oneOrMore` or `any` (zero or more); OK needs at least one for `one` and `oneOrMore`;
  - `description`: the prompt shown above the list.
  Absent means `{ "kind": "none" }`.
- `x-toolTypes`: the tool types (the library's `type` names, `flatEndMill`, `drill`, `tap`, ...) the tool picker
  offers first: an array, or `{ "property": "<choice>", "values": { "<value>": [types], ... }, "default": [types] }`
  when they depend on a choice (drill: by `cycle`). Other tools are listed after a "Show every tool" switch; the type's
  `validate` stays the judge. Absent: every tool.

Lengths are stored in mm, times in seconds; the operation's own `Heights` and `CuttingData` are not params. Say in
the schema's top `description` what the type reads from them and what it ignores (drill ignores `feedPlane` and
`bottom`), and give `x-heightsUsed` the reference of each height it reads.

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
