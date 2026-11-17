// Spike S3: STEP import / heal / tessellation benchmark and cancel-latency probe for OCCT 8.0.1.
//
//   s3bench run    <file.step> [--threads N] [--occt-threads N] [--tiers low,med,high]
//                              [--modes st,occt,pool,hybrid] [--default-heal]
//   s3bench cancel <file.step> [--trials N] [--seed S] [--threads N] [--tiers ...] [--modes ...]
//   s3bench heal   <file.step> [--threads N]     whole-shape healing against one job per solid
//   s3bench faces  <file.step> [--tiers ...]     slowest faces to mesh, each face meshed alone
//   common: --algo default|watson|delabella
//
// Every measurement is printed as one line:  R|<file>|<stage>|<config>|<key=value>...
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Geom_Surface.hxx>
#include <Bnd_Box.hxx>
#include <DESTEP_ConfigurationNode.hxx>
#include <DESTEP_Provider.hxx>
#include <IMeshTools_Parameters.hxx>
#include <Message_ProgressIndicator.hxx>
#include <Message_ProgressRange.hxx>
#include <Message_ProgressScope.hxx>
#include <OSD_ThreadPool.hxx>
#include <Poly_Triangulation.hxx>
#include <STEPControl_Reader.hxx>
#include <ShapeFix_Shape.hxx>
#include <ShapeProcess.hxx>
#include <Standard_Failure.hxx>
#include <Standard_Version.hxx>
#include <TCollection_AsciiString.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Shape.hxx>
#include <XSControl_WorkSession.hxx>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <random>
#include <streambuf>
#include <string>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <dbghelp.h>

using Clock = std::chrono::steady_clock;
static double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }
static long long nowNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); }

static double peakWsMb()
{
    PROCESS_MEMORY_COUNTERS pmc{};
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
    return double(pmc.PeakWorkingSetSize) / (1024.0 * 1024.0);
}
static double curWsMb()
{
    PROCESS_MEMORY_COUNTERS pmc{};
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
    return double(pmc.WorkingSetSize) / (1024.0 * 1024.0);
}

// Progress indicator with a cancel flag. Also records how often and how regularly OCCT polls UserBreak().
class CancelIndicator : public Message_ProgressIndicator {
public:
    std::atomic<bool> flag{false};
    std::atomic<long long> last{0}, maxGap{0};
    std::atomic<unsigned long long> polls{0};
    bool UserBreak() override
    {
        const long long n = nowNs();
        const long long prev = last.exchange(n);
        long long gap = n - prev, cur = maxGap.load();
        while (gap > cur && !maxGap.compare_exchange_weak(cur, gap)) {}
        polls.fetch_add(1, std::memory_order_relaxed);
        return flag.load(std::memory_order_relaxed);
    }
    void Show(const Message_ProgressScope&, const bool) override {}
    void arm(bool cancelled = false)
    {
        flag = cancelled; last = nowNs(); maxGap = 0; polls = 0;
    }
    double finishMaxGapMs()
    {
        const long long gap = nowNs() - last.load();
        return double(std::max(gap, maxGap.load())) / 1e6;
    }
};

// Sets the cancel flag after a delay unless stopped first. Records when the flag was set.
class Watchdog {
public:
    Watchdog(std::atomic<bool>& flag, double delayMs) : flag_(flag)
    {
        th_ = std::thread([this, delayMs] {
            std::unique_lock<std::mutex> lk(m_);
            if (!cv_.wait_for(lk, std::chrono::duration<double, std::milli>(delayMs), [this] { return stop_; })) {
                flag_ = true;
                setAt_ = Clock::now();
                fired_ = true;
            }
        });
    }
    // Returns latency in ms from flag set to now, or -1 if the stage ended before the flag was set.
    double stop()
    {
        const auto ret = Clock::now();
        { std::lock_guard<std::mutex> lk(m_); stop_ = true; }
        cv_.notify_all();
        th_.join();
        return fired_ ? std::chrono::duration<double, std::milli>(ret - setAt_).count() : -1.0;
    }
private:
    std::atomic<bool>& flag_;
    std::thread th_;
    std::mutex m_;
    std::condition_variable cv_;
    bool stop_ = false, fired_ = false;
    Clock::time_point setAt_;
};

