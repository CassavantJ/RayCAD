let count = 0;
definePost(testPost({
  onLinear(m, out) {
    count++;
    if (count === 500) {
      const tool = null;
      out.block("T" + tool.number); /*HERE*/
    }
    out.block(out.axis("X", m.x));
  },
}));
