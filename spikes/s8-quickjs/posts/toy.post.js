// S8 toy post. Shape follows docs/interfaces/post_api.d.ts (version 1 draft).
definePost({
  title: "S8 toy ISO",
  vendor: "RayCAD spike",
  dialect: "generic-iso",
  fileExtension: ".nc",
  apiVersion: 1,
  capabilities: { arcs: "IJK", helical: true, maxArcDegrees: 360, cannedCycles: [], rigidTap: false, cutterComp: false },
  properties: {},

  onProgramStart(ctx, out) {
    out.raw("%");
    out.comment(ctx.programComment);
    out.block("G90", "G21", "G17");
  },
  onSectionStart(section, ctx, out) {
    out.comment(section.name);
    out.block("T" + section.tool.number, "M6");
    out.block("S" + section.rpm, section.clockwise ? "M3" : "M4");
    out.resetModal();
  },
  onRapid(m, out) {
    const x = out.axis("X", m.x), y = out.axis("Y", m.y), z = out.axis("Z", m.z);
    if (x === null && y === null && z === null) return;
    out.block(out.modal("motion", "G0"), x, y, z);
  },
  onLinear(m, out) {
    const x = out.axis("X", m.x), y = out.axis("Y", m.y), z = out.axis("Z", m.z);
    if (x === null && y === null && z === null) return;
    out.block(out.modal("motion", "G1"), x, y, z, out.feed(m.feed));
  },
  onArc(m, out) {
    const x = out.axis("X", m.x), y = out.axis("Y", m.y), z = out.axis("Z", m.z);
    out.block(out.modal("motion", m.cw ? "G2" : "G3"), x, y, z,
              out.word("I", m.i), out.word("J", m.j), out.feed(m.feed));
  },
  onSectionEnd(section, ctx, out) {
    out.block("M5");
  },
  onProgramEnd(ctx, out) {
    out.block("M30");
    out.raw("%");
  },
});
