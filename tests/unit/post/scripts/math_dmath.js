// The trigonometric functions come from the host's deterministic library.
definePost(testPost({
  onProgramStart(ctx, out) {
    const xs = [0.5, -1.25, 3, 1e-3];
    for (const x of xs) {
      out.raw([Math.sin(x), Math.cos(x), Math.tan(x), Math.atan(x), Math.atan2(x, 2), Math.hypot(x, 3),
        Math.asin(x / 4), Math.acos(x / 4)].join(" "));
    }
  },
}));