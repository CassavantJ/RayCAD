// Spike S10: SQLite document file, kill-during-save, lock file, incremental save cost.
// Throwaway code. One executable, several sub-commands (see usage()).
#include <windows.h>
#include <sqlite3.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

// ------------------------------------------------------------------ SHA-256
struct Hash {
    uint8_t b[32];
    bool operator<(const Hash& o) const { return memcmp(b, o.b, 32) < 0; }
    bool operator==(const Hash& o) const { return memcmp(b, o.b, 32) == 0; }
};
static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
static inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static void sha256Block(uint32_t s[8], const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i*4] << 24 | (uint32_t)p[i*4+1] << 16 | (uint32_t)p[i*4+2] << 8 | p[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i-15], 7) ^ rotr(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = rotr(w[i-2], 17) ^ rotr(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=s[0],b=s[1],c=s[2],d=s[3],e=s[4],f=s[5],g=s[6],h=s[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (rotr(e,6)^rotr(e,11)^rotr(e,25)) + ((e&f)^(~e&g)) + K256[i] + w[i];
        uint32_t t2 = (rotr(a,2)^rotr(a,13)^rotr(a,22)) + ((a&b)^(a&c)^(b&c));
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    s[0]+=a; s[1]+=b; s[2]+=c; s[3]+=d; s[4]+=e; s[5]+=f; s[6]+=g; s[7]+=h;
}
static Hash sha256(const void* data, size_t n) {
    uint32_t s[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    const uint8_t* p = (const uint8_t*)data;
    size_t full = n / 64;
    for (size_t i = 0; i < full; i++) sha256Block(s, p + i * 64);
    uint8_t tail[128] = {0};
    size_t rem = n - full * 64;
    memcpy(tail, p + full * 64, rem);
    tail[rem] = 0x80;
    size_t tl = rem < 56 ? 64 : 128;
    uint64_t bits = (uint64_t)n * 8;
    for (int i = 0; i < 8; i++) tail[tl - 1 - i] = (uint8_t)(bits >> (8 * i));
    sha256Block(s, tail);
    if (tl == 128) sha256Block(s, tail + 64);
    Hash h;
    for (int i = 0; i < 8; i++) { h.b[i*4] = s[i] >> 24; h.b[i*4+1] = s[i] >> 16; h.b[i*4+2] = s[i] >> 8; h.b[i*4+3] = s[i]; }
    return h;
}

// ------------------------------------------------------------------ small utilities
static double nowMs() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return 1000.0 * (double)c.QuadPart / (double)f.QuadPart;
}
static int64_t unixMs() {
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (int64_t)(t / 10000) - 11644473600000LL;
}
static std::string isoNow() {
    SYSTEMTIME st; GetSystemTime(&st);
    char b[40]; sprintf(b, "%04d-%02d-%02dT%02d:%02d:%02dZ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return b;
}
static int64_t fileSize(const std::string& p) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(p.c_str(), GetFileExInfoStandard, &d)) return -1;
    return ((int64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
}
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
    uint64_t range(uint64_t lo, uint64_t hi) { return lo + next() % (hi - lo + 1); }
    void fill(std::vector<uint8_t>& v) {
        size_t i = 0;
        for (; i + 8 <= v.size(); i += 8) { uint64_t x = next(); memcpy(&v[i], &x, 8); }
        for (; i < v.size(); i++) v[i] = (uint8_t)next();
    }
};

// Shared memory block: the writer reports progress to the harness (survives TerminateProcess).
static const LONG64 UNSET = INT64_MIN;
struct Shm {
    volatile LONG64 phase;      // 0 starting, 1 idle (between saves), 2 in transaction (statements), 3 inside COMMIT
    volatile LONG64 startHead;  // head seq the writer found at open (UNSET until open is done)
    volatile LONG64 started;    // seq of the save now in progress / last begun
    volatile LONG64 acked;      // seq of the last save whose COMMIT returned OK
    volatile LONG64 busy;       // number of SQLITE_BUSY retries
    volatile LONG64 aux;        // lock demo phase
};
static Shm g_localShm;
static Shm* g_shm = &g_localShm;
static Shm* mapShm(const char* name, bool create, HANDLE* hOut = nullptr) {
    HANDLE h = create ? CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Shm), name)
                      : OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!h) return nullptr;
    if (hOut) *hOut = h;
    return (Shm*)MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shm));
}
static void resetShm(Shm* s) { s->phase = 0; s->startHead = UNSET; s->started = UNSET; s->acked = UNSET; s->busy = 0; s->aux = 0; }

// ------------------------------------------------------------------ document file
static const char* kSchema =
    "PRAGMA application_id = 0x52415943;"
    "PRAGMA user_version = 1;"
    "CREATE TABLE meta    (key TEXT PRIMARY KEY, value TEXT NOT NULL);"
    "CREATE TABLE objects (hash BLOB PRIMARY KEY, kind INTEGER NOT NULL, raw_size INTEGER NOT NULL, data BLOB NOT NULL) WITHOUT ROWID;"
    "CREATE TABLE commits (id BLOB PRIMARY KEY, root BLOB NOT NULL, author TEXT, time_ms INTEGER NOT NULL, message TEXT, named INTEGER NOT NULL DEFAULT 0) WITHOUT ROWID;"
    "CREATE TABLE commit_parents (commit_id BLOB NOT NULL, ord INTEGER NOT NULL, parent BLOB NOT NULL, PRIMARY KEY (commit_id, ord)) WITHOUT ROWID;"
    "CREATE TABLE refs    (name TEXT PRIMARY KEY, commit_id BLOB NOT NULL);"
    "CREATE TABLE derived (input_hash BLOB PRIMARY KEY, object_hash BLOB NOT NULL, kernel_version TEXT NOT NULL) WITHOUT ROWID;"
    "CREATE TABLE thumbs  (commit_id BLOB PRIMARY KEY, png BLOB NOT NULL) WITHOUT ROWID;";

static int busyHandler(void*, int n) {
    InterlockedIncrement64(&g_shm->busy);
    if (n > 1000) return 0;   // give up after about 5 s
    Sleep(5);
    return 1;
}

struct Doc {
    sqlite3* db = nullptr;
    std::vector<Hash> slots;   // the manifest: one object hash per slot
    Hash head{};               // head commit id
    int64_t seq = -1;          // "save N" number of the head commit, -1 = no commit
    std::string pragmas;       // what the PRAGMAs actually returned
};

#define CHECK_RC(rc, db, what) do { if ((rc) != SQLITE_OK && (rc) != SQLITE_DONE && (rc) != SQLITE_ROW) { \
    err = std::string(what) + ": rc=" + std::to_string(rc) + " " + ((db) ? sqlite3_errmsg(db) : ""); return false; } } while (0)

