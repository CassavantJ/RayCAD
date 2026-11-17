# S8: QuickJS-NG as the post-processor host

## Question
Can QuickJS-NG run G-code post scripts fast enough, and can the host keep a script inside a sandbox: bounded memory, bounded CPU time, no clock, no random numbers, no I/O, and errors that name the script line?

## Short answer
Keep QuickJS-NG. A toy post in the shape of `docs/interfaces/post_api.d.ts` turns 1,000,000 moves into 15.6 MB of G-code in about 0.68 s on a quiet machine, with a JavaScript heap that never exceeds 0.19 MB. The output is byte-identical on every run and identical to a C++ reference that formats straight from integer nanometres.

Four things did not work the obvious way and need to go into the design:
1. `JS_Eval` does not work without the Eval intrinsic, and the Eval intrinsic is also what makes `eval()` and `Function("...")` work. The script has to be compiled in a second context.
2. The CPU-time interrupt is polled once per 10,000 loop back-edges or calls. A loop that spends its time inside native calls stops late: 7.4 s late in one test, 919 s late in another. A one-line engine patch fixes most of it.
3. The hard heap limit alone can leave the host with an exception that is `null`, with no message and no line. A soft limit checked in the interrupt handler gives a proper error with a line.
4. The heap limit does not cover the output buffer, which lives in the host. It needs its own limit.

## Setup
- Machine: Core i9-13900K (24 cores), 64 GB RAM, Windows 11 Home 10.0.26200. High tier. Nothing was run on a Low-tier machine. Other spike agents were building on the same machine and no core pinning was used, so timings carry noise.
- Compiler: MSVC 19.51.36260 (toolset 14.51, Visual Studio 2026), CMake 4.3.1 + Ninja, Release (`/O2 /Ob2 /DNDEBUG /MD`), built with `spikes\build.cmd s8-quickjs`.
- QuickJS-NG: tag `v0.18.0` (released 2026-11-01), commit `d7215b0ad3fc2c562e8317fd9bca058fd4863804`, MIT, shallow clone in `third_party/quickjs`. Only the `qjs` static library is built and linked. `quickjs-libc.c` (the `std`, `os`, `bjson` modules, `console`, `print`) is not compiled into the host.
- Host: `host.cpp`. Script: `posts/toy.post.js`. Tests: `tests/*.js`, run by `s8host test`.

How to repeat:
```
spikes\build.cmd s8-quickjs
cd spikes\s8-quickjs
build\s8host.exe test tests posts\toy.post.js
build\s8host.exe bench posts\toy.post.js 1000000 --repeat 10 --out build\js.nc
build\s8host.exe bench posts\toy.post.js 1000000 --reuse --repeat 10
build\s8host.exe native 1000000 --out build\native.nc
```
Switches (environment variables):
- `S8_SLOW=1` runs the 15-minute probe.
- `S8_SOFT_HEAP=0` uses the hard heap limit only.
- `S8_EVAL_IN_SANDBOX=1` shows the eval leak.
- `S8_PROMISE=0` shows the `import()` leak.

## Method
**Toolpath.** The host generates 1,000,000 moves with a fixed-seed generator: 30,000 rapids, 850,000 linear feeds, 120,000 arcs (quarter arcs and full circles). Coordinates are `int64` nanometres. A quarter of the values sit exactly on a rounding tie at three decimals of a millimetre, a quarter on whole micrometres, the rest are arbitrary. One linear move in 16 is zero-length, many change one axis only, and the feed changes on one cut in ten.

**Script API.** The toy post calls `definePost({...})` with `onProgramStart`, `onSectionStart`, `onRapid`, `onLinear`, `onArc`, `onSectionEnd`, `onProgramEnd`. For each move the host builds a move object (6 properties for a linear, 15 for an arc) with values in millimetres as doubles, and calls the handler. The script calls back into host functions `out.axis`, `out.feed`, `out.word`, `out.modal`, `out.block`. A linear move is 1 call into the script and 6 calls back. All number formatting and modal state are in the host. The host records a line map entry for every output line.

**Formatting.** `out.axis(letter, value)` snaps the double back to integer nanometres (`llround(value * 1e6)`), rounds half-even with integer arithmetic to 3 decimals, compares with the last value written for that letter, and returns the word or `null`. Words look like `X67.836`, `Y260.`, `Z-8.364`, `F1200.`.

