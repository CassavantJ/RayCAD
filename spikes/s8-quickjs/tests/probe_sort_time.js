// Not a pass/fail test: how long one native sort of 1,000,000 short strings takes is measured by the host's wall clock.
const a = [];
for (let i = 0; i < 1000000; i++) a.push("k" + ((i * 7919) % 1000003));
a.sort();
a.reverse();
a.sort();
"sorted " + a.length;
