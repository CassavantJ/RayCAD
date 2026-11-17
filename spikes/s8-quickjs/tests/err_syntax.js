// line 1
definePost({
  onLinear(m, out) {
    out.block("G1" out.axis("X", m.x)); /*HERE*/
  },
});
