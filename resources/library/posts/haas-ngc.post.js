// RayCAD post processor: Haas vertical mills with the Next Generation Control (NGC).
// Written from scratch for RayCAD's own post API (include/ray/post_api.d.ts). It is not derived from any other CAM
// system's post processor.
//
// SOURCES AND HOW FAR THEY WERE CHECKED
//  [H1] Haas Automation, "Mill Series Programming Workbook", 2015, haascnc.com (read for this post; cited by its
//       printed page numbers, e.g. [H1 p.42]). It was written for the controls of its time; the NGC keeps the same
//       G and M code meanings for everything this post writes, which the author takes from Haas's statement that
//       NGC runs programs of the earlier control [unverified on a machine].
//  [H2] Haas Mill Operator's Manual, G-codes chapter (online at haascnc.com, "codes-settings" pages). The pages
//       refused automated reading, so the G187 rules below come from what search results quote from them and from
//       the Haas Automation UK tips "Smooth Operator" and "Varying Accuracy" (haas.co.uk, read): G187 P1/P2/P3 =
//       rough/medium/finish smoothness, E = maximum corner rounding (overrides Setting 85), G187 alone returns to
//       Setting 191's default, M30 or reset cancels the E value. Treat G187 as [H2, partly verified].
//  [N]  T. Kramer et al., "The NIST RS274NGC Interpreter - Version 3", NISTIR 6556, NIST, 2000, for the common
//       meaning of words Haas shares with RS-274.
//
// SAFETY RULES THIS POST ENFORCES (it fails with a message naming the cause rather than post something doubtful)
//  - Safe start: G20/G21 alone (on a Haas it only checks Setting 9; a mismatch alarms [H1 p.44]), then G17 G40 G49 G80
//    G90 G94 (the power-up defaults restated [H1 p.18]).
//  - Tool change "T.. M06" [H1 p.12]; M06 itself stops the spindle and coolant and takes Z to machine zero [H1 p.19];
//    the post also retracts with G53 G49 Z0. first [H1 p.43] unless the property says otherwise.
//  - After a tool change: "G00 G90 G54 X.. Y.." (plus G17), "S.. M03", then "G43 H.. Z.." from the tool-change
//    height, then coolant [H1 p.12, p.42]. Modal words are written again.
//  - Only one M code per block [H1 p.11, p.19].
//  - Cutter compensation: G41/G42 with D on a linear X/Y move, never on an arc or a Z-only move, cancelled with G40
//    on a linear X/Y move, XY plane only (G18/G19 compensation is G141, not written) [H1 p.67].
//  - Canned cycles: always X and Y on the cycle's first block (Setting 28 changes what a block without them does
//    [H1 p.73]); G80 before other moves, tool changes and the end [H1 p.73].
//  - Dwell P always written with a fraction in seconds ("P0.300"): a P without a fraction is milliseconds on a Haas
//    [H1 p.48].
//  - No feed move with the spindle stopped; spindle speed checked against the machine's maximum.
//  - Program end: Z to machine zero first (G53 G49 Z0.), spindle and coolant off, Y forward (G53 Y0.), M30 [H1 p.43].
//
// G AND M WORDS THIS POST CAN WRITE (for the independent G-code reader, T-043)
//  G0 G1 G2 G3 G4 G17 G18 G19 G20 G21 G28 G40 G41 G42 G43 G49 G53 G54..G59 G110..G129 G154 G73 G74 G80 G81 G82 G83
//  G84 G85 G89 G90 G91 G94 G98 G99 G187; M1 M3 M4 M5 M6 M30 and the coolant codes of property coolantCodes (default
//  M8 M9 M88 M89 M83 M84). Other words: N O T S F H D X Y Z I J K R Q P E, comments in ( ), "%" at start and end.

"use strict";