**Reference.** `s8host native` produces the same program in C++ straight from the integer nanometres, with no JavaScript and no doubles.

**Sandbox.** The script context is `JS_NewContextRaw` plus base objects, RegExp, JSON, Map/Set and Promise. Not added: Date, Eval, Proxy, typed arrays and Atomics, WeakRef and FinalizationRegistry, `performance`, `atob`/`btoa`. `Math.random` and `queueMicrotask` are deleted after the base objects are created. No module loader is installed. The host never runs the promise job queue.

**Limits used by the host.**

| Limit | Mechanism | Value in the benchmark |
|---|---|---|
| Heap, hard | `JS_SetMemoryLimit` | 256 MB |
| Heap, soft | interrupt handler compares the allocator's live bytes with 7/8 of the hard limit | 224 MB |
| CPU time | interrupt handler reads `GetThreadTimes` (at most about once per millisecond) | 60 s |
| JS stack | `JS_SetMaxStackSize` | 256 KB, on the default 1 MB MSVC thread stack |
| Output size | checked in the host's `emit`; raises an uncatchable error | 1 GB |

The allocator passed to `JS_NewRuntime2` counts live and peak bytes with `_msize`; that is the source of the "JS heap" numbers. Process numbers come from `GetProcessMemoryInfo`.

## Measured: speed and memory for 1,000,000 moves
Release build, one thread. "Wall" covers creating the runtime, compiling the script, all moves and freeing the runtime.

| Variant | Wall time, 1 M moves | Per move |
|---|---|---|
| JavaScript post, new move object per move (the API as drafted) | 674–694 ms over 5 separate processes (quiet) | about 0.68 µs |
| JavaScript post, three move objects reused | 460–472 ms over 10 runs (quiet) | about 0.47 µs |
| C++ reference, no JavaScript | 93–105 ms over 4 runs | about 0.10 µs |

- Spread under load: other sessions on the same machine gave 675–1142 ms and 797–922 ms for the first variant, and 485–709 ms for the second. Machine load from other agents is the likely cause; that is a guess.
- Script compile plus runtime start-up: 0.3–0.6 ms. Teardown: 0.1–0.3 ms. Bytecode for the toy post: 2,963 bytes.
- Scaling: 100,000 moves took 70 ms; 10,000,000 moves took 7,042 ms. Linear.
- Output: 15,623,823 bytes, 947,566 lines (52,434 zero-length moves produced no line).

| Memory, 1 M run | Size |
|---|---|
| JavaScript heap, peak | 0.19 MB (also 0.19 MB at 10 M moves) |
| JavaScript heap after `JS_FreeRuntime` | 0 bytes |
| Process peak working set, whole run | 86–88 MB |
| Process peak working set before the post starts | 49.9 MB |
| of which the spike's toolpath array (1 M × 48 bytes) | 45.8 MB |
| Output text | 14.9 MB |
| Line map (947,566 × 12 bytes) | 10.8 MB |

The post adds about 37 MB to the process for 1 M moves, nearly all of it the output string and line map with their growth slack. The engine's share is 0.19 MB because QuickJS frees by reference count: each move object dies when its handler returns.

## Measured: sandbox tests
`s8host test` runs each script in a fresh runtime and exits 0 only if all pass. Final run on the unmodified v0.18.0 engine: all passed (the two native-sort probes report lateness without being judged).

### (a) Heap limit stops a runaway script cleanly: PASS
Limit 32 MB hard, 28 MB soft.

| Test | What the script does | Result | Heap peak | Time | Error reported |
|---|---|---|---|---|---|
| `heap_runaway.js` | pushes 1000-element arrays forever | heap-limit | 32.08 MB | 52 ms | `InternalError: out of memory`, line 3 |
| `heap_string.js` | keeps 200-character strings forever | heap-limit | 28.48 MB | 50 ms | `InternalError: interrupted`, line 1 (loop is on line 2) |
| `heap_big_single.js` | one 100 MB string | heap-limit | 0.17 MB | 0.3 ms | `InternalError: out of memory`, line 2 |
| `heap_in_callback.js` | retains objects inside `onLinear` | heap-limit | 28.57 MB | 61 ms | `InternalError: interrupted`, line 4 |
| `heap_catch.js` | wraps the allocation in `try/catch` and carries on | heap-limit | 32.08 MB | 51 ms | `InternalError: out of memory`, line 5 |
| `string_too_long.js` | doubles a string forever | script-error | 0.17 MB | 0.2 ms | `InternalError: string too long`, line 4 |

