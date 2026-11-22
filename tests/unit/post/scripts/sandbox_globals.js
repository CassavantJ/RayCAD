// Probes everything a post must not reach (after spike S8's probe). The result goes to the output as one JSON line.
function probe(f) { try { return String(f()); } catch (e) { return "throws " + e.name + ": " + e.message; } }
const G = globalThis;
const mustBeUndefined = ["Date", "console", "print", "std", "os", "bjson", "require", "process", "performance",
  "setTimeout", "setInterval", "clearTimeout", "clearInterval", "setImmediate", "queueMicrotask", "scriptArgs",
  "navigator", "fetch", "XMLHttpRequest", "WebSocket", "Worker", "Atomics", "SharedArrayBuffer", "ArrayBuffer",
  "WeakRef", "FinalizationRegistry", "Proxy", "atob", "btoa", "gc", "__loadScript", "loadScript", "load", "read",
  "readFile", "module", "exports", "importScripts", "Intl", "Deno", "Bun"];
const present = mustBeUndefined.filter((n) => typeof G[n] !== "undefined");
const probes = {
  "typeof Date": typeof Date,
  "typeof Math.random": typeof Math.random,
  "'random' in Math": "random" in Math,
  "new Date()": probe(() => new Date()),
  "Date.now()": probe(() => Date.now()),
  "Math.random()": probe(() => Math.random()),
  "eval('1+1')": probe(() => eval("1+1")),
  "(0,eval)('1+1')": probe(() => (0, eval)("1+1")),
  "globalThis.eval('1+1')": probe(() => globalThis.eval("1+1")),
  "Function('return 1')()": probe(() => Function("return 1")()),
  "new Function('return 1')()": probe(() => new Function("return 1")()),
  "(function(){}).constructor('return this')()": probe(() => (function () {}).constructor("return this")()),
  "generator constructor": probe(() => (function* () {}).constructor("yield 1")),
  "async constructor": probe(() => (async function () {}).constructor("return 1")),
  "setTimeout(f, 0)": probe(() => setTimeout(() => {}, 0)),
  "console.log('x')": probe(() => console.log("x")),
  "std.open": probe(() => std.open("x", "w")),
  "os.exec": probe(() => os.exec(["cmd"])),
  "require('fs')": probe(() => require("fs")),
  "typeof import('std').then": probe(() => typeof import("std").then),
  "Error.prepareStackTrace": typeof Error.prepareStackTrace,
  "Error.captureStackTrace": typeof Error.captureStackTrace,
  "(1234.5).toLocaleString()": probe(() => (1234.5).toLocaleString()),
  "new RegExp('a+').test('aa')": probe(() => new RegExp("a+").test("aa")),
  "JSON round trip": probe(() => JSON.stringify(JSON.parse('{"a":[1,2]}'))),
  "new Map().size": probe(() => new Map().size),
  "Promise": typeof Promise,
};
const leaks = present.length > 0 || probes["typeof Date"] !== "undefined" ||
  probes["typeof Math.random"] !== "undefined" || probes["'random' in Math"] !== false ||
  probes["Error.prepareStackTrace"] !== "undefined" || probes["Error.captureStackTrace"] !== "undefined" ||
  ["new Date()", "Date.now()", "Math.random()", "eval('1+1')", "(0,eval)('1+1')", "globalThis.eval('1+1')",
    "Function('return 1')()", "new Function('return 1')()", "(function(){}).constructor('return this')()",
    "generator constructor", "async constructor", "setTimeout(f, 0)", "console.log('x')", "std.open", "os.exec",
    "require('fs')"].some((k) => !probes[k].startsWith("throws"));
// import() gives a promise that never settles: the host has no module loader and never runs promise jobs.
let importSettled = false;
import("std").then(() => { importSettled = true; }, () => { importSettled = true; });
definePost(testPost({
  onProgramStart(ctx, out) {
    out.raw(JSON.stringify({ verdict: leaks ? "LEAK" : "clean", importSettled, present, probes,
      globals: Object.getOwnPropertyNames(G).sort() }));
  },
}));