// File stream buffer that reports end-of-file as soon as the cancel flag is set.
class CancelFileBuf : public std::streambuf {
public:
    CancelFileBuf(const std::string& path, std::atomic<bool>* flag) : flag_(flag), buf_(64 * 1024)
    {
        f_ = std::fopen(path.c_str(), "rb");
    }
    ~CancelFileBuf() override { if (f_) std::fclose(f_); }
    bool ok() const { return f_ != nullptr; }
    long long consumed = 0;
    bool sawRealEof = false, cancelledRead = false;
    Clock::time_point eofAt;
protected:
    int_type underflow() override
    {
        if (flag_ && flag_->load()) { cancelledRead = true; return traits_type::eof(); }
        const size_t n = std::fread(buf_.data(), 1, buf_.size(), f_);
        if (n == 0) {
            if (!sawRealEof) { sawRealEof = true; eofAt = Clock::now(); }
            return traits_type::eof();
        }
        consumed += (long long)n;
        setg(buf_.data(), buf_.data(), buf_.data() + n);
        return traits_type::to_int_type(buf_[0]);
    }
private:
    std::FILE* f_ = nullptr;
    std::atomic<bool>* flag_;
    std::vector<char> buf_;
};

struct Tier { const char* name; double chord; double angleDeg; };
static const Tier kTiers[3] = {{"low", 0.2, 20.0}, {"med", 0.05, 12.0}, {"high", 0.01, 6.0}};

struct MeshStats {
    unsigned long long tris = 0, nodes = 0, hash = 1469598103934665603ull;
    int faces = 0, unmeshed = 0;
    std::string unmeshedInfo;
    Bnd_Box box;
};
static int countOf(const TopoDS_Shape& s, TopAbs_ShapeEnum k, TopAbs_ShapeEnum avoid = TopAbs_SHAPE);
static void fnv(unsigned long long& h, const void* p, size_t n)
{
    const unsigned char* b = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
}
static MeshStats meshStats(const TopoDS_Shape& s)
{
    MeshStats st;
    for (TopExp_Explorer ex(s, TopAbs_FACE); ex.More(); ex.Next()) {
        ++st.faces;
        TopLoc_Location loc;
        Handle(Poly_Triangulation) t = BRep_Tool::Triangulation(TopoDS::Face(ex.Current()), loc);
        if (t.IsNull()) {
            ++st.unmeshed;
            if (st.unmeshed <= 3) {
                st.unmeshedInfo += BRep_Tool::Surface(TopoDS::Face(ex.Current()))->DynamicType()->Name();
                st.unmeshedInfo += "(wires=" + std::to_string(countOf(ex.Current(), TopAbs_WIRE)) + ") ";
            }
            continue;
        }
        st.tris += (unsigned long long)t->NbTriangles();
        st.nodes += (unsigned long long)t->NbNodes();
        const gp_Trsf tr = loc.Transformation();
        for (int i = 1; i <= t->NbNodes(); ++i) {
            const gp_Pnt p = t->Node(i);
            const double c[3] = {p.X(), p.Y(), p.Z()};
            fnv(st.hash, c, sizeof(c));
            st.box.Add(p.Transformed(tr));
        }
        for (int i = 1; i <= t->NbTriangles(); ++i) {
            int a, b, c;
            t->Triangle(i).Get(a, b, c);
            const int v[3] = {a, b, c};
            fnv(st.hash, v, sizeof(v));
        }
    }
    return st;
}
static std::string boxStr(const Bnd_Box& b)
{
    if (b.IsVoid()) return "void";
    double x0, y0, z0, x1, y1, z1;
    b.Get(x0, y0, z0, x1, y1, z1);
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%.17g,%.17g,%.17g,%.17g,%.17g,%.17g", x0, y0, z0, x1, y1, z1);
    return buf;
}

static IMeshTools_MeshAlgoType gAlgo = IMeshTools_MeshAlgoType_DEFAULT;
static IMeshTools_Parameters meshParams(const Tier& t, bool inParallel)
{
    IMeshTools_Parameters p;
    p.MeshAlgo = gAlgo;
    p.Deflection = t.chord;
    p.Angle = t.angleDeg * 3.14159265358979323846 / 180.0;
    p.Relative = false;
    p.InParallel = inParallel;
    return p;
}

