const keep = [];
definePost({
  onLinear(m, out) {
    for (let i = 0; i < 2000; i++) keep.push({ x: m.x, y: m.y, z: m.z, note: "retained " + i });
    out.block(out.axis("X", m.x));
  },
});
