// RayCAD post-processor script API, version 1 (PLAN DRAFT)
// A post is one .post.js file that calls definePost({...}). This API is original to RayCAD; it does not
// mirror any vendor's post API. Everything here is deterministic: no Date, no Math.random, no I/O.
// Host notes (T-039): the script runs in strict mode; Math.sin, cos, tan, asin, acos, atan, atan2 and hypot are the
// host's deterministic versions. `this` in every handler is the object given to definePost. Numbers reach the script
// in output units and are snapped back to integer nanometres by `out`, so print them through `out`, not toFixed.
// The canned-cycle settlement (cam.h) added CycleMove.returnLevel, peckClearance and leftHand, and gave top and
// clearance their IR meaning (the hole top and the initial level).
// T-040 (the shipped posts) added, all optional and backwards compatible: capabilities.arcPlanes and helicalPlanes
// (the host linearises arcs in other planes), out.setFormat (decimals per dialect, trailing-zero style, comment
// length), the decimals argument of out.feed (tapping feeds), out.sequence.max, and machine.controlDialect,
// maxBlockNumber and maxProgramNameLength in ProgramContext.
// T-042: the dialect names list the two Mazak posts, mazak-smooth and mazak-matrix (one per control generation), in
// place of mazak-eia. No other change.

declare function definePost(post: PostDefinition): void;

interface PostDefinition {
  title: string;
  vendor: string;
  dialect: "haas-ngc" | "haas-classic" | "fanuc" | "mazak-smooth" | "mazak-matrix" | "generic-iso" | string;
  fileExtension: string;
  apiVersion: 1;
  capabilities: {
    arcs: "IJK" | "R" | "none";
    helical: boolean;
    maxArcDegrees: number;          // 360 allows full circles; 180 or 90 makes the host split arcs
    cannedCycles: CycleKind[];      // cycles not listed are expanded by the host into rapid/linear moves
    rigidTap: boolean;
    cutterComp: boolean;
    arcPlanes?: ("XY" | "XZ" | "YZ")[];       // planes the control cuts arcs in (default all three); arcs in
                                               // other planes reach onLinear as lines within 1 um
    helicalPlanes?: ("XY" | "XZ" | "YZ")[];   // planes it cuts helical arcs in (default all three)
  };
  properties: Record<string, PropertySpec>;      // the host also reads lineEnding ("crlf" | "lf", default "crlf")
                                                 // and includeDate (bool, default false), and declares them if the
                                                 // post does not

  onProgramStart(ctx: ProgramContext, out: Writer): void;
  onSectionStart(section: Section, ctx: ProgramContext, out: Writer): void;   // tool change, WCS, spindle, coolant
  onRapid(m: MoveXYZ, out: Writer): void;
  onLinear(m: MoveXYZ & { feed: number; role: MoveRole; comp: 0 | 1 | 2 }, out: Writer): void;
  onArc(m: ArcMove, out: Writer): void;
  onCycle(c: CycleMove, out: Writer): void;          // called once per hole; host tracks modal state via out.modal
  onCycleEnd(out: Writer): void;
  onDwell(seconds: number, out: Writer): void;
  onSectionEnd(section: Section, ctx: ProgramContext, out: Writer): void;
  onProgramEnd(ctx: ProgramContext, out: Writer): void;
}

type PropertySpec =
  | { type: "bool"; title: string; default: boolean; group?: string; description?: string }
  | { type: "int" | "number"; title: string; default: number; min?: number; max?: number; group?: string; description?: string }
  | { type: "enum"; title: string; default: string; values: string[]; group?: string; description?: string }
  | { type: "string"; title: string; default: string; group?: string; description?: string };

type CycleKind = "drill" | "peck" | "chipBreak" | "tap" | "bore" | "ream";
type MoveRole = "cut" | "plunge" | "ramp" | "leadIn" | "leadOut" | "link" | "retract" | "approach";