static std::vector<TopoDS_Shape> bodiesOf(const TopoDS_Shape& s)
{
    std::vector<TopoDS_Shape> v;
    for (TopExp_Explorer ex(s, TopAbs_SOLID); ex.More(); ex.Next()) v.push_back(ex.Current());
    // Faces outside any solid (surface bodies, loose faces) are meshed together as one extra job.
    TopoDS_Compound loose;
    BRep_Builder bb;
    bb.MakeCompound(loose);
    int n = 0;
    for (TopExp_Explorer ex(s, TopAbs_FACE, TopAbs_SOLID); ex.More(); ex.Next(), ++n) bb.Add(loose, ex.Current());
    if (n > 0) v.push_back(loose);
    return v;
}
static int countOf(const TopoDS_Shape& s, TopAbs_ShapeEnum k, TopAbs_ShapeEnum avoid)
{
    int n = 0;
    for (TopExp_Explorer ex(s, k, avoid); ex.More(); ex.Next()) ++n;
    return n;
}

// OR of BRepMesh status flags (IMeshData_Status) of the meshers used by the last runMesh call.
static std::atomic<int> gStatusFlags{0};
// Diagnostic: pass an empty progress range to the per-body jobs (no indicator, no cancel).
static bool gNoProgress = false;

// mode: "st" whole shape single thread; "occt" whole shape InParallel; "pool" one job per body on a
// thread pool, each job single-threaded; "hybrid" one job per body, each with InParallel.
// Returns elapsed ms.
static double runMesh(const TopoDS_Shape& shape, const Tier& tier, const std::string& mode, unsigned threads,
                      const Handle(CancelIndicator)& ind)
{
    const auto t0 = Clock::now();
    if (mode == "st" || mode == "occt") {
        BRepMesh_IncrementalMesh m(shape, meshParams(tier, mode == "occt"), ind->Start());
        gStatusFlags.fetch_or(m.GetStatusFlags());
        return msSince(t0);
    }
    const bool inner = mode == "hybrid";
    std::vector<TopoDS_Shape> bodies = bodiesOf(shape);
    if (bodies.empty()) bodies.push_back(shape);
    Message_ProgressScope scope(ind->Start(), "mesh", double(bodies.size()));
    std::vector<Message_ProgressRange> ranges;
    ranges.reserve(bodies.size());
    for (size_t i = 0; i < bodies.size(); ++i) ranges.emplace_back(scope.Next());
    std::atomic<size_t> next{0};
    auto work = [&] {
        for (;;) {
            const size_t i = next.fetch_add(1);
            if (i >= bodies.size()) return;
            if (ind->flag.load()) continue; // cancelled: drain the queue without meshing
            try {
                BRepMesh_IncrementalMesh m(bodies[i], meshParams(tier, inner),
                                           gNoProgress ? Message_ProgressRange() : ranges[i]);
                gStatusFlags.fetch_or(m.GetStatusFlags());
            } catch (const Standard_Failure& e) {
                std::printf("X|pool job %zu Standard_Failure: %s\n", i, e.what());
            } catch (const std::exception& e) {
                std::printf("X|pool job %zu std::exception: %s\n", i, e.what());
            }
        }
    };
    const unsigned n = std::max(1u, std::min<unsigned>(threads, unsigned(bodies.size())));
    std::vector<std::thread> th;
    for (unsigned i = 1; i < n; ++i) th.emplace_back(work);
    work();
    for (auto& t : th) t.join();
    return msSince(t0);
}

static std::vector<std::string> split(const std::string& s)
{
    std::vector<std::string> v;
    size_t a = 0;
    while (a <= s.size()) {
        size_t b = s.find(',', a);
        if (b == std::string::npos) b = s.size();
        if (b > a) v.push_back(s.substr(a, b - a));
        a = b + 1;
    }
    return v;
}
static const Tier* tierByName(const std::string& n)
{
    for (const Tier& t : kTiers) if (n == t.name) return &t;
    return nullptr;
}

struct Args {
    std::string cmd, file, name;
    unsigned threads = std::thread::hardware_concurrency();
    int occtThreads = -1, trials = 3;
    unsigned seed = 12345;
    std::vector<std::string> tiers{"low", "med", "high"}, modes{"st", "occt", "pool", "hybrid"};
    bool defaultHeal = false, skipHeal = false, copyBodies = false;
    int repeat = 1;
};

static void resetTransfer(STEPControl_Reader& r, bool defaultHeal)
{
    r.WS()->InitTransferReader(5);
    r.ClearShapes();
    if (defaultHeal) {
        ShapeProcess::OperationsFlags f;
        f.set(ShapeProcess::Operation::FixShape);
        r.SetShapeProcessFlags(f);
    } else {
        r.SetShapeProcessFlags(ShapeProcess::OperationsFlags());
    }
}

