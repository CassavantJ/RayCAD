// RayCAD post processor: Mazak vertical machining centres with a MAZATROL Smooth control (SmoothX, SmoothG, SmoothAi,
// and the SmoothEz / SmoothC family members), EIA/ISO programs only, 3 axes.
// Written from scratch for RayCAD's own post API (include/ray/post_api.d.ts). It is not derived from any other CAM
// system's post processor. It writes EIA/ISO G-code text only: no MAZATROL conversational programs and no Mazak
// program file formats.
//
// SOURCES AND HOW FAR THEY WERE CHECKED (read this before trusting a line of output)
//  [M]  Mazak's EIA/ISO programming manuals for the MAZATROL Smooth series. Mazak supplies them with the machine and
//       through its service centres; they are not openly published, and the author of this post did not read them
//       (the copies that turn up on document-sharing sites are not Mazak's publications and were not used). EVERY
//       Mazak-specific rule below is therefore marked [unverified]: it is the common ISO / FANUC-compatible form that
//       EIA/ISO programs on these controls are generally written in, not a rule read in a Mazak document.
//  [N]  T. Kramer, F. Proctor, E. Messina, "The NIST RS274NGC Interpreter - Version 3", NISTIR 6556, NIST, 2000
//       (public domain, read). Where a word's meaning is the RS-274 one, the [N] section is cited; that the control
//       follows it is still [unverified].
//  [ISO] ISO 6983-1:2009, program format and address words (not read: title only).
//
// HOW CONSERVATIVE IT IS
//  Because nothing Mazak-specific could be read, the post writes the smallest common subset and takes the safe
//  side wherever controls are known to differ:
//  - Canned cycles: Q and P on every cycle block; G98 never relied on after a lower hole of the same run (the run is
//    ended and restarted from the clearance); G84, G85 and G89 never use G98 (G99, then G80 and a rapid up), so the
//    motion is the toolpath's whether these cycles feed out to R or to the initial level.
//  - No retract before M6 only when the user asserts that the machine's M6 leaves Z up (m6LeavesZRetracted).
//  - Helical arcs in the XY plane only; tool pre-select off by default; high-speed / smoothing codes off by default.
//  - A dwell is warned about: its P unit is [unverified].
//
// SAFETY RULES THIS POST ENFORCES (it fails with a message naming the cause rather than post something doubtful)
//  - Units alone, then G17 G40 G49 G80 G90 G94 [N 3.5.6, 3.5.7, 3.5.10, 3.5.11, 3.5.15, 3.5.17, 3.5.19; unverified].
//  - Z retract before every tool change (G91 G28 Z0. by default) unless the M6 assertion above is given.
//  - After a tool change: units, G0 G90 G54 G17 X Y, S M3/M4, then G43 Z H on the first Z approach, then coolant.
//  - One M code per block. Cutter compensation only on linear X/Y moves in the XY plane, never across a rapid.
//  - G80 before other moves, tool changes and the end. No feed move with the spindle stopped; spindle speed checked
//    against the machine's maximum; tap feed = pitch x rpm and the spindle direction checked.
//  - Program end: Z retract first, spindle and coolant off, the optional XY return, M30, "%".
//
// G AND M WORDS THIS POST CAN WRITE (for the independent G-code reader, T-043)
//  G0 G1 G2 G3 G4 G17 G18 G19 G20 G21 G28 G40 G41 G42 G43 G49 G53 G54..G59 G54.1 G73 G74 G80 G81 G82 G83 G84 G84.2
//  G84.3 G85 G89 G90 G91 G94 G98 G99, and with highSpeedMode G61.1 G64 G05 (G05 P2 / G05 P0); M1 M3 M4 M5 M6 M29 M30
//  and the codes of coolantCodes (default M8 M9). Other words: N O T S F H D X Y Z I J K R Q P, comments in ( ), "%".

"use strict";

const CONTROL = "MAZATROL Smooth";

// ---------- small helpers (no I/O, no clock: everything is a pure function of the program) ----------