- "Cleanly" was checked three ways: the host keeps running; the allocator count is 0 after `JS_FreeRuntime`; and a normal post run afterwards in the same process gives the reference bytes (`toy_after_failures`).
- Peak was 0.08 MB above the 32 MB limit in two tests (the limit counts requested sizes, the allocator real block sizes).
- String doubling hits the engine's maximum string length before the heap limit, because concatenation is lazy. It is still stopped.
- With the soft limit off (`S8_SOFT_HEAP=0`), `heap_in_callback.js` ends with the pending exception equal to `null`: no message, no stack, no line.
- The out-of-memory error is an ordinary catchable exception. In `heap_catch.js` the script still died because the `catch` could not allocate either. The soft limit is uncatchable.

### (b) CPU-time interrupt stops an infinite loop: PASS for JavaScript loops, late for native-heavy loops
Limit 0.5 s of thread CPU time.

| Test | What the script does | Stopped after (CPU) | Line reported |
|---|---|---|---|
| `cpu_loop.js` | `for (;;) n++` at top level | 516 ms | 1 (loop is on line 2) |
| `cpu_loop_catch.js` | same inside `try`, with loops in `catch` and `finally` | 516 ms | 1; neither `catch` nor `finally` ran on |
| `cpu_in_callback.js` | `while (true)` inside `onLinear` | 516 ms | 3, correct |
| `cpu_regex.js` | catastrophic regex backtracking | 516 ms | 2, correct |
| `cpu_native_sort.js` | `for (;;) { a.sort(); a.reverse(); }` on 20,000 strings | 7,906 ms (7.4 s late) | 3 |
| `cpu_native_sort_big.js` | same on 1,000,000 strings, limit 2 s | 921,359 ms (919 s late) | 3 |

- `GetThreadTimes` resolution is 15.6 ms here, hence 515.6 everywhere.
- The handler is polled only every 10,000 back-edges or calls (`JS_INTERRUPT_COUNTER_INIT` in `quickjs.c`, not configurable). The 1 M-move benchmark polled 855 times in 0.68 s, so ordinary posts are polled about every 0.8 ms.
- Changing that constant was tried (engine source restored afterwards):

| `JS_INTERRUPT_COUNTER_INIT` | 20k-string sort loop (limit 0.5 s) | 1M-string sort loop (limit 2 s) | 1 M-move benchmark |
|---|---|---|---|
| 10000 (stock) | 7.4–9.2 s late | 919 s late | 662–720 ms |
| 100 | on time | 8.9 s late | 662–716 ms |
| 1 | on time | 0.19 s late | 766–854 ms |

- Even at 1, one native call cannot be interrupted. `probe_sort_time.js` (build 1,000,000 strings, sort, reverse, sort) took 0.78–1.33 s in total. The heap limit bounds how big such a call can get.

### (c) `Date`, `Math.random` and I/O are unreachable: PASS
`sandbox_globals.js` probes from inside the script. Verdict "clean".

| Probe | Result |
|---|---|
| `typeof Date` | `undefined`; `new Date()` throws `ReferenceError: Date is not defined` |
| `typeof Math.random` | `undefined`; `"random" in Math` is `false`; calling it throws `TypeError: not a function` |
| `console.log("x")`, `std.open(...)`, `os.exec(...)` | each throws `ReferenceError: ... is not defined` |
| `eval("1+1")`, `(0, eval)("1+1")` | throws `TypeError: eval is not supported` |
| `Function("return 1")()`, `(function(){}).constructor("return this")()`, generator constructor | throws `TypeError: eval is not supported` |
| `import * as std from "std"` (`sandbox_import.js`) | `SyntaxError` at line 1 |
| `import("std")` | returns a promise that is never settled (host never runs jobs; no module loader) |
| 34 other names (`print`, `require`, `process`, `performance`, `setTimeout`, `queueMicrotask`, `fetch`, `Worker`, `Atomics`, `SharedArrayBuffer`, `ArrayBuffer`, `WeakRef`, `FinalizationRegistry`, `Proxy`, `atob`, `gc`, …) | all `undefined` |
| `(1234.5).toLocaleString()` | `1234.5`; `typeof Intl` is `undefined` |
| `new Error("x").stack` | script name, line and column only; no path, no time |