static int cmdRun(const Args& a)
{
    const char* F = a.name.c_str();
    const double sizeMb = double(std::filesystem::file_size(a.file)) / (1024.0 * 1024.0);
    std::printf("R|%s|file|-|sizeMB=%.2f|occt=%s|threads=%u|occtThreads=%d\n", F, sizeMb, OCC_VERSION_COMPLETE,
                a.threads, a.occtThreads);
    TopoDS_Shape shape;
    {
        auto t = Clock::now();
        auto reader = std::make_unique<STEPControl_Reader>();
        std::printf("R|%s|init|-|ms=%.1f|peakMB=%.0f\n", F, msSince(t), peakWsMb());
        t = Clock::now();
        const IFSelect_ReturnStatus st = reader->ReadFile(a.file.c_str());
        std::printf("R|%s|read|-|ms=%.1f|status=%d|peakMB=%.0f|curMB=%.0f\n", F, msSince(t), int(st), peakWsMb(), curWsMb());
        if (st != IFSelect_RetDone) return 1;
        Handle(CancelIndicator) ind = new CancelIndicator;
        resetTransfer(*reader, a.defaultHeal);
        ind->arm();
        t = Clock::now();
        const int roots = reader->TransferRoots(ind->Start());
        const double ms = msSince(t);
        std::printf("R|%s|transfer|%s|ms=%.1f|roots=%d|polls=%llu|maxPollGapMs=%.1f|peakMB=%.0f|curMB=%.0f\n", F,
                    a.defaultHeal ? "default-heal" : "no-heal", ms, roots, ind->polls.load(), ind->finishMaxGapMs(),
                    peakWsMb(), curWsMb());
        shape = reader->OneShape();
        t = Clock::now();
        reader.reset();
        std::printf("R|%s|release-reader|-|ms=%.1f|curMB=%.0f\n", F, msSince(t), curWsMb());
    }
    if (shape.IsNull()) { std::printf("R|%s|error|null shape\n", F); return 1; }
    Handle(CancelIndicator) ind = new CancelIndicator;
    if (!a.defaultHeal && !a.skipHeal) {
        ind->arm();
        const auto t = Clock::now();
        Handle(ShapeFix_Shape) fix = new ShapeFix_Shape(shape);
        const bool changed = fix->Perform(ind->Start());
        const double ms = msSince(t);
        shape = fix->Shape();
        std::printf("R|%s|heal|ShapeFix_Shape|ms=%.1f|changed=%d|polls=%llu|maxPollGapMs=%.1f|peakMB=%.0f|curMB=%.0f\n", F, ms,
                    int(changed), ind->polls.load(), ind->finishMaxGapMs(), peakWsMb(), curWsMb());
    }
    {
        Bnd_Box b;
        BRepBndLib::Add(shape, b, false);
        std::printf("R|%s|shape|-|solids=%d|shells=%d|faces=%d|edges=%d|freeFaces=%d|brepBox=%s\n", F,
                    countOf(shape, TopAbs_SOLID), countOf(shape, TopAbs_SHELL), countOf(shape, TopAbs_FACE),
                    countOf(shape, TopAbs_EDGE), countOf(shape, TopAbs_FACE, TopAbs_SOLID), boxStr(b).c_str());
    }
    if (a.copyBodies) {
        // Diagnostic: replace every solid by a deep copy (geometry copied too), so that no TShape,
        // surface or curve object can be shared between two solids.
        TopoDS_Compound c;
        BRep_Builder bb;
        bb.MakeCompound(c);
        for (const TopoDS_Shape& b : bodiesOf(shape)) bb.Add(c, BRepBuilderAPI_Copy(b, true, false).Shape());
        shape = c;
    }
    for (const std::string& tn : a.tiers) {
        const Tier* tier = tierByName(tn);
        if (!tier) continue;
        for (const std::string& mode : a.modes)
          for (int rep = 0; rep < a.repeat; ++rep) {
            BRepTools::Clean(shape);
            ind->arm();
            gStatusFlags = 0;
            const double ms = runMesh(shape, *tier, mode, a.threads, ind);
            const double gap = ind->finishMaxGapMs();
            const double peak = peakWsMb(), cur = curWsMb();
            const MeshStats st = meshStats(shape);
            std::printf("R|%s|mesh|%s/%s|ms=%.1f|tris=%llu|nodes=%llu|hash=%016llx|unmeshed=%d|polls=%llu|maxPollGapMs=%.1f|"
                        "peakMB=%.0f|curMB=%.0f|meshBox=%s|status=0x%x|unmeshedFaces=%s\n",
                        F, tier->name, mode.c_str(), ms, st.tris, st.nodes, st.hash, st.unmeshed, ind->polls.load(), gap,
                        peak, cur, boxStr(st.box).c_str(), gStatusFlags.load(), st.unmeshedInfo.c_str());
            std::fflush(stdout);
        }
    }
    return 0;
}