static bool execSql(sqlite3* db, const char* sql, std::string& err, std::string* firstCol = nullptr) {
    sqlite3_stmt* st = nullptr; const char* tail = sql;
    while (tail && *tail) {
        int rc = sqlite3_prepare_v2(db, tail, -1, &st, &tail);
        CHECK_RC(rc, db, sql);
        if (!st) continue;
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            if (firstCol && sqlite3_column_text(st, 0)) { if (!firstCol->empty()) *firstCol += "|"; *firstCol += (const char*)sqlite3_column_text(st, 0); }
        }
        sqlite3_finalize(st);
        CHECK_RC(rc, db, sql);
    }
    return true;
}
static bool getObject(sqlite3* db, const Hash& h, std::vector<uint8_t>& out, int64_t* rawSize, bool& found, std::string& err) {
    sqlite3_stmt* st = nullptr;
    int rc = sqlite3_prepare_v2(db, "SELECT data, raw_size FROM objects WHERE hash=?", -1, &st, nullptr);
    CHECK_RC(rc, db, "prepare getObject");
    sqlite3_bind_blob(st, 1, h.b, 32, SQLITE_STATIC);
    rc = sqlite3_step(st);
    found = rc == SQLITE_ROW;
    if (found) {
        const uint8_t* p = (const uint8_t*)sqlite3_column_blob(st, 0);
        int n = sqlite3_column_bytes(st, 0);
        out.assign(p, p + n);
        if (rawSize) *rawSize = sqlite3_column_int64(st, 1);
    }
    sqlite3_finalize(st);
    CHECK_RC(rc, db, "step getObject");
    return true;
}
static bool putObject(sqlite3* db, int kind, const std::vector<uint8_t>& data, Hash& h, std::string& err) {
    h = sha256(data.data(), data.size());
    sqlite3_stmt* st = nullptr;
    int rc = sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO objects(hash,kind,raw_size,data) VALUES(?,?,?,?)", -1, &st, nullptr);
    CHECK_RC(rc, db, "prepare putObject");
    sqlite3_bind_blob(st, 1, h.b, 32, SQLITE_STATIC);
    sqlite3_bind_int(st, 2, kind);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)data.size());
    sqlite3_bind_blob64(st, 4, data.data(), data.size(), SQLITE_STATIC);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    CHECK_RC(rc, db, "step putObject");
    return true;
}
static std::vector<uint8_t> encodeManifest(const std::vector<Hash>& slots) {
    std::vector<uint8_t> m(8 + slots.size() * 32);
    memcpy(m.data(), "MANI", 4);
    uint32_t n = (uint32_t)slots.size(); memcpy(m.data() + 4, &n, 4);
    for (size_t i = 0; i < slots.size(); i++) memcpy(m.data() + 8 + i * 32, slots[i].b, 32);
    return m;
}
static bool decodeManifest(const std::vector<uint8_t>& m, std::vector<Hash>& slots) {
    if (m.size() < 8 || memcmp(m.data(), "MANI", 4) != 0) return false;
    uint32_t n; memcpy(&n, m.data() + 4, 4);
    if (m.size() != 8 + (size_t)n * 32) return false;
    slots.resize(n);
    for (uint32_t i = 0; i < n; i++) memcpy(slots[i].b, m.data() + 8 + i * 32, 32);
    return true;
}
static Hash commitId(const Hash* parent, const Hash& root, const std::string& author, int64_t timeMs, const std::string& msg) {
    std::vector<uint8_t> buf;
    if (parent) buf.insert(buf.end(), parent->b, parent->b + 32);
    buf.insert(buf.end(), root.b, root.b + 32);
    buf.insert(buf.end(), author.begin(), author.end()); buf.push_back(0);
    buf.insert(buf.end(), (uint8_t*)&timeMs, (uint8_t*)&timeMs + 8);
    buf.insert(buf.end(), msg.begin(), msg.end());
    return sha256(buf.data(), buf.size());
}
// Inside an open transaction: write manifest, commit row, parent row, move the ref.
static bool writeCommit(Doc& d, std::string& err) {
    Hash root;
    if (!putObject(d.db, 0, encodeManifest(d.slots), root, err)) return false;
    int64_t t = unixMs();
    std::string author = "spike", msg = "save " + std::to_string(d.seq + 1);
    Hash id = commitId(d.seq >= 0 ? &d.head : nullptr, root, author, t, msg);
    sqlite3_stmt* st = nullptr;
    int rc = sqlite3_prepare_v2(d.db, "INSERT INTO commits(id,root,author,time_ms,message) VALUES(?,?,?,?,?)", -1, &st, nullptr);
    CHECK_RC(rc, d.db, "prepare commit");
    sqlite3_bind_blob(st, 1, id.b, 32, SQLITE_STATIC); sqlite3_bind_blob(st, 2, root.b, 32, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, author.c_str(), -1, SQLITE_STATIC); sqlite3_bind_int64(st, 4, t);
    sqlite3_bind_text(st, 5, msg.c_str(), -1, SQLITE_STATIC);
    rc = sqlite3_step(st); sqlite3_finalize(st);
    CHECK_RC(rc, d.db, "insert commit");
    if (d.seq >= 0) {
        rc = sqlite3_prepare_v2(d.db, "INSERT INTO commit_parents(commit_id,ord,parent) VALUES(?,0,?)", -1, &st, nullptr);
        CHECK_RC(rc, d.db, "prepare parent");
        sqlite3_bind_blob(st, 1, id.b, 32, SQLITE_STATIC); sqlite3_bind_blob(st, 2, d.head.b, 32, SQLITE_STATIC);
        rc = sqlite3_step(st); sqlite3_finalize(st);
        CHECK_RC(rc, d.db, "insert parent");
    }
    rc = sqlite3_prepare_v2(d.db, "INSERT INTO refs(name,commit_id) VALUES('heads/main',?) ON CONFLICT(name) DO UPDATE SET commit_id=excluded.commit_id", -1, &st, nullptr);
    CHECK_RC(rc, d.db, "prepare ref");
    sqlite3_bind_blob(st, 1, id.b, 32, SQLITE_STATIC);
    rc = sqlite3_step(st); sqlite3_finalize(st);
    CHECK_RC(rc, d.db, "move ref");
    d.head = id;   // caller bumps seq after COMMIT
    return true;
}
static bool loadHead(Doc& d, std::string& err) {
    sqlite3_stmt* st = nullptr;
    int rc = sqlite3_prepare_v2(d.db, "SELECT c.id, c.root, c.message FROM refs r JOIN commits c ON c.id=r.commit_id WHERE r.name='heads/main'", -1, &st, nullptr);
    CHECK_RC(rc, d.db, "prepare loadHead");
    rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) { sqlite3_finalize(st); err = "no head commit (rc=" + std::to_string(rc) + ")"; return false; }
    Hash root;
    memcpy(d.head.b, sqlite3_column_blob(st, 0), 32); memcpy(root.b, sqlite3_column_blob(st, 1), 32);
    d.seq = atoll((const char*)sqlite3_column_text(st, 2) + 5);
    sqlite3_finalize(st);
    std::vector<uint8_t> m; bool found = false;
    if (!getObject(d.db, root, m, nullptr, found, err)) return false;
    if (!found || !decodeManifest(m, d.slots)) { err = "manifest missing or malformed"; return false; }
    return true;
}
// Opens with the PLAN.md section 4.1 settings. Creates schema plus commit 0 (in one transaction) when the file is new.
static bool openDoc(const std::string& path, Doc& d, std::string& err, int initSlots, size_t initSlotBytes, Rng& rng) {
    int rc = sqlite3_open_v2(path.c_str(), &d.db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    CHECK_RC(rc, d.db, "open");
    sqlite3_busy_handler(d.db, busyHandler, nullptr);
    std::string a, b, c;
    if (!execSql(d.db, "PRAGMA journal_mode=DELETE", err, &a)) return false;
    if (!execSql(d.db, "PRAGMA synchronous=FULL; PRAGMA synchronous", err, &b)) return false;
    if (!execSql(d.db, "PRAGMA locking_mode=EXCLUSIVE", err, &c)) return false;
    d.pragmas = "journal_mode=" + a + " synchronous=" + b + " locking_mode=" + c;
    std::string n;
    if (!execSql(d.db, "SELECT count(*) FROM sqlite_schema", err, &n)) return false;   // triggers hot-journal rollback if needed
    if (n == "0") {
        g_shm->startHead = -1; g_shm->started = 0; g_shm->phase = 2;
        if (!execSql(d.db, "BEGIN IMMEDIATE", err)) return false;
        if (!execSql(d.db, kSchema, err)) return false;
        if (!execSql(d.db, "INSERT INTO meta VALUES('file_id','spike-s10'),('created_by','s10 spike')", err)) return false;
        d.slots.resize(initSlots);
        std::vector<uint8_t> blob(initSlotBytes);
        for (int i = 0; i < initSlots; i++) { rng.fill(blob); if (!putObject(d.db, 1, blob, d.slots[i], err)) return false; }
        if (!writeCommit(d, err)) return false;
        g_shm->phase = 3;
        if (!execSql(d.db, "COMMIT", err)) return false;
        d.seq = 0; g_shm->acked = 0; g_shm->phase = 1;
    } else {
        if (!loadHead(d, err)) return false;
        g_shm->startHead = d.seq; g_shm->phase = 1;
    }
    return true;
}
// One save: replace some slots with new blobs, write commit, move ref. One transaction.
static bool saveDoc(Doc& d, const std::vector<std::pair<int, std::vector<uint8_t>>>& changes, std::string& err) {
    Doc backup; backup.slots = d.slots; backup.head = d.head;
    g_shm->started = d.seq + 1; g_shm->phase = 2;
    if (!execSql(d.db, "BEGIN IMMEDIATE", err)) return false;
    for (auto& ch : changes) if (!putObject(d.db, 1, ch.second, d.slots[ch.first], err)) return false;
    if (!writeCommit(d, err)) return false;
    g_shm->phase = 3;
    if (!execSql(d.db, "COMMIT", err)) { d.slots = backup.slots; d.head = backup.head; return false; }
    d.seq++; g_shm->acked = d.seq; g_shm->phase = 1;
    return true;
}

// ------------------------------------------------------------------ checker
struct CheckResult { bool ok = false; int64_t headSeq = -1; int64_t objects = 0; int64_t bytesHashed = 0; std::string err; double ms = 0; };
static CheckResult checkFile(const std::string& path) {
    CheckResult r; double t0 = nowMs();
    auto fail = [&](const std::string& e) { r.ok = false; r.err = e; r.ms = nowMs() - t0; return r; };
    if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) { r.ok = true; r.ms = nowMs() - t0; return r; }   // never created
    sqlite3* db = nullptr; std::string err;
    int rc = sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr);
    if (rc != SQLITE_OK) { std::string e = std::string("open: ") + (db ? sqlite3_errmsg(db) : "?"); sqlite3_close(db); return fail(e); }
    sqlite3_busy_handler(db, busyHandler, nullptr);
    struct Closer { sqlite3* d; ~Closer() { sqlite3_close(d); } } closer{db};
    std::string ic;
    if (!execSql(db, "PRAGMA integrity_check", err, &ic)) return fail("integrity_check failed to run: " + err);
    if (ic != "ok") return fail("integrity_check: " + ic);
    std::string n;
    if (!execSql(db, "SELECT count(*) FROM sqlite_schema", err, &n)) return fail(err);
    if (n == "0") { r.ok = true; r.ms = nowMs() - t0; return r; }   // creation transaction never committed
    Doc d; d.db = db;
    if (!loadHead(d, err)) return fail("head: " + err);
    r.headSeq = d.seq;
    std::set<Hash> reachable;
    Hash cur = d.head; int64_t seq = d.seq;
    for (;;) {
        sqlite3_stmt* st = nullptr;
        rc = sqlite3_prepare_v2(db, "SELECT c.root, c.author, c.time_ms, c.message, (SELECT parent FROM commit_parents p WHERE p.commit_id=c.id AND p.ord=0) FROM commits c WHERE c.id=?", -1, &st, nullptr);
        if (rc != SQLITE_OK) return fail("prepare walk");
        sqlite3_bind_blob(st, 1, cur.b, 32, SQLITE_STATIC);
        if (sqlite3_step(st) != SQLITE_ROW) { sqlite3_finalize(st); return fail("commit row missing at seq " + std::to_string(seq)); }
        Hash root, parent; bool hasParent = sqlite3_column_type(st, 4) != SQLITE_NULL;
        memcpy(root.b, sqlite3_column_blob(st, 0), 32);
        std::string author = (const char*)sqlite3_column_text(st, 1), msg = (const char*)sqlite3_column_text(st, 3);
        int64_t t = sqlite3_column_int64(st, 2);
        if (hasParent) memcpy(parent.b, sqlite3_column_blob(st, 4), 32);
        sqlite3_finalize(st);
        if (msg != "save " + std::to_string(seq)) return fail("commit message '" + msg + "' but expected seq " + std::to_string(seq));
        if (!(commitId(hasParent ? &parent : nullptr, root, author, t, msg) == cur)) return fail("commit id hash mismatch at seq " + std::to_string(seq));
        std::vector<uint8_t> data; bool found = false; int64_t raw = 0;
        if (!getObject(db, root, data, &raw, found, err)) return fail(err);
        if (!found) return fail("manifest object missing at seq " + std::to_string(seq));
        if (!(sha256(data.data(), data.size()) == root) || raw != (int64_t)data.size()) return fail("manifest hash mismatch at seq " + std::to_string(seq));
        reachable.insert(root); r.bytesHashed += data.size();
        std::vector<Hash> slots;
        if (!decodeManifest(data, slots)) return fail("manifest malformed at seq " + std::to_string(seq));
        for (auto& h : slots) {
            if (reachable.count(h)) continue;
            if (!getObject(db, h, data, &raw, found, err)) return fail(err);
            if (!found) return fail("blob missing at seq " + std::to_string(seq));
            if (!(sha256(data.data(), data.size()) == h) || raw != (int64_t)data.size()) return fail("blob hash mismatch at seq " + std::to_string(seq));
            reachable.insert(h); r.bytesHashed += data.size();
        }
        if (seq == 0) { if (hasParent) return fail("commit 0 has a parent"); break; }
        if (!hasParent) return fail("parent missing at seq " + std::to_string(seq));
        cur = parent; seq--;
    }
    std::string no, nc;
    if (!execSql(db, "SELECT count(*) FROM objects", err, &no) || !execSql(db, "SELECT count(*) FROM commits", err, &nc)) return fail(err);
    r.objects = atoll(no.c_str());
    if (r.objects != (int64_t)reachable.size()) return fail("orphan objects: table has " + no + ", reachable " + std::to_string(reachable.size()));
    if (atoll(nc.c_str()) != d.seq + 1) return fail("commit count " + nc + " but head seq " + std::to_string(d.seq));
    r.ok = true; r.ms = nowMs() - t0;
    return r;
}

