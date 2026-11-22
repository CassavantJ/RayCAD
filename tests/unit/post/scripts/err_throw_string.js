definePost(testPost({
  onProgramStart(ctx, out) {
    throw "just a string";
  },
}));
