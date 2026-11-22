// An async handler would silently do nothing (the host never runs promise jobs): the host refuses it.
definePost(testPost({
  async onProgramStart(ctx, out) {
    out.raw("never");
  },
}));