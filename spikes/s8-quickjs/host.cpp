// Spike S8: QuickJS-NG as the sandboxed host for G-code post scripts. Throwaway code.
//   s8host bench  <script> <nMoves> [--reuse] [--repeat N] [--out file]
//   s8host native <nMoves> [--out file]            C++ baseline, same formatting rules, no JS
//   s8host test   <tests-dir> <toy-post>           sandbox / limit / error-reporting tests
#include <windows.h>
#include <psapi.h>
#include <malloc.h>
#include <intrin.h>

#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include "quickjs.h"
}

// ---------------------------------------------------------------- utilities
static double wallSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}
static double threadCpuSeconds() {   // user + kernel time of the calling thread
    FILETIME c, e, k, u;
    GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u);
    auto t = [](FILETIME f) { return double((uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime) * 1e-7; };
    return t(k) + t(u);
}
static size_t processPeakWorkingSet() {
    PROCESS_MEMORY_COUNTERS pmc{};
    K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc);
    return pmc.PeakWorkingSetSize;
}
static size_t processPeakCommit() {
    PROCESS_MEMORY_COUNTERS pmc{};
    K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc);
    return pmc.PeakPagefileUsage;
}
static uint64_t fnv1a64(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h;
}
static bool readFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss; ss << f.rdbuf(); out = ss.str();
    return true;
}
static std::string baseName(const std::string& p) {
    size_t i = p.find_last_of("/\\");
    return i == std::string::npos ? p : p.substr(i + 1);
}
static double mb(size_t b) { return double(b) / (1024.0 * 1024.0); }

// ---------------------------------------------------------------- toolpath (integer nanometres)
enum MoveKind : uint8_t { Rapid, Linear, ArcCW, ArcCCW };
struct Move { int64_t x, y, z, cx, cy; int32_t feed; MoveKind kind; bool full; };

