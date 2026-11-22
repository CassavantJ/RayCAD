// Test post for the post host (T-039), written against include/ray/post_api.d.ts only. Not a shipped post: it emits
// ISO-style words so that every host service shows up in its output: arcs split by the host to 180 degrees, helical
// IJK arcs in all three planes, the drill, peck, chipBreak and tap cycles as canned cycles with G98 / G99 per hole
// and G74 for left-hand taps (bore and ream left to the host), dwell, cutter compensation, sequence numbers, modal
// words, formats, comments, warnings and the
// program and section objects. Its goldens are tests/golden/gcode/host/iso-test_*.nc.
definePost({
  title: "Host test, ISO words",
  vendor: "RayCAD tests",
  dialect: "generic-iso",
  fileExtension: ".nc",
  apiVersion: 1,
  capabilities: {
    arcs: "IJK",
    helical: true,
    maxArcDegrees: 180,
    cannedCycles: ["drill", "peck", "chipBreak", "tap"],
    rigidTap: true,
    cutterComp: true,
  },
  properties: {
    numberBlocks: { type: "bool", title: "Sequence numbers", default: true, group: "Format" },
    numberStep: { type: "int", title: "Sequence step", default: 5, min: 1, max: 100, group: "Format" },
    homeZ: { type: "number", title: "Return height", default: 50, description: "Z of the final retract" },
    coolantStyle: { type: "enum", title: "Coolant codes", default: "m8", values: ["m8", "none"] },
    header: { type: "string", title: "Header text", default: "made by the host test" },
  },

  onProgramStart(ctx, out) {
    out.raw("%");
    out.raw("O" + String(ctx.programNumber).padStart(4, "0") + " (" + ctx.documentName.toUpperCase() + ")");
    const p = ctx.properties;
    out.sequence.enabled = p.numberBlocks;
    out.sequence.start = 10;
    out.sequence.increment = p.numberStep;
    out.comment(ctx.programComment);
    out.comment(p.header + " for (" + ctx.userName + ")");
    if (ctx.date !== null) out.comment("date " + ctx.date);
    out.comment("machine " + ctx.machine.name + " / " + ctx.machine.model + " max " + ctx.machine.maxRpm + " rpm");
    const x = ctx.machine.travel.X;
    if (x) out.comment("x travel " + out.word("", x[0], { decimals: 1 }) + " to " + out.word("", x[1], { decimals: 1 }));
    out.comment("units " + ctx.units + " sections " + ctx.sections.length);
    for (const t of ctx.tools) {
      out.comment("T" + t.number + " " + t.type + " D" + out.word("", t.diameter, { decimals: 3 }) + " " + t.description);
    }
    out.block(out.modal("units", ctx.units === "inch" ? "G20" : "G21"), out.modal("distance", "G90"),
      out.modal("plane", "G17"), "G40", "G49", "G80");
    this.wcs = "G" + (53 + ctx.workOffset);
  },

  onSectionStart(section, ctx, out) {
    out.comment(section.index + ": " + section.name + " (" + section.opType + ")");
    if (section.toolChanged) {
      out.block("T" + section.tool.number, "M6");
      if (section.nextTool !== null) out.block("T" + section.nextTool.number);
      out.resetModal();
    } else {
      out.resetModal("motion", "feed", "axes");
      out.warn("section " + section.index + " keeps tool " + section.tool.number);
    }
    out.block(out.modal("wcs", this.wcs), out.modal("motion", "G0"), out.axis("X", section.start.x),
      out.axis("Y", section.start.y));
    out.block("S" + Math.round(section.rpm), section.clockwise ? "M3" : "M4");
    out.block("G43", out.axis("Z", section.start.z), "H" + section.tool.lengthOffset);
    if (ctx.properties.coolantStyle === "m8" && section.coolant !== "off") out.block("M8");
    out.comment("z " + out.word("", section.minZ, { decimals: 3 }) + " to " + out.word("", section.maxZ, { decimals: 3 }) +
      " cut " + out.word("", section.cutDistance, { decimals: 1 }) + " in " +
      out.word("", section.estimatedSeconds, { decimals: 0 }) + " s");
    this.comp = 0;
  },

  onRapid(m, out) {
    const words = [out.axis("X", m.x), out.axis("Y", m.y), out.axis("Z", m.z)];
    if (words.every((w) => w === null)) return;
    out.block(out.modal("motion", "G0"), ...words);
  },

  onLinear(m, out) {
    let compWord = null;
    if (m.comp !== this.comp) {
      compWord = ["G40", "G41", "G42"][m.comp];
      this.comp = m.comp;
    }
    const words = [out.axis("X", m.x), out.axis("Y", m.y), out.axis("Z", m.z)];
    if (compWord === null && words.every((w) => w === null)) return;
    out.block(compWord, out.modal("motion", "G1"), ...words, compWord !== null && m.comp ? "D" + m.comp : null,
      out.feed(m.feed));
  },

  onArc(m, out) {
    const plane = { XY: "G17", XZ: "G18", YZ: "G19" }[m.plane];
    const centre = {
      XY: [out.word("I", m.i), out.word("J", m.j)],
      XZ: [out.word("I", m.i), out.word("K", m.k)],
      YZ: [out.word("J", m.j), out.word("K", m.k)],
    }[m.plane];
    out.block(out.modal("plane", plane), out.modal("motion", m.cw ? "G2" : "G3"), out.axis("X", m.x),
      out.axis("Y", m.y), out.axis("Z", m.z), ...centre, out.feed(m.feed));
    if (m.sweepDegrees > 180.000001) out.fail("the host gave an arc of " + m.sweepDegrees + " degrees");
  },

  onCycle(c, out) {
    const codes = { drill: c.dwellSeconds > 0 ? "G82" : "G81", peck: "G83", chipBreak: "G73", tap: c.leftHand ? "G74" : "G84" };
    if (c.first) out.resetModal("cycle", "X", "Y", "Z", "feed");
    // G98 returns to the initial level (c.clearance, where the host has put the tool), G99 to the R plane.
    const ret = out.modal("return", c.returnLevel === "clearance" ? "G98" : "G99");
    const hole = [out.axis("X", c.x), out.axis("Y", c.y)];
    if (c.first) {
      const extra = [];
      if (c.kind === "peck" || c.kind === "chipBreak") extra.push(out.word("Q", c.peck));
      // A dwell is P on G82 and G84 / G74; this dialect's G83 and G73 have none.
      if (c.dwellSeconds > 0) {
        if (c.kind === "drill" || c.kind === "tap") {
          extra.push(out.word("P", c.dwellSeconds, { decimals: 3, trimZeros: true, forceDecimalPoint: false }));
        } else {
          out.warn("a " + c.kind + " cycle cannot dwell in this dialect; the dwell is not posted");
        }
      }
      // The peck clearance is a control parameter in this dialect: no word for it.
      if (c.peckClearance > 0) {
        out.warn("peck clearance " + out.word("", c.peckClearance) + " is the control's parameter in this dialect");
      }
      if (c.kind === "tap") out.comment("tap pitch " + out.word("", c.pitch, { decimals: 4 }) + " top " + out.word("", c.top));
      out.block(ret, out.modal("cycle", codes[c.kind]), ...hole, out.axis("Z", c.bottom),
        out.word("R", c.retract), ...extra, out.feed(c.feed));
    } else {
      out.block(ret, ...hole);
    }
  },

  onCycleEnd(out) {
    out.block(out.modal("cycle", "G80"));
    out.resetModal("motion", "Z");
  },

  onDwell(seconds, out) {
    out.block("G4", out.word("P", seconds, { decimals: 3, forceDecimalPoint: false }));
  },

  onSectionEnd(section, ctx, out) {
    if (this.comp !== 0) out.block("G40");
    if (section.coolant !== "off") out.block("M9");
    out.block("M5");
  },

  onProgramEnd(ctx, out) {
    out.sequence.enabled = false;
    out.block(out.modal("motion", "G0"), "G53", out.word("Z", ctx.properties.homeZ, { decimals: 1 }));
    // Math through the host's deterministic library: the same digits on every OS.
    const angle = Math.atan2(1, 3);
    out.comment("check " + out.word("A", Math.sin(angle) * 1000, { decimals: 6 }) + " " +
      out.word("B", Math.cos(angle) * 1000, { decimals: 6 }) + " " + out.word("C", Math.hypot(3, 4, 12), { decimals: 6 }));
    out.block("M30");
    out.raw("%");
  },
});
