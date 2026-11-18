// Hardware detection per platform (PLAN.md 6.1). Runs with no window and no GPU, never throws, and reports what it
// cannot find out as unknown ("" or 0, GpuType::Unknown, computeShaders false).
//
// Windows: the adapter Qt's Direct3D 11 backend will use. Like Qt, adapter 0 unless QT_D3D_ADAPTER_INDEX names
// another, or the software adapter (WARP) if QSG_RHI_PREFER_SOFTWARE_RENDERER is set. Name, vendor, dedicated
// video memory, the software flag and the driver version come from DXGI. A throw-away D3D11 device on that adapter
// gives the feature level (compute shaders and the largest texture) and whether its memory is unified with system
// memory (integrated GPU). dxgi.dll and d3d11.dll are loaded at run time, so nothing extra is linked. RAM is the
// installed memory (GetPhysicallyInstalledSystemMemory), cores are physical cores.
//
// Linux: the DRM card that drives the boot display (/sys/class/drm/card*/device: PCI ids, kernel driver, and VRAM
// where amdgpu publishes it), named from pci.ids when that file is installed. Qt Quick renders with OpenGL there;
// its version is unknown until a context exists. Software rendering is assumed when there is no DRM card, when the
// card's driver has no 3D engine, or when LIBGL_ALWAYS_SOFTWARE, GALLIUM_DRIVER=llvmpipe/softpipe or
// QT_QUICK_BACKEND=software ask for it. Unknown on Linux: VRAM on NVIDIA and Intel (the drivers do not publish
// it), and compute support on drivers other than amdgpu, i915, xe and nvidia.
//
// macOS: RAM, physical cores and the OS version (the Metal driver ships with it) from sysctl. On Apple silicon
// the GPU is part of the SoC: integrated, unified memory, VRAM unknown. On an Intel Mac the GPU is unknown: naming
// it needs Metal or IOKit, which can fill these fields later from src/render/rhi. Every Mac that runs macOS 14
// supports Metal compute and 16384-pixel textures.
#include "tier_internal.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>

#include <iterator>
#include <vector>
#elif defined(__linux__)
#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <vector>
#elif defined(__APPLE__)
#include <sys/types.h>

#include <sys/sysctl.h>
#endif

namespace ray::render::detail {
namespace {

int fallbackCoreCount()
{
    // Logical processors: only used when the physical count cannot be read.
    const unsigned n = std::thread::hardware_concurrency();
    return static_cast<int>(n);
}

#if defined(_WIN32)

// Releases a COM pointer on scope exit.
template <class T>
class ComRef {
public:
    ComRef() = default;
    ComRef(const ComRef&) = delete;
    ComRef& operator=(const ComRef&) = delete;
    ~ComRef()
    {
        if (ptr_ != nullptr) {
            ptr_->Release();
        }
    }
    T** out() { return &ptr_; }
    T* get() const { return ptr_; }
    T* operator->() const { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }

private:
    T* ptr_ = nullptr;
};

// A system DLL loaded for the duration of detection. Declared before any COM object so it is freed last.
class SystemLibrary {
public:
    explicit SystemLibrary(const wchar_t* name) : module_(LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
    {
    }
    SystemLibrary(const SystemLibrary&) = delete;
    SystemLibrary& operator=(const SystemLibrary&) = delete;
    ~SystemLibrary()
    {
        if (module_ != nullptr) {
            FreeLibrary(module_);
        }
    }
    template <class F>
    F get(const char* name) const
    {
        return module_ != nullptr ? reinterpret_cast<F>(GetProcAddress(module_, name)) : nullptr;
    }

private:
    HMODULE module_;
};

std::string toUtf8(const wchar_t* text)
{
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
    out.resize(static_cast<std::size_t>(size - 1));
    return out;
}

int physicalCoresWindows()
{
    DWORD length = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
    if (length == 0) {
        return 0;
    }
    std::vector<std::byte> buffer(length);
    auto* first = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data());
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, first, &length)) {
        return 0;
    }
    int cores = 0;
    for (DWORD offset = 0; offset < length;) {
        const auto* info = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + offset);
        if (info->Size == 0) {
            break;
        }
        if (info->Relationship == RelationProcessorCore) {
            ++cores;
        }
        offset += info->Size;
    }
    return cores;
}