static uint64_t g_rng;
static uint64_t rnd() {   // splitmix64, fixed seed: the toolpath is the same on every run
    uint64_t z = (g_rng += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static int64_t rndRange(int64_t lo, int64_t hi) { return lo + int64_t(rnd() % uint64_t(hi - lo + 1)); }
static int64_t snap(int64_t v) {   // a quarter of values land exactly on a rounding tie at 3 decimals of a mm
    switch (rnd() & 3) {
        case 0: return (v / 1000) * 1000 + (v < 0 ? -500 : 500);
        case 1: return (v / 1000) * 1000;
        default: return v;
    }
}
static std::vector<Move> makeToolpath(size_t n) {
    g_rng = 0x5EED5EED5EED5EEDull;
    std::vector<Move> mv; mv.reserve(n);
    const int64_t MM = 1000000;
    int64_t x = 0, y = 0, z = 25 * MM;
    auto push = [&](MoveKind k, int32_t feed, int64_t cx = 0, int64_t cy = 0, bool full = false) {
        if (mv.size() < n) mv.push_back(Move{x, y, z, cx, cy, feed, k, full});
    };
    while (mv.size() < n) {
        x = snap(rndRange(0, 500 * MM)); y = snap(rndRange(0, 500 * MM)); z = 25 * MM; push(Rapid, 0);
        z = 2 * MM; push(Rapid, 0);
        z = -snap(rndRange(1 * MM, 10 * MM)); push(Linear, 300);
        for (int i = 0; i < 96; ++i) {
            int32_t feed = (rnd() % 10 == 0) ? 800 : 1200;
            if (i % 8 == 7) {
                int64_t rx = snap(rndRange(MM / 2, 5 * MM)) * ((rnd() & 1) ? 1 : -1);
                int64_t ry = snap(rndRange(MM / 2, 5 * MM)) * ((rnd() & 1) ? 1 : -1);
                int64_t cx = x + rx, cy = y + ry;
                bool cw = rnd() & 1, full = (i % 32 == 31);
                if (!full) {   // quarter turn about the centre: exact in integers
                    int64_t vx = -rx, vy = -ry;
                    if (cw) { x = cx + vy; y = cy - vx; } else { x = cx - vy; y = cy + vx; }
                }
                push(cw ? ArcCW : ArcCCW, feed, cx, cy, full);
            } else {
                uint64_t r = rnd() % 16;
                if (r == 0) { /* zero-length move: must be suppressed entirely */ }
                else if (r < 6) x = snap(x + rndRange(-5 * MM, 5 * MM));
                else if (r < 11) y = snap(y + rndRange(-5 * MM, 5 * MM));
                else { x = snap(x + rndRange(-5 * MM, 5 * MM)); y = snap(y + rndRange(-5 * MM, 5 * MM)); }
                push(Linear, feed);
            }
        }
        z = 25 * MM; push(Rapid, 0);
    }
    return mv;
}

// ---------------------------------------------------------------- number formatting (integer only)
static const int64_t kPow10[] = {1, 10, 100, 1000, 10000, 100000, 1000000};
static int64_t divHalfEven(int64_t v, int64_t d) {
    int64_t q = v / d, r = v % d, ar = r < 0 ? -r : r;
    if (2 * ar > d || (2 * ar == d && (q & 1))) q += (v < 0 ? -1 : 1);
    return q;
}
// q is the value scaled by 10^decimals. Trailing zeros trimmed, decimal point always kept, never "-0".
static void appendFixed(std::string& s, int64_t q, int decimals) {
    if (q < 0) { s += '-'; q = -q; }
    char buf[32];
    auto r = std::to_chars(buf, buf + sizeof buf, q / kPow10[decimals]);
    s.append(buf, r.ptr);
    s += '.';
    int64_t fp = q % kPow10[decimals];
    if (fp) {
        char fb[8];
        for (int i = decimals - 1; i >= 0; --i) { fb[i] = char('0' + fp % 10); fp /= 10; }
        int len = decimals;
        while (len > 0 && fb[len - 1] == '0') --len;
        s.append(fb, size_t(len));
    }
}

// ---------------------------------------------------------------- output + modal state (shared by JS host and native baseline)
struct LineMap { uint32_t line, section, move; };
struct Writer {
    std::string out;
    std::vector<LineMap> lineMap;
    std::vector<std::string> warnings;
    size_t maxOut = size_t(1) << 30;
    bool outLimitHit = false;
    uint32_t lineNo = 0, curSection = 0, curMove = 0;
    int64_t axisLast[26]{}; bool axisHas[26]{};
    int64_t feedLast = 0; bool feedHas = false;
    std::vector<std::pair<std::string, std::string>> modal;
    std::string scratch;

    bool emit(const char* p, size_t n) {
        if (out.size() + n + 2 > maxOut) { outLimitHit = true; return false; }
        out.append(p, n); out += "\r\n";
        lineMap.push_back(LineMap{lineNo++, curSection, curMove});
        return true;
    }
    void reset() { memset(axisHas, 0, sizeof axisHas); feedHas = false; modal.clear(); }
    // Returns false when unchanged. nm is the exact position; the word carries 3 decimals of a millimetre.
    bool axis(char letter, int64_t nm, std::string& word) {
        int idx = (letter | 32) - 'a';
        if (idx < 0 || idx > 25) return false;
        int64_t q = divHalfEven(nm, 1000);
        if (axisHas[idx] && axisLast[idx] == q) return false;
        axisHas[idx] = true; axisLast[idx] = q;
        word.clear(); word += letter; appendFixed(word, q, 3);
        return true;
    }
    bool feed(int64_t tenths, std::string& word) {
        if (feedHas && feedLast == tenths) return false;
        feedHas = true; feedLast = tenths;
        word.clear(); word += 'F'; appendFixed(word, tenths, 1);
        return true;
    }
    bool modalWord(const std::string& group, const std::string& word) {
        for (auto& g : modal) if (g.first == group) { if (g.second == word) return false; g.second = word; return true; }
        modal.emplace_back(group, word);
        return true;
    }
};

// ---------------------------------------------------------------- native baseline (mirrors posts/toy.post.js)
static void nativePost(const std::vector<Move>& mv, Writer& w) {
    auto line = [&](const std::string& s) { w.emit(s.data(), s.size()); };
    line("%"); line("(S8 SPIKE PROGRAM)"); line("G90 G21 G17");
    line("(TOY SECTION)"); line("T7 M6"); line("S12000 M3"); w.reset();
    std::string wx, wy, wz, wf, l;
    int64_t px = 0, py = 0;
    for (size_t i = 0; i < mv.size(); ++i) {
        const Move& m = mv[i];
        w.curMove = uint32_t(i);
        bool hx = w.axis('X', m.x, wx), hy = w.axis('Y', m.y, wy), hz = w.axis('Z', m.z, wz);
        l.clear();
        auto add = [&](const std::string& s) { if (!l.empty()) l += ' '; l += s; };
        if (m.kind == Rapid || m.kind == Linear) {
            if (hx || hy || hz) {
                const char* g = m.kind == Rapid ? "G0" : "G1";
                if (w.modalWord("motion", g)) add(g);
                if (hx) add(wx); if (hy) add(wy); if (hz) add(wz);
                if (m.kind == Linear && w.feed(int64_t(m.feed) * 10, wf)) add(wf);
                line(l);
            }
        } else {
            const char* g = m.kind == ArcCW ? "G2" : "G3";
            if (w.modalWord("motion", g)) add(g);
            if (hx) add(wx); if (hy) add(wy); if (hz) add(wz);
            std::string t;
            t = "I"; appendFixed(t, divHalfEven(m.cx - px, 1000), 3); add(t);
            t = "J"; appendFixed(t, divHalfEven(m.cy - py, 1000), 3); add(t);
            if (w.feed(int64_t(m.feed) * 10, wf)) add(wf);
            line(l);
        }
        px = m.x; py = m.y;
    }
    line("M5"); line("M30"); line("%");
}

// ---------------------------------------------------------------- QuickJS host
struct AllocStats { size_t cur = 0, peak = 0; };
static void* tMalloc(void* o, size_t n) {
    void* p = malloc(n);
    if (p) { auto* s = static_cast<AllocStats*>(o); s->cur += _msize(p); if (s->cur > s->peak) s->peak = s->cur; }
    return p;
}
static void* tCalloc(void* o, size_t c, size_t n) {
    void* p = calloc(c, n);
    if (p) { auto* s = static_cast<AllocStats*>(o); s->cur += _msize(p); if (s->cur > s->peak) s->peak = s->cur; }
    return p;
}
static void tFree(void* o, void* p) {
    if (!p) return;
    static_cast<AllocStats*>(o)->cur -= _msize(p);
    free(p);
}
static void* tRealloc(void* o, void* p, size_t n) {
    if (!p) return tMalloc(o, n);
    if (n == 0) { tFree(o, p); return nullptr; }
    auto* s = static_cast<AllocStats*>(o);
    size_t old = _msize(p);
    void* q = realloc(p, n);
    if (!q) return nullptr;
    s->cur += _msize(q); s->cur -= old;
    if (s->cur > s->peak) s->peak = s->cur;
    return q;
}
static size_t tUsable(const void* p) { return _msize(const_cast<void*>(p)); }

struct Limits {
    size_t heapBytes = size_t(256) << 20;
    double cpuSeconds = 60.0;
    size_t stackBytes = 256 * 1024;       // JS stack; the exe keeps MSVC's default 1 MB thread stack
    size_t maxOutputBytes = size_t(1) << 30;
};
struct RunResult {
    std::string kind = "ok";              // ok | script-error | heap-limit | cpu-limit | output-limit
    std::string message, stack, file, completion;
    int line = -1, col = -1;
    double wallLoad = 0, wallMoves = 0, wallTeardown = 0, wallTotal = 0, cpuTotal = 0;
    size_t jsHeapPeak = 0, jsHeapAtEnd = 0, jsHeapAfterFree = 0;
    uint64_t interruptCalls = 0;
    size_t bytecodeBytes = 0;
    std::string out; size_t lines = 0, lineMapEntries = 0, warnings = 0;
};
static bool g_softHeap = true;          // S8_SOFT_HEAP=0: hard JS_SetMemoryLimit only
static bool g_promise = true;           // S8_PROMISE=0: leave Promise out (then import() leaks, see REPORT.md)
static bool g_evalInSandbox = false;   // S8_EVAL_IN_SANDBOX=1: demonstrates why the two-context compile is needed
struct Host {
    Writer w;
    JSValue postDef = JS_UNDEFINED;
    double cpuStart = 0, cpuLimit = 0;
    bool cpuHit = false, heapHit = false, armed = true;
    const AllocStats* stats = nullptr; size_t softHeap = 0;
    uint64_t interruptCalls = 0, clockReads = 0;
    unsigned long long lastTsc = 0;
};
static Host* hostOf(JSContext* ctx) { return static_cast<Host*>(JS_GetContextOpaque(ctx)); }

static int interruptHandler(JSContext* ctx, void* opaque) {
    (void)ctx;
    Host* h = static_cast<Host*>(opaque);
    ++h->interruptCalls;
    if (!h->armed) return 0;
    // Soft heap limit: stopping here gives an uncatchable error that still has a stack (file and line).
    // JS_SetMemoryLimit stays as the hard backstop for a single huge allocation between two polls.
    if (h->softHeap && h->stats->cur > h->softHeap) { h->heapHit = true; return 1; }
    // GetThreadTimes is a system call: ask at most about once per millisecond (3e6 TSC ticks; the tick rate is a guess,
    // it only sets how often the clock is read). This keeps the handler cheap if the engine is patched to poll more often.
    unsigned long long tsc = __rdtsc();
    if (tsc - h->lastTsc < 3000000ull) return 0;
    h->lastTsc = tsc;
    ++h->clockReads;
    if (threadCpuSeconds() - h->cpuStart > h->cpuLimit) { h->cpuHit = true; return 1; }
    return 0;
}
static JSValue throwUncatchable(JSContext* ctx, const char* msg) {
    JS_ThrowInternalError(ctx, "%s", msg);
    JSValue e = JS_GetException(ctx);
    JS_SetUncatchableError(ctx, e);
    return JS_Throw(ctx, e);
}
static JSValue emitOrThrow(JSContext* ctx, Host* h, const char* p, size_t n) {
    if (!h->w.emit(p, n)) return throwUncatchable(ctx, "post output limit exceeded");
    return JS_UNDEFINED;
}
static JSValue jsDefinePost(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Host* h = hostOf(ctx);
    if (argc < 1 || !JS_IsObject(argv[0])) return JS_ThrowTypeError(ctx, "definePost: object expected");
    if (!JS_IsUndefined(h->postDef)) return JS_ThrowTypeError(ctx, "definePost called twice");
    h->postDef = JS_DupValue(ctx, argv[0]);
    return JS_UNDEFINED;
}
static JSValue jsBlock(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Host* h = hostOf(ctx);
    std::string& s = h->w.scratch; s.clear();
    for (int i = 0; i < argc; ++i) {
        if (JS_IsNull(argv[i]) || JS_IsUndefined(argv[i])) continue;
        size_t n; const char* p = JS_ToCStringLen(ctx, &n, argv[i]);
        if (!p) return JS_EXCEPTION;
        if (n) { if (!s.empty()) s += ' '; s.append(p, n); }
        JS_FreeCString(ctx, p);
    }
    if (s.empty()) return JS_UNDEFINED;
    return emitOrThrow(ctx, h, s.data(), s.size());
}
static JSValue jsRaw(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    size_t n; const char* p = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!p) return JS_EXCEPTION;
    JSValue r = emitOrThrow(ctx, hostOf(ctx), p, n);
    JS_FreeCString(ctx, p);
    return r;
}
static JSValue jsComment(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    size_t n; const char* p = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!p) return JS_EXCEPTION;
    std::string s = "(";
    for (size_t i = 0; i < n; ++i) {   // toy sanitiser: upper-case ASCII, no nested parentheses
        unsigned char c = static_cast<unsigned char>(p[i]);
        if (c == '(' || c == ')' || c < 32 || c > 126) c = ' ';
        s += char(toupper(c));
    }
    s += ')';
    JS_FreeCString(ctx, p);
    return emitOrThrow(ctx, hostOf(ctx), s.data(), s.size());
}
static bool letterArg(JSContext* ctx, JSValueConst v, char& letter) {
    size_t n; const char* p = JS_ToCStringLen(ctx, &n, v);
    if (!p) return false;
    letter = n ? p[0] : '?';
    JS_FreeCString(ctx, p);
    return true;
}
// Script values are doubles in millimetres. Snap back to the integer-nanometre grid, then format with integers only.
static bool toNm(JSContext* ctx, JSValueConst v, int64_t& nm) {
    double d;
    if (JS_ToFloat64(ctx, &d, v)) return false;
    if (!std::isfinite(d) || std::fabs(d) > 1e9) { JS_ThrowRangeError(ctx, "coordinate is not a finite number in range"); return false; }
    nm = std::llround(d * 1e6);
    return true;
}
static JSValue jsAxis(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Host* h = hostOf(ctx);
    char letter; int64_t nm;
    if (argc < 2) return JS_ThrowTypeError(ctx, "axis(letter, value)");
    if (!letterArg(ctx, argv[0], letter) || !toNm(ctx, argv[1], nm)) return JS_EXCEPTION;
    std::string& s = h->w.scratch;
    if (!h->w.axis(letter, nm, s)) return JS_NULL;
    return JS_NewStringLen(ctx, s.data(), s.size());
}
static JSValue jsWord(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    char letter; int64_t nm; int decimals = 3;
    if (argc < 2) return JS_ThrowTypeError(ctx, "word(letter, value, fmt?)");
    if (!letterArg(ctx, argv[0], letter) || !toNm(ctx, argv[1], nm)) return JS_EXCEPTION;
    if (argc > 2 && JS_IsObject(argv[2])) {
        JSValue d = JS_GetPropertyStr(ctx, argv[2], "decimals");
        int32_t di = 3;
        if (JS_ToInt32(ctx, &di, d)) return JS_EXCEPTION;
        JS_FreeValue(ctx, d);
        decimals = di < 0 ? 0 : di > 6 ? 6 : di;
    }
    std::string s; s += letter;
    appendFixed(s, divHalfEven(nm, kPow10[6 - decimals]), decimals);
    return JS_NewStringLen(ctx, s.data(), s.size());
}
static JSValue jsFeed(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Host* h = hostOf(ctx);
    int64_t nm;
    if (argc < 1) return JS_ThrowTypeError(ctx, "feed(value)");
    if (!toNm(ctx, argv[0], nm)) return JS_EXCEPTION;
    std::string& s = h->w.scratch;
    if (!h->w.feed(divHalfEven(nm, 100000), s)) return JS_NULL;
    return JS_NewStringLen(ctx, s.data(), s.size());
}
static JSValue jsModal(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Host* h = hostOf(ctx);
    if (argc < 2) return JS_ThrowTypeError(ctx, "modal(group, word)");
    size_t gn, wn;
    const char* g = JS_ToCStringLen(ctx, &gn, argv[0]);
    if (!g) return JS_EXCEPTION;
    const char* wd = JS_ToCStringLen(ctx, &wn, argv[1]);
    if (!wd) { JS_FreeCString(ctx, g); return JS_EXCEPTION; }
    bool changed = h->w.modalWord(std::string(g, gn), std::string(wd, wn));
    JS_FreeCString(ctx, g); JS_FreeCString(ctx, wd);
    return changed ? JS_DupValue(ctx, argv[1]) : JS_NULL;
}
static JSValue jsResetModal(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    hostOf(ctx)->w.reset();   // toy: resets every group
    return JS_UNDEFINED;
}
static JSValue jsWarn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    const char* p = JS_ToCString(ctx, argv[0]);
    if (!p) return JS_EXCEPTION;
    Host* h = hostOf(ctx);
    if (h->w.warnings.size() < 1000) h->w.warnings.emplace_back(p);
    JS_FreeCString(ctx, p);
    return JS_UNDEFINED;
}
static JSValue jsFail(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    const char* p = argc > 0 ? JS_ToCString(ctx, argv[0]) : nullptr;
    JSValue r = JS_ThrowPlainError(ctx, "%s", p ? p : "post failed");
    if (p) JS_FreeCString(ctx, p);
    return r;
}

