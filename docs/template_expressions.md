# Operation templates: rules and expressions

An operation template (task T-057) is a saved operation keyed by what it is for: its type and parameters, a rule for
choosing the tool, a feeds rule and a heights rule. Applying a template makes ordinary operations: each remembers the
template it came from (`templateId`, shown in the CAM tree), but nothing links it to the template afterwards. Editing a
template changes no existing operation; "Re-apply template" (`cam.template.reapply`) does that on purpose, for one
operation.

Templates live in the libraries, one file each: `templates/<uuid>.optemplate.json`
(`schemas/optemplate.schema.json`). The built-in library ships starter templates for the eight starter materials
(`tools/gen_library.py`); your own go into your library (`cam.template.saveFrom`, or "Save as template" on an
operation row). The rules are implemented in `src/cam/templates.h`.

## A template file

```json
{ "schema": 1, "id": "<uuid>", "name": "Tap drill, chip break", "opType": "drill", "featureKind": "hole.tapped",
  "material": "<6061-T6 uuid>", "machine": null,
  "set": "Tapped holes", "sequence": 20,
  "when": { "feature.depthRatio": { "above": 3, "max": 5 } },
  "toolQuery": { "type": "drill",
                 "diameter": { "min": "$feature.diameter - 0.03 mm", "max": "$feature.diameter + 0.03 mm" },
                 "prefer": { "diameter": "$feature.diameter" } },
  "params": { "cycle": "chipBreak", "peck": "1 * $tool.diameter" },
  "feeds": { "mode": "calculated", "opClass": "drill", "coolant": "flood" },
  "heights": { "clearance": "0.6 in", "retract": "0.1 in" },
  "comment": "..." }
```

| Key | Meaning |
|-|-|
| `opType` | The operation type (`drill`; the milling types as they arrive). |
| `featureKind` | What it applies to: `hole` (any hole), `hole.tapped` (a hole with a thread spec), `hole.plain` (without), `hole.through`, `hole.blind`, `hole.counterbored`, `hole.countersunk`; empty for any. |
| `material`, `machine` | The stock material and machine it is for; `null` for any. A set is looked up for the setup's material and machine. |
| `set` | The template set it belongs to: the templates applied together in one action ("Tapped holes"). |
| `sequence` | Its place in the set. Templates of one set with the same sequence are **alternatives**: each hole takes the first whose `featureKind` and `when` match it (this is how the peck rules by depth ratio work). |
| `when` | Conditions on `feature.*` variables, all of which must hold: a number, a text or `true`/`false` means equal; an object takes `min` and `max` (inclusive), `above` and `below` (exclusive). A hole without the variable (no counterbore, no thread) does not match. |
| `toolQuery` | The tool rule: a library query (below). |
| `params` | The operation's parameters, as its type's schema defines them. |
| `feeds` | `{"mode": "calculated", "opClass"?}`: the feeds and speeds calculator for the chosen tool, the setup's stock material and machine, as the operation panel's "Use calculated" (plunge 50 %, ramp 75 % of the cut feed). `{"mode": "fixed", "rpm", "feedCut", "feedPlunge"?, "feedRamp"?}` in rpm and mm/min. Both take `coolant` (`off`, `flood`, `mist`, `throughTool`, `air`; default `flood`) and `clockwise` (default true). Absent: calculated. |
| `heights` | `clearance`, `retract`, `feedPlane`, `top`, `bottom` as lengths; the others take the operation panel's defaults in the document's units. |

### The tool rule

`toolQuery` is a library query (`LibraryStack::queryTools`): `type` (a type name or a list), the numeric fields
(`diameter`, `shankDiameter`, `fluteLength`, `shoulderLength`, `overallLength`, `cornerRadius`, `tipAngle` in
degrees, `tipDiameter`, `threadPitch`, `flutes`) as a value or `{"min", "max"}`, and `threadSpec`, `bodyMaterial`,
`material`, `opClass` as texts. Two more keys belong to templates:

- `prefer`: `{"<numeric field>": <value>}`: of the tools that match, the one whose field is nearest the value (ties: the
  smaller diameter, then the id);
