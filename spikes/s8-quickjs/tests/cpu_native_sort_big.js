// Long-running native code with no JS callbacks: the interrupt can only be seen between calls.
const a = [];
for (let i = 0; i < 1000000; i++) a.push("k" + ((i * 7919) % 1000003));
for (;;) {
  a.sort();
  a.reverse();
}