interface MoveXYZ { x: number; y: number; z: number; }      // output units (inch or mm), already converted by the host
interface ArcMove extends MoveXYZ {
  cw: boolean; plane: "XY" | "XZ" | "YZ";               // cw as G2, seen from +Z (XY), +Y (XZ) or +X (YZ)
  cx: number; cy: number; cz: number;                       // absolute centre
  i: number; j: number; k: number;                          // centre relative to start
  radius: number; sweepDegrees: number; feed: number; comp: 0 | 1 | 2;
}
interface CycleMove {
  kind: CycleKind; x: number; y: number; top: number; bottom: number; retract: number; clearance: number;
  returnLevel: "retract" | "clearance";
  feed: number; peck: number; peckClearance: number; dwellSeconds: number; pitch: number; leftHand: boolean;
  first: boolean;
  // One hole, with the toolpath IR's values (cam.h "Canned cycles"): top is the hole top, retract the R plane,
  // clearance the initial level. returnLevel: where the tool goes after this hole, "retract" (G99) or "clearance"
  // (G98); it may change from hole to hole. peck: Q, measured from the R plane, 0 = no pecking. peckClearance: the G83
  // re-entry / G73 back-off, 0 = the control's own parameter (emit nothing). leftHand: tapping with G74 instead of G84.
  // dwellSeconds: at the bottom; G82 and G89 take it, other cycles only where the control's cycle has a P word (warn or
  // fail otherwise). Tap feed is pitch times rpm, per minute; rigid or floating tapping is the post's choice.
  // first: the run's first hole, or one whose cycle parameters (all of the above but returnLevel) changed.
  // At a run's first hole the tool's Z is `clearance` (when it was higher, the host has already emitted rapids over
  // the hole and down to it), so the control's initial level is `clearance`. After each hole the tool is at
  // (x, y, return level).
}

interface ToolInfo {
  number: number; lengthOffset: number; diameterOffset: number;
  type: string; diameter: number; cornerRadius: number; fluteLength: number; flutes: number;
  description: string; holderDescription: string; stickOut: number; threadSpec: string;
}
interface Section {
  index: number; name: string; opType: string;
  tool: ToolInfo; toolChanged: boolean; nextTool: ToolInfo | null;
  rpm: number; clockwise: boolean; coolant: "off" | "flood" | "mist" | "through" | "air";
  start: MoveXYZ; clearanceZ: number; minZ: number; maxZ: number;
  cutDistance: number; estimatedSeconds: number;
}
interface ProgramContext {
  programNumber: number; programComment: string; documentName: string; setupName: string; userName: string;
  units: "inch" | "mm"; workOffset: number;                 // 1 => G54
  date: string | null;                                      // null unless includeDate is on
  machine: { name: string; model: string; maxRpm: number; rigidTapping: boolean; hasToolSetter: boolean;
             travel: Record<string, [number, number]>;
             controlDialect: string; maxBlockNumber: number; maxProgramNameLength: number };   // 0 = not known
  properties: Record<string, boolean | number | string>;
  tools: ToolInfo[];                                        // every tool in the program, in first-use order
  sections: Section[];
}

interface Writer {
  block(...words: (string | null | undefined)[]): void;     // joins non-empty words; adds N-number if enabled; one line
  comment(text: string): void;                              // host sanitises to the dialect's legal comment characters
  raw(line: string): void;
  // Modal words: return the word only when the value changed since last output (or after reset).
  modal(group: string, word: string): string | null;        // e.g. out.modal("motion", "G1")
  axis(letter: string, value: number): string | null;       // formatted with the dialect's coordinate format; null if unchanged
  word(letter: string, value: number, fmt?: FormatSpec): string;   // always emitted
  feed(value: number, decimals?: number): string | null;    // decimals: this word only (0..6), e.g. a tapping feed
  resetModal(...groups: string[]): void;                    // no args resets all, e.g. after a tool change;
                                                            // "feed", "axes" and one letter "A".."Z" (that axis) also reset
  setFormat(f: OutputFormat): void;                         // keys left out keep their value; forgets axes and feed
  sequence: { enabled: boolean; start: number; increment: number; onlyOnToolChange: boolean;
              max: number };                                // N numbers wrap to start past max; 0 = no limit
                                                            // (starts as machine.maxBlockNumber)
  warn(message: string): void;                              // surfaces in the UI; does not stop posting
  fail(message: string): never;                             // stops posting with an error
}
interface FormatSpec { decimals: number; trimZeros?: boolean; forceDecimalPoint?: boolean; leadingZero?: boolean; scale?: number; }
// Output format (T-040). Defaults: axisDecimals 3 (mm) or 4 (inch), feedDecimals 1 (mm) or 2 (inch), trailingZeros
// "trim". trailingZeros: "trim" X12. X12.5, "one" X12.0 X12.5, "all" X12.000 X12.500; a decimal point is always
// written. It applies to out.axis, out.feed and out.word without a format. commentMaxLength: sanitised comments are
// cut to this many characters (0 = no limit).
interface OutputFormat { axisDecimals?: number; feedDecimals?: number; trailingZeros?: "trim" | "one" | "all";
                         commentMaxLength?: number; }