// The comment characters the host keeps (src/post/writer.h sanitizeComment), upper case; for the program name on the
// O line, which goes out with out.raw.
function cleanText(text) {
  let s = "";
  let space = false;
  for (const ch of String(text)) {
    if (!/[A-Za-z0-9.,\-+/*=:#_]/.test(ch)) { space = s.length > 0; continue; }
    if (space) { s += " "; space = false; }
    s += ch.toUpperCase();
  }
  return s;
}

function near(a, b) { return Math.abs(a - b) <= 1e-9 * Math.max(1, Math.abs(a), Math.abs(b)); }

// Coolant codes from the property text "flood=M8/M9 mist=/ through=/ air=/": mode -> { on, off } ("" = none).
function parseCoolantCodes(text, out) {
  const codes = { flood: null, mist: null, through: null, air: null };
  for (const item of String(text).trim().split(/\s+/)) {
    if (item === "") continue;
    const m = /^(flood|mist|through|air)=(M\d{1,3})?\/(M\d{1,3})?$/.exec(item);
    if (!m) {
      out.fail("post property coolantCodes: \"" + item + "\" is not of the form mode=Mon/Moff with mode flood, mist, " +
        "through or air (an empty code is allowed, e.g. through=/)");
    }
    codes[m[1]] = { on: m[2] || "", off: m[3] || "" };
  }
  for (const k of Object.keys(codes)) {
    if (codes[k] === null) codes[k] = { on: "", off: "" };
  }
  return codes;
}

// Whether a hole can continue the active canned cycle: the same G code and the same words present.
function sameCycleShape(active, code, words) {
  if (!active || active.code !== code) return false;
  for (const k of Object.keys(words)) {
    if ((words[k] === null) !== (active.words[k] === null)) return false;
  }
  return true;
}

// The cycle words of a hole that differ from those in force.
function changedCycleWords(active, words) {
  return Object.keys(words).map((k) => (words[k] !== null && words[k] !== active.words[k] ? words[k] : null));
}

// Points of an arc split into n equal pieces (R-format output, each piece at most 90 degrees). In-plane axes (u, v)
// and normal (w) as the host gives them: XY (x, y | z), XZ (z, x | y), YZ (y, z | x); cw is G2 seen from +normal.
function arcPieces(m, start, n) {
  const ax = { XY: ["x", "y", "z"], XZ: ["z", "x", "y"], YZ: ["y", "z", "x"] }[m.plane];
  const c = { x: m.cx, y: m.cy, z: m.cz };
  const a0 = Math.atan2(start[ax[1]] - c[ax[1]], start[ax[0]] - c[ax[0]]);
  const dir = m.cw ? -1 : 1;
  const sweep = m.sweepDegrees * Math.PI / 180;
  const end = { x: m.x, y: m.y, z: m.z };
  const pieces = [];
  for (let k = 1; k <= n; k++) {
    if (k === n) { pieces.push(end); break; }
    const a = a0 + dir * sweep * k / n;
    const p = {};
    p[ax[0]] = c[ax[0]] + m.radius * Math.cos(a);
    p[ax[1]] = c[ax[1]] + m.radius * Math.sin(a);
    p[ax[2]] = start[ax[2]] + (end[ax[2]] - start[ax[2]]) * k / n;
    pieces.push(p);
  }
  return pieces;
}

definePost({
  title: "Mazak VMC, MAZATROL Smooth, EIA/ISO programs",
  vendor: "RayCAD",
  dialect: "mazak-smooth",
  fileExtension: ".nc",
  apiVersion: 1,
  capabilities: {
    arcs: "IJK",
    helical: true,
    // Arcs are split into pieces of at most 180 degrees: whether the control takes a full circle in one IJK block
    // is [unverified]; two halves are correct on any control that takes IJK arcs [N 3.5.3.2].
    maxArcDegrees: 180,
    cannedCycles: ["drill", "peck", "chipBreak", "tap", "bore", "ream"],
    rigidTap: true,
    cutterComp: true,
    arcPlanes: ["XY", "XZ", "YZ"], // G17 G18 G19 [N 3.5.6; unverified]
    helicalPlanes: ["XY"],         // conservative: XZ / YZ helices are cut as lines by the host
  },
  properties: {
    sequenceNumbers: { type: "enum", title: "Sequence numbers", default: "off", values: ["off", "every", "toolChange"],
      group: "Format", description: "N numbers on every block, only on the first block of each tool, or none." },
    sequenceStart: { type: "int", title: "Sequence start", default: 10, min: 1, max: 99999, group: "Format" },
    sequenceIncrement: { type: "int", title: "Sequence increment", default: 10, min: 1, max: 1000, group: "Format" },
    decimalStyle: { type: "enum", title: "Decimal point style", default: "trim", values: ["trim", "one", "all"],
      group: "Format", description: "X12. (trim), X12.0 (one) or X12.000 (all). A decimal point is always written, so " +
        "a number never depends on the control's least-increment rule for numbers without one [unverified]." },
    commentMaxLength: { type: "int", title: "Longest comment", default: 0, min: 0, max: 1000, group: "Format",
      description: "Comments are cut to this many characters; 0 = no limit." },
    programNumberDigits: { type: "enum", title: "Program number digits", default: "4", values: ["4", "8"],
      group: "Program", description: "O0001-O9999, or 8 digits, as the control's work numbers allow [unverified]." },
    toolList: { type: "bool", title: "Tool list in header", default: true, group: "Program" },
    optionalStop: { type: "bool", title: "Optional stop before tool changes", default: false, group: "Program",
      description: "M1 before every tool change after the first [N 3.6.1]." },
    // A one-block form (T<this> T<next> M6) is not offered: if the control mounted the last T word of the block, every
    // operation would run with the wrong tool and length offset. Not until the control's manual confirms it.
    toolPreselect: { type: "enum", title: "Next-tool pre-select", default: "off", values: ["off", "separateT"],
      group: "Tool change", description: "off; separateT: the next tool's T word in a block of its own after the M6 " +
        "block [N 3.7.3]. How the control reads it is [unverified]." },
    toolChangeRetract: { type: "enum", title: "Retract before tool change", default: "G28", values: ["G28", "G53", "none"],
      group: "Tool change", description: "G91 G28 Z0. then G90, G0 G53 Z0., or nothing (needs m6LeavesZRetracted) " +
        "[unverified]." },
    m6LeavesZRetracted: { type: "bool", title: "M6 leaves Z retracted (allows retract none)", default: false,
      group: "Tool change", description: "Set this only if the machine's tool change always ends with Z at the " +
        "tool-change height. With toolChangeRetract none the post moves X/Y right after M6 at whatever height M6 " +
        "left the spindle; without this assurance it refuses none." },
    endRetract: { type: "enum", title: "Retract at program end", default: "G28", values: ["G28", "G53"],
      group: "Program end" },
    endPosition: { type: "enum", title: "After the Z retract", default: "zOnly", values: ["zOnly", "zThenY", "zThenXY"],
      group: "Program end", description: "Also send Y, or X and Y, home after Z." },
    coolantCodes: { type: "string", title: "Coolant codes", default: "flood=M8/M9 mist=/ through=/ air=/",
      group: "Coolant", description: "On/off M codes per coolant mode. M8 flood and M9 off are the RS274 codes " +
        "[N 3.6.4; unverified on this control]; mist, through-spindle and air codes are not known: set them, e.g. " +
        "through=M51/M9. A mode used with no code stops posting." },
    dwellUnits: { type: "enum", title: "Dwell P units", default: "milliseconds", values: ["milliseconds", "seconds"],
      group: "Cycles", description: "P of G4, G82, G84 and G89: integer milliseconds (P300) or seconds (P0.3). " +
        "[unverified]: every program with a dwell gets a warning until the owner confirms the unit." },
    tapping: { type: "enum", title: "Tapping", default: "auto", values: ["auto", "rigidM29", "G84.2", "plainG84"],
      group: "Cycles", description: "rigidM29: M29 S.. before G84/G74. G84.2: synchronous tapping G84.2 (G84.3 left " +
        "hand). plainG84: G84/G74 with the spindle started first (floating, or synchronous by parameter). auto: " +
        "rigidM29 when the machine definition says rigid tapping, else plainG84 with a warning. All [unverified]." },
    highSpeedMode: { type: "enum", title: "High-speed / smoothing mode", default: "off", values: ["off", "G61.1", "G05P2"],
      group: "Motion", description: "Written once after the safe start and cancelled before M30 (G61.1 ... G64, or " +
        "G05 P2 ... G05 P0). Both forms are [unverified] for this control: a warning is given when used." },
    arcFormat: { type: "enum", title: "Arc centre format", default: "IJK", values: ["IJK", "R"], group: "Motion",
      description: "IJK (centre offsets) or R (radius; arcs split at 90 degrees [N 3.5.3.1])." },
  },

  // ---------- program start ----------
  onProgramStart(ctx, out) {
    const p = ctx.properties;
    this.inch = ctx.units === "inch";
    this.units = this.inch ? "G20" : "G21";
    this.coolantCodes = parseCoolantCodes(p.coolantCodes, out);
    this.tapping = p.tapping === "auto" ? (ctx.machine.rigidTapping ? "rigidM29" : "plainG84") : p.tapping;
    this.autoPlain = p.tapping === "auto" && !ctx.machine.rigidTapping;
    this.arcFormat = p.arcFormat;
    this.dwellUnits = p.dwellUnits;
    this.highSpeed = p.highSpeedMode;
    this.wcs = this.wcsCode(ctx.workOffset, out);
    if (p.toolChangeRetract === "none" && !p.m6LeavesZRetracted) {
      out.fail("post property toolChangeRetract is none, but nothing says the machine's M6 leaves Z retracted: the " +
        "first X/Y move after a tool change would run at an unknown height. Choose a retract, or set the post " +
        "property m6LeavesZRetracted if the machine's tool change always ends with Z up");
    }
    this.toolLoaded = false;
    this.spindle = null;
    this.rpm = 0;
    this.coolant = "off";
    this.comp = 0;
    this.cycleActive = false;
    this.cycle = null;
    this.pos = { x: null, y: null, z: null };
    this.h = null;
    this.maxRpm = ctx.machine.maxRpm;

    out.setFormat({ axisDecimals: this.inch ? 4 : 3, feedDecimals: this.inch ? 2 : 1, trailingZeros: p.decimalStyle,
      commentMaxLength: p.commentMaxLength });
    out.sequence.enabled = p.sequenceNumbers !== "off";
    out.sequence.onlyOnToolChange = p.sequenceNumbers === "toolChange";
    out.sequence.start = p.sequenceStart;
    out.sequence.increment = p.sequenceIncrement;
    if (out.sequence.max === 0) out.sequence.max = 99999; // five digits [N 3.3.1; unverified]

    const digits = Number(p.programNumberDigits);
    const number = ctx.programNumber;
    const limit = digits === 8 ? 99999999 : 9999;
    if (!(Number.isInteger(number) && number >= 1 && number <= limit)) {
      out.fail("program number " + number + " is outside O1 to O" + limit + " (" + digits + "-digit program numbers on " +
        "the " + CONTROL + " post); set the setup's program number or the property programNumberDigits");
    }
    const name = "O" + String(number).padStart(digits, "0");
    if (ctx.machine.maxProgramNameLength > 0 && name.length > ctx.machine.maxProgramNameLength) {
      out.fail("program name " + name + " is longer than the machine's limit of " + ctx.machine.maxProgramNameLength +
        " characters (machine definition maxProgramNameLength)");
    }
    // "%", then O and the program name in a comment on the same line [unverified].
    out.raw("%");
    let title = cleanText(ctx.programComment !== "" ? ctx.programComment : ctx.documentName);
    if (p.commentMaxLength > 0) title = title.substring(0, p.commentMaxLength).trim();
    out.raw(title === "" ? name : name + " (" + title + ")");
    out.comment("Document " + ctx.documentName + " setup " + ctx.setupName);
    if (ctx.date !== null) out.comment("Posted " + ctx.date);
    out.comment("Machine " + ctx.machine.name + " " + CONTROL + " EIA/ISO");
    if (p.toolList) {
      for (const t of ctx.tools) {
        out.comment("T" + t.number + " D" + out.word("", t.diameter) + " " + t.type + " " + t.description);
      }
    }
    // Safe start: units alone, then the modes cancelled or set [N 3.5; unverified on this control].
    out.block(out.modal("units", this.units));
    out.block(out.modal("plane", "G17"), "G40", "G49", "G80", out.modal("distance", "G90"), "G94");
    if (this.highSpeed !== "off") {
      out.warn("post property highSpeedMode = " + this.highSpeed + ": the code form is [unverified] for the " + CONTROL +
        " control; check it in the machine's EIA/ISO programming manual before running the program");
      out.block(this.highSpeed === "G61.1" ? "G61.1" : "G05 P2");
    }
  },

  wcsCode(offset, out) {
    if (offset >= 1 && offset <= 6) return "G" + (53 + offset);
    if (offset >= 7 && offset <= 54) return "G54.1 P" + (offset - 6); // additional work offsets [unverified]
    out.fail("work offset " + offset + " has no code on the " + CONTROL + " post (1-6 are G54-G59, 7-54 are G54.1 " +
      "P1-P48)");
  },

  // ---------- sections ----------
  onSectionStart(section, ctx, out) {
    const p = ctx.properties;
    const t = section.tool;
    if (!(section.rpm > 0)) out.fail("section " + section.index + " (" + section.name + ") has no spindle speed");
    const rpm = Math.round(section.rpm);
    if (this.maxRpm > 0 && rpm > this.maxRpm) {
      out.fail("section " + section.index + " (" + section.name + "): " + rpm + " rpm is above the machine's " +
        this.maxRpm + " rpm");
    }
    const tapSync = t.type === "tap" && (this.tapping === "rigidM29" || this.tapping === "G84.2");
    out.comment(section.name);
    if (section.toolChanged || !this.toolLoaded) {
      if (!this.toolLoaded) {
        this.retract(p.toolChangeRetract, out); // Z is wherever the last program left it
      } else if (p.optionalStop) {
        out.block("M1");
      }
      out.comment("T" + t.number + " " + t.description + (t.holderDescription !== "" ? " in " + t.holderDescription : ""));
      const next = section.nextTool !== null ? "T" + section.nextTool.number : null;
      // T.. M6 [N 3.6.3, 3.7.3; unverified]; the pre-select forms by property.
      out.block("T" + t.number, "M6");
      if (p.toolPreselect === "separateT" && next !== null) out.block(next);
      this.toolLoaded = true;
      this.spindle = null;
      this.h = null;
      this.pos = { x: null, y: null, z: null };
      out.resetModal();
      out.block(out.modal("units", this.units));
      out.block(out.modal("motion", "G0"), out.modal("distance", "G90"), out.modal("wcs", this.wcs),
        out.modal("plane", "G17"), out.axis("X", section.start.x), out.axis("Y", section.start.y));
      this.startSpindle(section, rpm, tapSync, out);
      // G43 Z.. H.. on the first Z approach [N 3.5.11; unverified: whether the control takes the length from the
      // tool data instead, and with which parameter].
      out.block("G43", out.axis("Z", section.start.z), "H" + t.lengthOffset);
      this.h = t.lengthOffset;
    } else {
      if (section.coolant !== this.coolant) this.coolantOff(out);
      out.resetModal("motion", "feed", "axes", "plane", "distance", "wcs");
      if (this.pos.z === null || section.start.z > this.pos.z) {
        out.block(out.modal("motion", "G0"), out.axis("Z", section.start.z));
      }
      out.block(out.modal("motion", "G0"), out.modal("distance", "G90"), out.modal("wcs", this.wcs),
        out.modal("plane", "G17"), out.axis("X", section.start.x), out.axis("Y", section.start.y));
      this.startSpindle(section, rpm, tapSync, out);
      if (this.h !== t.lengthOffset) {
        out.block("G43", out.axis("Z", section.start.z), "H" + t.lengthOffset);
        this.h = t.lengthOffset;
      } else {
        out.block(out.modal("motion", "G0"), out.axis("Z", section.start.z));
      }
    }
    this.pos = { x: section.start.x, y: section.start.y, z: section.start.z };
    if (section.coolant !== this.coolant) this.coolantOn(section.coolant, out);
    this.section = section;
    this.clearanceZ = section.clearanceZ;
    this.sectionClockwise = section.clockwise;
    this.rpm = rpm;
    this.comp = 0;
    this.d = t.diameterOffset;
  },

  startSpindle(section, rpm, tapSync, out) {
    if (tapSync) {
      // Synchronous tapping (M29 or G84.2): the cycle runs the spindle; it is left stopped [unverified].
      if (this.spindle !== null) out.block("M5");
      out.block("S" + rpm);
      this.spindle = null;
      return;
    }
    const dir = section.clockwise ? "M3" : "M4";
    out.block("S" + rpm, dir); // [N 3.6.2, 3.7.2]
    this.spindle = dir;
  },

  coolantOn(mode, out) {
    if (mode === "off") { this.coolant = "off"; return; }
    const code = this.coolantCodes[mode].on;
    if (code === "") {
      out.fail("coolant mode '" + mode + "' has no M code on this machine; set it in the post property coolantCodes " +
        "(e.g. " + mode + "=M51/M9) or choose another coolant for the operation");
    }
    out.block(code);
    this.coolant = mode;
  },

  coolantOff(out) {
    if (this.coolant === "off") return;
    const code = this.coolantCodes[this.coolant].off;
    if (code !== "") out.block(code);
    this.coolant = "off";
  },

  retract(method, out) {
    // Z first: whether G28 (or G53) with Z alone moves only Z is [unverified] (RS274/NGC's G28 goes to a home
    // position that may include X and Y [N 3.5.8]). So when the tool is below the operation's clearance height, it
    // rapids straight up to that height before the reference or machine-zero return.
    if (method !== "none" && this.pos.z !== null && this.clearanceZ !== undefined && this.pos.z < this.clearanceZ) {
      out.block(out.modal("motion", "G0"), out.axis("Z", this.clearanceZ));
      this.pos.z = this.clearanceZ;
    }
    if (method === "G28") {
      out.block("G91", "G28", out.word("Z", 0)); // Z home through the current point [N 3.5.8; unverified]
      out.block("G90");
      out.resetModal("distance", "motion");
      out.modal("distance", "G90");
    } else if (method === "G53") {
      out.block("G0", "G53", out.word("Z", 0)); // machine Z zero [N 3.5.12; unverified]
      out.modal("motion", "G0");
    }
    out.resetModal("Z");
    this.pos.z = null;
  },

  onSectionEnd(section, ctx, out) {
    if (this.comp !== 0) {
      out.fail("section " + section.index + " (" + section.name + ") ends with cutter compensation on; the toolpath " +
        "must cancel it (G40) on a linear move");
    }
    if (this.cycleActive) {
      out.block(out.modal("cycle", "G80"));
      this.cycleActive = false;
    }
    const next = ctx.sections[section.index + 1];
    const last = next === undefined;
    if (!last && !next.toolChanged) return;
    const p = ctx.properties;
    this.retract(last ? p.endRetract : p.toolChangeRetract, out);
    out.block("M5");
    this.spindle = null;
    this.coolantOff(out);
    if (last && p.endPosition !== "zOnly") {
      const axes = p.endPosition === "zThenXY" ? [out.word("X", 0), out.word("Y", 0)] : [out.word("Y", 0)];
      if (p.endRetract === "G28") {
        out.block("G91", "G28", ...axes);
        out.block("G90");
      } else {
        out.block("G0", "G53", ...axes);
      }
    }
  },

  onProgramEnd(ctx, out) {
    if (this.cycleActive) out.block("G80");
    if (this.spindle !== null) out.block("M5");
    this.coolantOff(out);
    if (this.highSpeed !== "off") out.block(this.highSpeed === "G61.1" ? "G64" : "G05 P0");
    out.block("M30"); // [N 3.6.1; unverified]
    out.raw("%");
  },

  // ---------- motion ----------
  needSpindle(what, out) {
    if (this.spindle === null) {
      out.fail("a " + what + " in section " + this.section.index + " (" + this.section.name + ") with the spindle " +
        "stopped");
    }
  },

  onRapid(m, out) {
    if (this.comp !== 0) {
      out.fail("a rapid move with cutter compensation on in section " + this.section.index + " (" + this.section.name +
        "); the toolpath must cancel compensation (G40) on a linear move first");
    }
    const words = [out.axis("X", m.x), out.axis("Y", m.y), out.axis("Z", m.z)];
    this.pos = { x: m.x, y: m.y, z: m.z };
    if (words.every((w) => w === null)) return;
    out.block(out.modal("motion", "G0"), ...words);
  },

  onLinear(m, out) {
    this.needSpindle("feed move", out);
    let compWord = null;
    let x = out.axis("X", m.x);
    let y = out.axis("Y", m.y);
    if (m.comp !== this.comp) {
      // [N 3.5.10, Appendix B; unverified]: on and off on a straight X/Y move in the XY plane.
      if (this.comp !== 0 && m.comp !== 0) {
        out.fail("cutter compensation changes from " + ["", "G41", "G42"][this.comp] + " to " + ["", "G41", "G42"][m.comp] +
          " without a G40 in section " + this.section.index + " (" + this.section.name + ")");
      }
      if (near(m.x, this.pos.x) && near(m.y, this.pos.y)) {
        out.fail("cutter compensation " + (m.comp === 0 ? "G40" : ["", "G41", "G42"][m.comp]) + " on a move without X or " +
          "Y motion in section " + this.section.index + " (" + this.section.name + "); it needs an X/Y move");
      }
      compWord = ["G40", "G41", "G42"][m.comp];
      x = x || out.word("X", m.x);
      y = y || out.word("Y", m.y);
      this.comp = m.comp;
    }
    const z = out.axis("Z", m.z);
    this.pos = { x: m.x, y: m.y, z: m.z };
    if (compWord === null && x === null && y === null && z === null) return;
    out.block(compWord !== null ? out.modal("plane", "G17") : null, compWord, out.modal("motion", "G1"), x, y, z,
      compWord !== null && m.comp !== 0 ? "D" + this.d : null, out.feed(m.feed));
  },

  onArc(m, out) {
    this.needSpindle("feed move (arc)", out);
    if (m.comp !== this.comp) {
      out.fail("cutter compensation starts or ends on an arc in section " + this.section.index + " (" +
        this.section.name + "); it may only start or end on a linear move");
    }
    if (this.comp !== 0 && m.plane !== "XY") {
      out.fail("an arc in the " + m.plane + " plane with cutter compensation on in section " + this.section.index +
        "; compensation is posted in the XY plane only");
    }
    const plane = out.modal("plane", { XY: "G17", XZ: "G18", YZ: "G19" }[m.plane]);
    const start = { x: this.pos.x, y: this.pos.y, z: this.pos.z };
    if (this.arcFormat === "R") {
      const n = Math.max(1, Math.ceil(m.sweepDegrees / 90 - 1e-9));
      let from = start;
      for (const piece of arcPieces(m, start, n)) {
        out.block(plane, out.modal("motion", m.cw ? "G2" : "G3"), ...this.arcAxes(m.plane, piece, out),
          out.word("R", m.radius), out.feed(m.feed));
        from = piece;
      }
      this.pos = from;
      return;
    }
    // Centre format [N 3.5.3.2; unverified]: both offsets of the plane, both end-point words of the plane.
    const centre = { XY: [out.word("I", m.i), out.word("J", m.j)], XZ: [out.word("I", m.i), out.word("K", m.k)],
      YZ: [out.word("J", m.j), out.word("K", m.k)] }[m.plane];
    out.block(plane, out.modal("motion", m.cw ? "G2" : "G3"), ...this.arcAxes(m.plane, m, out), ...centre,
      out.feed(m.feed));
    this.pos = { x: m.x, y: m.y, z: m.z };
  },

  arcAxes(plane, p, out) {
    const inPlane = { XY: ["X", "Y"], XZ: ["X", "Z"], YZ: ["Y", "Z"] }[plane];
    const words = [];
    for (const letter of ["X", "Y", "Z"]) {
      const v = p[letter.toLowerCase()];
      const w = out.axis(letter, v);
      words.push(w !== null ? w : inPlane.includes(letter) ? out.word(letter, v) : null);
    }
    return words;
  },

  // P in the unit of dwellUnits; every program with a dwell is warned about, the unit being [unverified].
  dwellWord(seconds, out) {
    this.warnOnce(out, "dwell posted as P in " + this.dwellUnits + " (property dwellUnits): the dwell unit is " +
      "[unverified] for the " + CONTROL + " control; check it before running the program");
    if (this.dwellUnits === "seconds") return out.word("P", seconds, { decimals: 3 });
    return out.word("P", Math.round(seconds * 1000), { decimals: 0, forceDecimalPoint: false });
  },

  // ---------- canned cycles ----------
  onCycle(c, out) {
    const name = "section " + this.section.index + " (" + this.section.name + ")";
    let code;
    let peckWord = null;
    const dwell = c.dwellSeconds > 0;
    switch (c.kind) {
      case "drill": code = dwell ? "G82" : "G81"; break;
      case "peck": code = c.peck > 0 ? "G83" : (dwell ? "G82" : "G81"); break;
      case "chipBreak": code = c.peck > 0 ? "G73" : (dwell ? "G82" : "G81"); break;
      case "tap":
        code = this.tapping === "G84.2" ? (c.leftHand ? "G84.3" : "G84.2") : (c.leftHand ? "G74" : "G84");
        break;
      case "bore":
      case "ream": code = dwell ? "G89" : "G85"; break;
    }
    if (code === "G83" || code === "G73") {
      peckWord = out.word("Q", c.peck);
      if (c.peckClearance > 0) {
        this.warnOnce(out, name + ": the peck clearance " + out.word("", c.peckClearance) + " is not posted; the " +
          "control takes the " + code + " return amount from a parameter [unverified]");
      }
    }
    let dwellWord = null;
    if (dwell) {
      if (code === "G82" || code === "G89" || code === "G84" || code === "G74") {
        dwellWord = this.dwellWord(c.dwellSeconds, out);
      } else {
        this.warnOnce(out, name + ": " + code + " is posted without a dwell word (whether it takes P is [unverified]); " +
          "the " + c.dwellSeconds + " s dwell at the bottom is not posted");
      }
    }
    if (c.kind === "tap") {
      this.checkTap(c, name, out);
    } else {
      this.needSpindle("drilling cycle", out);
    }
    const words = { Z: out.word("Z", c.bottom), R: out.word("R", c.retract), Q: peckWord, P: dwellWord };
    // Feed-out cycles never with G98 (they may feed all the way up to the initial level [N 3.5.16.5-.10]): G99, then
    // G80 and a rapid up to the clearance after the hole.
    const feedOut = c.kind === "tap" || code === "G85" || code === "G89";
    const upAfter = feedOut && c.returnLevel === "clearance";
    const retWord = c.returnLevel === "clearance" && !feedOut ? "G98" : "G99";
    // G98 is not relied on after a lower hole of the same run: end the run, go up, restart [N 3.5.20; unverified].
    const lowG98 = this.cycleActive && retWord === "G98" && !near(this.pos.z, c.clearance);
    if (this.cycleActive && (lowG98 || (c.first && !sameCycleShape(this.cycle, code, words)))) {
      out.block(out.modal("cycle", "G80"));
      out.resetModal("motion", "Z");
      if (!near(this.pos.z, c.clearance)) out.block(out.modal("motion", "G0"), out.axis("Z", c.clearance));
      this.cycleActive = false;
    }
    const feed = () => (c.kind === "tap" ? out.feed(c.feed, this.inch ? 4 : 3) : out.feed(c.feed));
    if (!this.cycleActive) {
      out.resetModal("cycle", "return", "feed");
      if (c.kind === "tap" && this.tapping === "rigidM29") out.block("M29", "S" + this.rpm);
      out.block(out.modal("plane", "G17"), out.modal("return", retWord), out.modal("cycle", code),
        out.axis("X", c.x) || out.word("X", c.x), out.axis("Y", c.y) || out.word("Y", c.y), words.Z, words.R, words.Q,
        words.P, feed());
      this.cycleActive = true;
    } else {
      // Later holes: X/Y, Z and R when they change (sticky [N 3.5.16]), Q and P on every block (not assumed modal).
      let x = out.axis("X", c.x);
      let y = out.axis("Y", c.y);
      if (x === null && y === null) { x = out.word("X", c.x); y = out.word("Y", c.y); }
      out.block(out.modal("return", retWord), x, y, ...changedCycleWords(this.cycle, { Z: words.Z, R: words.R }),
        words.Q, words.P, feed());
    }
    if (upAfter) {
      out.block(out.modal("cycle", "G80"));
      out.resetModal("motion", "Z", "feed", "return");
      out.block(out.modal("motion", "G0"), out.axis("Z", c.clearance));
      this.cycleActive = false;
    }
    this.cycle = { code: code, words: words };
    this.pos = { x: c.x, y: c.y, z: c.returnLevel === "clearance" ? c.clearance : c.retract };
  },

  checkTap(c, name, out) {
    const sync = this.tapping === "rigidM29" || this.tapping === "G84.2";
    const wanted = c.leftHand ? "M4" : "M3";
    if (!sync && this.spindle !== wanted) {
      out.fail(name + ": " + (c.leftHand ? "G74" : "G84") + " needs the spindle turning " +
        (c.leftHand ? "counter-clockwise (M4)" : "clockwise (M3)") + "; set the operation's spindle direction");
    }
    if (sync && this.sectionClockwise === c.leftHand) {
      out.fail(name + ": a " + (c.leftHand ? "left" : "right") + "-hand tap in an operation whose spindle turns " +
        (this.sectionClockwise ? "clockwise" : "counter-clockwise"));
    }
    if (this.autoPlain) {
      this.warnOnce(out, name + ": the machine definition does not say rigid tapping, so G84/G74 is posted without " +
        "M29 (floating tapping, or synchronous by parameter); set the post property tapping to choose [unverified]");
    }
    if (c.pitch <= 0) out.fail(name + ": a tapping cycle without a thread pitch");
    const expected = c.pitch * this.rpm;
    if (Math.abs(c.feed - expected) > 1e-3 * expected) {
      out.fail(name + ": tapping feed " + out.word("", c.feed, { decimals: 4 }) + " is not pitch x rpm = " +
        out.word("", expected, { decimals: 4 }) + "; a mismatched tap feed breaks the tap");
    }
  },

  // A warning once per program, not once per hole.
  warnOnce(out, message) {
    if (this.warned === undefined) this.warned = new Set();
    if (this.warned.has(message)) return;
    this.warned.add(message);
    out.warn(message);
  },

  onCycleEnd(out) {
    out.block(out.modal("cycle", "G80"));
    this.cycleActive = false;
    out.resetModal("motion", "Z", "feed", "return");
  },

  onDwell(seconds, out) {
    out.block("G4", this.dwellWord(seconds, out)); // [N 3.5.4; unit unverified]
  },
});
