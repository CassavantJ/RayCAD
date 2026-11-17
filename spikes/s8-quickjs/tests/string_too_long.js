// String doubling: concatenation is lazy (ropes), so this hits the engine's maximum string length, not the heap limit.
let s = "0123456789abcdef";
for (;;) {
  s = s + s;
}
