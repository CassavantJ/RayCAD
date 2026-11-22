// Deep recursion inside native code (JSON parser, regexp compiler, nested array join) must not crash the host.
function probe(f) { try { return String(f()).slice(0, 40); } catch (e) { return "throws " + e.name + ": " + String(e.message).slice(0, 50); } }
const r = {
  jsonDeep: probe(() => JSON.parse("[".repeat(200000) + "]".repeat(200000)).length),
  regexDeep: probe(() => new RegExp("(".repeat(50000) + "a" + ")".repeat(50000)).test("a")),
  nestedJoin: probe(() => { let a = []; for (let i = 0; i < 200000; i++) a = [a]; return String(a).length; }),
  nestedStringify: probe(() => { let a = []; for (let i = 0; i < 200000; i++) a = [a]; return JSON.stringify(a).length; }),
};
definePost(testPost({ onProgramStart(ctx, out) { out.raw("survived " + JSON.stringify(r)); } }));
