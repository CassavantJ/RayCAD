// RayCAD shim for FreeCAD <Base/Console.h>, as used by PlaneGCS (GCS.cpp).
// PlaneGCS only calls Base::Console().log(fmt, ...) and .warning(fmt, ...).
// Messages go to an optional sink; with no sink they are dropped.
#pragma once
#include <cstdio>
#include <string>

namespace Base
{
class ConsoleSingleton
{
public:
    using Sink = void (*)(int level, const char* text);  // level: 0 = log, 1 = warning
    Sink sink = nullptr;

    template<typename... Args>
    void log(const char* fmt, Args&&... args)
    {
        emit(0, fmt, static_cast<Args&&>(args)...);
    }
    template<typename... Args>
    void warning(const char* fmt, Args&&... args)
    {
        emit(1, fmt, static_cast<Args&&>(args)...);
    }

private:
    void emit(int level, const char* text)
    {
        if (sink) {
            sink(level, text);
        }
    }
    template<typename... Args>
    void emit(int level, const char* fmt, Args&&... args)
    {
        if (!sink) {
            return;
        }
        char buf[1024];
        std::snprintf(buf, sizeof(buf), fmt, args...);
        sink(level, buf);
    }
};

inline ConsoleSingleton& Console()
{
    static ConsoleSingleton instance;
    return instance;
}
}  // namespace Base