static void fillError(JSContext* ctx, JSRuntime* rt, Host& h, const std::string& scriptName, RunResult& r) {
    // Lift the limits while the error is read out, so that reading it cannot itself fail.
    h.armed = false;
    JS_SetMemoryLimit(rt, 0);
    JSValue exc = JS_GetException(ctx);
    bool isNull = JS_IsNull(exc) || JS_IsUninitialized(exc);
    if (!isNull) {
        const char* m = JS_ToCString(ctx, exc);
        if (m) { r.message = m; JS_FreeCString(ctx, m); } else { JS_FreeValue(ctx, JS_GetException(ctx)); r.message = "(unprintable exception)"; }
        if (JS_IsObject(exc)) {
            JSValue st = JS_GetPropertyStr(ctx, exc, "stack");
            if (JS_IsString(st)) { const char* s = JS_ToCString(ctx, st); if (s) { r.stack = s; JS_FreeCString(ctx, s); } }
            if (JS_IsException(st)) JS_FreeValue(ctx, JS_GetException(ctx));
            JS_FreeValue(ctx, st);
        }
    } else {
        r.message = "(null exception)";
    }
    JS_FreeValue(ctx, exc);
    if (h.heapHit) r.kind = "heap-limit";
    else if (h.cpuHit) r.kind = "cpu-limit";
    else if (h.w.outLimitHit) r.kind = "output-limit";
    else if (isNull || r.message.find("out of memory") != std::string::npos) r.kind = "heap-limit";
    else r.kind = "script-error";
    // First stack frame that names the script: "    at fn (name:LINE:COL)" or "    at name:LINE:COL".
    std::string key = scriptName + ":";
    size_t p = r.stack.find(key);
    if (p != std::string::npos) {
        r.file = scriptName;
        const char* s = r.stack.c_str() + p + key.size();
        char* e;
        long l = strtol(s, &e, 10);
        if (e != s) { r.line = int(l); if (*e == ':') r.col = int(strtol(e + 1, nullptr, 10)); }
    }
}

