// A post that warns forever, swallowing errors, is stopped by the warning limit.
definePost(testPost({
  onProgramStart(ctx, out) {
    for (let i = 0; ; i++) {
      try { out.warn("warning " + i); } catch (e) { /* try to swallow the limit */ } /*HERE*/
    }
  },
}));