// Diagnostic: mesh every face on its own and list the slowest ones.
static int cmdFaces(const Args& a)
{
    const char* F = a.name.c_str();
    STEPControl_Reader reader;
    if (reader.ReadFile(a.file.c_str()) != IFSelect_RetDone) return 1;
    resetTransfer(reader, false);
    reader.TransferRoots();
    TopoDS_Shape shape = reader.OneShape();
    if (!a.skipHeal) {
        Handle(ShapeFix_Shape) fix = new ShapeFix_Shape(shape);
        fix->Perform();
        shape = fix->Shape();
    }
    for (const std::string& tn : a.tiers) {
        const Tier* tier = tierByName(tn);
        if (!tier) continue;
        BRepTools::Clean(shape);
        struct Row { double ms; int wires; int edges; unsigned long long tris; std::string type; };
        std::vector<Row> rows;
        double total = 0;
        for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next()) {
            const TopoDS_Face f = TopoDS::Face(ex.Current());
            const auto t = Clock::now();
            BRepMesh_IncrementalMesh m(f, meshParams(*tier, false));
            Row r;
            r.ms = msSince(t);
            total += r.ms;
            r.wires = countOf(f, TopAbs_WIRE);
            r.edges = countOf(f, TopAbs_EDGE);
            TopLoc_Location loc;
            Handle(Poly_Triangulation) tr = BRep_Tool::Triangulation(f, loc);
            r.tris = tr.IsNull() ? 0 : (unsigned long long)tr->NbTriangles();
            r.type = BRep_Tool::Surface(f)->DynamicType()->Name();
            rows.push_back(r);
        }
        int failed = 0;
        for (const Row& r : rows)
            if (r.tris == 0 && ++failed <= 12)
                std::printf("D|%s|%s|no triangulation|type=%s|wires=%d|edges=%d|ms=%.1f\n", F, tier->name, r.type.c_str(), r.wires,
                            r.edges, r.ms);
        std::printf("D|%s|%s|facesWithoutTriangulation=%d\n", F, tier->name, failed);
        std::sort(rows.begin(), rows.end(), [](const Row& x, const Row& y) { return x.ms > y.ms; });
        std::printf("D|%s|%s|faces=%zu|sumMs=%.1f|peakMB=%.0f\n", F, tier->name, rows.size(), total, peakWsMb());
        for (size_t i = 0; i < rows.size() && i < 6; ++i)
            std::printf("D|%s|%s|top%zu|ms=%.1f|type=%s|wires=%d|edges=%d|tris=%llu\n", F, tier->name, i + 1, rows[i].ms,
                        rows[i].type.c_str(), rows[i].wires, rows[i].edges, rows[i].tris);
    }
    return 0;
}