// The program name on the O line, cleaned like the host's comments (src/post/writer.h): Haas keeps lower case.
function cleanText(text) {
  let s = "";
  let space = false;
  for (const ch of String(text)) {
    if (!/[A-Za-z0-9.,\-+/*=:#_]/.test(ch)) { space = s.length > 0; continue; }
    if (space) { s += " "; space = false; }
    s += ch;
  }
  return s;
}

function near(a, b) { return Math.abs(a - b) <= 1e-9 * Math.max(1, Math.abs(a), Math.abs(b)); }

function parseCoolantCodes(text, out) {
  const codes = { flood: null, mist: null, through: null, air: null };
  for (const item of String(text).trim().split(/\s+/)) {
    if (item === "") continue;
    const m = /^(flood|mist|through|air)=(M\d{1,3})?\/(M\d{1,3})?$/.exec(item);
    if (!m) {
      out.fail("post property coolantCodes: \"" + item + "\" is not of the form mode=Mon/Moff with mode flood, mist, " +
        "through or air (an empty code is allowed, e.g. mist=/)");
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

// Points of an arc split into n equal pieces, for R-format output (u, v in the plane, w the normal, as the host
// gives them: XY (x, y | z), XZ (z, x | y), YZ (y, z | x)).
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

const CONTROL = "Haas NGC";

definePost({
  title: "Haas mill, Next Generation Control (NGC)",
  vendor: "RayCAD",
  dialect: "haas-ngc",
  fileExtension: ".nc",
  apiVersion: 1,
  capabilities: {
    arcs: "IJK",
    helical: true,
    // A full circle needs I/J; R cannot do 360 degrees [H1 p.33, p.70].
    maxArcDegrees: 360,
    cannedCycles: ["drill", "peck", "chipBreak", "tap", "bore", "ream"],
    rigidTap: true,
    cutterComp: true,
    arcPlanes: ["XY", "XZ", "YZ"],   // G17, G18, G19 [H1 p.33]
    helicalPlanes: ["XY"],           // helical milling as documented: G02/G03 in XY with Z [H1 p.70]
  },
  properties: {
    sequenceNumbers: { type: "enum", title: "Sequence numbers", default: "off", values: ["off", "every", "toolChange"],
      group: "Format", description: "N numbers (N1-N99999 [H1 p.11]) on every block, on each tool's first block, or none." },
    sequenceStart: { type: "int", title: "Sequence start", default: 10, min: 1, max: 99999, group: "Format" },
    sequenceIncrement: { type: "int", title: "Sequence increment", default: 10, min: 1, max: 1000, group: "Format" },
    decimalStyle: { type: "enum", title: "Decimal point style", default: "trim", values: ["trim", "one", "all"],
      group: "Format", description: "X12. (trim), X12.0 (one) or X12.000 (all). A decimal point is always written: " +
        "without one, X12 means 12 ten-thousandths of an inch or 12 thousandths of a mm [H1 p.28]." },
    commentMaxLength: { type: "int", title: "Longest comment", default: 0, min: 0, max: 1000, group: "Format",
      description: "Comments are cut to this many characters; 0 = no limit." },
    toolList: { type: "bool", title: "Tool list in header", default: true, group: "Program" },
    optionalStop: { type: "bool", title: "Optional stop before tool changes", default: false, group: "Program",
      description: "M01 before every tool change after the first [H1 p.19]." },
    toolPreselect: { type: "bool", title: "Pre-select next tool", default: true, group: "Tool change",
      description: "The next tool's T word right after M06 (T may come in any block before its M06 [H1 p.27])." },
    toolChangeRetract: { type: "enum", title: "Retract before tool change", default: "G53", values: ["G53", "G28", "none"],
      group: "Tool change", description: "G00 G53 G49 Z0. [H1 p.43], G28 G91 Z0. then G90 [H1 p.43], or nothing (M06 " +
        "takes Z to machine zero itself [H1 p.19])." },
    endRetract: { type: "enum", title: "Retract at program end", default: "G53", values: ["G53", "G28"],
      group: "Program end" },
    endPosition: { type: "enum", title: "After the Z retract", default: "zThenY", values: ["zOnly", "zThenY", "zThenXY"],
      group: "Program end", description: "Bring the table forward (Y to machine zero [H1 p.43]), also X, or neither." },
    coolantCodes: { type: "string", title: "Coolant codes", default: "flood=M08/M09 mist=/ through=M88/M89 air=M83/M84",
      group: "Coolant", description: "On/off M codes per coolant mode: M08/M09 flood, M88/M89 through-spindle " +
        "(option), M83/M84 auto air jet (option) [H1 p.20-21]. The code list has no mist code: set one if the machine " +
        "has it." },
    smoothing: { type: "enum", title: "G187 smoothing", default: "off", values: ["off", "rough", "medium", "finish"],
      group: "Motion", description: "G187 P1/P2/P3 at the start of the program; off leaves the control's Setting 191 " +
        "[H2, partly verified]." },
    smoothingCorner: { type: "number", title: "G187 corner rounding (mm)", default: 0, min: 0, max: 6.35, group: "Motion",
      description: "G187 E, the maximum corner rounding, in mm (written in the program's units; the E range is 0.0001 " +
        "to 0.25 in [H1 p.25]). 0 = no E word." },
    peckReduction: { type: "bool", title: "G83 I/J/K peck reduction", default: false, group: "Cycles",
      description: "Peck drilling with I (first peck), J (reduction per peck) and K (minimum peck) instead of Q " +
        "[H1 p.77]. Off: equal pecks of Q." },
    peckReductionJ: { type: "number", title: "J, as a fraction of the peck", default: 0.25, min: 0, max: 1,
      group: "Cycles" },
    peckMinimumK: { type: "number", title: "K, as a fraction of the peck", default: 0.25, min: 0.05, max: 1,
      group: "Cycles" },
    arcFormat: { type: "enum", title: "Arc centre format", default: "IJK", values: ["IJK", "R"], group: "Motion",
      description: "IJK (full circles allowed) or R (arcs split at 90 degrees; R cannot make a full circle " +
        "[H1 p.33])." },
  },

  onProgramStart(ctx, out) {
    const p = ctx.properties;
    this.inch = ctx.units === "inch";
    this.units = this.inch ? "G20" : "G21";
    this.coolantCodes = parseCoolantCodes(p.coolantCodes, out);
    this.arcFormat = p.arcFormat;
    this.peckReduction = p.peckReduction;
    this.peckJ = p.peckReductionJ;
    this.peckK = p.peckMinimumK;
    this.machineRigid = ctx.machine.rigidTapping;
    this.tapWarned = false;
    this.wcs = this.wcsCode(ctx.workOffset, out);
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
    this.smoothing = p.smoothing;

    // Axis words to 4 decimals in inch, 3 in mm [H1 p.28]; feeds to 2 and 1 (tapping feeds to 4 and 3, the F
    // resolution [H1 p.25]).
    out.setFormat({ axisDecimals: this.inch ? 4 : 3, feedDecimals: this.inch ? 2 : 1, trailingZeros: p.decimalStyle,
      commentMaxLength: p.commentMaxLength });
    out.sequence.enabled = p.sequenceNumbers !== "off";
    out.sequence.onlyOnToolChange = p.sequenceNumbers === "toolChange";
    out.sequence.start = p.sequenceStart;
    out.sequence.increment = p.sequenceIncrement;
    if (out.sequence.max === 0) out.sequence.max = 99999; // [H1 p.11, p.26]

    // Program number O00001-O99999 and the program name in parentheses on the same line [H1 p.26].
    const number = ctx.programNumber;
    if (!(Number.isInteger(number) && number >= 1 && number <= 99999)) {
      out.fail("program number " + number + " is outside O00001 to O99999 (" + CONTROL + " program numbers); set the " +
        "setup's program number");
    }
    const name = "O" + String(number).padStart(5, "0");
    if (ctx.machine.maxProgramNameLength > 0 && name.length > ctx.machine.maxProgramNameLength) {
      out.fail("program name " + name + " is longer than the machine's limit of " + ctx.machine.maxProgramNameLength +
        " characters (machine definition maxProgramNameLength)");
    }
    out.raw("%");
    let title = cleanText(ctx.programComment !== "" ? ctx.programComment : ctx.documentName);
    if (p.commentMaxLength > 0) title = title.substring(0, p.commentMaxLength).trim();
    out.raw(title === "" ? name : name + " (" + title + ")");
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
    if (this.smoothing !== "off") {
      const level = { rough: "P1", medium: "P2", finish: "P3" }[this.smoothing];
      const corner = p.smoothingCorner > 0
        ? out.word("E", this.inch ? p.smoothingCorner / 25.4 : p.smoothingCorner, { decimals: this.inch ? 4 : 3 })
        : null;
      out.block("G187", level, corner);
    }
  },

  wcsCode(offset, out) {
    // G54-G59 for 1-6, G110-G129 for 7-26, G154 P21-P99 for 27-105 [H1 p.45].
    if (offset >= 1 && offset <= 6) return "G" + (53 + offset);
    if (offset >= 7 && offset <= 26) return "G" + (103 + offset);
    if (offset >= 27 && offset <= 105) return "G154 P" + (offset - 6);
    out.fail("work offset " + offset + " has no " + CONTROL + " code (1-105)");
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
        out.block("M01");
      }
      out.comment("T" + t.number + " " + t.description + (t.holderDescription !== "" ? " in " + t.holderDescription : ""));
      out.block("T" + t.number, "M06");
      if (p.toolPreselect && section.nextTool !== null) out.block("T" + section.nextTool.number);
      this.toolLoaded = true;
      this.spindle = null;
      this.h = null;
      this.pos = { x: null, y: null, z: null };
      out.resetModal();
      out.block(out.modal("units", this.units));
      out.block(out.modal("motion", "G00"), out.modal("distance", "G90"), out.modal("wcs", this.wcs),
        out.modal("plane", "G17"), out.axis("X", section.start.x), out.axis("Y", section.start.y));
      this.startSpindle(section, rpm, out);
      out.block("G43", "H" + t.lengthOffset, out.axis("Z", section.start.z));
      this.h = t.lengthOffset;
    } else {
      if (section.coolant !== this.coolant) this.coolantOff(out);
      out.resetModal("motion", "feed", "axes", "plane", "distance", "wcs");
      if (this.pos.z === null || section.start.z > this.pos.z) {
        out.block(out.modal("motion", "G00"), out.axis("Z", section.start.z));
      }
      out.block(out.modal("motion", "G00"), out.modal("distance", "G90"), out.modal("wcs", this.wcs),
        out.modal("plane", "G17"), out.axis("X", section.start.x), out.axis("Y", section.start.y));
      this.startSpindle(section, rpm, out);
      if (this.h !== t.lengthOffset) {
        out.block("G43", "H" + t.lengthOffset, out.axis("Z", section.start.z));
        this.h = t.lengthOffset;
      } else {
        out.block(out.modal("motion", "G00"), out.axis("Z", section.start.z));
      }
    }
    this.pos = { x: section.start.x, y: section.start.y, z: section.start.z };
    if (section.coolant !== this.coolant) this.coolantOn(section.coolant, out);
    this.section = section;
    this.sectionClockwise = section.clockwise;
    this.rpm = rpm;
    this.comp = 0;
    this.d = t.diameterOffset;
  },

  startSpindle(section, rpm, out) {
    // S and the direction in one block [H1 p.12]. G84/G74 would start the spindle themselves [H1 p.82]; it is
    // started anyway, so that no feed move ever begins with the spindle stopped.
    const dir = section.clockwise ? "M03" : "M04";
    out.block("S" + rpm, dir);
    this.spindle = dir;
  },

  coolantOn(mode, out) {
    if (mode === "off") { this.coolant = "off"; return; }
    const code = this.coolantCodes[mode].on;
    if (code === "") {
      out.fail("coolant mode '" + mode + "' has no M code on this machine; set it in the post property coolantCodes " +
        "(e.g. " + mode + "=M7/M9) or choose another coolant for the operation");
    }
    out.block(code);
    this.coolant = mode;
  },

  coolantOff(out) {
    if (this.coolant === "off") return;
    const code = this.coolantCodes[this.coolant].off;
    if (code !== "") out.block(code);
    // M89 stops the spindle as well [H1 p.129]: the next section starts it again.
    if (this.coolant === "through") this.spindle = null;
    this.coolant = "off";
  },

  retract(method, out) {
    if (method === "G53") {
      out.block("G00", "G53", "G49", out.word("Z", 0)); // [H1 p.43, p.46-47]
      out.modal("motion", "G00");
      this.h = null;
    } else if (method === "G28") {
      out.block("G28", "G91", out.word("Z", 0)); // [H1 p.43]: G91 makes Z0. the current point, so only Z moves home
      out.block("G90");
      out.resetModal("distance", "motion");
      out.modal("distance", "G90");
      this.h = null; // G28 cancels the length offset [H1 p.25]
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
    // Z to machine zero first, then spindle and coolant off [H1 p.43, one M code per block].
    this.retract(last ? p.endRetract : p.toolChangeRetract, out);
    out.block("M05");
    this.spindle = null;
    this.coolantOff(out);
    if (last && p.endPosition !== "zOnly") {
      const axes = p.endPosition === "zThenXY" ? [out.word("X", 0), out.word("Y", 0)] : [out.word("Y", 0)];
      if (p.endRetract === "G28") {
        out.block("G28", "G91", ...axes);
        out.block("G90");
      } else {
        out.block("G00", "G53", ...axes);
      }
    }
  },

  onProgramEnd(ctx, out) {
    if (this.cycleActive) out.block("G80");
    if (this.spindle !== null) out.block("M05");
    this.coolantOff(out);
    if (this.smoothing !== "off") out.block("G187"); // back to Setting 191 [H2]
    out.block("M30"); // end and reset [H1 p.43]
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
    out.block(out.modal("motion", "G00"), ...words);
  },

  onLinear(m, out) {
    this.needSpindle("feed move", out);
    let compWord = null;
    let x = out.axis("X", m.x);
    let y = out.axis("Y", m.y);
    if (m.comp !== this.comp) {
      // [H1 p.67]: on or off only with an X/Y linear move; not with a Z move, not in G02/G03.
      if (this.comp !== 0 && m.comp !== 0) {
        out.fail("cutter compensation changes from " + ["", "G41", "G42"][this.comp] + " to " + ["", "G41", "G42"][m.comp] +
          " without a G40 in section " + this.section.index + " (" + this.section.name + ")");
      }
      if (near(m.x, this.pos.x) && near(m.y, this.pos.y)) {
        out.fail("cutter compensation " + (m.comp === 0 ? "G40" : ["", "G41", "G42"][m.comp]) + " on a move without X or " +
          "Y motion in section " + this.section.index + " (" + this.section.name + "); it needs an X/Y move [H1 p.67]");
      }
      compWord = ["G40", "G41", "G42"][m.comp];
      // Both X and Y on the cancel block, so that neither axis moves unexpectedly [H1 p.67].
      x = x || out.word("X", m.x);
      y = y || out.word("Y", m.y);
      this.comp = m.comp;
    }
    const z = out.axis("Z", m.z);
    this.pos = { x: m.x, y: m.y, z: m.z };
    if (compWord === null && x === null && y === null && z === null) return;
    out.block(compWord !== null ? out.modal("plane", "G17") : null, out.modal("motion", "G01"), compWord, x, y, z,
      compWord !== null && m.comp !== 0 ? "D" + this.d : null, out.feed(m.feed));
  },

  onArc(m, out) {
    this.needSpindle("feed move (arc)", out);
    if (m.comp !== this.comp) {
      out.fail("cutter compensation starts or ends on an arc in section " + this.section.index + " (" +
        this.section.name + "); it may only start or end on a linear move [H1 p.67]");
    }
    if (this.comp !== 0 && m.plane !== "XY") {
      out.fail("an arc in the " + m.plane + " plane with cutter compensation on in section " + this.section.index +
        "; compensation in G18/G19 needs G141, which this post does not write [H1 p.67]");
    }
    const plane = out.modal("plane", { XY: "G17", XZ: "G18", YZ: "G19" }[m.plane]);
    const start = { x: this.pos.x, y: this.pos.y, z: this.pos.z };
    if (this.arcFormat === "R") {
      const n = Math.max(1, Math.ceil(m.sweepDegrees / 90 - 1e-9));
      let from = start;
      for (const piece of arcPieces(m, start, n)) {
        out.block(plane, out.modal("motion", m.cw ? "G02" : "G03"), ...this.arcAxes(m.plane, piece, out),
          out.word("R", m.radius), out.feed(m.feed));
        from = piece;
      }
      this.pos = from;
      return;
    }
    // I, J, K: incremental from the start point to the centre [H1 p.33].
    const centre = { XY: [out.word("I", m.i), out.word("J", m.j)], XZ: [out.word("I", m.i), out.word("K", m.k)],
      YZ: [out.word("J", m.j), out.word("K", m.k)] }[m.plane];
    out.block(plane, out.modal("motion", m.cw ? "G02" : "G03"), ...this.arcAxes(m.plane, m, out), ...centre,
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

  // Seconds with three decimals, never trimmed: "P0.300", "P2.000" (a P without a fraction is milliseconds [H1 p.48]).
  dwellWord(seconds, out) {
    return out.word("P", seconds, { decimals: 3, trimZeros: false });
  },

  onCycle(c, out) {
    const name = "section " + this.section.index + " (" + this.section.name + ")";
    let code;
    let peckWords = [];
    let dwellWord = null;
    switch (c.kind) {
      case "drill": code = c.dwellSeconds > 0 ? "G82" : "G81"; break;              // [H1 p.75, p.76]
      case "peck": code = c.peck > 0 ? "G83" : (c.dwellSeconds > 0 ? "G82" : "G81"); break; // [H1 p.77]
      case "chipBreak": code = c.peck > 0 ? "G73" : (c.dwellSeconds > 0 ? "G82" : "G81"); break; // [H1 p.95]
      case "tap": code = c.leftHand ? "G74" : "G84"; break;                          // [H1 p.82, p.83]
      case "bore":
      case "ream": code = c.dwellSeconds > 0 ? "G89" : "G85"; break;                 // [H1 p.84, p.88]
    }
    if (code === "G83" && this.peckReduction) {
      const q = c.peck;
      peckWords = [out.word("I", q), out.word("J", q * this.peckJ), out.word("K", q * this.peckK)];
    } else if (code === "G83" || code === "G73") {
      peckWords = [out.word("Q", c.peck)];
    }
    if (c.dwellSeconds > 0) {
      // P on G82, G83, G73 and G89 [H1 p.26]; G84, G74 and G85 have none.
      if (code === "G82" || code === "G83" || code === "G73" || code === "G89") {
        dwellWord = this.dwellWord(c.dwellSeconds, out);
      } else if (c.first) {
        this.warnOnce(out, name + ": " + CONTROL + " " + code + " has no dwell word; the " + c.dwellSeconds + " s dwell at the " +
          "bottom is not posted");
      }
    }
    if ((code === "G83" || code === "G73") && c.peckClearance > 0 && c.first) {
      this.warnOnce(out, name + ": the peck clearance " + out.word("", c.peckClearance) + " is not posted; the control takes it " +
        "from Setting 22 [H1 p.77, p.95]");
    }
    if (c.kind === "tap") {
      if (this.sectionClockwise === c.leftHand) {
        out.fail(name + ": a " + (c.leftHand ? "left" : "right") + "-hand tap in an operation whose spindle turns " +
          (this.sectionClockwise ? "clockwise" : "counter-clockwise"));
      }
      if (!this.machineRigid && !this.tapWarned) {
        this.tapWarned = true;
        this.warnOnce(out, name + ": " + CONTROL + " G84/G74 is rigid tapping [H1 p.82] and the machine definition does not list " +
          "rigid tapping; check the machine has it");
      }
      if (c.pitch <= 0) out.fail(name + ": a tapping cycle without a thread pitch");
      const expected = c.pitch * this.rpm;
      if (Math.abs(c.feed - expected) > 1e-3 * expected) {
        out.fail(name + ": tapping feed " + out.word("", c.feed, { decimals: 4 }) + " is not pitch x rpm = " +
          out.word("", expected, { decimals: 4 }) + "; a mismatched tap feed breaks the tap [H1 p.82]");
      }
    } else {
      this.needSpindle("drilling cycle", out);
    }
    const words = { Z: out.word("Z", c.bottom), Q: peckWords.length > 0 ? peckWords.join(" ") : null, P: dwellWord,
      R: out.word("R", c.retract) };
    if (this.cycleActive && c.first && !sameCycleShape(this.cycle, code, words)) {
      // Another cycle code inside a run: G80, back up to the initial level, then the new cycle.
      out.block(out.modal("cycle", "G80"));
      out.resetModal("motion", "Z");
      if (!near(this.pos.z, c.clearance)) out.block(out.modal("motion", "G00"), out.axis("Z", c.clearance));
      this.cycleActive = false;
    }
    const feed = () => (c.kind === "tap" ? out.feed(c.feed, this.inch ? 4 : 3) : out.feed(c.feed));
    if (!this.cycleActive) {
      out.resetModal("cycle", "return", "feed");
      // G98 initial level, G99 R plane, per hole [H1 p.79]; X and Y always (Setting 28 [H1 p.73]).
      out.block(out.modal("plane", "G17"), out.modal("cycle", code),
        out.modal("return", c.returnLevel === "clearance" ? "G98" : "G99"),
        out.axis("X", c.x) || out.word("X", c.x), out.axis("Y", c.y) || out.word("Y", c.y),
        words.Z, words.Q, words.P, words.R, feed());
      this.cycleActive = true;
    } else {
      // Later holes: the position and the words that changed; R and Z may change while the cycle stays active
      // [H1 p.72].
      let x = out.axis("X", c.x);
      let y = out.axis("Y", c.y);
      if (x === null && y === null) { x = out.word("X", c.x); y = out.word("Y", c.y); }
      out.block(out.modal("return", c.returnLevel === "clearance" ? "G98" : "G99"), x, y,
        ...changedCycleWords(this.cycle, words), feed());
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
    out.block("G04", this.dwellWord(seconds, out)); // [H1 p.48]
  },
});