Complete list of globals visible to the script: `AggregateError`, `Array`, `AsyncDisposableStack`, `BigInt`, `Boolean`, `DisposableStack`, `Error`, `EvalError`, `Function`, `Infinity`, `InternalError`, `Iterator`, `JSON`, `Map`, `Math`, `NaN`, `Number`, `Object`, `Promise`, `RangeError`, `ReferenceError`, `Reflect`, `RegExp`, `Set`, `String`, `SuppressedError`, `Symbol`, `SyntaxError`, `TypeError`, `URIError`, `WeakMap`, `WeakSet`, `decodeURI`, `decodeURIComponent`, `encodeURI`, `encodeURIComponent`, `escape`, `eval` (throws), `globalThis`, `isFinite`, `isNaN`, `parseFloat`, `parseInt`, `undefined`, `unescape`, plus the host's `definePost`.

Two results that changed the host design:
- With the Eval intrinsic in the script context (`S8_EVAL_IN_SANDBOX=1`) the same test reports "LEAK": `eval("1+1")` gives 2 and `(function(){}).constructor("return this")()` returns the global object. Without it, `JS_Eval` itself fails with `TypeError: eval is not supported`.
- With Promise left out (`S8_PROMISE=0`), three `import("std")` calls leave 8,696 bytes allocated after `JS_FreeRuntime`. With Promise added the count is 0. This looks like an engine leak in an unusual configuration; not reported upstream.

### (d) A script exception reports file name and line: PASS, with three exceptions

| Test | Reported | Expected line |
|---|---|---|
| `err_syntax.js` | `SyntaxError: Unexpected identifier 'out'` at `err_syntax.js:4:20` | 4 |
| `err_toplevel.js` | `ReferenceError: notDefinedAnywhere is not defined` at `err_toplevel.js:3:19` | 3 |
| `err_runtime.js` | `TypeError: cannot read property 'number' of null` at `onLinear (err_runtime.js:7:23)` | 7 |
| `err_nested.js` | `RangeError: bad arc radius 2.080` at `checkArc (err_nested.js:3:55)`, then `onArc (err_nested.js:8:14)` | 3 |
| `err_fail.js` | `Error: tool 7 has no length offset` at `onSectionStart (err_fail.js:4:48)` | 4 |
| `stack_overflow.js` | `RangeError: Maximum call stack size exceeded` at `depth (stack_overflow.js:2:20)` | 2 |
| `err_throw_string.js` | message `just a string` only | none available |

File and line come from the error's `stack` property; the host takes the first frame naming the script. Exceptions:
- A thrown value that is not an `Error` has no stack, so no line.
- An interrupt (CPU or soft heap) raised in top-level code reported line 1 in three tests where the loop was on line 2 or 3. Inside a function the line was right.
- The hard heap limit can produce a `null` exception (see (a)).

### Other robustness checks
- `native_recursion.js`: `JSON.parse` of 200,000 nested arrays, a regex with 50,000 nested groups, `String()` and `JSON.stringify` of a 200,000-deep array. Each ended in a catchable `RangeError: Maximum call stack size exceeded` or `SyntaxError: too many captures`. The host did not crash, with a 256 KB JS stack on a 1 MB thread stack.
- `output_flood.js`: a loop calling `out.raw` with 1,000-character lines forever, swallowing exceptions, was stopped by the host's output limit (16 MB in the test) after 21 ms, at line 5.

## Determinism
- 10 runs in one process (fresh runtime each): identical FNV-1a hash `03b3f4c90419f020`, for both variants.
- Files from 5 separate processes plus the C++ reference: one SHA-256, `64fec35d822a48405dd9fa56a6027c3f8ebc5caaea9642c7097527f0a613ba29`.
- The JavaScript output equals the integer-only C++ reference byte for byte, including values on rounding ties. This holds because the host snaps script doubles back to integer nanometres before rounding.
- Also identical with the interrupt constant patched to 1.
- Not measured: other operating systems, compilers, CPUs. Known risk: `Math.sin`, `cos`, `tan`, `pow`, `exp`, `log` and friends call the C runtime directly (`quickjs.c` around line 49126), and C runtimes can differ in the last bit. The toy post does not use them. Number-to-text conversion uses the engine's own `dtoa.c`.

