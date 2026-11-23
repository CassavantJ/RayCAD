// RayCAD post processor: generic ISO / RS-274 milling (3-axis vertical machining centre).
// Written from scratch for RayCAD's own post API (include/ray/post_api.d.ts). It is not derived from any other CAM
// system's post processor.
//
// For a control that follows the common ISO 6983 / RS-274 word meanings and has no post of its own. It writes only
// words whose meaning the public RS274/NGC description fixes, in their most portable form; anything a control may
// treat differently is a property or is left to the host (chip-break drilling is expanded into plain moves, there is
// no left-hand tapping cycle). Prove every program on the machine before cutting metal.
//
// SOURCES
//  [N]   T. Kramer, F. Proctor, E. Messina, "The NIST RS274NGC Interpreter - Version 3", NISTIR 6556, NIST, 2000
//        (public domain; read for this post). Sections are cited as [N 3.5.3] and so on.
//  [ISO] ISO 6983-1:2009 "Automation systems and integration - Numerical control of machines - Program format and
//        definitions of address words - Part 1: Data format for positioning, line motion and contouring control
//        systems". Not read (sold by ISO): cited by title only; nothing here depends on it alone.
//
// SAFETY RULES THIS POST ENFORCES (it fails with a message naming the cause rather than post something doubtful)
//  - Safe start: units alone [N 3.5.7: "program G20 or G21 near the beginning of a program before any motion"], then
//    G17 G40 G49 G80 G90 G94 (plane, cutter radius compensation off, no length offset, no cycle motion, absolute,
//    feed per minute) [N 3.5.6, 3.5.10, 3.5.11, 3.5.15, 3.5.17, 3.5.19].
//  - Before every tool change Z goes to machine zero (G0 G53 Z0. [N 3.5.12]) unless the property says otherwise:
//    RS274/NGC's M6 makes no retract of its own [N 3.6.3].
//  - After every tool change the work offset, absolute mode, plane and units are restated with the first XY move, the
//    spindle is started before the length offset is applied on the first Z approach (G43 Z.. H.. [N 3.5.11]).
//  - Cutter radius compensation only in the XY plane [N 3.5.10], started and ended on linear moves with X/Y motion
//    (Appendix B); never with a rapid while it is on.
//  - G80 before any non-cycle move, before tool changes and at the end.
//  - No feed move with the spindle stopped; spindle speed checked against the machine's maximum.
//  - Program end: Z to machine zero first, spindle and coolant off, optional XY return, M30 [N 3.6.1].
//
// G AND M WORDS THIS POST CAN WRITE (for the independent G-code reader, T-043)
//  G0 G1 G2 G3 G4 G17 G18 G19 G20 G21 G28 G40 G41 G42 G43 G49 G53 G54..G59 G59.1 G59.2 G59.3 G80 G81 G82 G83 G84 G85
//  G89 G90 G91 G94 G98 G99; M1 M3 M4 M5 M6 M30 and the coolant codes of property coolantCodes (default M7 M8 M9).
//  Other words: N O T S F H D X Y Z I J K R Q P, comments in ( ), "%" at start and end.

"use strict";

// The characters every dialect accepts in a comment, as the host sanitises them (src/post/writer.h); for the program
// name on the O line, which is written with out.raw.
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

// Coolant codes from the property text "flood=M8/M9 mist=M7/M9 through=/ air=/": mode -> { on, off } ("" = none).
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

// Whether a hole can continue the active canned cycle: the same G code and the same words present (their values may
// change from hole to hole).
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

