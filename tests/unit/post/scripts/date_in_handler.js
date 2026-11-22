// Date does not exist, also inside a handler; the error names the line.
definePost(testPost({
  onProgramStart(ctx, out) {
    out.raw("" + Date.now()); /*HERE*/
  },
}));