static RunResult runPost(const std::string& scriptPath, const std::vector<Move>& moves, const Limits& lim, bool reuseObjects) {
    RunResult r;
    std::string src;
    std::string name = baseName(scriptPath);
    if (!readFile(scriptPath, src)) { r.kind = "host-error"; r.message = "cannot read " + scriptPath; return r; }

    static const JSMallocFunctions mf = {tCalloc, tMalloc, tFree, tRealloc, tUsable};
    AllocStats stats;
    Host h;
    h.w.maxOut = lim.maxOutputBytes;
    h.cpuLimit = lim.cpuSeconds;
    double t0 = wallSeconds(), c0 = threadCpuSeconds();
    h.cpuStart = c0;
    h.stats = &stats;
    h.softHeap = g_softHeap ? lim.heapBytes - lim.heapBytes / 8 : 0;

    JSRuntime* rt = JS_NewRuntime2(&mf, &stats);
    JS_SetMemoryLimit(rt, lim.heapBytes);
    JS_SetMaxStackSize(rt, lim.stackBytes);
    JS_SetInterruptHandler(rt, interruptHandler, &h);
    // Raw context: only the intrinsics named here exist. Not added: Date, eval/Function-from-string, Proxy,
    // typed arrays/Atomics, Promise, WeakRef/FinalizationRegistry, performance, atob/btoa. quickjs-libc is not linked.
    JSContext* ctx = JS_NewContextRaw(rt);
    JS_SetContextOpaque(ctx, &h);
    JS_AddIntrinsicBaseObjects(ctx);
    JS_AddIntrinsicRegExp(ctx);
    JS_AddIntrinsicJSON(ctx);
    JS_AddIntrinsicMapSet(ctx);
    if (g_promise) JS_AddIntrinsicPromise(ctx);
    JSValue global = JS_GetGlobalObject(ctx);
    {
        JSValue math = JS_GetPropertyStr(ctx, global, "Math");
        JSAtom a = JS_NewAtom(ctx, "random"); JS_DeleteProperty(ctx, math, a, 0); JS_FreeAtom(ctx, a);
        JS_FreeValue(ctx, math);
        a = JS_NewAtom(ctx, "queueMicrotask"); JS_DeleteProperty(ctx, global, a, 0); JS_FreeAtom(ctx, a);
    }
    JS_SetPropertyStr(ctx, global, "definePost", JS_NewCFunction(ctx, jsDefinePost, "definePost", 1));

    JSValue out = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, out, "block", JS_NewCFunction(ctx, jsBlock, "block", 0));
    JS_SetPropertyStr(ctx, out, "raw", JS_NewCFunction(ctx, jsRaw, "raw", 1));
    JS_SetPropertyStr(ctx, out, "comment", JS_NewCFunction(ctx, jsComment, "comment", 1));
    JS_SetPropertyStr(ctx, out, "axis", JS_NewCFunction(ctx, jsAxis, "axis", 2));
    JS_SetPropertyStr(ctx, out, "word", JS_NewCFunction(ctx, jsWord, "word", 3));
    JS_SetPropertyStr(ctx, out, "feed", JS_NewCFunction(ctx, jsFeed, "feed", 1));
    JS_SetPropertyStr(ctx, out, "modal", JS_NewCFunction(ctx, jsModal, "modal", 2));
    JS_SetPropertyStr(ctx, out, "resetModal", JS_NewCFunction(ctx, jsResetModal, "resetModal", 0));
    JS_SetPropertyStr(ctx, out, "warn", JS_NewCFunction(ctx, jsWarn, "warn", 1));
    JS_SetPropertyStr(ctx, out, "fail", JS_NewCFunction(ctx, jsFail, "fail", 1));

    static const char ctxJson[] =
        "{\"programNumber\":1,\"programComment\":\"S8 spike program\",\"documentName\":\"spike\",\"setupName\":\"Setup1\","
        "\"userName\":\"\",\"units\":\"mm\",\"workOffset\":1,\"date\":null,\"properties\":{},\"tools\":[],\"sections\":[]}";
    static const char secJson[] =
        "{\"index\":0,\"name\":\"toy section\",\"opType\":\"contour\",\"tool\":{\"number\":7,\"diameter\":6},"
        "\"toolChanged\":true,\"nextTool\":null,\"rpm\":12000,\"clockwise\":true,\"coolant\":\"flood\"}";
    JSValue pctx = JS_ParseJSON(ctx, ctxJson, sizeof ctxJson - 1, "<ctx>");
    JSValue psec = JS_ParseJSON(ctx, secJson, sizeof secJson - 1, "<section>");

    enum { aX, aY, aZ, aFeed, aRole, aComp, aCw, aPlane, aCx, aCy, aCz, aI, aJ, aK, aRadius, aSweep, aCount };
    static const char* atomNames[aCount] = {"x", "y", "z", "feed", "role", "comp", "cw", "plane", "cx", "cy", "cz",
                                            "i", "j", "k", "radius", "sweepDegrees"};
    JSAtom atom[aCount];
    for (int i = 0; i < aCount; ++i) atom[i] = JS_NewAtom(ctx, atomNames[i]);
    JSValue sCut = JS_NewString(ctx, "cut"), sXY = JS_NewString(ctx, "XY");
    JSValue fn[9];
    for (auto& f : fn) f = JS_UNDEFINED;
    JSValue reuse[3] = {JS_NewObject(ctx), JS_NewObject(ctx), JS_NewObject(ctx)};

    bool failed = false;
    JSContext* errCtx = ctx;   // context holding the pending exception when failed is set
    JSValue ev = JS_UNDEFINED;
    if (g_evalInSandbox) {
        // Naive variant, kept only to show the leak: the compiler hook also enables eval() and Function("...").
        JS_AddIntrinsicEval(ctx);
        ev = JS_Eval(ctx, src.data(), src.size(), name.c_str(), JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_STRICT);
    } else {
        // JS_Eval needs JS_AddIntrinsicEval, which is also what makes eval() and Function("...") work. So the source
        // is compiled in a second, throwaway context of the same runtime (same heap limit, same interrupt handler),
        // and only the bytecode is moved into the sandbox context, which never gets the compiler hook.
        JSContext* cctx = JS_NewContextRaw(rt);
        JS_SetContextOpaque(cctx, &h);
        JS_AddIntrinsicBaseObjects(cctx);
        JS_AddIntrinsicRegExpCompiler(cctx);   // regexp literals are compiled at parse time
        JS_AddIntrinsicEval(cctx);
        JSValue fo = JS_Eval(cctx, src.data(), src.size(), name.c_str(),
                             JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_STRICT | JS_EVAL_FLAG_COMPILE_ONLY);
        uint8_t* bc = nullptr; size_t bcLen = 0;
        if (!JS_IsException(fo)) bc = JS_WriteObject(cctx, &bcLen, fo, JS_WRITE_OBJ_BYTECODE);
        if (JS_IsException(fo) || !bc) {
            failed = true; errCtx = cctx;
            fillError(cctx, rt, h, name, r);
        }
        JS_FreeValue(cctx, fo);
        JS_FreeContext(cctx);
        r.bytecodeBytes = bcLen;
        if (!failed) {
            JSValue fn2 = JS_ReadObject(ctx, bc, bcLen, JS_READ_OBJ_BYTECODE);
            ev = JS_IsException(fn2) ? fn2 : JS_EvalFunction(ctx, fn2);
        }
        if (bc) js_free_rt(rt, bc);
    }
    bool compileFailed = failed;
    if (!failed && JS_IsException(ev)) failed = true;
    else if (!failed && JS_IsUndefined(h.postDef)) {
        const char* s = JS_ToCString(ctx, ev);
        if (s) { r.completion = s; JS_FreeCString(ctx, s); } else failed = true;
    }
    JS_FreeValue(ctx, ev);
    double t1 = wallSeconds();

    if (!failed && !JS_IsUndefined(h.postDef)) {
        static const char* fnNames[9] = {"onProgramStart", "onSectionStart", "onRapid", "onLinear", "onArc",
                                         "onSectionEnd", "onProgramEnd", "onCycle", "onDwell"};
        for (int i = 0; i < 9; ++i) fn[i] = JS_GetPropertyStr(ctx, h.postDef, fnNames[i]);
        auto call = [&](int which, int argc, JSValue* argv) -> bool {
            if (!JS_IsFunction(ctx, fn[which])) return true;
            JSValue rv = JS_Call(ctx, fn[which], h.postDef, argc, argv);
            if (JS_IsException(rv)) return false;
            JS_FreeValue(ctx, rv);
            return true;
        };
        const double NM = 1e-6;
        JSValue a2[2] = {pctx, out};
        JSValue a3[3] = {psec, pctx, out};
        failed = !call(0, 2, a2) || !call(1, 3, a3);
        int64_t px = 0, py = 0;
        for (size_t i = 0; !failed && i < moves.size(); ++i) {
            const Move& m = moves[i];
            h.w.curMove = uint32_t(i);
            JSValue obj;
            int which;
            JSAtom props[16]; JSValue vals[16]; int n = 0;
            auto put = [&](int a, JSValue v) { props[n] = atom[a]; vals[n] = v; ++n; };
            put(aX, JS_NewFloat64(ctx, double(m.x) * NM));
            put(aY, JS_NewFloat64(ctx, double(m.y) * NM));
            put(aZ, JS_NewFloat64(ctx, double(m.z) * NM));
            if (m.kind == Rapid) which = 2;
            else if (m.kind == Linear) {
                which = 3;
                put(aFeed, JS_NewFloat64(ctx, m.feed)); put(aRole, JS_DupValue(ctx, sCut)); put(aComp, JS_NewInt32(ctx, 0));
            } else {
                which = 4;
                double di = double(m.cx - px) * NM, dj = double(m.cy - py) * NM;
                put(aCw, JS_NewBool(ctx, m.kind == ArcCW)); put(aPlane, JS_DupValue(ctx, sXY));
                put(aCx, JS_NewFloat64(ctx, double(m.cx) * NM)); put(aCy, JS_NewFloat64(ctx, double(m.cy) * NM));
                put(aCz, JS_NewFloat64(ctx, double(m.z) * NM));
                put(aI, JS_NewFloat64(ctx, di)); put(aJ, JS_NewFloat64(ctx, dj)); put(aK, JS_NewFloat64(ctx, 0));
                put(aRadius, JS_NewFloat64(ctx, std::sqrt(di * di + dj * dj)));
                put(aSweep, JS_NewFloat64(ctx, m.full ? 360 : 90));
                put(aFeed, JS_NewFloat64(ctx, m.feed)); put(aComp, JS_NewInt32(ctx, 0));
            }
            if (reuseObjects) {
                obj = JS_DupValue(ctx, reuse[which - 2]);
                for (int k = 0; k < n; ++k) JS_SetProperty(ctx, obj, props[k], vals[k]);
            } else {
                obj = JS_NewObjectFrom(ctx, n, props, vals);
            }
            JSValue argv2[2] = {obj, out};
            failed = !call(which, 2, argv2);
            JS_FreeValue(ctx, obj);
            px = m.x; py = m.y;
        }
        if (!failed) failed = !call(5, 3, a3) || !call(6, 2, a2);
    }
    double t2 = wallSeconds();
    if (failed && !compileFailed) fillError(ctx, rt, h, name, r);
    (void)errCtx;
    r.jsHeapAtEnd = stats.cur;
    r.jsHeapPeak = stats.peak;

    for (auto& f : fn) JS_FreeValue(ctx, f);
    for (auto& o : reuse) JS_FreeValue(ctx, o);
    for (int i = 0; i < aCount; ++i) JS_FreeAtom(ctx, atom[i]);
    JS_FreeValue(ctx, sCut); JS_FreeValue(ctx, sXY);
    JS_FreeValue(ctx, pctx); JS_FreeValue(ctx, psec);
    JS_FreeValue(ctx, out); JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, h.postDef);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    double t3 = wallSeconds();

    r.jsHeapAfterFree = stats.cur;
    r.wallLoad = t1 - t0; r.wallMoves = t2 - t1; r.wallTeardown = t3 - t2; r.wallTotal = t3 - t0;
    r.cpuTotal = threadCpuSeconds() - c0;
    r.interruptCalls = h.interruptCalls;
    r.lines = h.w.lineNo; r.lineMapEntries = h.w.lineMap.size(); r.warnings = h.w.warnings.size();
    r.out = std::move(h.w.out);
    return r;
}