std::uint64_t installedRamWindows()
{
    ULONGLONG kib = 0;
    if (GetPhysicallyInstalledSystemMemory(&kib) && kib > 0) {
        return static_cast<std::uint64_t>(kib) * 1024u;
    }
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof status;
    if (GlobalMemoryStatusEx(&status)) {
        return status.ullTotalPhys;
    }
    return 0;
}

void detectGpuWindows(HardwareInfo& hw)
{
    SystemLibrary dxgi(L"dxgi.dll");
    SystemLibrary d3d11(L"d3d11.dll");
    using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
    const auto createFactory = dxgi.get<CreateFactoryFn>("CreateDXGIFactory1");
    if (createFactory == nullptr) {
        return;
    }
    ComRef<IDXGIFactory1> factory;
    if (FAILED(createFactory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.out())))) {
        return;
    }

    std::vector<DXGI_ADAPTER_DESC1> descs;
    for (UINT i = 0;; ++i) {
        ComRef<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, adapter.out()) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        DXGI_ADAPTER_DESC1 desc{};
        if (!adapter || FAILED(adapter->GetDesc1(&desc))) {
            desc = DXGI_ADAPTER_DESC1{};
        }
        descs.push_back(desc);
    }
    if (descs.empty()) {
        return;
    }

    // The same choice Qt's D3D11 backend makes.
    UINT chosen = 0;
    const char* requested = std::getenv("QT_D3D_ADAPTER_INDEX");
    if (requested != nullptr && *requested != '\0') {
        const long index = std::strtol(requested, nullptr, 10);
        if (index >= 0 && static_cast<std::size_t>(index) < descs.size()) {
            chosen = static_cast<UINT>(index);
        }
    } else if (envValueIsTrue(std::getenv("QSG_RHI_PREFER_SOFTWARE_RENDERER"))) {
        for (UINT i = 0; i < descs.size(); ++i) {
            if ((descs[i].Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
                chosen = i;
                break;
            }
        }
    }

    ComRef<IDXGIAdapter1> adapter;
    if (FAILED(factory->EnumAdapters1(chosen, adapter.out())) || !adapter) {
        return;
    }
    const DXGI_ADAPTER_DESC1& desc = descs[chosen];
    const bool softwareFlag = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
    hw.gpuName = toUtf8(desc.Description);
    hw.gpuVendor = pciVendorName(desc.VendorId);
    hw.vramBytes = static_cast<std::uint64_t>(desc.DedicatedVideoMemory);
    hw.graphicsApi = "Direct3D 11";

    LARGE_INTEGER umd{};
    if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
        hw.driverVersion = formatWindowsDriverVersion(static_cast<std::uint64_t>(umd.QuadPart));
    }

    std::optional<bool> unifiedMemory;
    const auto createDevice = d3d11.get<PFN_D3D11_CREATE_DEVICE>("D3D11CreateDevice");
    if (createDevice != nullptr) {
        static constexpr D3D_FEATURE_LEVEL kLevels[] = {
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
            D3D_FEATURE_LEVEL_9_3,  D3D_FEATURE_LEVEL_9_2,  D3D_FEATURE_LEVEL_9_1};
        ComRef<ID3D11Device> device;
        D3D_FEATURE_LEVEL level{};
        HRESULT result =
            createDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, kLevels,
                         static_cast<UINT>(std::size(kLevels)), D3D11_SDK_VERSION, device.out(), &level, nullptr);
        if (result == E_INVALIDARG) {
            // A runtime that does not know feature level 11_1.
            result = createDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, kLevels + 1,
                                  static_cast<UINT>(std::size(kLevels) - 1), D3D11_SDK_VERSION, device.out(), &level,
                                  nullptr);
        }
        if (SUCCEEDED(result) && device) {
            hw.computeShaders = level >= D3D_FEATURE_LEVEL_11_0;
            hw.maxTextureSize = level >= D3D_FEATURE_LEVEL_11_0   ? 16384
                                : level >= D3D_FEATURE_LEVEL_10_0 ? 8192
                                : level >= D3D_FEATURE_LEVEL_9_3  ? 4096
                                                                  : 2048;
            D3D11_FEATURE_DATA_D3D11_OPTIONS2 options{};
            if (SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2, &options,
                                                      static_cast<UINT>(sizeof options)))) {
                unifiedMemory = options.UnifiedMemoryArchitecture != FALSE;
            }
        }
    }
    hw.gpuType = classifyDxgiAdapter(desc.VendorId, softwareFlag, hw.vramBytes, unifiedMemory);
    if (hw.gpuType == GpuType::Software) {
        hw.vramBytes = 0; // WARP has no video memory of its own
    }
}

