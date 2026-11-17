function depth(n) {
  return depth(n + 1) + 1; /*HERE*/
}
depth(0);
