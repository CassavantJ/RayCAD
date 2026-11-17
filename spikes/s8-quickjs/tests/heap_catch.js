// A hostile script that swallows the out-of-memory error and carries on.
const keep = [];
for (;;) {
  try {
    keep.push(new Array(1000).fill(1.5));
  } catch (e) {
    // ignore and keep going
  }
}