// Healing variants: ShapeFix_Shape on the whole shape against one ShapeFix_Shape per solid on a
// thread pool. Reports time, and Low-tier triangle count and hash so the results can be compared.
static int cmdHeal(const Args& a)
{
    const char* F = a.name.c_str();
    TopoDS_Shape shape;
    {
        STEPControl_Reader reader;
        if (reader.ReadFile(a.file.c_str()) != IFSelect_RetDone) return 1;
        resetTransfer(reader, false);
        reader.TransferRoots();
        shape = reader.OneShape();
    }
    Handle(CancelIndicator) ind = new CancelIndicator;
    auto report = [&](const char* cfg, double ms, const TopoDS_Shape& s) {
        BRepTools::Clean(s);
        ind->arm();
        runMesh(s, kTiers[0], "pool", a.threads, ind);
        const MeshStats st = meshStats(s);
        std::printf("R|%s|heal|%s|ms=%.1f|solids=%d|faces=%d|lowTris=%llu|lowHash=%016llx|unmeshed=%d|peakMB=%.0f\n", F, cfg, ms,
                    countOf(s, TopAbs_SOLID), st.faces, st.tris, st.hash, st.unmeshed, peakWsMb());
        std::fflush(stdout);
    };
    report("none", 0.0, shape);
    {
        const auto t = Clock::now();
        std::vector<TopoDS_Shape> bodies = bodiesOf(shape);
        std::vector<TopoDS_Shape> out(bodies.size());
        std::atomic<size_t> next{0};
        auto work = [&] {
            for (;;) {
                const size_t i = next.fetch_add(1);
                if (i >= bodies.size()) return;
                try {
                    Handle(ShapeFix_Shape) fix = new ShapeFix_Shape(bodies[i]);
                    fix->Perform();
                    out[i] = fix->Shape();
                } catch (const Standard_Failure&) {
                    out[i] = bodies[i];
                }
            }
        };
        const unsigned n = std::max(1u, std::min<unsigned>(a.threads, unsigned(bodies.size())));
        std::vector<std::thread> th;
        for (unsigned i = 1; i < n; ++i) th.emplace_back(work);
        work();
        for (auto& x : th) x.join();
        const double ms = msSince(t);
        TopoDS_Compound c;
        BRep_Builder bb;
        bb.MakeCompound(c);
        for (const TopoDS_Shape& b : out) bb.Add(c, b);
        report("ShapeFix_Shape per solid on pool", ms, c);
    }
    {
        const auto t = Clock::now();
        Handle(ShapeFix_Shape) fix = new ShapeFix_Shape(shape);
        fix->Perform();
        const double ms = msSince(t);
        report("ShapeFix_Shape whole shape", ms, fix->Shape());
    }
    return 0;
}