// Points of an arc split into n equal pieces, for R-format output. In-plane axes (u, v) and normal (w) as the host
// gives them: XY (x, y | z), XZ (z, x | y), YZ (y, z | x); cw is G2 seen from the positive normal [N 3.5.3].
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
  title: "Generic ISO / RS-274 milling",
  vendor: "RayCAD",
  dialect: "generic-iso",
  fileExtension: ".nc",
  apiVersion: 1,
  // The real controls a program from this post has run on (post_api.d.ts). None yet: see docs/post_review/generic-iso.md.
  verifiedOn: [],
  capabilities: {
    arcs: "IJK",
    helical: true,
    // RS274/NGC takes a full circle in centre format [N 3.5.3.2], but controls differ on full circles and on arcs
    // of more than half a turn; this generic post has the host split every arc into pieces of at most 180 degrees.
    maxArcDegrees: 180,
    // No chip-break cycle (G73 is not an RS274/NGC code [N 3.4 table 4]): the host expands it into plain moves.
    cannedCycles: ["drill", "peck", "tap", "bore", "ream"],
    rigidTap: true,
    cutterComp: true,
    arcPlanes: ["XY", "XZ", "YZ"],
    helicalPlanes: ["XY", "XZ", "YZ"], // [N 3.5.3]: the helix axis may be parallel to X, Y or Z
  },
  properties: {
    sequenceNumbers: { type: "enum", title: "Sequence numbers", default: "off", values: ["off", "every", "toolChange"],
      group: "Format", description: "N numbers on every block, only on the first block of each tool, or none." },
    sequenceStart: { type: "int", title: "Sequence start", default: 10, min: 1, max: 99999, group: "Format" },
    sequenceIncrement: { type: "int", title: "Sequence increment", default: 10, min: 1, max: 1000, group: "Format" },
    decimalStyle: { type: "enum", title: "Decimal point style", default: "trim", values: ["trim", "one", "all"],
      group: "Format", description: "X12. (trim), X12.0 (one) or X12.000 (all); a decimal point is always written." },
    commentMaxLength: { type: "int", title: "Longest comment", default: 0, min: 0, max: 1000, group: "Format",
      description: "Comments are cut to this many characters; 0 = no limit." },
    programNumberLine: { type: "enum", title: "Program number line", default: "O", values: ["O", "none"],
      group: "Program", description: "O and four digits with the program name in a comment, or no O line " +
        "(RS274/NGC itself has no O word [N 3.3.2 table 3])." },
    toolList: { type: "bool", title: "Tool list in header", default: true, group: "Program" },
    optionalStop: { type: "bool", title: "Optional stop before tool changes", default: false, group: "Program",
      description: "M1 before every tool change after the first [N 3.6.1]." },
    toolPreselect: { type: "bool", title: "Pre-select next tool", default: true, group: "Tool change",
      description: "The next tool's T word right after M6 [N 3.7.3]." },
    toolChangeRetract: { type: "enum", title: "Retract before tool change", default: "G53", values: ["G53", "G28", "none"],
      group: "Tool change", description: "G0 G53 Z0. (machine zero, Z only [N 3.5.12]), G91 G28 Z0. then G90 (home " +
        "position by way of the current point [N 3.5.8]; on some controls G28 homes every axis), or nothing." },
    m6LeavesZRetracted: { type: "bool", title: "M6 leaves Z retracted (allows retract none)", default: false,
      group: "Tool change", description: "Set this only if the machine's tool change always ends with Z at the "
        + "tool-change height. With toolChangeRetract none the post moves X/Y right after M6 at whatever height M6 "
        + "left the spindle; without this assurance it refuses none." },
    endRetract: { type: "enum", title: "Retract at program end", default: "G53", values: ["G53", "G28"],
      group: "Program end" },
    endPosition: { type: "enum", title: "After the Z retract", default: "zOnly", values: ["zOnly", "zThenY", "zThenXY"],
      group: "Program end", description: "Also send Y, or X and Y, to machine zero after Z." },
    coolantCodes: { type: "string", title: "Coolant codes", default: "flood=M8/M9 mist=M7/M9 through=/ air=/",
      group: "Coolant", description: "On/off M codes per coolant mode. M7 mist, M8 flood, M9 off [N 3.6.4]; " +
        "through-spindle coolant and air have no standard code: set them, e.g. through=M88/M89." },
    dwellUnits: { type: "enum", title: "Dwell P units", default: "seconds", values: ["seconds", "milliseconds"],
      group: "Cycles", description: "P of G4, G82 and G89 in seconds [N 3.5.4, 3.5.16.3] or integer milliseconds." },
    arcFormat: { type: "enum", title: "Arc centre format", default: "IJK", values: ["IJK", "R"], group: "Motion",
      description: "IJK (centre offsets) or R (radius; arcs split at 90 degrees, as radius arcs near 180 degrees are " +
        "ill-conditioned [N 3.5.3.1])." },
  },

  onProgramStart(ctx, out) {
    const p = ctx.properties;
    this.inch = ctx.units === "inch";
    this.units = this.inch ? "G20" : "G21";
    this.coolantCodes = parseCoolantCodes(p.coolantCodes, out);
    this.arcFormat = p.arcFormat;
    this.dwellUnits = p.dwellUnits;
    this.wcs = this.wcsCode(ctx.workOffset, out);
    // toolChangeRetract none is accepted only when the user asserts that M6 leaves Z retracted: otherwise the first X/Y
    // move after a tool change would run at an unknown height (RS274/NGC's M6 leaves the axes where they are [N 3.6.3]).
    if (p.toolChangeRetract === "none" && !p.m6LeavesZRetracted) {
      out.fail("post property toolChangeRetract is none, but nothing says the machine's M6 leaves Z retracted: the "
        + "first X/Y move after a tool change would run at an unknown height. Choose a retract, or set the post "
        + "property m6LeavesZRetracted if the machine's tool change always ends with Z up");
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
    if (out.sequence.max === 0) out.sequence.max = 99999; // at most five digits [N 3.3.1]

    out.raw("%");
    if (p.programNumberLine === "O") {
      const number = ctx.programNumber;
      if (!(Number.isInteger(number) && number >= 1 && number <= 9999)) {
        out.fail("program number " + number + " is outside O1 to O9999; set the setup's program number, or the " +
          "property programNumberLine to none");
      }
      const name = "O" + String(number).padStart(4, "0");
      if (ctx.machine.maxProgramNameLength > 0 && name.length > ctx.machine.maxProgramNameLength) {
        out.fail("program name " + name + " is longer than the machine's limit of " + ctx.machine.maxProgramNameLength +
          " characters (machine definition maxProgramNameLength)");
      }
      let title = cleanText(ctx.programComment !== "" ? ctx.programComment : ctx.documentName);
      if (p.commentMaxLength > 0) title = title.substring(0, p.commentMaxLength).trim();
      out.raw(title === "" ? name : name + " (" + title + ")");
    } else {
      out.comment(ctx.programComment !== "" ? ctx.programComment : ctx.documentName);
    }
    out.comment("Document " + ctx.documentName + " setup " + ctx.setupName);
    if (ctx.date !== null) out.comment("Posted " + ctx.date);
    out.comment("Machine " + ctx.machine.name);
    if (p.toolList) {
      for (const t of ctx.tools) {
        out.comment("T" + t.number + " D" + out.word("", t.diameter) + " " + t.type + " " + t.description);
      }
    }
    out.block(out.modal("units", this.units));
    out.block(out.modal("plane", "G17"), "G40", "G49", "G80", out.modal("distance", "G90"), "G94");
  },

  wcsCode(offset, out) {
    // G54..G59 and G59.1..G59.3 for coordinate systems 1..9 [N 3.5.13].
    const codes = ["G54", "G55", "G56", "G57", "G58", "G59", "G59.1", "G59.2", "G59.3"];
    if (offset >= 1 && offset <= 9) return codes[offset - 1];
    out.fail("work offset " + offset + " has no RS274 code (1-9 are G54-G59.3)");
  },

  onSectionStart(section, ctx, out) {
    const p = ctx.properties;
    const t = section.tool;
    if (!(section.rpm > 0)) out.fail("section " + section.index + " (" + section.name + ") has no spindle speed");
    const rpm = Math.round(section.rpm);
    if (this.maxRpm > 0 && rpm > this.maxRpm) {
      out.fail("section " + section.index + " (" + section.name + "): " + rpm + " rpm is above the machine's " +
        this.maxRpm + " rpm");
    }
    out.comment(section.name);
    if (section.toolChanged || !this.toolLoaded) {
      if (!this.toolLoaded) {
        this.retract(p.toolChangeRetract, out);
      } else if (p.optionalStop) {
        out.block("M1");
      }
      out.comment("T" + t.number + " " + t.description + (t.holderDescription !== "" ? " in " + t.holderDescription : ""));
      // T and M6 [N 3.6.3, 3.7.3]; the next tool's T word right after, for a changer that turns while cutting.
      out.block("T" + t.number, "M6");
      if (p.toolPreselect && section.nextTool !== null) out.block("T" + section.nextTool.number);
      this.toolLoaded = true;
      this.spindle = null;
      this.h = null;
      this.pos = { x: null, y: null, z: null };
      out.resetModal();
      out.block(out.modal("units", this.units));
      out.block(out.modal("motion", "G0"), out.modal("distance", "G90"), out.modal("wcs", this.wcs),
        out.modal("plane", "G17"), out.axis("X", section.start.x), out.axis("Y", section.start.y));
      this.startSpindle(section, rpm, out);
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
      this.startSpindle(section, rpm, out);
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

  startSpindle(section, rpm, out) {
    const dir = section.clockwise ? "M3" : "M4";
    out.block("S" + rpm, dir); // [N 3.6.2, 3.7.2]
    this.spindle = dir;
  },

  coolantOn(mode, out) {
    if (mode === "off") { this.coolant = "off"; return; }
    const code = this.coolantCodes[mode].on;
    if (code === "") {
      out.fail("coolant mode '" + mode + "' has no M code on this machine; set it in the post property coolantCodes " +
        "(e.g. " + mode + "=M88/M89) or choose another coolant for the operation");
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
    // Z first for G28: RS274/NGC's G28 goes to "the home position" by way of the programmed point [N 3.5.8], which
    // may include X and Y; so when the tool is below the operation's clearance height it rapids straight up to that
    // height first. G53 Z0. moves only Z [N 3.5.12: only the axes programmed], so it needs no such move.
    if (method === "G28" && this.pos.z !== null && this.clearanceZ !== undefined && this.pos.z < this.clearanceZ) {
      out.block(out.modal("motion", "G0"), out.axis("Z", this.clearanceZ));
      this.pos.z = this.clearanceZ;
    }
    if (method === "G53") {
      out.block("G0", "G53", out.word("Z", 0));
      out.modal("motion", "G0");
    } else if (method === "G28") {
      out.block("G91", "G28", out.word("Z", 0));
      out.block("G90");
      out.resetModal("distance", "motion");
      out.modal("distance", "G90");
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
    out.block("M30"); // [N 3.6.1]
    out.raw("%");
  },

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
      // [N 3.5.10, Appendix B]: compensation in the XY plane; on and off on a straight move with X/Y motion.
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
        "; compensation works in the XY plane only [N 3.5.10]");
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
    // Centre format [N 3.5.3.2]: both offsets of the plane, and both end-point words of the plane.
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

  dwellWord(seconds, out) {
    if (this.dwellUnits === "milliseconds") {
      return out.word("P", Math.round(seconds * 1000), { decimals: 0, forceDecimalPoint: false });
    }
    return out.word("P", seconds, { decimals: 3 });
  },

  onCycle(c, out) {
    const name = "section " + this.section.index + " (" + this.section.name + ")";
    let code;
    let peckWord = null;
    let dwellWord = null;
    switch (c.kind) {
      case "drill": code = c.dwellSeconds > 0 ? "G82" : "G81"; break; // [N 3.5.16.2, 3.5.16.3]
      case "peck": code = c.peck > 0 ? "G83" : (c.dwellSeconds > 0 ? "G82" : "G81"); break; // [N 3.5.16.4]
      case "tap": code = "G84"; break; // [N 3.5.16.5]
      case "bore":
      case "ream": code = c.dwellSeconds > 0 ? "G89" : "G85"; break; // [N 3.5.16.6, 3.5.16.10]
      default: out.fail(name + ": a " + c.kind + " cycle, which this post does not write");
    }
    if (code === "G83") peckWord = out.word("Q", c.peck);
    if (c.dwellSeconds > 0) {
      if (code === "G82" || code === "G89") {
        dwellWord = this.dwellWord(c.dwellSeconds, out);
      } else if (c.first) {
        this.warnOnce(out, name + ": RS274 " + code + " has no dwell word; the " + c.dwellSeconds + " s dwell at the bottom is " +
          "not posted");
      }
    }
    if (code === "G83" && c.peckClearance > 0 && c.first) {
      this.warnOnce(out, name + ": the peck clearance " + out.word("", c.peckClearance) + " is not posted; G83 backs off by the " +
        "control's own amount [N 3.5.16.4]");
    }
    if (c.kind === "tap") {
      if (c.leftHand) {
        out.fail(name + ": a left-hand tap; RS274 has no left-hand tapping cycle (only G84 [N 3.5.16.5])");
      }
      if (this.spindle !== "M3") {
        out.fail(name + ": G84 needs the spindle turning clockwise (M3) before the cycle [N 3.5.16.5]");
      }
      if (c.pitch <= 0) out.fail(name + ": a tapping cycle without a thread pitch");
      const expected = c.pitch * this.rpm;
      if (Math.abs(c.feed - expected) > 1e-3 * expected) {
        out.fail(name + ": tapping feed " + out.word("", c.feed, { decimals: 4 }) + " is not pitch x rpm = " +
          out.word("", expected, { decimals: 4 }) + "; a mismatched tap feed breaks the tap");
      }
    } else {
      this.needSpindle("drilling cycle", out);
    }
    const words = { Z: out.word("Z", c.bottom), R: out.word("R", c.retract), Q: peckWord, P: dwellWord };
    // A G98 hole after G99 holes of the same run: RS274/NGC's G98 returns to the Z "just before the canned cycle
    // started" [N 3.5.20], which one reading (the example of N 3.5.16.2) takes per block: that would be the previous
    // hole's R plane, not the clearance the toolpath asks for. So the run is ended, Z goes up to the clearance, and
    // the cycle starts again there; both readings then return to the clearance.
    // G84, G85 and G89 feed all the way out to the clear level [N 3.5.16.5, 3.5.16.6, 3.5.16.10], so with G98 they
    // would feed up to the clearance where the toolpath feeds out to R and rapids on. These holes are posted with G99
    // and followed by G80 and a rapid up to the clearance (the toolpath's motion exactly), and no G98 is written.
    const feedOut = code === "G84" || code === "G85" || code === "G89";
    const upAfter = feedOut && c.returnLevel === "clearance";
    const retWord = c.returnLevel === "clearance" && !feedOut ? "G98" : "G99";
    const lowG98 = this.cycleActive && retWord === "G98" && !near(this.pos.z, c.clearance);
    if (this.cycleActive && (lowG98 || (c.first && !sameCycleShape(this.cycle, code, words)))) {
      // Another cycle code inside a run, or the G98 case above: G80, back up to the initial level, then the cycle.
      out.block(out.modal("cycle", "G80"));
      out.resetModal("motion", "Z");
      if (!near(this.pos.z, c.clearance)) out.block(out.modal("motion", "G0"), out.axis("Z", c.clearance));
      this.cycleActive = false;
    }
    const feed = () => (c.kind === "tap" ? out.feed(c.feed, this.inch ? 4 : 3) : out.feed(c.feed));
    if (!this.cycleActive) {
      out.resetModal("cycle", "return", "feed");
      // G98 back to the initial level, G99 to the R plane, per hole [N 3.5.20]; X and Y always.
      out.block(out.modal("plane", "G17"), out.modal("return", retWord), out.modal("cycle", code),
        out.axis("X", c.x) || out.word("X", c.x), out.axis("Y", c.y) || out.word("Y", c.y), words.Z, words.R, words.Q,
        words.P, feed());
      this.cycleActive = true;
    } else {
      // Later holes: the position, Z and R when they change (only R and Z are sticky [N 3.5.16]), and Q and P on
      // every block (required items of G83, G82 and G89 [N 3.5, 3.5.16.3-4, 3.5.16.10]).
      let x = out.axis("X", c.x);
      let y = out.axis("Y", c.y);
      if (x === null && y === null) { x = out.word("X", c.x); y = out.word("Y", c.y); }
      const changed = changedCycleWords(this.cycle, { Z: words.Z, R: words.R });
      out.block(out.modal("return", retWord), x, y, ...changed, words.Q, words.P, feed());
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

  // A cycle warning once per program, not once per hole.
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
    out.block("G4", this.dwellWord(seconds, out)); // [N 3.5.4]
  },
});
