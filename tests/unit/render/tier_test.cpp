// Hardware detection and tier choice (task T-013, PLAN.md 6.1 and 6.2).
#include "tier_internal.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>

using namespace ray::render;
using detail::kGiB;

namespace {

HardwareInfo machine(GpuType type, std::uint64_t vram, std::uint64_t ram, int cores, bool compute,
                     std::string name = {})
{
    HardwareInfo hw;
    hw.gpuName = std::move(name);
    hw.gpuType = type;
    hw.vramBytes = vram;
    hw.ramBytes = ram;
    hw.physicalCores = cores;
    hw.computeShaders = compute;
    hw.maxTextureSize = 16384;
    return hw;
}

constexpr std::uint64_t kMiB = std::uint64_t{1} << 20;

// Sets or clears an environment variable for one scope, and restores the old value afterwards.
class ScopedEnv {
public:
    ScopedEnv(const char* name, const char* value) : name_(name)
    {
        if (const char* old = std::getenv(name)) {
            old_ = std::string(old);
        }
        set(value);
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;
    ~ScopedEnv() { set(old_ ? old_->c_str() : nullptr); }

private:
    void set(const char* value)
    {
#if defined(_WIN32)
        _putenv_s(name_.c_str(), value != nullptr ? value : "");
#else
        if (value != nullptr) {
            setenv(name_.c_str(), value, 1);
        } else {
            unsetenv(name_.c_str());
        }
#endif
    }

    std::string name_;
    std::optional<std::string> old_;
};

const char* typeName(GpuType type)
{
    switch (type) {
    case GpuType::Integrated:
        return "integrated";
    case GpuType::Discrete:
        return "discrete";
    case GpuType::Software:
        return "software";
    case GpuType::Unknown:
        break;
    }
    return "unknown";
}

} // namespace

TEST_CASE("chooseTier: the three reference machines of the brief", "[tier]")
{
    // Low: Intel UHD 620-class integrated GPU, 4-core CPU, 8 GB RAM.
    const HardwareInfo low = machine(GpuType::Integrated, 128 * kMiB, 8 * kGiB, 4, true, "Intel(R) UHD Graphics 620");
    CHECK(chooseTier(low) == Tier::Low);
    // Medium: GTX 1650-class GPU, 4 GB VRAM, 16 GB RAM.
    const HardwareInfo medium = machine(GpuType::Discrete, 4 * kGiB, 16 * kGiB, 6, true, "NVIDIA GeForce GTX 1650");
    CHECK(chooseTier(medium) == Tier::Medium);
    // High: RTX 3060-class or better, 8 GB+ VRAM, 32 GB RAM.
    const HardwareInfo high = machine(GpuType::Discrete, 12 * kGiB, 32 * kGiB, 8, true, "NVIDIA GeForce RTX 3060");
    CHECK(chooseTier(high) == Tier::High);
    // The 8 GB RTX 3060, as DXGI reports it (a little under 8 GiB), is still High.
    CHECK(chooseTier(machine(GpuType::Discrete, 8 * kGiB - 160 * kMiB, 32 * kGiB, 8, true)) == Tier::High);
}

TEST_CASE("chooseTier: the development PC of the spikes", "[tier]")
{
    // Core i9-13900K with an RTX 3090 and the UHD 770 of the CPU (spike S1).
    CHECK(chooseTier(machine(GpuType::Discrete, 24 * kGiB, 64 * kGiB, 24, true, "NVIDIA GeForce RTX 3090")) ==
          Tier::High);
    CHECK(chooseTier(machine(GpuType::Integrated, 128 * kMiB, 64 * kGiB, 24, true, "Intel(R) UHD Graphics 770")) ==
          Tier::Low);
}

TEST_CASE("chooseTier: software rasterisers are always Low, with MSAA and glass off", "[tier]")
{
    const HardwareInfo rasterisers[] = {
        // WARP on a 24-core desktop drew 22 FPS in spike S1, under the Low budget.
        machine(GpuType::Software, 0, 64 * kGiB, 24, true, "Microsoft Basic Render Driver"),
        machine(GpuType::Software, 0, 64 * kGiB, 32, true, "llvmpipe (LLVM 17.0.6, 256 bits)"),
        machine(GpuType::Software, 0, 64 * kGiB, 32, true, "SwiftShader Device (Subzero)"),
        // Misreported as a discrete GPU with plenty of memory: the name still gives it away.
        machine(GpuType::Discrete, 16 * kGiB, 64 * kGiB, 32, true, "llvmpipe (LLVM 15.0.7, 256 bits)"),
        machine(GpuType::Discrete, 16 * kGiB, 64 * kGiB, 32, true, "Google SwiftShader"),
        machine(GpuType::Discrete, 16 * kGiB, 64 * kGiB, 32, true, "WARP"),
    };
    for (const HardwareInfo& hw : rasterisers) {
        INFO(hw.gpuName);
        CHECK(hw.softwareRasterizer() == (hw.gpuType == GpuType::Software));
        const Tier tier = chooseTier(hw);
        CHECK(tier == Tier::Low);
        const TierPreset preset = presetFor(tier);
        CHECK(preset.msaaSamples == 0);
        CHECK_FALSE(preset.liquidGlass);
    }
}

TEST_CASE("chooseTier: each rule of PLAN.md 6.1", "[tier]")
{
    // Low: integrated, VRAM under 3 GB, RAM under 12 GB.
    CHECK(chooseTier(machine(GpuType::Integrated, 2 * kGiB, 64 * kGiB, 16, true)) == Tier::Low);
    CHECK(chooseTier(machine(GpuType::Discrete, 2 * kGiB, 32 * kGiB, 8, true)) == Tier::Low);
    CHECK(chooseTier(machine(GpuType::Discrete, 3 * kGiB, 32 * kGiB, 8, true)) == Tier::Medium);
    CHECK(chooseTier(machine(GpuType::Discrete, 8 * kGiB, 8 * kGiB, 8, true)) == Tier::Low);
    CHECK(chooseTier(machine(GpuType::Discrete, 8 * kGiB, 10 * kGiB, 8, true)) == Tier::Low);
    // A "12 GB" machine that reports a little less than 12 GiB is not Low.
    CHECK(chooseTier(machine(GpuType::Discrete, 8 * kGiB, 12 * kGiB - 300 * kMiB, 8, true)) == Tier::Medium);
    // High needs every condition: discrete, VRAM 8 GB, RAM 24 GB, compute shaders.
    CHECK(chooseTier(machine(GpuType::Discrete, 8 * kGiB, 24 * kGiB, 8, true)) == Tier::High);
    CHECK(chooseTier(machine(GpuType::Discrete, 6 * kGiB, 32 * kGiB, 8, true)) == Tier::Medium);
    CHECK(chooseTier(machine(GpuType::Discrete, 8 * kGiB, 16 * kGiB, 8, true)) == Tier::Medium);
    CHECK(chooseTier(machine(GpuType::Discrete, 8 * kGiB, 32 * kGiB, 8, false)) == Tier::Medium);
}

TEST_CASE("chooseTier: unknown values", "[tier]")
{
    // Nothing detected: Low.
    CHECK(chooseTier(HardwareInfo{}) == Tier::Low);
    CHECK(chooseTier(machine(GpuType::Unknown, 16 * kGiB, 64 * kGiB, 16, true)) == Tier::Low);
    // A discrete GPU of unknown memory (NVIDIA on Linux): not Low, but never High.
    CHECK(chooseTier(machine(GpuType::Discrete, 0, 64 * kGiB, 16, true)) == Tier::Medium);
    CHECK(chooseTier(machine(GpuType::Discrete, 16 * kGiB, 0, 16, true)) == Tier::Medium);
    CHECK(chooseTier(machine(GpuType::Discrete, 0, 0, 0, false)) == Tier::Medium);
}

TEST_CASE("presetFor: the values of PLAN.md 6.2", "[tier]")
{
    const TierPreset low = presetFor(Tier::Low);
    CHECK(low.tier == Tier::Low);
    CHECK(low.meshChordTolMm == 0.2);
    CHECK(low.meshAngleTolDeg == 20.0);
    CHECK(low.msaaSamples == 0);
    CHECK_FALSE(low.liquidGlass);
    CHECK_FALSE(low.transparency);
    CHECK_FALSE(low.ambientOcclusion);
    CHECK(low.maxBackplotSegments == 2'000'000);
    CHECK(low.simCellMm == 0.2);

    const TierPreset medium = presetFor(Tier::Medium);
    CHECK(medium.tier == Tier::Medium);
    CHECK(medium.meshChordTolMm == 0.05);
    CHECK(medium.meshAngleTolDeg == 12.0);
    CHECK(medium.msaaSamples == 4);
    CHECK_FALSE(medium.liquidGlass);
    CHECK(medium.transparency);
    CHECK(medium.sectionCaps);
    CHECK_FALSE(medium.shadows);
    CHECK(medium.maxBackplotSegments == 8'000'000);
    CHECK(medium.simCellMm == 0.1);

    const TierPreset high = presetFor(Tier::High);
    CHECK(high.tier == Tier::High);
    CHECK(high.meshChordTolMm == 0.01);
    CHECK(high.meshAngleTolDeg == 6.0);
    CHECK(high.msaaSamples == 4);
    CHECK(high.liquidGlass);
    CHECK(high.ambientOcclusion);
    CHECK(high.shadows);
    CHECK(high.smoothEdges);
    CHECK(high.maxBackplotSegments == 32'000'000);
    CHECK(high.simCellMm == 0.05);

    for (Tier tier : {Tier::Low, Tier::Medium, Tier::High}) {
        const TierPreset p = presetFor(tier);
        CHECK_FALSE(p.gpuSimulation); // v2
        CHECK(p.workerThreads == 0);
    }
}

TEST_CASE("classifyDxgiAdapter", "[tier][windows]")
{
    using detail::classifyDxgiAdapter;
    // WARP: the software flag, or Microsoft's vendor id.
    CHECK(classifyDxgiAdapter(detail::kVendorMicrosoft, true, 0, true) == GpuType::Software);
    CHECK(classifyDxgiAdapter(detail::kVendorMicrosoft, false, 0, std::nullopt) == GpuType::Software);
    // The driver's unified-memory answer decides.
    CHECK(classifyDxgiAdapter(detail::kVendorIntel, false, 128 * kMiB, true) == GpuType::Integrated);
    CHECK(classifyDxgiAdapter(detail::kVendorAmd, false, 4 * kGiB, true) == GpuType::Integrated);
    CHECK(classifyDxgiAdapter(detail::kVendorIntel, false, 16 * kGiB, false) == GpuType::Discrete); // Arc
    CHECK(classifyDxgiAdapter(detail::kVendorNvidia, false, 24 * kGiB, false) == GpuType::Discrete);
    // Without it, dedicated memory under 1 GiB means integrated.
    CHECK(classifyDxgiAdapter(detail::kVendorIntel, false, 128 * kMiB, std::nullopt) == GpuType::Integrated);
    CHECK(classifyDxgiAdapter(detail::kVendorAmd, false, 512 * kMiB, std::nullopt) == GpuType::Integrated);
    CHECK(classifyDxgiAdapter(detail::kVendorNvidia, false, 4 * kGiB, std::nullopt) == GpuType::Discrete);
    CHECK(classifyDxgiAdapter(detail::kVendorNvidia, false, 0, std::nullopt) == GpuType::Unknown);
}

TEST_CASE("formatWindowsDriverVersion", "[tier][windows]")
{
    const std::uint64_t umd = (std::uint64_t{32} << 48) | (std::uint64_t{0} << 32) | (std::uint64_t{15} << 16) | 6094;
    CHECK(detail::formatWindowsDriverVersion(umd) == "32.0.15.6094");
    CHECK(detail::formatWindowsDriverVersion(0).empty());
}

TEST_CASE("classifyLinuxGpu", "[tier][linux]")
{
    using detail::classifyLinuxGpu;
    CHECK(classifyLinuxGpu(detail::kVendorIntel, 0x5917, "i915", 0) == GpuType::Integrated);  // UHD 620
    CHECK(classifyLinuxGpu(detail::kVendorIntel, 0xA780, "i915", 0) == GpuType::Integrated);  // UHD 770
    CHECK(classifyLinuxGpu(detail::kVendorIntel, 0x56A0, "i915", 0) == GpuType::Discrete);    // Arc A770
    CHECK(classifyLinuxGpu(detail::kVendorIntel, 0xE20B, "xe", 0) == GpuType::Discrete);      // Arc B580
    CHECK(classifyLinuxGpu(detail::kVendorNvidia, 0x2204, "nvidia", 0) == GpuType::Discrete); // RTX 3090
    CHECK(classifyLinuxGpu(detail::kVendorAmd, 0x73BF, "amdgpu", 16 * kGiB) == GpuType::Discrete);
    CHECK(classifyLinuxGpu(detail::kVendorAmd, 0x15BF, "amdgpu", 512 * kMiB) == GpuType::Integrated); // APU
    CHECK(classifyLinuxGpu(detail::kVendorAmd, 0x15BF, "amdgpu", 0) == GpuType::Unknown);
    // Display-only drivers: virtual machines (hosted CI runners) and server BMCs.
    CHECK(classifyLinuxGpu(detail::kVendorMicrosoft, 0x5353, "hyperv_drm", 0) == GpuType::Software);
    CHECK(classifyLinuxGpu(0x1234, 0x1111, "bochs-drm", 0) == GpuType::Software);
    CHECK(classifyLinuxGpu(0x1A03, 0x2000, "ast", 0) == GpuType::Software);
    CHECK(classifyLinuxGpu(0x1AF4, 0x1050, "virtio-pci", 0) == GpuType::Unknown);
}

TEST_CASE("pciIdsDeviceName", "[tier][linux]")
{
    const std::string pciIds = "# comment\n"
                               "10de  NVIDIA Corporation\n"
                               "\t2204  GA102 [GeForce RTX 3090]\n"
                               "\t\t10de 1454  GA102 [GeForce RTX 3090 Founders Edition]\n"
                               "8086  Intel Corporation\n"
                               "\t5917  UHD Graphics 620\n"
                               "\ta780  Raptor Lake-S GT1 [UHD Graphics 770]\r\n"
                               "8087  Intel Corporation\n"
                               "\t0001  Not a GPU\n";
    CHECK(detail::pciIdsDeviceName(pciIds, 0x10DE, 0x2204) == "GA102 [GeForce RTX 3090]");
    CHECK(detail::pciIdsDeviceName(pciIds, 0x8086, 0x5917) == "UHD Graphics 620");
    CHECK(detail::pciIdsDeviceName(pciIds, 0x8086, 0xA780) == "Raptor Lake-S GT1 [UHD Graphics 770]");
    CHECK(detail::pciIdsDeviceName(pciIds, 0x8086, 0x0001).empty());
    CHECK(detail::pciIdsDeviceName(pciIds, 0x1002, 0x73BF).empty());
    CHECK(detail::pciIdsDeviceName("", 0x10DE, 0x2204).empty());
}

TEST_CASE("pciVendorName, isSoftwareRendererName, envValueIsTrue", "[tier]")
{
    CHECK(detail::pciVendorName(detail::kVendorNvidia) == "NVIDIA");
    CHECK(detail::pciVendorName(detail::kVendorIntel) == "Intel");
    CHECK(detail::pciVendorName(detail::kVendorAmd) == "AMD");
    CHECK(detail::pciVendorName(0x1234) == "PCI 0x1234");
    CHECK(detail::pciVendorName(0).empty());

    CHECK(detail::isSoftwareRendererName("Microsoft Basic Render Driver"));
    CHECK(detail::isSoftwareRendererName("llvmpipe (LLVM 17.0.6, 256 bits)"));
    CHECK(detail::isSoftwareRendererName("softpipe"));
    CHECK(detail::isSoftwareRendererName("SwiftShader Device (Subzero) (0x0000C0DE)"));
    CHECK(detail::isSoftwareRendererName("WARP"));
    CHECK(detail::isSoftwareRendererName("Software rasteriser (llvmpipe expected)"));
    CHECK_FALSE(detail::isSoftwareRendererName("NVIDIA GeForce RTX 3090"));
    CHECK_FALSE(detail::isSoftwareRendererName("Intel(R) UHD Graphics 770"));
    CHECK_FALSE(detail::isSoftwareRendererName("Warpspeed 9000"));
    CHECK_FALSE(detail::isSoftwareRendererName(""));

    CHECK(detail::envValueIsTrue("1"));
    CHECK(detail::envValueIsTrue("TRUE"));
    CHECK_FALSE(detail::envValueIsTrue("0"));
    CHECK_FALSE(detail::envValueIsTrue(""));
    CHECK_FALSE(detail::envValueIsTrue(nullptr));
}

namespace {
// Runs detectHardware and prints what it found, for the test log.
HardwareInfo detectAndPrint()
{
    const auto start = std::chrono::steady_clock::now();
    const HardwareInfo hw = detectHardware();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    const Tier tier = chooseTier(hw);
    std::printf("detectHardware (%.1f ms): vendor '%s', name '%s', type %s, driver '%s', api '%s', VRAM %.2f GiB, "
                "RAM %.2f GiB, %d physical cores, compute %s, max texture %d -> tier %s\n",
                ms, hw.gpuVendor.c_str(), hw.gpuName.c_str(), typeName(hw.gpuType), hw.driverVersion.c_str(),
                hw.graphicsApi.c_str(), static_cast<double>(hw.vramBytes) / static_cast<double>(kGiB),
                static_cast<double>(hw.ramBytes) / static_cast<double>(kGiB), hw.physicalCores,
                hw.computeShaders ? "yes" : "no", hw.maxTextureSize,
                tier == Tier::High     ? "High"
                : tier == Tier::Medium ? "Medium"
                                       : "Low");
    return hw;
}
} // namespace

// Hidden: run by hand with QT_D3D_ADAPTER_INDEX or QSG_RHI_PREFER_SOFTWARE_RENDERER set to see another adapter.
TEST_CASE("detectHardware with the caller's environment", "[.print]")
{
    detectAndPrint();
    SUCCEED();
}

TEST_CASE("detectHardware on this machine: no window, never fails", "[tier][detect]")
{
    // Qt's adapter choice must not be steered by the caller's environment here.
    ScopedEnv adapter("QT_D3D_ADAPTER_INDEX", nullptr);
    ScopedEnv software("QSG_RHI_PREFER_SOFTWARE_RENDERER", nullptr);
    const HardwareInfo hw = detectAndPrint();
    CHECK(hw.ramBytes > 0);
    CHECK(hw.physicalCores > 0);
#if defined(_WIN32)
    // DXGI always lists at least the Basic Render Driver, so the adapter is never unknown on Windows.
    CHECK(hw.graphicsApi == "Direct3D 11");
    CHECK_FALSE(hw.gpuName.empty());
    CHECK_FALSE(hw.gpuVendor.empty());
    CHECK(hw.gpuType != GpuType::Unknown);
    CHECK_FALSE(hw.driverVersion.empty());
    CHECK(hw.maxTextureSize >= 2048);
    if (hw.gpuType == GpuType::Discrete) {
        CHECK(hw.vramBytes > 0);
    }
#endif
    CHECK((hw.gpuType != GpuType::Unknown || chooseTier(hw) == Tier::Low));
}

TEST_CASE("detectHardware reports the software rasteriser Qt is told to use", "[tier][detect]")
{
#if defined(_WIN32)
    ScopedEnv adapter("QT_D3D_ADAPTER_INDEX", nullptr);
    ScopedEnv software("QSG_RHI_PREFER_SOFTWARE_RENDERER", "1");
    const HardwareInfo hw = detectAndPrint();
    CHECK(hw.gpuType == GpuType::Software);
    CHECK(hw.gpuName.find("Basic Render") != std::string::npos);
    CHECK(hw.gpuVendor == "Microsoft");
    CHECK(hw.vramBytes == 0);
    CHECK(hw.computeShaders); // WARP is feature level 11_1
    CHECK(chooseTier(hw) == Tier::Low);
#elif defined(__linux__)
    ScopedEnv software("LIBGL_ALWAYS_SOFTWARE", "1");
    const HardwareInfo hw = detectAndPrint();
    CHECK(hw.gpuType == GpuType::Software);
    CHECK(chooseTier(hw) == Tier::Low);
#else
    SUCCEED("Metal has no software rasteriser");
#endif
}
