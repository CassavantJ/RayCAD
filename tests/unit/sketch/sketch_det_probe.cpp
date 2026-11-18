// Determinism probe (task T-090): pins this process to one core of the requested class ("p": highest efficiency
// class, "e": lowest) before any solver code runs, then prints the solver fingerprint of the 200-curve S7 sketch.
// solver_determinism_test.cpp runs it once per class and compares. Prints "no-hybrid" where the CPU has one class or
// the platform has no way to ask (non-Windows).
#include "sketch_det_hash.h"

#include <cstdio>
#include <cstring>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

int main(int argc, char** argv)
{
    const bool wantP = argc > 1 && std::strcmp(argv[1], "p") == 0;
#ifdef _WIN32
    ULONG length = 0;
    GetSystemCpuSetInformation(nullptr, 0, &length, GetCurrentProcess(), 0);
    std::vector<unsigned char> buffer(length);
    auto* info = reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data());
    if (length == 0 || !GetSystemCpuSetInformation(info, length, &length, GetCurrentProcess(), 0)) {
        std::printf("no-hybrid (no CPU set information)\n");
        return 0;
    }
    int minClass = 255, maxClass = -1;
    struct Cpu {
        int index;
        int cls;
        int group;
    };
    std::vector<Cpu> cpus;
    for (ULONG off = 0; off < length;) {
        auto* e = reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data() + off);
        if (e->Type == CpuSetInformation) {
            const int cls = e->CpuSet.EfficiencyClass;
            cpus.push_back({e->CpuSet.LogicalProcessorIndex, cls, e->CpuSet.Group});
            minClass = cls < minClass ? cls : minClass;
            maxClass = cls > maxClass ? cls : maxClass;
        }
        off += e->Size;
    }
    if (minClass == maxClass) {
        std::printf("no-hybrid (one efficiency class)\n");
        return 0;
    }
    const int want = wantP ? maxClass : minClass;
    int chosen = -1;
    for (const Cpu& c : cpus) {
        if (c.cls == want && c.group == 0 && c.index < 64) {
            chosen = c.index; // the last one of the class: away from CPU 0, where the system is busiest
        }
    }
    if (chosen < 0 || !SetProcessAffinityMask(GetCurrentProcess(), DWORD_PTR(1) << chosen)) {
        std::printf("no-hybrid (could not pin)\n");
        return 0;
    }
    SwitchToThread();
    const auto grid = ray::sketch::test::makeGrid(4, ray::sketch::test::GridVariant::Nearly, 0.3);
    const std::uint64_t h = ray::sketch::test::detHash(grid, 3);
    std::printf("hash %016llx core %s cpu %d now on %lu\n", static_cast<unsigned long long>(h), wantP ? "P" : "E",
                chosen, GetCurrentProcessorNumber());
    return 0;
#else
    (void)wantP;
    std::printf("no-hybrid (core classes are only probed on Windows)\n");
    return 0;
#endif
}
