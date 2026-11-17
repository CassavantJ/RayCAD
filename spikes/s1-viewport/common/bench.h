// Spike S1: measurement harness shared by A and B. Header only.
#pragma once
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMatrix4x4>
#include <QQuickWindow>
#include <QThread>
#include <QTimer>
#include <rhi/qrhi.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include "scene.h"

namespace s1 {

struct Options {
    bool glass = true;
    double seconds = 10.0, warmup = 1.5;
    int msaa = 1;
    int width = 1920, height = 1080;   // physical pixels
    bool gpuTime = false, pick = false, resize = false, idle = false;
    QString out, grab, label;
};

inline Options parseOptions(const QCoreApplication &app)
{
    QCommandLineParser p;
    p.addOptions({{"glass", "1 or 0", "v", "1"}, {"seconds", "orbit seconds", "v", "10"}, {"msaa", "samples", "v", "1"},
                  {"width", "px", "v", "1920"}, {"height", "px", "v", "1080"}, {"out", "json lines file", "f"},
                  {"grab", "png file", "f"}, {"label", "free text", "t"}, {"gpu-time", "collect GPU timestamps"},
                  {"pick", "run pick test"}, {"resize", "run resize test"}, {"idle", "run idle test"}});
    p.process(app);
    Options o;
    o.glass = p.value("glass").toInt() != 0;
    o.seconds = p.value("seconds").toDouble();
    o.msaa = p.value("msaa").toInt();
    o.width = p.value("width").toInt();
    o.height = p.value("height").toInt();
    o.out = p.value("out"); o.grab = p.value("grab"); o.label = p.value("label");
    o.gpuTime = p.isSet("gpu-time"); o.pick = p.isSet("pick"); o.resize = p.isSet("resize"); o.idle = p.isSet("idle");
    return o;
}

// ---- globals shared between GUI and render thread ----
inline QElapsedTimer &clock() { static QElapsedTimer t = [] { QElapsedTimer e; e.start(); return e; }(); return t; }
inline std::atomic<double> g_frozen{-1.0};     // >= 0: orbit is frozen at this time value
inline std::atomic<bool> g_animate{true};      // renderer re-requests a frame after every frame while true
inline std::atomic<int> g_rtW{0}, g_rtH{0}, g_rtReallocs{0};
inline double orbitSeconds()
{
    const double f = g_frozen.load();
    return f >= 0 ? f : clock().nsecsElapsed() * 1e-9;
}

// Debug aid: print a stack trace when the process faults (set S1_CRASH_TRACE=1).
inline LONG CALLBACK crashTrace(EXCEPTION_POINTERS *e)
{
    const DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code != 0xC0000005 && code != 0xC0000374 && code != 0xC0000409) return EXCEPTION_CONTINUE_SEARCH;
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    SymInitialize(proc, nullptr, TRUE);
    void *frames[48];
    const USHORT n = CaptureStackBackTrace(0, 48, frames, nullptr);
    std::fprintf(stderr, "FAULT %08lx at %p\n", code, e->ExceptionRecord->ExceptionAddress);
    for (USHORT i = 0; i < n; ++i) {
        char buf[sizeof(SYMBOL_INFO) + 256] = {};
        auto *sym = reinterpret_cast<SYMBOL_INFO *>(buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO); sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        char mod[MAX_PATH] = "?";
        HMODULE hm = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, LPCSTR(frames[i]), &hm))
            GetModuleFileNameA(hm, mod, MAX_PATH);
        const char *base = std::strrchr(mod, '\\');
        if (SymFromAddr(proc, DWORD64(frames[i]), &disp, sym))
            std::fprintf(stderr, "  %s!%s+0x%llx\n", base ? base + 1 : mod, sym->Name, disp);
        else
            std::fprintf(stderr, "  %s+0x%llx\n", base ? base + 1 : mod, DWORD64(frames[i]) - DWORD64(hm));
    }
    std::fflush(stderr);
    TerminateProcess(proc, 3);
    return EXCEPTION_CONTINUE_SEARCH;
}
inline void installCrashTrace()
{
    if (qEnvironmentVariableIntValue("S1_CRASH_TRACE")) AddVectoredExceptionHandler(1, crashTrace);
    // Other spikes load every core of this machine while S1 measures. High priority keeps their batch work
    // from pre-empting the benchmark threads (it cannot undo shared cache, memory bandwidth or turbo effects).
    if (!qEnvironmentVariableIntValue("S1_NORMAL_PRIORITY")) SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
}