// ------------------------------------------------------------------ journal state (read raw, before anything opens the db)
// Returns: "none", "empty", "zeroed" (header cleared = no transaction in flight), "hdr0" (valid header, 0 records synced),
// "hot" (valid header with synced page records: the database file may be partly overwritten).
static std::string journalState(const std::string& dbPath, uint32_t* nRecOut = nullptr, int64_t* sizeOut = nullptr) {
    std::string j = dbPath + "-journal";
    HANDLE h = CreateFileA(j.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "none";
    LARGE_INTEGER sz; GetFileSizeEx(h, &sz);
    if (sizeOut) *sizeOut = sz.QuadPart;
    uint8_t hdr[12] = {0}; DWORD got = 0;
    ReadFile(h, hdr, 12, &got, nullptr);
    CloseHandle(h);
    if (sz.QuadPart == 0) return "empty";
    static const uint8_t magic[8] = {0xd9, 0xd5, 0x05, 0xf9, 0x20, 0xa1, 0x63, 0xd7};
    if (got < 12 || memcmp(hdr, magic, 8) != 0) return "zeroed";
    uint32_t nRec = (uint32_t)hdr[8] << 24 | (uint32_t)hdr[9] << 16 | (uint32_t)hdr[10] << 8 | hdr[11];
    if (nRecOut) *nRecOut = nRec;
    return nRec == 0 ? "hdr0" : "hot";
}

// ------------------------------------------------------------------ seqlock side file (for the snapshot-copy experiment)
// 16 bytes: int64 saving flag, int64 save counter. Writer sets saving=1 before BEGIN and saving=0,counter+1 after COMMIT.
static HANDLE g_seqFile = INVALID_HANDLE_VALUE;
static void seqWrite(int64_t saving, int64_t counter) {
    if (g_seqFile == INVALID_HANDLE_VALUE) return;
    int64_t v[2] = {saving, counter}; DWORD w; OVERLAPPED ov = {};
    WriteFile(g_seqFile, v, 16, &w, &ov);
}
static bool seqRead(const std::string& path, int64_t v[2]) {
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD got = 0; BOOL ok = ReadFile(h, v, 16, &got, nullptr);
    CloseHandle(h);
    return ok && got == 16;
}

// ------------------------------------------------------------------ writer process
// writer <path> <shmName> <maxBlobKB> <bigPercent> <seqlock 0|1> <sleepMsBetweenSaves>
static int cmdWriter(int argc, char** argv) {
    if (argc < 8) return 2;
    std::string path = argv[2];
    Shm* s = mapShm(argv[3], false);
    if (s) g_shm = s;
    int maxBlobKB = atoi(argv[4]), bigPct = atoi(argv[5]), seqlock = atoi(argv[6]), sleepMs = atoi(argv[7]);
    LARGE_INTEGER qpc; QueryPerformanceCounter(&qpc);
    Rng rng((uint64_t)qpc.QuadPart * 2654435761u + GetCurrentProcessId());
    Doc d; std::string err;
    if (!openDoc(path, d, err, 16, 1024, rng)) { fprintf(stderr, "writer: open failed: %s\n", err.c_str()); return 3; }
    if (seqlock) {
        g_seqFile = CreateFileA((path + ".seq").c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, 0, nullptr);
        seqWrite(0, d.seq);
    }
    for (;;) {
        std::vector<std::pair<int, std::vector<uint8_t>>> changes;
        int n = (int)rng.range(1, 4);
        for (int i = 0; i < n; i++) {
            size_t sz = (size_t)rng.range(1024, (uint64_t)maxBlobKB * 1024);
            if (bigPct > 0 && (int)rng.range(0, 99) < bigPct) sz = (size_t)rng.range(2u << 20, 6u << 20);   // exceeds the 2 MB page cache: forces a mid-transaction spill
            std::vector<uint8_t> blob(sz); rng.fill(blob);
            int slot;   // distinct slots, so every blob written by a save is referenced by its manifest
            do { slot = (int)rng.range(0, d.slots.size() - 1); } while (std::any_of(changes.begin(), changes.end(), [&](auto& c) { return c.first == slot; }));
            changes.emplace_back(slot, std::move(blob));
        }
        seqWrite(1, d.seq);
        if (!saveDoc(d, changes, err)) { fprintf(stderr, "writer: save failed: %s\n", err.c_str()); return 3; }
        seqWrite(0, d.seq);
        if (sleepMs) Sleep(sleepMs);
    }
}

// ------------------------------------------------------------------ process helpers
static std::string selfExe() { char b[MAX_PATH]; GetModuleFileNameA(nullptr, b, MAX_PATH); return b; }
static HANDLE spawn(const std::string& args) {
    std::string cmd = "\"" + selfExe() + "\" " + args;
    STARTUPINFOA si = {sizeof si}; PROCESS_INFORMATION pi;
    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) { fprintf(stderr, "CreateProcess failed %lu\n", GetLastError()); exit(9); }
    CloseHandle(pi.hThread);
    return pi.hProcess;
}
static void spinWaitMs(double ms) { double end = nowMs() + ms; while (nowMs() < end) { if (end - nowMs() > 3) Sleep(1); else YieldProcessor(); } }
static void removeDoc(const std::string& path) {
    DeleteFileA(path.c_str()); DeleteFileA((path + "-journal").c_str()); DeleteFileA((path + ".seq").c_str()); DeleteFileA((path + ".lock").c_str());
}