HardwareInfo detectWindows()
{
    HardwareInfo hw;
    hw.ramBytes = installedRamWindows();
    hw.physicalCores = physicalCoresWindows();
    detectGpuWindows(hw);
    return hw;
}

#elif defined(__linux__)

namespace fs = std::filesystem;

std::string readSmallFile(const fs::path& path, std::size_t limit = 4096)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    std::string text(limit, '\0');
    in.read(text.data(), static_cast<std::streamsize>(limit));
    text.resize(static_cast<std::size_t>(in.gcount()));
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.pop_back();
    }
    return text;
}

std::uint64_t parseNumber(const std::string& text, int base)
{
    if (text.empty()) {
        return 0;
    }
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text.c_str(), &end, base);
    return end != text.c_str() ? static_cast<std::uint64_t>(value) : 0;
}

struct DrmCard {
    int index = 0;
    fs::path device;
    std::uint32_t vendor = 0, deviceId = 0;
    bool bootVga = false;
    std::string driver;
    std::uint64_t vram = 0;
};

std::vector<DrmCard> drmCards()
{
    std::vector<DrmCard> cards;
    std::error_code ec;
    fs::directory_iterator it("/sys/class/drm", ec);
    if (ec) {
        return cards;
    }
    for (const fs::directory_entry& entry : it) {
        const std::string name = entry.path().filename().string();
        // "card0", not the connectors ("card0-HDMI-A-1") or render nodes ("renderD128").
        if (name.size() <= 4 || name.compare(0, 4, "card") != 0 ||
            name.find_first_not_of("0123456789", 4) != std::string::npos) {
            continue;
        }
        DrmCard card;
        card.index = std::atoi(name.c_str() + 4);
        card.device = entry.path() / "device";
        card.vendor = static_cast<std::uint32_t>(parseNumber(readSmallFile(card.device / "vendor"), 16));
        card.deviceId = static_cast<std::uint32_t>(parseNumber(readSmallFile(card.device / "device"), 16));
        card.bootVga = readSmallFile(card.device / "boot_vga") == "1";
        std::error_code linkError;
        const fs::path driver = fs::read_symlink(card.device / "driver", linkError);
        if (!linkError) {
            card.driver = driver.filename().string();
        }
        card.vram = parseNumber(readSmallFile(card.device / "mem_info_vram_total"), 10);
        cards.push_back(std::move(card));
    }
    std::sort(cards.begin(), cards.end(), [](const DrmCard& a, const DrmCard& b) {
        if (a.bootVga != b.bootVga) {
            return a.bootVga;
        }
        return a.index < b.index;
    });
    return cards;
}

bool softwareRequestedLinux()
{
    if (envValueIsTrue(std::getenv("LIBGL_ALWAYS_SOFTWARE"))) {
        return true;
    }
    const char* gallium = std::getenv("GALLIUM_DRIVER");
    if (gallium != nullptr && (std::string(gallium) == "llvmpipe" || std::string(gallium) == "softpipe")) {
        return true;
    }
    const char* backend = std::getenv("QT_QUICK_BACKEND");
    return backend != nullptr && std::string(backend) == "software";
}

std::string pciDeviceNameLinux(std::uint32_t vendor, std::uint32_t device)
{
    for (const char* path : {"/usr/share/hwdata/pci.ids", "/usr/share/misc/pci.ids", "/usr/share/pci.ids"}) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            continue;
        }
        std::ostringstream text;
        text << in.rdbuf();
        return pciIdsDeviceName(text.str(), vendor, device);
    }
    return {};
}

int physicalCoresLinux()
{
    // One entry per core: the set of logical CPUs that share it.
    std::set<std::string> cores;
    std::error_code ec;
    fs::directory_iterator it("/sys/devices/system/cpu", ec);
    if (ec) {
        return 0;
    }
    for (const fs::directory_entry& entry : it) {
        const std::string name = entry.path().filename().string();
        if (name.size() <= 3 || name.compare(0, 3, "cpu") != 0 ||
            name.find_first_not_of("0123456789", 3) != std::string::npos) {
            continue;
        }
        std::string siblings = readSmallFile(entry.path() / "topology" / "core_cpus_list");
        if (siblings.empty()) {
            siblings = readSmallFile(entry.path() / "topology" / "thread_siblings_list");
        }
        if (!siblings.empty()) {
            cores.insert(siblings);
        }
    }
    return static_cast<int>(cores.size());
}