inline QMatrix4x4 viewMatrix(double seconds)
{
    const Cam c = orbitCamera(seconds);
    QMatrix4x4 v;
    v.lookAt({c.eye[0], c.eye[1], c.eye[2]}, {c.target[0], c.target[1], c.target[2]}, {0, 0, 1});
    return v;
}
inline QMatrix4x4 projMatrix(float aspect)
{
    const Cam c = orbitCamera(0);
    QMatrix4x4 p;
    p.perspective(c.fovYDeg, aspect, c.zNear, c.zFar);
    return p;
}
// Item-coordinate position of a world point for the frozen camera (used by the scripted pick tests).
inline QPointF projectToItem(const std::array<float, 3> &p, double w, double h)
{
    const QVector3D ndc = (projMatrix(float(w / h)) * viewMatrix(g_frozen.load())).map(QVector3D(p[0], p[1], p[2]));
    return QPointF((ndc.x() * 0.5 + 0.5) * w, (0.5 - ndc.y() * 0.5) * h);
}

inline double msSinceProcessStart()
{
    FILETIME c, e, k, u, now;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    GetSystemTimeAsFileTime(&now);
    const auto to64 = [](FILETIME f) { return (quint64(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    return double(to64(now) - to64(c)) / 1e4;
}

// Whole-machine CPU busy fraction between two samples (other spikes ran on this machine at the same time).
struct CpuSample { quint64 idle = 0, total = 0; };
inline CpuSample cpuSample()
{
    FILETIME i, k, u;
    GetSystemTimes(&i, &k, &u);
    const auto to64 = [](FILETIME f) { return (quint64(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    return {to64(i), to64(k) + to64(u)};   // kernel time includes idle time
}
inline double cpuBusyPercent(CpuSample a, CpuSample b)
{
    return b.total > a.total ? 100.0 * (1.0 - double(b.idle - a.idle) / double(b.total - a.total)) : 0.0;
}

inline QJsonObject stats(std::vector<double> v)
{
    QJsonObject o;
    o["n"] = int(v.size());
    if (v.empty()) return o;
    std::sort(v.begin(), v.end());
    double sum = 0; for (double d : v) sum += d;
    const auto q = [&](double p) { return v[std::min(v.size() - 1, std::size_t(p * (v.size() - 1) + 0.5))]; };
    const auto r3 = [](double d) { return std::round(d * 1000.0) / 1000.0; };
    o["medianMs"] = r3(q(0.5)); o["p95Ms"] = r3(q(0.95)); o["p99Ms"] = r3(q(0.99));
    o["maxMs"] = r3(v.back()); o["minMs"] = r3(v.front()); o["meanMs"] = r3(sum / v.size());
    return o;
}
inline std::vector<double> deltasMs(const std::vector<qint64> &stamps)
{
    std::vector<double> d;
    for (std::size_t i = 1; i < stamps.size(); ++i) d.push_back((stamps[i] - stamps[i - 1]) * 1e-6);
    return d;
}

class Bench : public QObject
{
public:
    using Done = std::function<void(QJsonObject)>;
    std::function<void(Done)> pickTest;     // app specific, asynchronous
    QJsonObject extra;                      // app specific static facts

    Bench(QQuickWindow *w, const Options &o, double mainStartMs) : m_w(w), m_o(o), m_mainStartMs(mainStartMs)
    {
        m_orbit.reserve(1 << 20); m_resize.reserve(1 << 18); m_gpu.reserve(1 << 20);
        connect(w, &QQuickWindow::frameSwapped, this, [this] { onSwap(); }, Qt::DirectConnection);
        if (o.gpuTime)
            connect(w, &QQuickWindow::afterRendering, this, [this] { onAfterRendering(); }, Qt::DirectConnection);
    }

private:
    enum Phase { Warmup = 0, Orbit = 1, Resize = 2, Idle = 3, Other = 4 };

    void onSwap()   // render thread
    {
        const qint64 t = clock().nsecsElapsed();
        if (!m_first.exchange(true)) {
            m_firstFrameProcMs = msSinceProcessStart();
            m_threaded = QThread::currentThread() != qGuiApp->thread();
            if (QRhi *rhi = m_w->rhi()) {
                const QRhiDriverInfo di = rhi->driverInfo();
                m_backend = QString::fromLatin1(rhi->backendName());
                m_device = QString::fromUtf8(di.deviceName);
                m_vendorId = di.vendorId; m_deviceId = di.deviceId; m_deviceType = int(di.deviceType);
            }
            if (m_w->swapChain())
                m_noVsync = m_w->swapChain()->flags().testFlag(QRhiSwapChain::NoVSync);
            QMetaObject::invokeMethod(this, [this] { firstFrameSeen(); }, Qt::QueuedConnection);
        }
        switch (m_phase.load()) {
        case Orbit: m_orbit.push_back(t); break;
        case Resize: m_resize.push_back(t); break;
        case Idle: ++m_idleFrames; break;
        default: break;
        }
    }
    void onAfterRendering()   // render thread, inside the frame
    {
        if (m_phase.load() != Orbit || !m_w->swapChain()) return;
        const double g = m_w->swapChain()->currentFrameCommandBuffer()->lastCompletedGpuTime();
        if (g > 0 && g != m_lastGpu) { m_lastGpu = g; m_gpu.push_back(g * 1000.0); }
    }

    void firstFrameSeen()
    {
        QTimer::singleShot(int(m_o.warmup * 1000), this, [this] {
            m_cpu0 = cpuSample();
            m_phase = Orbit;
            QTimer::singleShot(int(m_o.seconds * 1000), this, [this] {
                m_phase = Other;
                m_cpuBusy = cpuBusyPercent(m_cpu0, cpuSample());
                afterOrbit();
            });
        });
    }
    void afterOrbit()
    {
        g_frozen = 1.25;   // fixed camera for grab and pick
        if (!m_o.grab.isEmpty()) {
            QTimer::singleShot(200, this, [this] {
                m_w->grabWindow().save(m_o.grab);
                runPick();
            });
        } else runPick();
    }
    void runPick()
    {
        if (m_o.pick && pickTest) pickTest([this](QJsonObject r) { m_pick = r; runResize(); });
        else runResize();
    }
    void runResize()
    {
        if (!m_o.resize) { runIdle(); return; }
        g_frozen = -1.0;
        m_reallocs0 = g_rtReallocs.load();
        m_phase = Resize;
        m_step = 0;
        auto *t = new QTimer(this);
        const QSize full = m_w->size();
        connect(t, &QTimer::timeout, this, [this, t, full] {
            const int n = 60;
            if (m_step >= n) {
                t->stop(); t->deleteLater();
                m_w->resize(full);
                m_phase = Other;
                m_resizeReallocs = g_rtReallocs.load() - m_reallocs0;
                QTimer::singleShot(200, this, [this] { runIdle(); });
                return;
            }
            const double k = 0.5 - 0.5 * std::cos(2.0 * 3.14159265 * (m_step + 1) / n);   // 1 -> 2/3 -> 1
            m_w->resize(int(full.width() * (1.0 - k / 3.0)), int(full.height() * (1.0 - k / 3.0)));
            ++m_step;
        });
        t->start(33);
    }
    void runIdle()
    {
        if (!m_o.idle) { finish(); return; }
        g_animate = false;
        QTimer::singleShot(300, this, [this] {
            m_idleFrames = 0; m_phase = Idle;
            QTimer::singleShot(2000, this, [this] { m_phase = Other; m_idleDone = true; finish(); });
        });
    }
    void finish()
    {
        QJsonObject o = extra;
        o["label"] = m_o.label; o["glass"] = m_o.glass; o["msaa"] = m_o.msaa;
        o["backend"] = m_backend; o["device"] = m_device;
        o["vendorId"] = QString::number(m_vendorId, 16); o["deviceId"] = QString::number(m_deviceId, 16);
        static const char *types[] = {"Unknown", "Integrated", "Discrete", "External", "Virtual", "Cpu"};
        o["deviceType"] = types[std::clamp(m_deviceType, 0, 5)];
        o["threadedRenderLoop"] = m_threaded; o["noVsync"] = m_noVsync;
        o["rtSize"] = QString("%1x%2").arg(g_rtW.load()).arg(g_rtH.load());
        o["dpr"] = m_w->devicePixelRatio();
        o["firstFrameMsFromProcessStart"] = std::round(m_firstFrameProcMs);
        o["mainStartMsFromProcessStart"] = std::round(m_mainStartMs);
        QJsonObject orbit = stats(deltasMs(m_orbit));
        if (m_orbit.size() > 1)
            orbit["fpsMean"] = std::round(10.0 * (m_orbit.size() - 1) / ((m_orbit.back() - m_orbit.front()) * 1e-9)) / 10.0;
        o["orbit"] = orbit;
        o["machineCpuBusyPercentDuringOrbit"] = std::round(m_cpuBusy);
        if (m_o.gpuTime) o["gpu"] = stats(m_gpu);
        if (m_o.pick) o["pick"] = m_pick;
        if (m_o.resize) { QJsonObject r = stats(deltasMs(m_resize)); r["rtReallocs"] = m_resizeReallocs; r["steps"] = m_step; o["resize"] = r; }
        if (m_idleDone) o["idleFramesIn2s"] = m_idleFrames.load();
        const QByteArray line = QJsonDocument(o).toJson(QJsonDocument::Compact);
        std::printf("%s\n", line.constData());
        std::fflush(stdout);
        if (!m_o.out.isEmpty()) {
            QFile f(m_o.out);
            if (f.open(QIODevice::Append)) { f.write(line + "\n"); f.close(); }
        }
        m_w->close();
        qGuiApp->quit();
    }

    QQuickWindow *m_w; Options m_o; double m_mainStartMs;
    std::atomic<bool> m_first{false};
    std::atomic<int> m_phase{Warmup};
    std::atomic<int> m_idleFrames{0};
    std::vector<qint64> m_orbit, m_resize; std::vector<double> m_gpu;
    double m_lastGpu = 0, m_firstFrameProcMs = 0, m_cpuBusy = 0;
    CpuSample m_cpu0;
    bool m_threaded = false, m_noVsync = false, m_idleDone = false;
    QString m_backend, m_device; quint64 m_vendorId = 0, m_deviceId = 0; int m_deviceType = 0;
    QJsonObject m_pick; int m_step = 0, m_reallocs0 = 0, m_resizeReallocs = 0;
};

// Scripted pick test shared by A and B. Item needs pick(x, y) in item coordinates and a picked(int, double) signal.
// Picks the middle of the top annulus of each of the 20 bodies (never occluded): first with the viewport idle
// (one frame is rendered per pick), then while it renders continuously.
template <class Item>
void runPickTest(Item *vp, Bench::Done done)
{
    struct State { Mesh mesh; int i = 0, ok = 0; std::vector<double> idle, busy; QMetaObject::Connection c; };
    auto st = std::make_shared<State>();
    st->mesh = buildMesh();
    const int n = kBodiesX * kBodiesY;
    g_animate = false;
    auto next = [=] {
        const QPointF pt = projectToItem(st->mesh.probe[faceId(st->i % n, kTopFace) - 1], vp->width(), vp->height());
        vp->pick(pt.x(), pt.y());
    };
    st->c = QObject::connect(vp, &Item::picked, qGuiApp, [=](int id, double ms) {
        if (id == int(faceId(st->i % n, kTopFace))) ++st->ok;
        (st->i < n ? st->idle : st->busy).push_back(ms);
        ++st->i;
        if (st->i == n) { g_animate = true; vp->update(); }
        if (st->i == 2 * n) {
            QObject::disconnect(st->c);
            QJsonObject r;
            r["picks"] = 2 * n; r["correct"] = st->ok;
            r["idle"] = stats(st->idle); r["whileRendering"] = stats(st->busy);
            done(r);
            return;
        }
        QTimer::singleShot(st->i < n ? 30 : 5, qGuiApp, next);
    });
    QTimer::singleShot(100, qGuiApp, next);
}

} // namespace s1