static int cmdCancel(const Args& a)
{
    const char* F = a.name.c_str();
    std::mt19937 rng(a.seed);
    std::uniform_real_distribution<double> frac(0.05, 0.95);
    Handle(CancelIndicator) ind = new CancelIndicator;

    // --- read: baseline with ReadFile
    auto reader = std::make_unique<STEPControl_Reader>();
    auto t = Clock::now();
    if (reader->ReadFile(a.file.c_str()) != IFSelect_RetDone) return 1;
    const double readMs = msSince(t);
    std::printf("C|%s|read|ReadFile|baselineMs=%.1f|note=no progress or cancel argument exists\n", F, readMs);

    // --- read: DESTEP_Provider::Read with a progress range that is already cancelled
    {
        Handle(DESTEP_ConfigurationNode) node = new DESTEP_ConfigurationNode;
        Handle(DESTEP_Provider) prov = new DESTEP_Provider(node);
        TopoDS_Shape s;
        ind->arm(true);
        t = Clock::now();
        const bool ok = prov->Read(TCollection_AsciiString(a.file.c_str()), s, ind->Start());
        std::printf("C|%s|read|DESTEP_Provider precancelled|returnedAfterMs=%.1f|ok=%d|polls=%llu|shapeNull=%d\n", F, msSince(t),
                    int(ok), ind->polls.load(), int(s.IsNull()));
    }
    std::fflush(stdout);

    // --- read: ReadStream through a stream that can be cut short
    double streamMs = 0, eofMs = 0;
    {
        std::atomic<bool> never{false};
        CancelFileBuf buf(a.file, &never);
        std::istream is(&buf);
        STEPControl_Reader r2;
        t = Clock::now();
        const IFSelect_ReturnStatus st = r2.ReadStream(a.file.c_str(), is);
        streamMs = msSince(t);
        eofMs = std::chrono::duration<double, std::milli>(buf.eofAt - t).count();
        std::printf("C|%s|read|ReadStream uncancelled|ms=%.1f|status=%d|streamConsumedAtMs=%.1f|afterStreamMs=%.1f\n", F, streamMs,
                    int(st), eofMs, streamMs - eofMs);
    }
    for (int k = 0; k < a.trials + 2; ++k) {
        std::atomic<bool> flag{false};
        CancelFileBuf buf(a.file, &flag);
        std::istream is(&buf);
        STEPControl_Reader r2;
        const double u = frac(rng);
        Watchdog wd(flag, u * streamMs);
        t = Clock::now();
        const IFSelect_ReturnStatus st = r2.ReadStream(a.file.c_str(), is);
        const double lat = wd.stop();
        const double el = msSince(t);
        std::printf("C|%s|read|ReadStream cancel|at=%.2f|phase=%s|latencyMs=%.1f|elapsedMs=%.1f|baselineMs=%.1f|status=%d\n", F, u,
                    buf.cancelledRead ? "parse" : "load(after stream consumed)", lat, el, streamMs, int(st));
        std::fflush(stdout);
    }

    // --- transfer, both without and with OCCT's built-in FixShape
    TopoDS_Shape shape;
    for (int heal = 0; heal < 2; ++heal) {
        const char* cfg = heal ? "default-heal" : "no-heal";
        resetTransfer(*reader, heal != 0);
        ind->arm();
        t = Clock::now();
        reader->TransferRoots(ind->Start());
        const double base = msSince(t);
        std::printf("C|%s|transfer|%s|baselineMs=%.1f|polls=%llu|maxPollGapMs=%.1f\n", F, cfg, base, ind->polls.load(),
                    ind->finishMaxGapMs());
        if (!heal) shape = reader->OneShape();
        for (int k = 0; k < a.trials; ++k) {
            resetTransfer(*reader, heal != 0);
            ind->arm();
            const double u = frac(rng);
            Watchdog wd(ind->flag, u * base);
            t = Clock::now();
            const int roots = reader->TransferRoots(ind->Start());
            const double lat = wd.stop();
            std::printf("C|%s|transfer|%s cancel|at=%.2f|latencyMs=%.1f|elapsedMs=%.1f|baselineMs=%.1f|roots=%d\n", F, cfg, u, lat,
                        msSince(t), base, roots);
            std::fflush(stdout);
        }
    }
    reader.reset();

    // --- heal
    {
        ind->arm();
        t = Clock::now();
        Handle(ShapeFix_Shape) fix = new ShapeFix_Shape(shape);
        fix->Perform(ind->Start());
        const double base = msSince(t);
        std::printf("C|%s|heal|ShapeFix_Shape|baselineMs=%.1f|polls=%llu|maxPollGapMs=%.1f\n", F, base, ind->polls.load(),
                    ind->finishMaxGapMs());
        for (int k = 0; k < a.trials; ++k) {
            ind->arm();
            const double u = frac(rng);
            Watchdog wd(ind->flag, u * base);
            t = Clock::now();
            Handle(ShapeFix_Shape) f2 = new ShapeFix_Shape(shape);
            f2->Perform(ind->Start());
            const double lat = wd.stop();
            std::printf("C|%s|heal|ShapeFix_Shape cancel|at=%.2f|latencyMs=%.1f|elapsedMs=%.1f|baselineMs=%.1f\n", F, u, lat,
                        msSince(t), base);
            std::fflush(stdout);
        }
        shape = fix->Shape();
    }

    // --- mesh
    for (const std::string& tn : a.tiers) {
        const Tier* tier = tierByName(tn);
        if (!tier) continue;
        for (const std::string& mode : a.modes) {
            BRepTools::Clean(shape);
            ind->arm();
            const double base = runMesh(shape, *tier, mode, a.threads, ind);
            std::printf("C|%s|mesh|%s/%s|baselineMs=%.1f|polls=%llu|maxPollGapMs=%.1f\n", F, tier->name, mode.c_str(), base,
                        ind->polls.load(), ind->finishMaxGapMs());
            for (int k = 0; k < a.trials; ++k) {
                BRepTools::Clean(shape);
                ind->arm();
                const double u = frac(rng);
                Watchdog wd(ind->flag, u * base);
                const double el = runMesh(shape, *tier, mode, a.threads, ind);
                const double lat = wd.stop();
                const MeshStats st = meshStats(shape);
                std::printf("C|%s|mesh|%s/%s cancel|at=%.2f|latencyMs=%.1f|elapsedMs=%.1f|baselineMs=%.1f|facesMeshed=%d/%d\n", F,
                            tier->name, mode.c_str(), u, lat, el, base, st.faces - st.unmeshed, st.faces);
                std::fflush(stdout);
            }
        }
    }
    return 0;
}