// ------------------------------------------------------------------ kill harness
// kill <path> <iterations> <csvOut> [maxDelayMs=200] [seed=1] [rotateMB=100]
static int cmdKill(int argc, char** argv) {
    if (argc < 5) return 2;
    std::string path = argv[2]; int iters = atoi(argv[3]); std::string csvPath = argv[4];
    double maxDelay = argc > 5 ? atof(argv[5]) : 200; uint64_t seed = argc > 6 ? strtoull(argv[6], nullptr, 10) : 1;
    int64_t rotateBytes = (argc > 7 ? atoll(argv[7]) : 100) << 20;
    std::mt19937_64 rng(seed);
    char shmName[64]; sprintf(shmName, "s10shm_%lu", GetCurrentProcessId());
    Shm* shm = mapShm(shmName, true);
    FILE* csv = fopen(csvPath.c_str(), "w");
    fprintf(csv, "iter,delay_ms,phase,journal,journal_nrec,journal_bytes,extra_kill,extra_journal,lo,hi,head_seq,db_bytes,objects,check_ms,busy,ok,err\n");
    removeDoc(path);
    int64_t lo = -1, hi = -1;   // the head seq after recovery must be within [lo, hi]
    std::map<std::string, int> jCount, phaseCount, jByPhase; int fails = 0, selfExit = 0, extraKills = 0, rotations = 0, totalKills = 0;
    int64_t busyTotal = 0, savesAcked = 0; double checkMsTotal = 0, t0 = nowMs();
    std::string writerArgs = "writer \"" + path + "\" " + shmName + " 256 10 0 0";
    auto account = [&]() {   // fold one dead writer's report into the bounds
        int64_t sh = shm->startHead, ak = shm->acked, sd = shm->started;
        if (sh != UNSET) {
            int64_t top = std::max<int64_t>(sh, ak == UNSET ? -1 : ak);
            int64_t begun = sd == UNSET ? top : std::max<int64_t>(top, sd);
            lo = std::max(lo, top); hi = std::max(hi, begun);
            if (ak != UNSET && ak > sh) savesAcked += ak - sh;
        }
        busyTotal += shm->busy;
    };
    for (int i = 0; i < iters; i++) {
        if (fileSize(path) > rotateBytes) { removeDoc(path); lo = hi = -1; rotations++; }
        resetShm(shm);
        double delay = std::uniform_real_distribution<double>(0, maxDelay)(rng);
        HANDLE p = spawn(writerArgs);
        spinWaitMs(delay);
        if (WaitForSingleObject(p, 0) == WAIT_OBJECT_0) { DWORD ec; GetExitCodeProcess(p, &ec); selfExit++; fprintf(stderr, "iter %d: writer exited by itself, code %lu\n", i, ec); }
        TerminateProcess(p, 99); WaitForSingleObject(p, INFINITE); CloseHandle(p); totalKills++;
        LONG64 phase = shm->phase;
        uint32_t nrec = 0; int64_t jbytes = 0;
        std::string js = journalState(path, &nrec, &jbytes);
        account();
        // One time in five: start a second writer on the un-recovered file and kill it very early (during open / hot-journal rollback).
        bool extra = (rng() % 5) == 0; std::string js2 = "-";
        if (extra) {
            resetShm(shm);
            HANDLE p2 = spawn(writerArgs);
            spinWaitMs(std::uniform_real_distribution<double>(0, 25)(rng));
            TerminateProcess(p2, 99); WaitForSingleObject(p2, INFINITE); CloseHandle(p2); extraKills++; totalKills++;
            js2 = journalState(path);
            account();
        }
        int64_t dbBytes = fileSize(path);
        CheckResult r = checkFile(path);
        bool ok = r.ok;
        if (ok && (r.headSeq < lo || r.headSeq > hi)) { ok = false; r.err = "head seq " + std::to_string(r.headSeq) + " outside expected [" + std::to_string(lo) + "," + std::to_string(hi) + "]"; }
        if (!ok) { fails++; fprintf(stderr, "iter %d: CHECK FAILED: %s\n", i, r.err.c_str()); }
        const char* phaseName = phase == 0 ? "starting" : phase == 1 ? "idle" : phase == 2 ? "txn-statements" : "in-COMMIT";
        jCount[js]++; phaseCount[phaseName]++; jByPhase[std::string(phaseName) + " / journal " + js]++;
        if (extra) jCount["extra:" + js2]++;
        fprintf(csv, "%d,%.3f,%s,%s,%u,%lld,%d,%s,%lld,%lld,%lld,%lld,%lld,%.1f,%lld,%d,%s\n", i, delay, phaseName, js.c_str(), nrec, (long long)jbytes, extra ? 1 : 0, js2.c_str(),
                (long long)lo, (long long)hi, (long long)r.headSeq, (long long)dbBytes, (long long)r.objects, r.ms, (long long)shm->busy, ok ? 1 : 0, r.err.c_str());
        fflush(csv);
        checkMsTotal += r.ms;
        if (r.ok) lo = hi = r.headSeq;
        if ((i + 1) % 50 == 0) { printf("  %d/%d iterations, %d failures, head seq %lld, db %.1f MB\n", i + 1, iters, fails, (long long)r.headSeq, dbBytes / 1048576.0); fflush(stdout); }
    }
    fclose(csv);
    printf("KILL TEST path=%s\n", path.c_str());
    printf("iterations (each = kill + check): %d   extra early kills on un-recovered file: %d   total TerminateProcess calls: %d\n", iters, extraKills, totalKills);
    printf("check failures: %d   writer exited by itself: %d   SQLITE_BUSY retries: %lld\n", fails, selfExit, (long long)busyTotal);
    printf("saves acknowledged by writers: %lld   file rotations (at %lld MB): %d   wall time %.1f s   mean check %.1f ms\n", (long long)savesAcked, (long long)(rotateBytes >> 20), rotations, (nowMs() - t0) / 1000, checkMsTotal / iters);
    printf("writer phase at kill:\n"); for (auto& kv : phaseCount) printf("  %-16s %d\n", kv.first.c_str(), kv.second);
    printf("journal file state right after kill (before any reopen):\n"); for (auto& kv : jCount) printf("  %-16s %d\n", kv.first.c_str(), kv.second);
    printf("phase x journal:\n"); for (auto& kv : jByPhase) printf("  %-40s %d\n", kv.first.c_str(), kv.second);
    return fails || selfExit ? 1 : 0;
}

