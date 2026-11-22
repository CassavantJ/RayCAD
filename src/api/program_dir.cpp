// The folder of the running program (files.h programDir), in a file of its own so <windows.h> stays out of the rest.
#include "files.h"

#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <cstdint>
#include <mach-o/dyld.h>
#endif

namespace ray::api::detail {

std::string programDir()
{
    std::filesystem::path exe;
#ifdef _WIN32
    std::vector<wchar_t> buffer(1024);
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (n == 0) {
            return {};
        }
        if (n < buffer.size()) {
            exe = std::filesystem::path(std::wstring(buffer.data(), n));
            break;
        }
        if (buffer.size() > 65536) {
            return {};
        }
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size + 1, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return {};
    }
    exe = std::filesystem::path(std::string(buffer.data()));
#else
    std::error_code ec;
    exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) {
        return {};
    }
#endif
    std::error_code ec2;
    const std::filesystem::path resolved = std::filesystem::weakly_canonical(exe, ec2);
    return fromFsPath((ec2 ? exe : resolved).parent_path());
}

} // namespace ray::api::detail
