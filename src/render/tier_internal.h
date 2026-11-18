// Private to src/render (and its unit tests): the pure pieces of hardware detection, so they can be tested on
// every platform, whatever the machine running the tests has. No Qt and no platform header here.
#pragma once

#include <ray/render.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ray::render::detail {

constexpr std::uint64_t kGiB = std::uint64_t{1} << 30;

// PCI vendor ids seen on desktop GPUs.
constexpr std::uint32_t kVendorAmd = 0x1002;
constexpr std::uint32_t kVendorAmdAlt = 0x1022;
constexpr std::uint32_t kVendorApple = 0x106B;
constexpr std::uint32_t kVendorArm = 0x13B5;
constexpr std::uint32_t kVendorGoogle = 0x1AE0; // SwiftShader reports this vendor id
constexpr std::uint32_t kVendorImagination = 0x1010;
constexpr std::uint32_t kVendorIntel = 0x8086;
constexpr std::uint32_t kVendorMicrosoft = 0x1414; // WARP / Basic Render Driver, Hyper-V
constexpr std::uint32_t kVendorNvidia = 0x10DE;
constexpr std::uint32_t kVendorQualcomm = 0x5143;

// "NVIDIA", "AMD", "Intel", ...; "PCI 0xABCD" for a vendor not in the list; "" for 0 (unknown).
std::string pciVendorName(std::uint32_t vendorId);

// True if a renderer or adapter name is a known software rasteriser (WARP, llvmpipe, softpipe, SwiftShader).
bool isSoftwareRendererName(std::string_view name);

// True for an environment variable value that switches something on ("1", "true", "yes", "on").
bool envValueIsTrue(const char* value);

// Windows: classifies a DXGI adapter. unifiedMemory is D3D11_FEATURE_DATA_D3D11_OPTIONS2::UnifiedMemoryArchitecture
// when the driver answered that query. Without it, an adapter with less than 1 GiB of dedicated memory counts as
// integrated (Intel reports 128 MiB, AMD APUs 512 MiB by default).
GpuType classifyDxgiAdapter(std::uint32_t vendorId, bool softwareFlag, std::uint64_t dedicatedVideoMemory,
                            std::optional<bool> unifiedMemory);

// Windows: the user-mode driver version from IDXGIAdapter::CheckInterfaceSupport, as "32.0.15.6094".
std::string formatWindowsDriverVersion(std::uint64_t umdVersion);

// Linux: classifies a /sys/class/drm card from its PCI ids, kernel driver name and amdgpu's mem_info_vram_total
// (0 when the driver does not publish it). Display-only drivers (virtual machines, server BMCs) have no 3D engine,
// so Mesa renders with llvmpipe on them: Software.
GpuType classifyLinuxGpu(std::uint32_t vendorId, std::uint32_t deviceId, std::string_view driver,
                         std::uint64_t vramBytes);

// Linux: the device name for vendor:device in the text of a pci.ids file; "" if absent.
std::string pciIdsDeviceName(std::string_view pciIds, std::uint32_t vendorId, std::uint32_t deviceId);

// The platform part of detectHardware (tier_detect.cpp). Never throws.
HardwareInfo detectHardwarePlatform();

} // namespace ray::render::detail
