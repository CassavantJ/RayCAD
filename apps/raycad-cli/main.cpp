// raycad-cli: headless RayCAD (PLAN.md section 1.1). Task T-001: it only reports its version.
#include <ray/api.h>

#include <cstdio>
#include <string_view>

namespace ray::detail {
const char* api_module() noexcept;
}

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--version" || arg == "-V") {
            std::printf("raycad-cli %s\n", RAY_VERSION_STRING);
            return 0;
        }
    }
    std::printf("raycad-cli %s\n", RAY_VERSION_STRING);
    std::printf("usage: raycad-cli --version\n");
    // Reference the api module so this executable really links the whole headless stack.
    return ray::detail::api_module() != nullptr ? 0 : 1;
}