HardwareInfo detectLinux()
{
    HardwareInfo hw;
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long pageSize = sysconf(_SC_PAGESIZE);
    if (pages > 0 && pageSize > 0) {
        hw.ramBytes = static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(pageSize);
    }
    hw.physicalCores = physicalCoresLinux();
    hw.graphicsApi = "OpenGL";

    const std::vector<DrmCard> cards = drmCards();
    if (softwareRequestedLinux() || cards.empty()) {
        hw.gpuType = GpuType::Software;
        hw.gpuName = "Software rasteriser (llvmpipe expected)";
        return hw;
    }
    const DrmCard& card = cards.front();
    hw.gpuVendor = pciVendorName(card.vendor);
    hw.gpuName = pciDeviceNameLinux(card.vendor, card.deviceId);
    if (hw.gpuName.empty() && card.vendor != 0) {
        char text[32];
        std::snprintf(text, sizeof text, "PCI %04x:%04x", static_cast<unsigned>(card.vendor),
                      static_cast<unsigned>(card.deviceId));
        hw.gpuName = text;
    }
    hw.gpuType = classifyLinuxGpu(card.vendor, card.deviceId, card.driver, card.vram);
    if (hw.gpuType == GpuType::Software) {
        hw.gpuName = "Software rasteriser (llvmpipe expected; " + card.driver + " has no 3D engine)";
        return hw;
    }
    hw.vramBytes = card.vram;
    if (!card.driver.empty()) {
        std::string version = readSmallFile(fs::path("/sys/module") / card.driver / "version");
        if (version.empty()) {
            utsname uts{};
            if (uname(&uts) == 0) {
                version = std::string("Linux ") + uts.release;
            }
        }
        hw.driverVersion = version.empty() ? card.driver : card.driver + " " + version;
    }
    const std::string& d = card.driver;
    if (d == "amdgpu" || d == "i915" || d == "xe" || d == "nvidia") {
        hw.computeShaders = true; // OpenGL 4.3 or later on every GPU these drivers support
        hw.maxTextureSize = 16384;
    }
    return hw;
}

#elif defined(__APPLE__)

template <class T>
bool sysctlValue(const char* name, T& out)
{
    T value{};
    std::size_t size = sizeof value;
    if (sysctlbyname(name, &value, &size, nullptr, 0) != 0 || size != sizeof value) {
        return false;
    }
    out = value;
    return true;
}

std::string sysctlString(const char* name)
{
    std::size_t size = 0;
    if (sysctlbyname(name, nullptr, &size, nullptr, 0) != 0 || size == 0) {
        return {};
    }
    std::string text(size, '\0');
    if (sysctlbyname(name, text.data(), &size, nullptr, 0) != 0) {
        return {};
    }
    text.resize(size);
    while (!text.empty() && text.back() == '\0') {
        text.pop_back();
    }
    return text;
}

HardwareInfo detectMac()
{
    HardwareInfo hw;
    std::uint64_t memory = 0;
    if (sysctlValue("hw.memsize", memory)) {
        hw.ramBytes = memory;
    }
    std::int32_t cores = 0;
    if (sysctlValue("hw.physicalcpu", cores)) {
        hw.physicalCores = cores;
    }
    hw.graphicsApi = "Metal";
    hw.computeShaders = true;
    hw.maxTextureSize = 16384;
    const std::string os = sysctlString("kern.osproductversion");
    if (!os.empty()) {
        hw.driverVersion = "macOS " + os;
    }
    const std::string brand = sysctlString("machdep.cpu.brand_string");
    if (brand.rfind("Apple ", 0) == 0) {
        hw.gpuVendor = "Apple";
        hw.gpuName = brand + " GPU";
        hw.gpuType = GpuType::Integrated; // unified memory; vramBytes stays unknown
    }
    return hw;
}

#endif

} // namespace

HardwareInfo detectHardwarePlatform()
{
    HardwareInfo hw;
    // Nothing below should throw, but std::filesystem iteration and allocation can: detection never fails, it
    // reports unknown instead.
    try {
#if defined(_WIN32)
        hw = detectWindows();
#elif defined(__linux__)
        hw = detectLinux();
#elif defined(__APPLE__)
        hw = detectMac();
#endif
    } catch (...) {
        hw = HardwareInfo{};
    }
    if (hw.physicalCores <= 0) {
        hw.physicalCores = fallbackCoreCount();
    }
    return hw;
}

} // namespace ray::render::detail