// ---------------------------------------------------------------- commands
static void writeOut(const std::string& path, const std::string& data) {
    if (path.empty()) return;
    std::ofstream f(path, std::ios::binary);
    f.write(data.data(), std::streamsize(data.size()));
}
static void printCounts(const std::vector<Move>& mv) {
    size_t c[4]{};
    for (auto& m : mv) ++c[m.kind];
    printf("moves=%zu rapid=%zu linear=%zu arc=%zu  toolpath_array=%.1f MB (sizeof(Move)=%zu)\n",
           mv.size(), c[0], c[1], c[2] + c[3], mb(mv.size() * sizeof(Move)), sizeof(Move));
}

static int cmdBench(int argc, char** argv) {
    std::string script = argv[2]; size_t n = strtoull(argv[3], nullptr, 10);
    bool reuse = false; int repeat = 1; std::string outPath;
    for (int i = 4; i < argc; ++i) {
        if (!strcmp(argv[i], "--reuse")) reuse = true;
        else if (!strcmp(argv[i], "--repeat") && i + 1 < argc) repeat = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) outPath = argv[++i];
    }
    double g0 = wallSeconds();
    std::vector<Move> mv = makeToolpath(n);
    printf("quickjs-ng %s  mode=%s\n", JS_GetVersion(), reuse ? "reused move objects" : "fresh move object per move");
    printCounts(mv);
    printf("toolpath generation %.1f ms; process peak working set before post: %.1f MB\n",
           (wallSeconds() - g0) * 1e3, mb(processPeakWorkingSet()));
    Limits lim;
    uint64_t firstHash = 0; bool same = true;
    for (int k = 0; k < repeat; ++k) {
        RunResult r = runPost(script, mv, lim, reuse);
        uint64_t hsh = fnv1a64(r.out);
        if (k == 0) firstHash = hsh; else if (hsh != firstHash) same = false;
        printf("run %d: kind=%s wall_total=%.1f ms (load %.2f, moves %.1f, teardown %.2f) cpu=%.1f ms  %.0f ns/move\n",
               k + 1, r.kind.c_str(), r.wallTotal * 1e3, r.wallLoad * 1e3, r.wallMoves * 1e3, r.wallTeardown * 1e3,
               r.cpuTotal * 1e3, r.wallMoves * 1e9 / double(mv.empty() ? 1 : mv.size()));
        printf("       output=%zu bytes (%.1f MB) lines=%zu linemap=%zu fnv1a64=%016llx\n",
               r.out.size(), mb(r.out.size()), r.lines, r.lineMapEntries, static_cast<unsigned long long>(hsh));
        printf("       bytecode=%zu bytes\n", r.bytecodeBytes);
        printf("       js_heap_peak=%.2f MB js_heap_at_end=%.2f MB js_heap_after_free=%zu bytes interrupt_handler_calls=%llu\n",
               mb(r.jsHeapPeak), mb(r.jsHeapAtEnd), r.jsHeapAfterFree, static_cast<unsigned long long>(r.interruptCalls));
        if (r.kind != "ok") printf("       ERROR %s %s:%d\n%s\n", r.message.c_str(), r.file.c_str(), r.line, r.stack.c_str());
        if (k == 0) writeOut(outPath, r.out);
    }
    if (repeat > 1) printf("identical output over %d in-process runs: %s\n", repeat, same ? "yes" : "NO");
    printf("process peak working set=%.1f MB peak commit=%.1f MB\n", mb(processPeakWorkingSet()), mb(processPeakCommit()));
    return 0;
}