## MSVC build notes
- Built on the first attempt with no source changes. Full build 33 s, nearly all of it `quickjs.c`. `add_subdirectory(third_party/quickjs EXCLUDE_FROM_ALL)` plus linking `qjs` is enough.
- Upstream compiles with `-std:c11 /experimental:c11atomics -Wall` and a list of disabled warnings. About 35 warnings still print from `quickjs.c` (C4146, C4701, C4703, C4702, C4098, C4334, C4132, C4668). None from `quickjs.h` included from C++20 at `/W4 /permissive-`.
- `/experimental:c11atomics` is required on MSVC.
- The engine's default JS stack limit (1 MB) equals MSVC's default thread stack. Upstream sets `/STACK:8388608` only for its own executables. An embedder must call `JS_SetMaxStackSize` with something smaller, or use a thread with a bigger stack.
- A custom allocator needs a usable-size function; on Windows that is `_msize`.
- `JS_INTERRUPT_COUNTER_INIT` has no `#ifndef`, so changing it means carrying a one-line patch.
- Sizes: `qjs.lib` 2.9 MB, `s8host.exe` 1.1 MB. Clone 5.8 MB, build folder 9 MB.

## Findings that affect the design
1. **Compile in a second context.** A throwaway context in the same runtime gets base objects, the RegExp compiler and Eval. Compile with `JS_EVAL_FLAG_COMPILE_ONLY`, serialise with `JS_WriteObject`, read into the script context with `JS_ReadObject`, run with `JS_EvalFunction`. The script context never gets Eval. Both contexts share the runtime, so the heap limit and interrupt cover compilation. Syntax errors come back with file and line. Cost under 0.6 ms.
2. **CPU limit and cancel are only as prompt as the poll.** UI cancel goes through the same handler, so it has the same lag.
3. **Soft heap limit in the interrupt handler, hard limit behind it.** The soft limit gives an uncatchable error with a stack. The hard limit is still needed for one big allocation between polls.
4. **Limit the output and the warnings list in the host.** They are host memory.
5. **Add Promise, never run jobs.** It avoids the `import()` leak. An `async` handler would silently do nothing, so the host should reject a handler that returns a promise (not implemented in the spike).
6. **Reusing move objects saves about 30 %** but lets a script see a move change under it if it keeps a reference. Not needed at these speeds.

## Not measured
- Low-tier hardware, macOS, Linux, ARM, other compilers.
- A realistic post. The toy has no tool table, canned cycles, per-move comments or sequence numbers.
- Cross-platform equality of the output.
- Inch output: whether `llround(value * 25.4e6)` always recovers the original nanometre value was not tested.
- The engine's own test suite under MSVC.
- Fuzzing.
- Wall-clock limits. Only thread CPU time is limited.

## Recommendation
Keep QuickJS-NG, pinned at v0.18.0 or later, built as the plain `qjs` library without `quickjs-libc`.

| Limit | Suggestion | Basis |
|---|---|---|
| Heap, hard | 64 MB | toy peaked at 0.19 MB; 64 MB is a guess at generous |
| Heap, soft | 7/8 of hard, checked in the interrupt handler | measured: gives a line where the hard limit gave `null` |
| JS stack | 256 KB with a thread stack of at least 1 MB | measured |
| CPU time | 10 s plus 20 µs per move, instead of a flat 60 s | measured 0.7 µs per move; the factor of about 30 is a guess |
| Output size | 1 MB plus 256 bytes per move | measured 15.6 bytes per move; the factor is a guess |
| Interrupt polling | patch `JS_INTERRUPT_COUNTER_INIT` to 100, or ask upstream for a setter | measured: no cost at 100, 15–25 % at 1 |

With polling at 100 a hostile script can still overrun the CPU limit by 100 times its longest native call (8.9 s late in the worst case tried). If that is not acceptable, use 1 and accept the slowdown, or run posts in a child process that can be killed.
