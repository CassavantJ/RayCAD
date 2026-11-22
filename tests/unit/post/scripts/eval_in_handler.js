// eval and the Function constructor throw inside handlers too.
definePost(testPost({
  onProgramStart(ctx, out) {
    let n = 0;
    try { eval("n = 1"); } catch (e) { out.raw("eval: " + e.name); }
    try { Function("return 2")(); } catch (e) { out.raw("Function: " + e.name); }
    out.raw("n=" + n);
  },
}));