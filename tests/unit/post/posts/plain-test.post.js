// Second test post for the post host (T-039), written against include/ray/post_api.d.ts only. Not a shipped post.
// It declares as little as a control could do: arcs as R words of at most 90 degrees, no helical arcs (the host
// turns helices into lines) and no canned cycles (the host expands drilling, pecking, chip breaking, boring and
// reaming into plain moves). Sequence numbers only on tool changes, lower-case comments passed through the host's
// Haas rules, LF line endings by default. Its goldens are tests/golden/gcode/host/plain-test_*.nc.
definePost({
  title: "Host test, plain moves",
  dialect: "haas-classic",
  fileExtension: ".tap",
  apiVersion: 1,
  capabilities: {
    arcs: "R",
    helical: false,
    maxArcDegrees: 90,
    cannedCycles: [],
    rigidTap: false,
    cutterComp: false,
  },
  properties: {
    lineEnding: { type: "enum", title: "Line endings", default: "lf", values: ["crlf", "lf"] },
    toolNumbers: { type: "bool", title: "Number tool changes", default: true },
  },

  onProgramStart(ctx, out) {
    out.raw("%\n:" + ctx.programNumber);
    out.comment("Setup " + ctx.setupName + ", " + ctx.units + "; work offset " + ctx.workOffset);
    out.sequence.enabled = ctx.properties.toolNumbers;
    out.sequence.onlyOnToolChange = true;
    out.sequence.start = 1;
    out.sequence.increment = 1;
    out.block(ctx.units === "inch" ? "G20" : "G21", "G90", "G17");
  },

  onSectionStart(section, ctx, out) {
    if (section.toolChanged) {
      out.block("T" + section.tool.number, "M6", "(" + section.tool.description.toUpperCase() + ")");
    }
    out.resetModal();
    out.block(out.modal("motion", "G0"), out.axis("X", section.start.x), out.axis("Y", section.start.y),
      "S" + section.rpm, "M3");
    out.block(out.axis("Z", section.start.z));
  },

  onRapid(m, out) {
    const w = [out.axis("X", m.x), out.axis("Y", m.y), out.axis("Z", m.z)];
    if (w[0] === null && w[1] === null && w[2] === null) return;
    out.block(out.modal("motion", "G0"), ...w);
  },

  onLinear(m, out) {
    const w = [out.axis("X", m.x), out.axis("Y", m.y), out.axis("Z", m.z)];
    if (w[0] === null && w[1] === null && w[2] === null) return;
    out.block(out.modal("motion", "G1"), ...w, out.feed(m.feed), m.role === "retract" ? "(OUT)" : null);
  },

  onArc(m, out) {
    if (m.plane !== "XY") out.fail("plain-test only cuts arcs in XY; got " + m.plane);
    if (m.sweepDegrees > 90.000001) out.fail("the host gave an arc of " + m.sweepDegrees + " degrees");
    out.block(out.modal("motion", m.cw ? "G2" : "G3"), out.axis("X", m.x), out.axis("Y", m.y), out.axis("Z", m.z),
      out.word("R", m.radius), out.feed(m.feed));
  },

  onDwell(seconds, out) {
    out.block("G4", out.word("P", seconds * 1000, { decimals: 0, forceDecimalPoint: false }));
  },

  onSectionEnd(section, ctx, out) {
    out.block("M5");
    out.comment("end of " + section.name + "; next tool " + (section.nextTool ? section.nextTool.number : "none"));
  },

  onProgramEnd(ctx, out) {
    out.block("M30");
    out.raw("%");
  },
});
