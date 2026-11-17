// Probes everything a post must not be able to reach. The completion value (a JSON string) goes back to the host.
function probe(f) { try { return String(f()); } catch (e) { return "throws " + e.name + ": " + e.message; } }
const G = globalThis;
const mustBeUndefined = ["Date", "console", "print", "std", "os", "bjson", "require", "process", "performance",
  "setTimeout", "setInterval", "queueMicrotask", "scriptArgs", "navigator", "fetch", "XMLHttpRequest", "WebSocket",
  "Worker", "Atomics", "SharedArrayBuffer", "ArrayBuffer", "WeakRef", "FinalizationRegistry", "Proxy",
  "atob", "btoa", "gc", "__loadScript", "loadScript", "load", "read", "readFile", "module", "exports", "importScripts"];
const present = mustBeUndefined.filter(n => typeof G[n] !== "undefined");
const probes = {
  "typeof Date": typeof Date,
  "typeof Math.random": typeof Math.random,
  "'random' in Math": "random" in Math,
  "new Date()": probe(() => new Date()),
  "Math.random()": probe(() => Math.random()),
  "eval('1+1')": probe(() => eval("1+1")),
  "(0,eval)('1+1')": probe(() => (0, eval)("1+1")),
  "Function('return 1')()": probe(() => Function("return 1")()),
  "(function(){}).constructor('return this')()": probe(() => (function () {}).constructor("return this")()),
  "console.log('x')": probe(() => console.log("x")),
  "std.open": probe(() => std.open("x", "w")),
  "os.exec": probe(() => os.exec(["cmd"])),
  "new RegExp('a+').test('aa')": probe(() => new RegExp("a+").test("aa")),
  "JSON round trip": probe(() => JSON.stringify(JSON.parse('{"a":[1,2]}'))),
  "new Map().size": probe(() => new Map().size),
  "import('std')": probe(() => import("std")),
  "(1234.5).toLocaleString()": probe(() => (1234.5).toLocaleString()),
  "new Error('x').stack": probe(() => new Error("x").stack.trim()),
  "typeof import('std').then": probe(() => typeof import("std").then),
  "async function call": probe(() => (async function () { return 1; })()),
  "generator constructor": probe(() => (function* () {}).constructor("yield 1")),
  "typeof Intl": typeof Intl,
};
const bad = present.length > 0 || probes["typeof Date"] !== "undefined" || probes["typeof Math.random"] !== "undefined" ||
  probes["'random' in Math"] !== false || !probes["eval('1+1')"].startsWith("throws") ||
  !probes["(0,eval)('1+1')"].startsWith("throws") || !probes["Function('return 1')()"].startsWith("throws") ||
  !probes["(function(){}).constructor('return this')()"].startsWith("throws");
JSON.stringify({ verdict: bad ? "LEAK" : "clean", present, probes,
  globals: Object.getOwnPropertyNames(G).sort(), math: Object.getOwnPropertyNames(Math).sort() });