static int cmdNative(int argc, char** argv) {
    size_t n = strtoull(argv[2], nullptr, 10);
    std::string outPath;
    for (int i = 3; i < argc; ++i) if (!strcmp(argv[i], "--out") && i + 1 < argc) outPath = argv[++i];
    std::vector<Move> mv = makeToolpath(n);
    printCounts(mv);
    Writer w;
    double t0 = wallSeconds();
    nativePost(mv, w);
    double t1 = wallSeconds();
    printf("native baseline: wall=%.1f ms %.0f ns/move output=%zu bytes lines=%u fnv1a64=%016llx\n", (t1 - t0) * 1e3,
           (t1 - t0) * 1e9 / double(mv.size()), w.out.size(), w.lineNo, static_cast<unsigned long long>(fnv1a64(w.out)));
    printf("process peak working set=%.1f MB\n", mb(processPeakWorkingSet()));
    writeOut(outPath, w.out);
    return 0;
}

struct TestCase {
    const char* file;
    const char* expectKind;
    const char* expectMsg;     // substring of the message, or nullptr
    bool expectLine;           // reported line must equal the line holding the /*HERE*/ marker
    size_t heapMb; double cpuSec; size_t outMb; size_t nMoves;
    const char* acceptance;
};
static int markerLine(const std::string& src) {
    size_t p = src.find("/*HERE*/");
    if (p == std::string::npos) return -1;
    int line = 1;
    for (size_t i = 0; i < p; ++i) if (src[i] == '\n') ++line;
    return line;
}
static int cmdTest(int argc, char** argv) {
    const char* filter = argc > 4 ? argv[4] : nullptr;
    std::string dir = argv[2], toy = argv[3];
    static const TestCase tests[] = {
        {"sandbox_globals.js",    "ok",           nullptr, false, 64, 5.0, 16, 0, "c"},
        {"probe_sort_time.js",    "ok",           nullptr, false, 256, 60.0, 16, 0, "probe"},
        {"sandbox_import.js",     "script-error", nullptr, false, 64, 5.0, 16, 0, "c"},
        {"err_syntax.js",         "script-error", "SyntaxError", true, 64, 5.0, 16, 0, "d"},
        {"err_toplevel.js",       "script-error", "ReferenceError", true, 64, 5.0, 16, 0, "d"},
        {"err_runtime.js",        "script-error", "TypeError", true, 64, 5.0, 16, 1000, "d"},
        {"err_nested.js",         "script-error", "RangeError: bad arc", true, 64, 5.0, 16, 1000, "d"},
        {"err_fail.js",           "script-error", "tool 7 has no length offset", true, 64, 5.0, 16, 1000, "d"},
        {"err_throw_string.js",   "script-error", "just a string", false, 64, 5.0, 16, 1000, "d (limit)"},
        {"stack_overflow.js",     "script-error", "Maximum call stack size exceeded", true, 64, 5.0, 16, 0, "d"},
        {"heap_runaway.js",       "heap-limit",   nullptr, false, 32, 20.0, 16, 0, "a"},
        {"heap_string.js",        "heap-limit",   nullptr, false, 32, 20.0, 16, 0, "a"},
        {"heap_big_single.js",    "heap-limit",   "out of memory", false, 32, 20.0, 16, 0, "a (hard)"},
        {"string_too_long.js",    "script-error", "string too long", false, 32, 20.0, 16, 0, "a (note)"},
        {"heap_in_callback.js",   "heap-limit",   nullptr, false, 32, 20.0, 16, 1000, "a"},
        {"heap_catch.js",         "heap-limit",   nullptr, false, 32, 5.0, 16, 0, "a"},
        {"cpu_loop.js",           "cpu-limit",    "interrupted", false, 64, 0.5, 16, 0, "b"},
        {"cpu_loop_catch.js",     "cpu-limit",    "interrupted", false, 64, 0.5, 16, 0, "b"},
        {"cpu_in_callback.js",    "cpu-limit",    "interrupted", false, 64, 0.5, 16, 1000, "b"},
        {"cpu_regex.js",          "cpu-limit",    nullptr, false, 64, 0.5, 16, 0, "b"},
        {"cpu_native_sort.js",    "cpu-limit",    nullptr, false, 256, 0.5, 16, 0, "b (native)"},
        {"cpu_native_sort_big.js", "cpu-limit",   nullptr, false, 256, 2.0, 16, 0, "b (native)"},
        {"native_recursion.js",   "ok",           nullptr, false, 64, 5.0, 16, 0, "robustness"},
        {"output_flood.js",       "output-limit", "output limit", false, 64, 20.0, 16, 0, "extra"},
    };
    int fails = 0;
    printf("%-22s %-10s %-13s %-13s %-9s %8s %8s %9s  %s\n", "test", "accept.", "expected", "got", "line", "wall ms", "cpu ms", "heapPk MB", "result");
    for (const TestCase& t : tests) {
        if (filter && !strstr(t.file, filter)) continue;
        if (!strcmp(t.file, "cpu_native_sort_big.js") && !getenv("S8_SLOW")) {
            printf("%-22s skipped (set S8_SLOW=1; takes minutes on an unpatched engine)\n", t.file);
            continue;
        }
        std::string path = dir + "/" + t.file, src;
        readFile(path, src);
        std::vector<Move> mv = makeToolpath(t.nMoves);
        Limits lim; lim.heapBytes = t.heapMb << 20; lim.cpuSeconds = t.cpuSec; lim.maxOutputBytes = t.outMb << 20;
        RunResult r = runPost(path, mv, lim, false);
        int want = t.expectLine ? markerLine(src) : -1;
        bool ok = r.kind == t.expectKind;
        if (t.expectMsg && r.message.find(t.expectMsg) == std::string::npos) ok = false;
        if (t.expectLine && (r.line != want || r.file != t.file)) ok = false;
        std::string why;
        if (!ok) why += " kind, message or line mismatch;";
        if (r.jsHeapAfterFree != 0) { ok = false; why += " runtime did not free everything (" + std::to_string(r.jsHeapAfterFree) + " bytes left);"; }
        if (r.jsHeapPeak > (t.heapMb << 20) + (size_t(1) << 20)) { ok = false; why += " heap peak more than 1 MB above the limit;"; }
        // A CPU limit that is honoured late is a failure: allow 100 ms beyond the limit.
        // The two "b (native)" probes spend their time inside native calls; their lateness is reported, not judged.
        if (!strcmp(t.expectKind, "cpu-limit") && r.cpuTotal > t.cpuSec + 0.1) {
            char b[96]; snprintf(b, sizeof b, " stopped %.0f ms after the %.0f ms limit;", (r.cpuTotal - t.cpuSec) * 1e3, t.cpuSec * 1e3);
            why += b;
            if (strcmp(t.acceptance, "b (native)")) ok = false;
        }
        if (!strcmp(t.file, "sandbox_globals.js") && r.completion.find("\"verdict\":\"clean\"") == std::string::npos) ok = false;
        if (!strcmp(t.file, "native_recursion.js") && r.completion.find("survived") == std::string::npos) ok = false;
        char lineBuf[32];
        if (t.expectLine) snprintf(lineBuf, sizeof lineBuf, "%d/%d", r.line, want); else snprintf(lineBuf, sizeof lineBuf, "%d", r.line);
        printf("%-22s %-10s %-13s %-13s %-9s %8.1f %8.1f %9.2f  %s\n", t.file, t.acceptance, t.expectKind, r.kind.c_str(),
               lineBuf, r.wallTotal * 1e3, r.cpuTotal * 1e3, mb(r.jsHeapPeak), ok ? "PASS" : "FAIL");
        std::string msg = r.message.substr(0, 110);
        if (!msg.empty()) printf("    message: %s\n", msg.c_str());
        if (!r.stack.empty()) {
            std::string st = r.stack.substr(0, 160);
            for (char& c : st) if (c == '\n') c = '|';
            printf("    stack:   %s\n", st.c_str());
        }
        if (!r.completion.empty()) printf("    result:  %s\n", r.completion.c_str());
        if (!why.empty()) printf("    why:    %s\n", why.c_str());
        if (!ok) ++fails;
    }
    // After every limit has been hit in this process, a normal post must still run and give the usual bytes.
    {
        std::vector<Move> mv = makeToolpath(10000);
        Limits lim;
        RunResult r = runPost(toy, mv, lim, false);
        Writer w; nativePost(mv, w);
        bool ok = r.kind == "ok" && r.out == w.out && r.jsHeapAfterFree == 0;
        printf("%-22s %-10s %-13s %-13s %-9s %8.1f %8.1f %9.2f  %s\n", "toy_after_failures", "recovery", "ok", r.kind.c_str(), "-",
               r.wallTotal * 1e3, r.cpuTotal * 1e3, mb(r.jsHeapPeak), ok ? "PASS" : "FAIL");
        if (!ok) ++fails;
    }
    printf("process peak working set=%.1f MB\n", mb(processPeakWorkingSet()));
    printf("%s (%d failed)\n", fails ? "TESTS FAILED" : "ALL TESTS PASSED", fails);
    return fails ? 1 : 0;
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (const char* e = getenv("S8_EVAL_IN_SANDBOX")) g_evalInSandbox = e[0] == '1';
    if (const char* e = getenv("S8_PROMISE")) g_promise = e[0] != '0';
    if (const char* e = getenv("S8_SOFT_HEAP")) g_softHeap = e[0] != '0';
    if (argc >= 4 && !strcmp(argv[1], "bench")) return cmdBench(argc, argv);
    if (argc >= 3 && !strcmp(argv[1], "native")) return cmdNative(argc, argv);
    if (argc >= 4 && !strcmp(argv[1], "test")) return cmdTest(argc, argv);
    fprintf(stderr, "usage: s8host bench <script> <nMoves> [--reuse] [--repeat N] [--out file]\n"
                    "       s8host native <nMoves> [--out file]\n"
                    "       s8host test <tests-dir> <toy-post>\n");
    return 2;
}