// Prints the faulting call stack with symbols (OCCT PDBs are looked up in the vcpkg bin folder).
static LONG WINAPI crashFilter(EXCEPTION_POINTERS* ep)
{
    static std::atomic<bool> once{false};
    if (once.exchange(true)) { Sleep(INFINITE); }
    std::printf("X|unhandled SEH exception code=0x%08lx address=%p peakMB=%.0f thread=%lu\n", ep->ExceptionRecord->ExceptionCode,
                ep->ExceptionRecord->ExceptionAddress, peakWsMb(), GetCurrentThreadId());
    if (ep->ExceptionRecord->ExceptionCode == 0xC0000005 && ep->ExceptionRecord->NumberParameters >= 2)
        std::printf("X|access violation %s address %p\n", ep->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
                    (void*)ep->ExceptionRecord->ExceptionInformation[1]);
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(proc, "M:\\dev\\vcpkg\\installed\\x64-windows\\bin", TRUE);
    CONTEXT ctx = *ep->ContextRecord;
    STACKFRAME64 f{};
    f.AddrPC.Offset = ctx.Rip; f.AddrPC.Mode = AddrModeFlat;
    f.AddrFrame.Offset = ctx.Rbp; f.AddrFrame.Mode = AddrModeFlat;
    f.AddrStack.Offset = ctx.Rsp; f.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < 40; ++i) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, GetCurrentThread(), &f, &ctx, nullptr, SymFunctionTableAccess64,
                         SymGetModuleBase64, nullptr) || f.AddrPC.Offset == 0)
            break;
        char buf[sizeof(SYMBOL_INFO) + 512] = {};
        SYMBOL_INFO* si = reinterpret_cast<SYMBOL_INFO*>(buf);
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen = 511;
        DWORD64 disp = 0;
        char mod[MAX_PATH] = "?";
        HMODULE hm = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)f.AddrPC.Offset, &hm))
            GetModuleFileNameA(hm, mod, MAX_PATH);
        const char* base = std::strrchr(mod, (char)92);
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD ld = 0;
        const bool hasSym = SymFromAddr(proc, f.AddrPC.Offset, &disp, si) != 0;
        const bool hasLine = SymGetLineFromAddr64(proc, f.AddrPC.Offset, &ld, &line) != 0;
        std::printf("X|#%02d %s!%s+0x%llx %s:%lu\n", i, base ? base + 1 : mod, hasSym ? si->Name : "?", (unsigned long long)disp,
                    hasLine ? line.FileName : "", hasLine ? line.LineNumber : 0);
    }
    std::fflush(stdout);
    TerminateProcess(proc, 139);
    return EXCEPTION_EXECUTE_HANDLER;
}

int main(int argc, char** argv)
{
    SetUnhandledExceptionFilter(crashFilter);
    std::set_terminate([] {
        std::printf("X|std::terminate called peakMB=%.0f\n", peakWsMb());
        std::fflush(stdout);
        std::_Exit(3);
    });
    Args a;
    if (argc < 3) {
        std::fprintf(stderr, "usage: s3bench run|cancel <file.step> [options]\n");
        return 2;
    }
    a.cmd = argv[1];
    a.file = argv[2];
    a.name = std::filesystem::path(a.file).filename().string();
    for (int i = 3; i < argc; ++i) {
        const std::string o = argv[i];
        auto val = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (o == "--threads") a.threads = unsigned(std::stoul(val()));
        else if (o == "--occt-threads") a.occtThreads = std::stoi(val());
        else if (o == "--trials") a.trials = std::stoi(val());
        else if (o == "--seed") a.seed = unsigned(std::stoul(val()));
        else if (o == "--tiers") a.tiers = split(val());
        else if (o == "--modes") a.modes = split(val());
        else if (o == "--default-heal") a.defaultHeal = true;
        else if (o == "--skip-heal") a.skipHeal = true;
        else if (o == "--copy-bodies") a.copyBodies = true;
        else if (o == "--no-progress") gNoProgress = true;
        else if (o == "--repeat") a.repeat = std::stoi(val());
        else if (o == "--algo") {
            const std::string v = val();
            gAlgo = v == "delabella" ? IMeshTools_MeshAlgoType_Delabella
                    : v == "watson" ? IMeshTools_MeshAlgoType_Watson : IMeshTools_MeshAlgoType_DEFAULT;
        }
        else { std::fprintf(stderr, "unknown option %s\n", o.c_str()); return 2; }
    }
    if (a.occtThreads > 0) OSD_ThreadPool::DefaultPool(a.occtThreads);
    try {
        if (a.cmd == "run") return cmdRun(a);
        if (a.cmd == "cancel") return cmdCancel(a);
        if (a.cmd == "faces") return cmdFaces(a);
        if (a.cmd == "heal") return cmdHeal(a);
    } catch (const Standard_Failure& e) {
        std::printf("R|%s|exception|%s\n", a.name.c_str(), e.what());
        return 1;
    }
    return 2;
}
