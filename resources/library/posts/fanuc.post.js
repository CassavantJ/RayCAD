// RayCAD post processor: FANUC machining centres (Series 0i-MF / 30i-MB class, 3-axis vertical).
// Written from scratch for RayCAD's own post API (include/ray/post_api.d.ts). It is not derived from any other CAM
// system's post processor.
//
// SOURCES AND HOW FAR THEY WERE CHECKED (read this before trusting a line of output)
//  [F1] FANUC Series 0i-MODEL F Operator's Manual (Common to Lathe System / Machining Center System), B-64604EN,
//       and FANUC Series 30i/31i/32i-MODEL B Operator's Manual (Machining Center System), B-64484EN-2. FANUC does not
//       publish these manuals openly and the author of this post could not read them: every FANUC-specific rule
//       below marked "[F1, unverified]" is from general knowledge of FANUC controls and must be checked against the
//       manual of the actual control (chapter names given are the usual ones, not checked page references).
//  [N]  T. Kramer, F. Proctor, E. Messina, "The NIST RS274NGC Interpreter - Version 3", NISTIR 6556, NIST, 2000
//       (public domain). RS274/NGC is the NC language FANUC-style controls follow; where FANUC and RS274/NGC agree
//       the [N] section is cited and the rule is treated as checked.
//  [ISO] ISO 6983-1:2009, Program format and definitions of address words (not read: cited by title only).
//
// WHAT THIS POST ASSUMES ABOUT THE MACHINE (review with the machine's own manual)
//  - Tool change is "T.. M6" and the builder's tool-change macro does whatever else it needs [F1, unverified]. The
//    post still retracts Z to the reference position before M6 (property toolChangeRetract).
//  - Tool length offset G43 with H, cutter radius offset G41/G42 with D, offsets measured as most shops do.
//  - Work offsets G54..G59 for workOffset 1..6; G54.1 P1..P48 for 7..54 (the additional work coordinate option)
//    [F1, unverified].
//
// SAFETY RULES THIS POST ENFORCES (fails with a message naming the cause rather than post something doubtful)
//  - Safe start block cancels cutter compensation, length offset and canned cycles and sets plane, absolute mode and
//    feed per minute (G17 G40 G49 G80 G90 G94); units in a block of their own before it [F1, unverified: "G20/G21 in
//    an independent block at the start of the program"].
//  - After every tool change: units, work offset, absolute mode and plane are restated with the first XY move, the
//    spindle is started (S.. M3/M4) before the length offset is applied on the first Z approach (G43 Z.. H..) from
//    the tool-change height. Every modal word is written again after a tool change.
//  - Cutter compensation starts and ends only on linear moves with X/Y motion, in the XY plane; a toolpath that
//    leaves it on, starts it on an arc, or rapids with it on is refused.
//  - G80 before any non-cycle move (the host ends cycle runs), before a tool change and at the end of the program.
//  - No feed move without the spindle turning; spindle speed checked against the machine's maximum.
//  - Program end: Z to the reference position first, spindle and coolant off, then the optional XY return, M30.
//
// G AND M WORDS THIS POST CAN WRITE (for the independent G-code reader, T-043)
//  G0 G1 G2 G3 G4 G17 G18 G19 G20 G21 G28 G40 G41 G42 G43 G49 G53 G54..G59 G54.1 G73 G74 G80 G81 G82 G83 G84 G85 G89
//  G90 G91 G94 G98 G99; M3 M4 M5 M6 M29 M30 and the coolant codes of property coolantCodes (default M7 M8 M9),
//  M1 with optionalStop. Other words: N O T S F H D X Y Z I J K R Q P, comments in ( ), "%" at start and end.

"use strict";

// ---------- small helpers (no I/O, no clock: everything is a pure function of the program) ----------

