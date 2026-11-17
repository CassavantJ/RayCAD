// The interrupt must not be catchable, and finally blocks must not get a second life.
let n = 0;
try {
  for (;;) { n++; }
} catch (e) {
  for (;;) { n++; }
} finally {
  for (;;) { n++; }
}
