function checkArc(m) {
  if (m.radius > 0.1) {
    throw new RangeError("bad arc radius " + m.radius.toFixed(3)); /*HERE*/
  }
}
definePost({
  onArc(m, out) {
    checkArc(m);
  },
});