// The characters FANUC accepts in a comment, as the host sanitises them (src/post/writer.h sanitizeComment); used
// for the program name on the O line, which goes out with out.raw.
function cleanText(text) {
  let s = "";
  let space = false;
  for (const ch of String(text)) {
    const ok = /[A-Za-z0-9.,\-+/*=:#_]/.test(ch);
    if (!ok) { space = s.length > 0; continue; }
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

// Points of an arc split into n equal pieces (for R-format output, where each piece must stay at or under 90
// degrees). The plane's in-plane axes (u, v) and normal axis (w) follow the host's convention: XY (x, y | z),
// XZ (z, x | y), YZ (y, z | x); cw is G2 seen from the positive normal.
function arcPieces(m, start, n) {
  const ax = { XY: ["x", "y", "z"], XZ: ["z", "x", "y"], YZ: ["y", "z", "x"] }[m.plane];
  const c = { x: m.cx, y: m.cy, z: m.cz };
  const su = start[ax[0]] - c[ax[0]];
  const sv = start[ax[1]] - c[ax[1]];
  const a0 = Math.atan2(sv, su);
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
  title: "FANUC machining centre (0i-MF / 30i-MB class)",
  vendor: "RayCAD",
  dialect: "fanuc",
  fileExtension: ".nc",
  apiVersion: 1,
  capabilities: {
    arcs: "IJK",
    helical: true,
    // IJK arcs up to a full circle: with I, J, K a block whose end point equals its start point is a full circle
    // [F1, unverified: "Circular interpolation G02/G03"; agrees with N 3.5.3.2].
    maxArcDegrees: 360,
    cannedCycles: ["drill", "peck", "chipBreak", "tap", "bore", "ream"],
    rigidTap: true,
    cutterComp: true,
    arcPlanes: ["XY", "XZ", "YZ"],
    // Helical interpolation is posted in the XY plane only; helices in G18/G19 are cut as lines by the host. FANUC
    // documents helical interpolation in any plane [F1, unverified], so this is a conservative choice.
    helicalPlanes: ["XY"],
  },
  properties: {
    sequenceNumbers: { type: "enum", title: "Sequence numbers", default: "off", values: ["off", "every", "toolChange"],
      group: "Format", description: "N numbers on every block, only on the first block of each tool, or none." },
    sequenceStart: { type: "int", title: "Sequence start", default: 10, min: 1, max: 99999, group: "Format" },
    sequenceIncrement: { type: "int", title: "Sequence increment", default: 10, min: 1, max: 1000, group: "Format" },
    decimalStyle: { type: "enum", title: "Decimal point style", default: "trim", values: ["trim", "one", "all"],
      group: "Format", description: "X12. (trim), X12.0 (one) or X12.000 (all). A decimal point is always written, so " +
        "the output does not depend on the calculator-type decimal point parameter [F1, unverified: parameter 3401#0]." },
    commentMaxLength: { type: "int", title: "Longest comment", default: 0, min: 0, max: 1000, group: "Format",
      description: "Comments are cut to this many characters; 0 = no limit." },
    programNumberDigits: { type: "enum", title: "Program number digits", default: "4", values: ["4", "8"],
      group: "Program", description: "O0001-O9999, or O00000001-O99999999 with the 8-digit program number option " +
        "[F1, unverified]." },
    toolList: { type: "bool", title: "Tool list in header", default: true, group: "Program" },
    optionalStop: { type: "bool", title: "Optional stop before tool changes", default: false, group: "Program",
      description: "M1 before every tool change after the first." },
    safeStartExtra: { type: "string", title: "Extra safe-start codes", default: "", group: "Program",
      description: "G codes added to the safe start block, e.g. \"G15 G50 G69\" when the control has those options. " +
        "Only G words are accepted." },
    toolPreselect: { type: "bool", title: "Pre-select next tool", default: true, group: "Tool change",
      description: "Write the next tool's T word right after M6 so the magazine can turn while cutting." },
    toolChangeRetract: { type: "enum", title: "Retract before tool change", default: "G28", values: ["G28", "G53", "none"],
      group: "Tool change", description: "G91 G28 Z0. (reference position, then G90), G0 G53 Z0. (machine zero), or " +
        "nothing when the builder's M6 macro retracts Z itself." },
    m6LeavesZRetracted: { type: "bool", title: "M6 leaves Z retracted (allows retract none)", default: false,
      group: "Tool change", description: "Set this only if the machine's tool change always ends with Z at the "
        + "tool-change height. With toolChangeRetract none the post moves X/Y right after M6 at whatever height M6 "
        + "left the spindle; without this assurance it refuses none." },
    endRetract: { type: "enum", title: "Retract at program end", default: "G28", values: ["G28", "G53"],
      group: "Program end" },
    endPosition: { type: "enum", title: "After the Z retract", default: "zOnly", values: ["zOnly", "zThenY", "zThenXY"],
      group: "Program end", description: "Also send Y (to bring the table forward) or X and Y home, after Z." },
    coolantCodes: { type: "string", title: "Coolant codes", default: "flood=M8/M9 mist=M7/M9 through=/ air=/",
      group: "Coolant", description: "On/off M codes per coolant mode. M7, M8 and M9 are the RS274 codes [N 3.6.4]; " +
        "through-spindle coolant and air blast are builder-specific on FANUC machines: set their codes, e.g. " +
        "through=M88/M89. A mode used with no code stops posting." },
    dwellUnits: { type: "enum", title: "G4 dwell word", default: "P-milliseconds", values: ["P-milliseconds", "X-seconds"],
      group: "Cycles", description: "G4 P1500 (integer milliseconds; P takes no decimal point) or G4 X1.5 (seconds) " +
        "[F1, unverified: 'Dwell G04']. Canned-cycle P is always milliseconds." },
    tapping: { type: "enum", title: "Tapping", default: "auto", values: ["auto", "rigidM29", "plainG84"],
      group: "Cycles", description: "rigidM29: M29 S.. before G84/G74 (rigid tapping by M code). plainG84: G84/G74 " +
        "alone (floating tapping, or rigid tapping by parameter 5200#0). auto: rigidM29 when the machine definition " +
        "says rigid tapping, else plainG84 with a warning [F1, unverified: 'Rigid tapping']." },
    arcFormat: { type: "enum", title: "Arc centre format", default: "IJK", values: ["IJK", "R"], group: "Motion",
      description: "IJK (centre offsets, full circles allowed) or R (radius; arcs are split at 90 degrees, as a radius " +
        "arc near 180 degrees or above is ill-conditioned [N 3.5.3.1])." },
  },

  // ---------- program start ----------
  onProgramStart(ctx, out) {
    const p = ctx.properties;
    this.inch = ctx.units === "inch";
    this.units = this.inch ? "G20" : "G21";
    this.coolantCodes = parseCoolantCodes(p.coolantCodes, out);
    this.tapping = p.tapping === "auto" ? (ctx.machine.rigidTapping ? "rigidM29" : "plainG84") : p.tapping;
    this.autoPlain = p.tapping === "auto" && !ctx.machine.rigidTapping;
    this.tapWarned = false;
    this.arcFormat = p.arcFormat;
    this.dwellUnits = p.dwellUnits;
    this.wcs = this.wcsCode(ctx.workOffset, out);
    // toolChangeRetract none is accepted only when the user asserts that M6 leaves Z retracted: otherwise the first X/Y
    // move after a tool change would run at an unknown height (the builder's M6 macro decides [F1, unverified]).
    if (p.toolChangeRetract === "none" && !p.m6LeavesZRetracted) {
      out.fail("post property toolChangeRetract is none, but nothing says the machine's M6 leaves Z retracted: the "
        + "first X/Y move after a tool change would run at an unknown height. Choose a retract, or set the post "
        + "property m6LeavesZRetracted if the machine's tool change always ends with Z up");
    }
    this.toolLoaded = false;
    this.spindle = null;          // "M3" | "M4" while turning
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
    // N1-N99999 [F1, unverified; N 3.3.1 allows five digits], unless the machine definition gives its own limit.
    if (out.sequence.max === 0) out.sequence.max = 99999;

    // Program number: O and 4 (or 8) digits [F1, unverified: 'Program number'].
    const digits = Number(p.programNumberDigits);
    const number = ctx.programNumber;
    const limit = digits === 8 ? 99999999 : 9999;
    if (!(Number.isInteger(number) && number >= 1 && number <= limit)) {
      out.fail("program number " + number + " is outside O1 to O" + limit + " (" + digits + "-digit FANUC program " +
        "numbers); set the setup's program number or the property programNumberDigits");
    }
    const name = "O" + String(number).padStart(digits, "0");
    if (ctx.machine.maxProgramNameLength > 0 && name.length > ctx.machine.maxProgramNameLength) {
      out.fail("program name " + name + " is longer than the machine's limit of " + ctx.machine.maxProgramNameLength +
        " characters (machine definition maxProgramNameLength)");
    }
    if (digits === 4 && number >= 8000) {
      out.warn("program number " + name + ": numbers 8000-9999 are often edit-protected on FANUC controls " +
        "(parameter 3202) and used for builder macros [F1, unverified]");
    }
    // "%" leader and the program number with its name in a comment on the same line [F1, unverified: 'Program
    // configuration'; the comment after O is shown as the program name in the directory].
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
    // Safe start: units alone, then every mode that could be left over from an earlier program cancelled or set
    // [F1, unverified: 'G code list'; the same codes and meanings in N 3.5 (G17 3.5.6, G40 3.5.10, G49 3.5.11,
    // G80 3.5.15, G90 3.5.17, G94 3.5.19)].
    out.block(out.modal("units", this.units));
    const extra = String(p.safeStartExtra).trim();
    if (extra !== "" && !/^(G\d{1,3}(\.\d)?)(\s+G\d{1,3}(\.\d)?)*$/.test(extra)) {
      out.fail("post property safeStartExtra: \"" + extra + "\" must be G words only, e.g. \"G15 G50 G69\"");
    }
    out.block(out.modal("plane", "G17"), "G40", "G49", "G80", out.modal("distance", "G90"), "G94",
      extra === "" ? null : extra);
  },

  wcsCode(offset, out) {
    if (offset >= 1 && offset <= 6) return "G" + (53 + offset);
    if (offset >= 7 && offset <= 54) return "G54.1 P" + (offset - 6); // additional work offsets [F1, unverified]
    out.fail("work offset " + offset + " has no FANUC code here (1-6 are G54-G59, 7-54 are G54.1 P1-P48)");
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
    const tapRigid = t.type === "tap" && this.tapping === "rigidM29";
    out.comment(section.name);
    if (section.toolChanged || !this.toolLoaded) {
      if (!this.toolLoaded) {
        // The first tool change: Z is wherever the last program or the operator left it.
        this.retract(p.toolChangeRetract, out);
      } else if (p.optionalStop) {
        out.block("M1");
      }
      // Tool change [F1, unverified: builder's M6 macro]: T and M6 in one block, the next tool pre-selected.
      out.comment("T" + t.number + " " + t.description + (t.holderDescription !== "" ? " in " + t.holderDescription : ""));
      out.block("T" + t.number, "M6");
      if (p.toolPreselect && section.nextTool !== null) out.block("T" + section.nextTool.number);
      this.toolLoaded = true;
      this.spindle = null;
      this.h = null;
      this.pos = { x: null, y: null, z: null };
      out.resetModal();
      // Units again, then work offset, absolute, plane and the first XY position from the tool-change height.
      out.block(out.modal("units", this.units));
      out.block(out.modal("motion", "G0"), out.modal("distance", "G90"), out.modal("wcs", this.wcs),
        out.modal("plane", "G17"), out.axis("X", section.start.x), out.axis("Y", section.start.y));
      this.startSpindle(section, rpm, tapRigid, out);
      // Length offset on the first Z approach, from the tool-change height [F1, unverified: 'Tool length offset';
      // N 3.5.11].
      out.block("G43", out.axis("Z", section.start.z), "H" + t.lengthOffset);
      this.h = t.lengthOffset;
      this.pos = { x: section.start.x, y: section.start.y, z: section.start.z };
      this.coolantOn(section.coolant, out);
    } else {
      // Same tool: restate the modes, change coolant if needed, and go to the start from above.
      if (section.coolant !== this.coolant) this.coolantOff(out);
      out.resetModal("motion", "feed", "axes", "plane", "distance", "wcs");
      const z = this.pos.z;
      if (z === null || section.start.z > z) {
        out.block(out.modal("motion", "G0"), out.axis("Z", section.start.z));
      }
      out.block(out.modal("motion", "G0"), out.modal("distance", "G90"), out.modal("wcs", this.wcs),
        out.modal("plane", "G17"), out.axis("X", section.start.x), out.axis("Y", section.start.y));
      this.startSpindle(section, rpm, tapRigid, out);
      if (this.h !== t.lengthOffset) {
        out.block("G43", out.axis("Z", section.start.z), "H" + t.lengthOffset);
        this.h = t.lengthOffset;
      } else {
        out.block(out.modal("motion", "G0"), out.axis("Z", section.start.z));
      }
      this.pos = { x: section.start.x, y: section.start.y, z: section.start.z };
      if (section.coolant !== this.coolant) this.coolantOn(section.coolant, out);
    }
    this.section = section;
    this.clearanceZ = section.clearanceZ;
    this.sectionClockwise = section.clockwise;
    this.rpm = rpm;
    this.comp = 0;
    this.d = t.diameterOffset;
  },

  startSpindle(section, rpm, tapRigid, out) {
    if (tapRigid) {
      // Rigid tapping by M29: the cycle runs the spindle; it is left stopped before M29 [F1, unverified].
      if (this.spindle !== null) out.block("M5");
      out.block("S" + rpm);
      this.spindle = null;
      return;
    }
    const dir = section.clockwise ? "M3" : "M4";
    out.block("S" + rpm, dir);
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
    // Z first: that G28 (or G53) with Z alone moves only Z is [F1, unverified] (RS274/NGC's G28 goes to a home
    // position that may include X and Y [N 3.5.8]). So when the tool is below the operation's clearance height, it
    // rapids straight up to that height before the reference or machine-zero return.
    if (method !== "none" && this.pos.z !== null && this.clearanceZ !== undefined && this.pos.z < this.clearanceZ) {
      out.block(out.modal("motion", "G0"), out.axis("Z", this.clearanceZ));
      this.pos.z = this.clearanceZ;
    }
    if (method === "G28") {
      // Reference position return in Z only, through the current position (incremental 0) [F1, unverified:
      // 'Reference position return G28'; N 3.5.8].
      out.block("G91", "G28", out.word("Z", 0));
      out.block("G90");
      out.resetModal("distance", "motion");
      out.modal("distance", "G90");
    } else if (method === "G53") {
      // Machine coordinate system, Z zero [F1, unverified: 'Machine coordinate system G53'; N 3.5.12].
      out.block("G0", "G53", out.word("Z", 0));
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
    // Tool change or program end: Z up first (unless toolChangeRetract is none: the builder's M6 macro retracts),
    // then spindle and coolant off [F1, unverified].
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
    // End of program and reset [F1, unverified: 'Program end M30'; N 3.6.1], then the "%" trailer.
    out.block("M30");
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
      // Cutter compensation on or off: a linear move with X/Y motion, in the XY plane [F1, unverified: 'Cutter
      // compensation C'; N 3.5.10 and Appendix B].
      if (this.comp !== 0 && m.comp !== 0) {
        out.fail("cutter compensation changes from " + ["", "G41", "G42"][this.comp] + " to " + ["", "G41", "G42"][m.comp] +
          " without a G40 in section " + this.section.index + " (" + this.section.name + ")");
      }
      if (near(m.x, this.pos.x) && near(m.y, this.pos.y)) {
        out.fail("cutter compensation " + (m.comp === 0 ? "G40" : ["", "G41", "G42"][m.comp]) + " on a move without X or " +
          "Y motion in section " + this.section.index + " (" + this.section.name + "); it needs an X/Y move");
      }
      compWord = ["G40", "G41", "G42"][m.comp];
      // Both X and Y on the block that starts or ends compensation.
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
      // Radius format: pieces of at most 90 degrees, positive R [N 3.5.3.1].
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
    // Centre format: both centre offsets of the plane, incremental from the start [F1, unverified; N 3.5.3.2].
    const centre = { XY: [out.word("I", m.i), out.word("J", m.j)], XZ: [out.word("I", m.i), out.word("K", m.k)],
      YZ: [out.word("J", m.j), out.word("K", m.k)] }[m.plane];
    out.block(plane, out.modal("motion", m.cw ? "G2" : "G3"), ...this.arcAxes(m.plane, m, out), ...centre,
      out.feed(m.feed));
    this.pos = { x: m.x, y: m.y, z: m.z };
  },

  // The end point of an arc: the plane's two axes always (a full circle has the same end as start), the third axis
  // when it changes (a helix).
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

  // ---------- canned cycles ----------
  onCycle(c, out) {
    const name = "section " + this.section.index + " (" + this.section.name + ")";
    let code;
    let peckWord = null;
    let dwellWord = null;
    // Milliseconds, an integer, for the P of G82, G84, G89 [F1, unverified: 'Canned cycles'].
    const dwellMs = c.dwellSeconds > 0 ? out.word("P", Math.round(c.dwellSeconds * 1000),
      { decimals: 0, forceDecimalPoint: false }) : null;
    switch (c.kind) {
      case "drill": code = c.dwellSeconds > 0 ? "G82" : "G81"; dwellWord = dwellMs; break;
      case "peck":
      case "chipBreak":
        if (c.peck > 0) {
          code = c.kind === "peck" ? "G83" : "G73";
          peckWord = out.word("Q", c.peck);
          if (c.dwellSeconds > 0 && c.first) {
            this.warnOnce(out, name + ": FANUC " + code + " has no dwell word; the " + c.dwellSeconds + " s dwell at the bottom " +
              "is not posted");
          }
        } else {
          code = c.dwellSeconds > 0 ? "G82" : "G81";
          dwellWord = dwellMs;
        }
        if (c.peckClearance > 0 && c.first) {
          this.warnOnce(out, name + ": the peck clearance " + out.word("", c.peckClearance) + " is not posted; FANUC takes it from " +
            "parameter " + (c.kind === "peck" ? "5115" : "5114") + " [F1, unverified]");
        }
        break;
      case "tap": code = c.leftHand ? "G74" : "G84"; dwellWord = dwellMs; break;
      case "bore":
      case "ream": code = c.dwellSeconds > 0 ? "G89" : "G85"; dwellWord = dwellMs; break;
    }
    if (c.kind === "tap") {
      this.checkTap(c, name, out);
    } else {
      this.needSpindle("drilling cycle", out);
    }
    const words = { Z: out.word("Z", c.bottom), R: out.word("R", c.retract), Q: peckWord, P: dwellWord };
    // A G98 hole after G99 holes of the same run. FANUC's G98 returns to the initial level, the Z where the cycle
    // started [F1, unverified]; as the manual could not be read, the post does not rely on it: the run is ended, Z
    // goes up to the clearance and the cycle starts again there, which gives the clearance on any reading of G98
    // (RS274/NGC's text can be read per block [N 3.5.20, 3.5.16.2]).
    const lowG98 = this.cycleActive && c.returnLevel === "clearance" && !near(this.pos.z, c.clearance);
    if (this.cycleActive && (lowG98 || (c.first && !sameCycleShape(this.cycle, code, words)))) {
      // Another cycle code (or a word dropped) inside a run, or the G98 case above: cancel, and go back up to the
      // initial level first, so that the control's initial level is the clearance again when the cycle restarts.
      out.block(out.modal("cycle", "G80"));
      out.resetModal("motion", "Z");
      if (!near(this.pos.z, c.clearance)) out.block(out.modal("motion", "G0"), out.axis("Z", c.clearance));
      this.cycleActive = false;
    }
    const tapFeed = () => out.feed(c.feed, this.inch ? 4 : 3);
    if (!this.cycleActive) {
      // The run's first hole: the whole cycle, X and Y always, G98/G99 always [F1, unverified; N 3.5.16, 3.5.20].
      out.resetModal("cycle", "return", "feed");
      if (c.kind === "tap" && this.tapping === "rigidM29") out.block("M29", "S" + this.rpm);
      out.block(out.modal("plane", "G17"), out.modal("return", c.returnLevel === "clearance" ? "G98" : "G99"),
        out.modal("cycle", code), out.axis("X", c.x) || out.word("X", c.x), out.axis("Y", c.y) || out.word("Y", c.y),
        words.Z, words.R, words.Q, words.P, c.kind === "tap" ? tapFeed() : out.feed(c.feed));
      this.cycleActive = true;
    } else {
      // A later hole: its position, and the cycle words that changed (Z, R, Q, P and F stay in force from block to
      // block [F1, unverified; N 3.5.16 "sticky" numbers]).
      let x = out.axis("X", c.x);
      let y = out.axis("Y", c.y);
      if (x === null && y === null) { x = out.word("X", c.x); y = out.word("Y", c.y); } // the same hole again
      out.block(out.modal("return", c.returnLevel === "clearance" ? "G98" : "G99"), x, y,
        ...changedCycleWords(this.cycle, words), c.kind === "tap" ? tapFeed() : out.feed(c.feed));
    }
    this.cycle = { code: code, words: words };
    this.pos = { x: c.x, y: c.y, z: c.returnLevel === "clearance" ? c.clearance : c.retract };
  },

  checkTap(c, name, out) {
    const wanted = c.leftHand ? "M4" : "M3";
    if (this.tapping !== "rigidM29" && this.spindle !== wanted) {
      // Floating tapping: the spindle must already turn the cutting way [F1, unverified; N 3.5.16.5 for G84].
      out.fail(name + ": " + (c.leftHand ? "G74" : "G84") + " needs the spindle turning " +
        (c.leftHand ? "counter-clockwise (M4)" : "clockwise (M3)") + "; set the operation's spindle direction");
    }
    if (this.tapping === "rigidM29" && this.sectionClockwise === c.leftHand) {
      out.fail(name + ": a " + (c.leftHand ? "left" : "right") + "-hand tap in an operation whose spindle turns " +
        (this.sectionClockwise ? "clockwise" : "counter-clockwise"));
    }
    if (this.autoPlain && !this.tapWarned) {
      this.tapWarned = true;
      this.warnOnce(out, name + ": the machine definition does not say rigid tapping, so G84/G74 is posted without M29 " +
        "(floating tapping, or rigid by parameter); set the post property tapping to choose");
    }
    if (c.pitch <= 0) out.fail(name + ": a tapping cycle without a thread pitch");
    const expected = c.pitch * this.rpm;
    if (Math.abs(c.feed - expected) > 1e-3 * expected) {
      out.fail(name + ": tapping feed " + out.word("", c.feed, { decimals: 4 }) + " is not pitch x rpm = " +
        out.word("", expected, { decimals: 4 }) + "; a mismatched tap feed breaks the tap");
    }
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
    // After a cycle the control's Z is the return level and the motion mode is G80: write G0/G1, Z and F again.
    out.resetModal("motion", "Z", "feed", "return");
  },

  onDwell(seconds, out) {
    if (this.dwellUnits === "X-seconds") {
      out.block("G4", out.word("X", seconds, { decimals: 3 }));
    } else {
      out.block("G4", out.word("P", Math.round(seconds * 1000), { decimals: 0, forceDecimalPoint: false }));
    }
  },
});