// ------------------------------------------------------------------ lock file
struct LockInfo { std::string user, host, since, heartbeat; long pid = 0; long long beat = 0; double ageMs = -1; bool ownerHandleOpen = false; };
struct LockHandle { HANDLE h = INVALID_HANDLE_VALUE; std::string path, user, host, since; long long beat = 0; };
static std::string lockBody(const LockHandle& l) {
    char b[512];
    sprintf(b, "user=%s\nhost=%s\npid=%lu\nsince=%s\nheartbeat=%s\ncount=%lld\n", l.user.c_str(), l.host.c_str(), GetCurrentProcessId(), l.since.c_str(), isoNow().c_str(), l.beat);
    std::string s = b; s.resize(256, ' ');   // fixed size, overwritten in place, so a reader never sees a short file
    return s;
}
static void lockBeat(LockHandle& l) {
    l.beat++;
    std::string s = lockBody(l); DWORD w; OVERLAPPED ov = {};
    WriteFile(l.h, s.data(), (DWORD)s.size(), &w, &ov);
    FlushFileBuffers(l.h);
}
static bool parseLock(const std::string& text, LockInfo& li) {
    auto get = [&](const char* key) { std::string k = std::string(key) + "="; size_t p = text.find(k); if (p == std::string::npos) return std::string(); size_t e = text.find('\n', p); return text.substr(p + k.size(), e - p - k.size()); };
    li.user = get("user"); li.host = get("host"); li.since = get("since"); li.heartbeat = get("heartbeat"); li.pid = atol(get("pid").c_str()); li.beat = atoll(get("count").c_str());
    return !li.user.empty() && !li.host.empty() && !li.since.empty();
}
static int64_t ftToMs(FILETIME ft) { return (int64_t)((((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10000); }
// Reads the lock file. Age is measured on the file server's clock: mtime of a probe file just created next to the lock minus mtime of the lock.
static bool readLock(const std::string& lockPath, LockInfo& li, DWORD* winErr) {
    HANDLE h = CreateFileA(lockPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) { *winErr = GetLastError(); return false; }
    char buf[512] = {0}; DWORD got = 0; ReadFile(h, buf, 511, &got, nullptr);
    FILETIME lockWrite; GetFileTime(h, nullptr, nullptr, &lockWrite);
    CloseHandle(h);
    if (!parseLock(std::string(buf, got), li)) { *winErr = ERROR_INVALID_DATA; return false; }
    char probe[600]; sprintf(probe, "%s.probe.%lu.%llu", lockPath.c_str(), GetCurrentProcessId(), (unsigned long long)GetTickCount64());
    HANDLE ph = CreateFileA(probe, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (ph != INVALID_HANDLE_VALUE) {
        DWORD w; WriteFile(ph, "x", 1, &w, nullptr); FlushFileBuffers(ph);
        FILETIME pw; GetFileTime(ph, nullptr, nullptr, &pw); CloseHandle(ph);
        li.ageMs = (double)(ftToMs(pw) - ftToMs(lockWrite));
    }
    // Owner-alive probe: the owner keeps its handle open without FILE_SHARE_DELETE, so asking for DELETE access fails while it lives.
    HANDLE dh = CreateFileA(lockPath.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (dh == INVALID_HANDLE_VALUE) li.ownerHandleOpen = GetLastError() == ERROR_SHARING_VIOLATION;
    else { li.ownerHandleOpen = false; CloseHandle(dh); }
    return true;
}
enum class LockResult { Acquired, InUse, Stale, Error };
// Plan design: create <file>.lock exclusively. If it exists and its heartbeat is younger than staleMs: InUse.
// Older: Stale; with breakStale the stale file is renamed away (only one breaker can win the rename) and creation is retried.
static LockResult acquireLock(const std::string& docPath, double staleMs, bool breakStale, LockHandle& out, LockInfo& owner, std::string& msg) {
    std::string lp = docPath + ".lock";
    for (int attempt = 0; attempt < 5; attempt++) {
        HANDLE h = CreateFileA(lp.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            char user[256] = "?", host[256] = "?"; DWORD n = 256; GetUserNameA(user, &n); n = 256; GetComputerNameA(host, &n);
            out.h = h; out.path = lp; out.user = user; out.host = host; out.since = isoNow(); out.beat = 0;
            lockBeat(out);
            msg = "lock acquired";
            return LockResult::Acquired;
        }
        DWORD e = GetLastError();
        if (e != ERROR_FILE_EXISTS && e != ERROR_ALREADY_EXISTS) { msg = "cannot create lock file, Windows error " + std::to_string(e); return LockResult::Error; }
        DWORD re = 0;
        if (!readLock(lp, owner, &re)) { if (re == ERROR_FILE_NOT_FOUND) continue; Sleep(50); continue; }   // vanished or mid-write: retry
        char b[600];
        if (owner.ageMs >= 0 && owner.ageMs < staleMs) {
            sprintf(b, "in use by %s@%s since %s (heartbeat %.1f s ago, pid %ld)", owner.user.c_str(), owner.host.c_str(), owner.since.c_str(), owner.ageMs / 1000, owner.pid);
            msg = b; return LockResult::InUse;
        }
        sprintf(b, "stale lock left by %s@%s since %s (last heartbeat %.1f s ago, limit %.1f s)", owner.user.c_str(), owner.host.c_str(), owner.since.c_str(), owner.ageMs / 1000, staleMs / 1000);
        msg = b;
        if (!breakStale) return LockResult::Stale;
        char broken[600]; sprintf(broken, "%s.broken.%lu.%llu", lp.c_str(), GetCurrentProcessId(), (unsigned long long)GetTickCount64());
        if (MoveFileExA(lp.c_str(), broken, 0)) DeleteFileA(broken);
        else if (GetLastError() == ERROR_SHARING_VIOLATION) { msg += "; cannot break: the owner process still has the lock file open"; return LockResult::InUse; }
        breakStale = true;   // loop: retry CREATE_NEW; if someone else won the race we report them as the owner
    }
    msg = "could not acquire lock after retries";
    return LockResult::Error;
}
static void releaseLock(LockHandle& l) { if (l.h != INVALID_HANDLE_VALUE) { CloseHandle(l.h); DeleteFileA(l.path.c_str()); l.h = INVALID_HANDLE_VALUE; } }
static const char* lrName(LockResult r) { return r == LockResult::Acquired ? "ACQUIRED" : r == LockResult::InUse ? "IN_USE" : r == LockResult::Stale ? "STALE" : "ERROR"; }

// hold <path> <shmName> <heartbeatMs> : take the lock, open the document, read, wait, save once, then heartbeat forever.
static int cmdHold(int argc, char** argv) {
    if (argc < 5) return 2;
    std::string path = argv[2]; Shm* s = mapShm(argv[3], false); if (s) g_shm = s; int hbMs = atoi(argv[4]);
    LockHandle lk; LockInfo owner; std::string msg;
    LockResult lr = acquireLock(path, 1e12, false, lk, owner, msg);
    if (lr != LockResult::Acquired) { fprintf(stderr, "hold: %s\n", msg.c_str()); return 4; }
    LARGE_INTEGER qpc; QueryPerformanceCounter(&qpc); Rng rng((uint64_t)qpc.QuadPart);
    Doc d; std::string err;
    if (!openDoc(path, d, err, 16, 1024, rng)) { fprintf(stderr, "hold: open failed: %s\n", err.c_str()); return 3; }
    g_shm->aux = 1;                                   // opened and read; no write yet
    while (g_shm->aux == 1) { Sleep(hbMs / 5 + 1); static double last = nowMs(); if (nowMs() - last >= hbMs) { lockBeat(lk); last = nowMs(); } }
    std::vector<std::pair<int, std::vector<uint8_t>>> ch; ch.emplace_back(0, std::vector<uint8_t>(5000, 7));
    if (!saveDoc(d, ch, err)) { fprintf(stderr, "hold: save failed: %s\n", err.c_str()); return 3; }
    g_shm->aux = 3;                                   // has written once
    for (;;) { Sleep(hbMs); lockBeat(lk); }
}
static std::string sqliteProbe(const std::string& path) {
    sqlite3* db = nullptr; std::string out;
    int rc = sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr);
    if (rc != SQLITE_OK) { out = std::string("open rc=") + std::to_string(rc); sqlite3_close(db); return out; }
    rc = sqlite3_exec(db, "SELECT count(*) FROM refs", nullptr, nullptr, nullptr);
    out = std::string("read: ") + (rc == SQLITE_OK ? "OK" : sqlite3_errstr(rc)) + " (rc=" + std::to_string(rc) + ")";
    rc = sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
    int rc2 = rc, rc3 = rc;
    if (rc == SQLITE_OK) { rc2 = sqlite3_exec(db, "INSERT INTO meta VALUES('intruder','x')", nullptr, nullptr, nullptr); rc3 = sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr); sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr); }
    out += std::string("; write: BEGIN IMMEDIATE ") + (rc == SQLITE_OK ? "OK" : sqlite3_errstr(rc)) + ", COMMIT " + (rc3 == SQLITE_OK ? "OK (intruder row written)" : sqlite3_errstr(rc3)) + " (rc=" + std::to_string(rc3) + ")";
    (void)rc2;
    sqlite3_close(db);
    return out;
}
// locktest <path> [heartbeatMs=500] [staleMs=2000]
static int cmdLockTest(int argc, char** argv) {
    if (argc < 3) return 2;
    std::string path = argv[2]; int hbMs = argc > 3 ? atoi(argv[3]) : 500; double staleMs = argc > 4 ? atof(argv[4]) : 2000;
    char shmName[64]; sprintf(shmName, "s10shm_%lu", GetCurrentProcessId());
    Shm* shm = mapShm(shmName, true); resetShm(shm);
    removeDoc(path);
    int bad = 0;
    auto expect = [&](bool c, const char* what) { printf("    %s: %s\n", c ? "PASS" : "FAIL", what); if (!c) bad++; };
    printf("LOCK TEST path=%s heartbeat=%d ms stale limit=%.0f ms (plan values: 30 s and 120 s)\n", path.c_str(), hbMs, staleMs);
    { Rng rng(3); Doc d; std::string err; if (!openDoc(path, d, err, 16, 1024, rng)) { printf("create failed: %s\n", err.c_str()); return 1; } sqlite3_close(d.db); resetShm(shm); }
    HANDLE holder = spawn("hold \"" + path + "\" " + shmName + " " + std::to_string(hbMs));
    while (shm->aux != 1) { if (WaitForSingleObject(holder, 10) == WAIT_OBJECT_0) { printf("holder died\n"); return 1; } }
    LockHandle lk; LockInfo owner; std::string msg;
    printf("[1] holder has opened and read the document, not yet written\n");
    LockResult lr = acquireLock(path, staleMs, false, lk, owner, msg);
    printf("    second opener: %s: %s\n", lrName(lr), msg.c_str());
    expect(lr == LockResult::InUse && !owner.user.empty() && !owner.host.empty() && !owner.since.empty(), "second opener told who holds the file and since when");
    expect(owner.ownerHandleOpen, "owner-alive probe (lock file handle still open) says alive");
    printf("    raw SQLite second connection, ignoring the lock file: %s\n", sqliteProbe(path).c_str());
    Sleep(hbMs * 3);
    LockInfo o2; DWORD e;
    readLock(path + ".lock", o2, &e);
    printf("    after %d ms: beat counter %lld -> %lld, heartbeat age on server clock %.0f ms\n", hbMs * 3, owner.beat, o2.beat, o2.ageMs);
    expect(o2.beat > owner.beat && o2.ageMs < staleMs, "heartbeat advances and lock stays fresh while the holder lives");
    shm->aux = 2;
    while (shm->aux != 3) { if (WaitForSingleObject(holder, 10) == WAIT_OBJECT_0) { printf("holder died\n"); return 1; } }
    printf("[2] holder has saved once (now holds SQLite EXCLUSIVE lock for the session)\n");
    printf("    raw SQLite second connection, ignoring the lock file: %s\n", sqliteProbe(path).c_str());
    lr = acquireLock(path, staleMs, true, lk, owner, msg);
    printf("    second opener asking to break: %s: %s\n", lrName(lr), msg.c_str());
    expect(lr == LockResult::InUse, "a live lock cannot be broken");
    printf("[3] holder killed with TerminateProcess\n");
    TerminateProcess(holder, 99); WaitForSingleObject(holder, INFINITE); CloseHandle(holder);
    double tKill = nowMs();
    lr = acquireLock(path, staleMs, false, lk, owner, msg);
    printf("    immediately after kill: %s: %s   owner-alive probe: %s\n", lrName(lr), msg.c_str(), owner.ownerHandleOpen ? "alive" : "dead");
    expect(!owner.ownerHandleOpen, "owner-alive probe reports dead immediately after the kill");
    while ((lr = acquireLock(path, staleMs, false, lk, owner, msg)) == LockResult::InUse) Sleep(20);
    printf("    %.0f ms after kill: %s: %s\n", nowMs() - tKill, lrName(lr), msg.c_str());
    expect(lr == LockResult::Stale, "stale lock detected by heartbeat age");
    lr = acquireLock(path, staleMs, true, lk, owner, msg);
    printf("    break: %s (%s)\n", lrName(lr), msg.c_str());
    expect(lr == LockResult::Acquired, "stale lock broken and re-acquired");
    CheckResult cr = checkFile(path);
    printf("    document after recovery: check %s, head seq %lld %s\n", cr.ok ? "ok" : "FAILED", (long long)cr.headSeq, cr.err.c_str());
    expect(cr.ok && cr.headSeq == 1, "document is intact at the holder's last save");
    {
        Rng rng(5); Doc d; std::string err; bool ok = openDoc(path, d, err, 16, 1024, rng);
        std::vector<std::pair<int, std::vector<uint8_t>>> ch; ch.emplace_back(1, std::vector<uint8_t>(3000, 9));
        ok = ok && saveDoc(d, ch, err); sqlite3_close(d.db);
        expect(ok, "new lock owner can open and save");
    }
    printf("[4] clean release\n");
    releaseLock(lk);
    expect(GetFileAttributesA((path + ".lock").c_str()) == INVALID_FILE_ATTRIBUTES, "lock file removed on release");
    lr = acquireLock(path, staleMs, false, lk, owner, msg);
    expect(lr == LockResult::Acquired, "next opener acquires without prompt");
    releaseLock(lk);
    printf("    journal file after clean close: %s\n", journalState(path).c_str());
    printf("LOCK TEST RESULT: %s\n", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}

// ------------------------------------------------------------------ 200 MB file: build and incremental save
// build <path> <slots> <slotBytes>
static int cmdBuild(int argc, char** argv) {
    if (argc < 5) return 2;
    std::string path = argv[2]; removeDoc(path);
    Rng rng(42); Doc d; std::string err; double t0 = nowMs();
    if (!openDoc(path, d, err, atoi(argv[3]), (size_t)atoll(argv[4]), rng)) { fprintf(stderr, "build failed: %s\n", err.c_str()); return 3; }
    sqlite3_close(d.db);
    printf("BUILD %s: %d objects x %lld bytes, file %.1f MB, %.1f s\n", path.c_str(), atoi(argv[3]), atoll(argv[4]), fileSize(path) / 1048576.0, (nowMs() - t0) / 1000);
    return 0;
}
// bench <path> <changeBytes> <reps>
static int cmdBench(int argc, char** argv) {
    if (argc < 5) return 2;
    std::string path = argv[2]; size_t changeBytes = (size_t)atoll(argv[3]); int reps = atoi(argv[4]);
    Rng rng(GetTickCount64()); Doc d; std::string err;
    int64_t size0 = fileSize(path);
    double t0 = nowMs();
    if (!openDoc(path, d, err, 0, 0, rng)) { fprintf(stderr, "bench open failed: %s\n", err.c_str()); return 3; }
    double openMs = nowMs() - t0;
    std::string ps, cs;
    execSql(d.db, "PRAGMA page_size", err, &ps); execSql(d.db, "PRAGMA cache_size", err, &cs);
    printf("BENCH path=%s\n  file %.1f MB, %zu manifest slots (manifest object %zu bytes), page_size=%s cache_size=%s\n  settings: %s\n", path.c_str(), size0 / 1048576.0, d.slots.size(), 8 + d.slots.size() * 32, ps.c_str(), cs.c_str(), d.pragmas.c_str());
    printf("  open + read ref, commit and manifest: %.1f ms (OS file cache state not controlled)\n", openMs);
    std::vector<double> ms; std::vector<uint64_t> bytes, ops;
    HANDLE me = GetCurrentProcess();
    for (int i = 0; i < reps; i++) {
        std::vector<std::pair<int, std::vector<uint8_t>>> ch;
        std::vector<uint8_t> blob(changeBytes); rng.fill(blob);
        ch.emplace_back((int)rng.range(0, d.slots.size() - 1), std::move(blob));
        IO_COUNTERS a, b; GetProcessIoCounters(me, &a);
        int64_t s0 = fileSize(path);
        double t = nowMs();
        if (!saveDoc(d, ch, err)) { fprintf(stderr, "bench save failed: %s\n", err.c_str()); return 3; }
        double dt = nowMs() - t;
        GetProcessIoCounters(me, &b);
        ms.push_back(dt); bytes.push_back(b.WriteTransferCount - a.WriteTransferCount); ops.push_back(b.WriteOperationCount - a.WriteOperationCount);
        printf("  save %2d: %7.2f ms, %8llu bytes written in %3llu write calls, file grew %lld bytes\n", i + 1, dt, (unsigned long long)bytes.back(), (unsigned long long)ops.back(), (long long)(fileSize(path) - s0));
    }
    auto med = [](std::vector<double> v) { std::sort(v.begin(), v.end()); return v[v.size() / 2]; };
    std::vector<double> bd(bytes.begin(), bytes.end());
    printf("  SUMMARY change=%zu bytes: save time median %.2f ms (min %.2f, max %.2f); bytes written median %.0f (min %.0f, max %.0f); file grew %lld bytes over %d saves\n",
           changeBytes, med(ms), *std::min_element(ms.begin(), ms.end()), *std::max_element(ms.begin(), ms.end()), med(bd), *std::min_element(bd.begin(), bd.end()), *std::max_element(bd.begin(), bd.end()),
           (long long)(fileSize(path) - size0), reps);
    sqlite3_close(d.db);
    return 0;
}
// check <path>
static int cmdCheck(int argc, char** argv) {
    if (argc < 3) return 2;
    CheckResult r = checkFile(argv[2]);
    printf("CHECK %s: %s head seq %lld, %lld objects, %.1f MB hashed, %.0f ms %s\n", argv[2], r.ok ? "ok" : "FAILED", (long long)r.headSeq, (long long)r.objects, r.bytesHashed / 1048576.0, r.ms, r.err.c_str());
    return r.ok ? 0 : 1;
}

// ------------------------------------------------------------------ snapshot-copy experiment (PLAN 4.3: readers use a snapshot copy)
// copytest <path> <copies> : a writer saves continuously; this process copies the database file and checks each copy.
static int cmdCopyTest(int argc, char** argv) {
    if (argc < 4) return 2;
    std::string path = argv[2]; int copies = atoi(argv[3]);
    char shmName[64]; sprintf(shmName, "s10shm_%lu", GetCurrentProcessId());
    Shm* shm = mapShm(shmName, true); resetShm(shm);
    removeDoc(path);
    std::string copy = path + ".copy";
    HANDLE w = spawn("writer \"" + path + "\" " + shmName + " 8 0 1 " + std::string(argc > 4 ? argv[4] : "0"));
    while (shm->acked == UNSET || shm->acked < 20) Sleep(5);
    std::mt19937_64 rng(7);
    int naiveBad = 0, naiveFail = 0, withJournalBad = 0, guardedBad = 0, guardedRetries = 0, guardedGaveUp = 0;
    std::map<std::string, int> errs;
    for (int i = 0; i < copies; i++) {
        spinWaitMs(std::uniform_real_distribution<double>(0, 20)(rng));
        // (a) naive: copy only the database file
        DeleteFileA(copy.c_str()); DeleteFileA((copy + "-journal").c_str());
        if (!CopyFileA(path.c_str(), copy.c_str(), FALSE)) naiveFail++;
        else { CheckResult r = checkFile(copy); if (!r.ok) { naiveBad++; errs["naive: " + r.err.substr(0, 60)]++; } }
        // (b) copy database then journal, let SQLite roll the copy back
        DeleteFileA(copy.c_str()); DeleteFileA((copy + "-journal").c_str());
        if (CopyFileA(path.c_str(), copy.c_str(), FALSE)) {
            CopyFileA((path + "-journal").c_str(), (copy + "-journal").c_str(), FALSE);
            CheckResult r = checkFile(copy); if (!r.ok) { withJournalBad++; errs["db+journal: " + r.err.substr(0, 60)]++; }
        }
        // (c) guarded by the writer's save counter (seqlock): retry until no save overlapped the copy
        bool got = false;
        for (int attempt = 0; attempt < 200 && !got; attempt++) {
            int64_t a[2], b[2];
            DeleteFileA(copy.c_str()); DeleteFileA((copy + "-journal").c_str());
            if (!seqRead(path + ".seq", a) || a[0] != 0) { guardedRetries++; Sleep(1); continue; }
            if (!CopyFileA(path.c_str(), copy.c_str(), FALSE)) { guardedRetries++; continue; }
            if (!seqRead(path + ".seq", b) || b[0] != 0 || b[1] != a[1]) { guardedRetries++; continue; }
            got = true;
            CheckResult r = checkFile(copy); if (!r.ok) { guardedBad++; errs["guarded: " + r.err.substr(0, 60)]++; }
        }
        if (!got) guardedGaveUp++;
        if (fileSize(path) > (300ll << 20)) { printf("  stopping early at copy %d: file reached 300 MB\n", i + 1); copies = i + 1; break; }
    }
    LONG64 saves = shm->acked;
    TerminateProcess(w, 99); WaitForSingleObject(w, INFINITE); CloseHandle(w);
    DeleteFileA(copy.c_str()); DeleteFileA((copy + "-journal").c_str());
    printf("COPY TEST path=%s copies=%d (writer made %lld saves meanwhile, file %.1f MB)\n", path.c_str(), copies, (long long)saves, fileSize(path) / 1048576.0);
    printf("  (a) copy of the database file alone:          %d bad of %d (%d copy calls failed)\n", naiveBad, copies - naiveFail, naiveFail);
    printf("  (b) copy of database then journal + rollback: %d bad of %d\n", withJournalBad, copies);
    printf("  (c) copy guarded by writer's save counter:    %d bad of %d (%d retries, %d gave up after 200 attempts)\n", guardedBad, copies - guardedGaveUp, guardedRetries, guardedGaveUp);
    for (auto& kv : errs) printf("      %4d x %s\n", kv.second, kv.first.c_str());
    return 0;
}

static int usage() {
    printf("s10 kill <path> <iterations> <csv> [maxDelayMs] [seed] [rotateMB]\n"
           "s10 locktest <path> [heartbeatMs] [staleMs]\n"
           "s10 build <path> <slots> <slotBytes>\n"
           "s10 bench <path> <changeBytes> <reps>\n"
           "s10 check <path>\n"
           "s10 copytest <path> <copies>\n"
           "s10 version\n"
           "(internal: writer, hold)\n");
    return 2;
}
int main(int argc, char** argv) {
    if (argc < 2) return usage();
    std::string c = argv[1];
    if (c == "writer") return cmdWriter(argc, argv);
    if (c == "hold") return cmdHold(argc, argv);
    if (c == "kill") return cmdKill(argc, argv);
    if (c == "locktest") return cmdLockTest(argc, argv);
    if (c == "build") return cmdBuild(argc, argv);
    if (c == "bench") return cmdBench(argc, argv);
    if (c == "check") return cmdCheck(argc, argv);
    if (c == "copytest") return cmdCopyTest(argc, argv);
    if (c == "version") { printf("SQLite %s %s\n", sqlite3_libversion(), sqlite3_sourceid()); return 0; }
    return usage();
}
