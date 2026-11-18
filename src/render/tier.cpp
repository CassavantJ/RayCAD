// Hardware tiers (PLAN.md 6.1 and 6.2): the tier rules, the presets, and the platform-independent helpers of
// hardware detection. The platform code is in tier_detect.cpp.
#include "tier_internal.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdio>

namespace ray::render {
namespace detail {

std::string pciVendorName(std::uint32_t vendorId)
{
    switch (vendorId) {
    case 0:
        return {};
    case kVendorAmd:
    case kVendorAmdAlt:
        return "AMD";
    case kVendorApple:
        return "Apple";
    case kVendorArm:
        return "ARM";
    case kVendorGoogle:
        return "Google";
    case kVendorImagination:
        return "Imagination";
    case kVendorIntel:
        return "Intel";
    case kVendorMicrosoft:
        return "Microsoft";
    case kVendorNvidia:
        return "NVIDIA";
    case kVendorQualcomm:
        return "Qualcomm";
    default:
        break;
    }
    char text[16];
    std::snprintf(text, sizeof text, "PCI 0x%04X", static_cast<unsigned>(vendorId & 0xFFFFu));
    return text;
}

namespace {
std::string lowered(std::string_view text)
{
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}
} // namespace

bool isSoftwareRendererName(std::string_view name)
{
    const std::string lower = lowered(name);
    static constexpr std::array<std::string_view, 6> kNames = {
        "llvmpipe", "softpipe", "swiftshader", "microsoft basic render driver", "warp", "software rasteri"};
    for (std::string_view known : kNames) {
        if (known == "warp") {
            // "WARP" alone, not as part of another word.
            if (lower == "warp" || lower.rfind("warp ", 0) == 0 || lower.find(" warp") != std::string::npos) {
                return true;
            }
        } else if (lower.find(known) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool envValueIsTrue(const char* value)
{
    if (value == nullptr) {
        return false;
    }
    const std::string lower = lowered(value);
    return lower == "1" || lower == "true" || lower == "yes" || lower == "on";
}

GpuType classifyDxgiAdapter(std::uint32_t vendorId, bool softwareFlag, std::uint64_t dedicatedVideoMemory,
                            std::optional<bool> unifiedMemory)
{
    // Microsoft's own adapters (Basic Render Driver, Basic Display Adapter, Hyper-V) all draw with WARP.
    if (softwareFlag || vendorId == kVendorMicrosoft) {
        return GpuType::Software;
    }
    if (unifiedMemory.has_value()) {
        return *unifiedMemory ? GpuType::Integrated : GpuType::Discrete;
    }
    if (dedicatedVideoMemory == 0) {
        return GpuType::Unknown;
    }
    return dedicatedVideoMemory < kGiB ? GpuType::Integrated : GpuType::Discrete;
}

std::string formatWindowsDriverVersion(std::uint64_t umdVersion)
{
    if (umdVersion == 0) {
        return {};
    }
    char text[48];
    std::snprintf(text, sizeof text, "%u.%u.%u.%u", static_cast<unsigned>((umdVersion >> 48) & 0xFFFFu),
                  static_cast<unsigned>((umdVersion >> 32) & 0xFFFFu),
                  static_cast<unsigned>((umdVersion >> 16) & 0xFFFFu), static_cast<unsigned>(umdVersion & 0xFFFFu));
    return text;
}

namespace {
// Intel's discrete GPUs: DG1 (Iris Xe MAX), Arc A-series (Alchemist, DG2) and Arc B-series (Battlemage).
bool isIntelDiscrete(std::uint32_t deviceId)
{
    return (deviceId >= 0x4905 && deviceId <= 0x4909) || (deviceId >= 0x5690 && deviceId <= 0x56BF) ||
           (deviceId >= 0xE202 && deviceId <= 0xE22F);
}
} // namespace

GpuType classifyLinuxGpu(std::uint32_t vendorId, std::uint32_t deviceId, std::string_view driver,
                         std::uint64_t vramBytes)
{
    // Kernel display drivers without a 3D engine: Mesa falls back to llvmpipe on these.
    static constexpr std::array<std::string_view, 14> kDisplayOnly = {
        "bochs",     "bochs-drm", "cirrus",    "cirrus-qemu", "qxl", "hyperv_drm", "hyperv_fb",
        "simpledrm", "efifb",     "vboxvideo", "mgag200",     "ast", "udl",        "vkms"};
    if (std::find(kDisplayOnly.begin(), kDisplayOnly.end(), driver) != kDisplayOnly.end()) {
        return GpuType::Software;
    }
    switch (vendorId) {
    case kVendorMicrosoft:
        return GpuType::Software;
    case kVendorIntel:
        return isIntelDiscrete(deviceId) ? GpuType::Discrete : GpuType::Integrated;
    case kVendorNvidia:
        return GpuType::Discrete;
    case kVendorAmd:
        // amdgpu publishes no APU flag. An APU's VRAM is a carve-out of system memory, 512 MiB by default.
        if (vramBytes == 0) {
            return GpuType::Unknown;
        }
        return vramBytes < 2 * kGiB ? GpuType::Integrated : GpuType::Discrete;
    case kVendorApple:
    case kVendorArm:
    case kVendorImagination:
    case kVendorQualcomm:
        return GpuType::Integrated;
    default:
        return GpuType::Unknown;
    }
}

namespace {
bool parseHex4(std::string_view text, std::uint32_t& out)
{
    if (text.size() < 4) {
        return false;
    }
    const auto result = std::from_chars(text.data(), text.data() + 4, out, 16);
    return result.ec == std::errc{} && result.ptr == text.data() + 4;
}
} // namespace

std::string pciIdsDeviceName(std::string_view pciIds, std::uint32_t vendorId, std::uint32_t deviceId)
{
    // Format: "vvvv  Vendor name" at column 0, "\tdddd  Device name" below it, "\t\t..." for subsystems.
    bool inVendor = false;
    std::size_t pos = 0;
    while (pos < pciIds.size()) {
        std::size_t end = pciIds.find('\n', pos);
        if (end == std::string_view::npos) {
            end = pciIds.size();
        }
        std::string_view line = pciIds.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (line.empty() || line.front() == '#') {
            continue;
        }
        std::uint32_t id = 0;
        if (line.front() != '\t') {
            if (inVendor) {
                return {}; // the next vendor started: the device is not listed
            }
            inVendor = parseHex4(line, id) && id == vendorId;
            continue;
        }
        if (!inVendor || line.size() < 2 || line[1] == '\t') {
            continue;
        }
        line.remove_prefix(1);
        if (parseHex4(line, id) && id == deviceId) {
            std::string_view name = line.substr(4);
            while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) {
                name.remove_prefix(1);
            }
            return std::string(name);
        }
    }
    return {};
}

} // namespace detail

namespace {
using detail::kGiB;

// Memory the firmware, the kernel and an integrated GPU keep back: a "12 GB" machine reports a little less than
// 12 GiB on some systems, and a "3 GB" card a little less than 3 GiB.
constexpr std::uint64_t kRamSlack = kGiB;
constexpr std::uint64_t kVramSlack = kGiB / 2;

bool atLeast(std::uint64_t value, std::uint64_t threshold, std::uint64_t slack)
{
    return value + slack >= threshold;
}
} // namespace

HardwareInfo detectHardware()
{
    return detail::detectHardwarePlatform();
}

// PLAN.md 6.1:
//   Low    if the renderer is software, or the GPU is integrated, or VRAM is under 3 GB, or RAM is under 12 GB;
//   High   if the GPU is discrete, VRAM is 8 GB or more, RAM is 24 GB or more and compute shaders are available;
//   Medium otherwise.
// Unknown values: a GPU of unknown type is Low (we never assume a GPU we could not see). An unknown VRAM or RAM
// size does not make a discrete GPU Low, but High needs every High condition to be known.
Tier chooseTier(const HardwareInfo& hw)
{
    if (hw.gpuType != GpuType::Discrete || detail::isSoftwareRendererName(hw.gpuName)) {
        return Tier::Low;
    }
    const bool vramKnown = hw.vramBytes > 0;
    const bool ramKnown = hw.ramBytes > 0;
    if (vramKnown && !atLeast(hw.vramBytes, 3 * kGiB, kVramSlack)) {
        return Tier::Low;
    }
    if (ramKnown && !atLeast(hw.ramBytes, 12 * kGiB, kRamSlack)) {
        return Tier::Low;
    }
    if (vramKnown && ramKnown && hw.computeShaders && atLeast(hw.vramBytes, 8 * kGiB, kVramSlack) &&
        atLeast(hw.ramBytes, 24 * kGiB, kRamSlack)) {
        return Tier::High;
    }
    return Tier::Medium;
}

// PLAN.md 6.2. Display numbers are starting values; code reads these fields and never tests the tier itself.
TierPreset presetFor(Tier tier)
{
    TierPreset p{};
    p.tier = tier;
    p.workerThreads = 0;     // physical cores - 1
    p.gpuSimulation = false; // GPU material removal is v2; until then High runs the CPU model on more threads
    switch (tier) {
    case Tier::Low:
        p.meshChordTolMm = 0.2;
        p.meshAngleTolDeg = 20.0;
        p.msaaSamples = 0; // edges drawn as 1 px lines
        p.ambientOcclusion = false;
        p.shadows = false;
        p.smoothEdges = false;
        p.transparency = false;
        p.sectionCaps = false;
        p.liquidGlass = false;
        p.maxBackplotSegments = 2'000'000;
        p.simCellMm = 0.2;
        break;
    case Tier::Medium:
        p.meshChordTolMm = 0.05;
        p.meshAngleTolDeg = 12.0;
        p.msaaSamples = 4;
        p.ambientOcclusion = false;
        p.shadows = false;
        p.smoothEdges = false;
        p.transparency = true;
        p.sectionCaps = true;
        p.liquidGlass = false;
        p.maxBackplotSegments = 8'000'000;
        p.simCellMm = 0.1;
        break;
    case Tier::High:
        p.meshChordTolMm = 0.01;
        p.meshAngleTolDeg = 6.0;
        p.msaaSamples = 4;
        p.ambientOcclusion = true;
        p.shadows = true;
        p.smoothEdges = true;
        p.transparency = true;
        p.sectionCaps = true;
        p.liquidGlass = true;
        p.maxBackplotSegments = 32'000'000;
        p.simCellMm = 0.05;
        break;
    }
    return p;
}

} // namespace ray::render