- `id`: `"<tool uuid>"`: "this exact tool" (the rest of the query must still match it).

When the feeds are calculated, tools with cutting data for the setup's material (and `opClass`) are preferred. When
no tool matches for some holes, applying refuses and creates nothing, naming the template, the rule as evaluated
("drill, diameter 5.075 to 5.135 mm") and the holes ("1/4-20 UNC at X 70.104 Y 10.16, ...").

## Expressions

Any numeric value in `toolQuery`, `params` (a parameter of type number, integer or boolean), `heights`, fixed `feeds`
and `prefer` may be a string: an expression of the **document's** expression language, the one parameters and feature
dimensions use: `+ - * / ^`, parentheses, the units `mm`, `in`, `deg`, `rad`, the functions `min`, `max`, `abs`,
`sqrt`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan2`, `floor`, `ceil`, `round`, `pi`, and the document's parameters
by name. Template variables are written `$feature.diameter` (or `feature.diameter`) and `$tool.diameter`.

Each variable goes into the expression with its unit, so units are checked: a length field must come out as a length.
`"$feature.diameter + 0.2 mm"` is right; `"$feature.diameter + 0.2"` is an error ("unit mismatch: cannot add a plain
number to a length"). Results are mm for lengths, degrees for angles, plain numbers otherwise; a plain number field
(a count, a time in seconds, rpm, a feed in mm/min) takes a plain expression (`"0.5 * $feature.depthRatio"`).

A text key (`threadSpec`, `bodyMaterial`, a choice parameter such as `cycle`) takes a literal, or a text variable as the
whole value: `"threadSpec": "$feature.threadSpec"`.

`tool.*` are known in `params`, `heights` and `feeds`, once the tool is chosen; in `toolQuery` and `when` only
`feature.*` are. Errors name the template, the field and the expression:
`Template 'Tap drill', params.peck: '0.5 * $tool.diametr': there is no variable tool.diametr`.

### feature.* (a recognised hole)

Lengths in mm, angles in degrees, the rest plain numbers (1 or 0 for yes or no). "Always" variables exist for every
hole; the others only where the description says, and naming one a hole does not have is an error (in a `when`, the
condition simply does not match).

| Name | Unit | Always | What |
|-|-|-|-|
| `feature.diameter` | length | yes | The bore's diameter: the lowest cylinder of the hole, the drilled size (a tapped hole of the hole feature is modelled at its tap drill). |
| `feature.depth` | length | yes | From the hole's top to the bottom of its full-diameter bore (a through hole: to where it breaks out). |
| `feature.totalDepth` | length | yes | From the hole's top to its lowest point (the tip of a drill point). |
| `feature.depthRatio` | plain | yes | `feature.depth / feature.diameter`. |
| `feature.through` | plain | yes | 1 for a through hole. |
| `feature.blind` | plain | yes | 1 for a blind hole. |
| `feature.flatBottom` | plain | yes | 1 for a blind hole with a flat floor. |
| `feature.topDiameter` | length | yes | The diameter at the hole's top: its countersink, chamfer or counterbore, else the bore. |
| `feature.spotDiameter` | length | yes | What a spot drill makes when it spots the hole by its model (the drilling cycle's spot diameter 0): its countersink or chamfer, else the bore. |
| `feature.tapped` | plain | yes | 1 when the hole has a thread spec. |
| `feature.threadSpec` | text | tapped | The thread designation, `M6x1`, `1/4-20 UNC`. |
| `feature.threadPitch` | length | tapped | The thread pitch. |
| `feature.threadsPerInch` | plain | tapped | 25.4 / the pitch. |
| `feature.threadMajor` | length | tapped, when the thread gives it | The nominal (major) diameter. |
| `feature.threadDepth` | length | tapped | The full-thread depth (the whole bore when the thread gives none). |
| `feature.counterbored` | plain | yes | 1 when the hole is counterbored. |
| `feature.counterboreDiameter` | length | counterbored | The counterbore's diameter. |
| `feature.counterboreDepth` | length | counterbored | The counterbore's depth from the hole's top. |
| `feature.countersunk` | plain | yes | 1 when the hole is countersunk. |
| `feature.countersinkDiameter` | length | countersunk | The countersink's top diameter. |
| `feature.countersinkAngle` | angle | countersunk | The countersink's included angle. |
| `feature.chamfered` | plain | yes | 1 when the hole's edge is chamfered (an edge break). |
| `feature.chamferDiameter` | length | chamfered | The chamfer's top diameter. |

### tool.* (the chosen tool)

| Name | Unit | What |
|-|-|-|
| `tool.diameter` | length | The cutting diameter. |
| `tool.shankDiameter` | length | The shank diameter. |
| `tool.fluteLength` | length | The flute length (length of cut). |
| `tool.shoulderLength` | length | The shoulder length. |
| `tool.overallLength` | length | The overall length. |
| `tool.cornerRadius` | length | The corner radius. |
| `tool.tipDiameter` | length | The tip diameter (chamfer and spot tools). |
| `tool.pointAngle` | angle | The included point angle (drills, spot drills, chamfer mills); 0 if none. |
| `tool.pointLength` | length | The length of the point: half of (diameter - tip diameter) over the tangent of half the point angle; 0 without a point angle. |
| `tool.flutes` | plain | The number of flutes. |
| `tool.threadPitch` | length | The thread pitch (taps, thread mills); 0 if none. |
| `tool.threadSpec` | text | The thread designation (taps); empty if none. |
| `tool.type` | text | The tool type: `drill`, `tap`, `spotDrill`, ... |

## Applying

`cam.template.apply` (and the Templates panel, `cam.template.preview` for what it would make) takes a setup, the
templates (by id, or a `set` looked up for the setup's material and machine) and the holes: those of the setup that
match a filter (`{"kind": "hole.tapped", "threadSpec": "M6x1"}`, `{"diameter": 6.6, "through": true}`, a group's key),
the holes of picked faces, or all of them. For each sequence of the set, each hole takes the first template that
matches it; holes whose template, tool, parameters, heights and feeds come out alike share one operation. The result
is one undo step (the `cam.batch` command): the library tools the document does not have yet are embedded, then the
operations are added at the end of the setup in sequence order (spot, drill, tap). Tool numbers: the number the
document already uses for the tool, else the tool's default number when free, else the next free number.

`cam.template.apply` changes toolpaths, so a script or an agent needs the user's confirmation, as for every CAM change.

## The starter templates

For each of the eight starter materials, three sets, all for drilling cycles:

- **Tapped holes**: spot drill to the thread's major diameter (a 90 degree chamfer at the thread's entry) with the
  smallest spot drill that makes it; the tap drill (the drill of the modelled bore's diameter, which the hole feature
  makes at the thread table's tap drill), with the peck rule; the tap of the hole's thread.
- **Drilled holes**: spot drill to the hole's chamfer or countersink, else its diameter; the drill of the hole's
  diameter, with the peck rule.
- **Reamed holes**: spot drill; a drill leaving `max(0.15 mm, min(0.3 mm, 0.03 * diameter))` on the diameter, with the
  peck rule; the reamer of the hole's diameter. Recognition cannot tell a reamed hole from a drilled one: apply this set
  to the holes you pick, or to a group.

Peck rule, by `feature.depthRatio` and material group (aluminium / steel / stainless / titanium): one feed (G81) up to
3 / 2.5 / 2 / 2; chip breaking (G73) every 1 / 0.5 / 0.3 / 0.25 drill diameters up to 5 / 4 / 3.5 / 3; full retracts
(G83) every 0.5 / 0.3 / 0.2 / 0.15 drill diameters beyond. These are conservative generic numbers chosen by
`tools/gen_library.py`, not copied from any table; check them against your machine, tools and material.

Milling templates follow the same format: an M3 operation type's author adds a function beside `hole_templates` in
`tools/gen_library.py` (with feature kinds such as `pocket.closed` once recognition provides them).
