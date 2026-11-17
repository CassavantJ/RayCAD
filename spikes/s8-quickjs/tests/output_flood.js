definePost({
  onProgramStart(ctx, out) {
    const line = "X".repeat(1000);
    for (;;) {
      try { out.raw(line); } catch (e) { /* try to swallow the limit */ }
    }
  },
});
