// Private to src/render/rhi: small helpers shared by the RHI code (task T-014). Every QRhi call of RayCAD lives in
// src/render/rhi (PLAN.md 2.1, Qt RHI has no compatibility promise between minor versions).
#pragma once

#include <ray/render.h>

#include <rhi/qrhi.h>

#include <QtGui/QMatrix4x4>

#include <atomic>
#include <cstdint>
#include <memory>

namespace ray::render::rhi {

// Counts the QRhi resources one renderer owns, so tests can prove that resizes and preset switches do not leak.
struct ResourceCounter {
    std::atomic<std::int64_t> live{0};
};

struct ResourceDeleter {
    ResourceCounter* counter = nullptr;
    void operator()(QRhiResource* resource) const
    {
        if (resource) {
            delete resource;
            if (counter)
                counter->live.fetch_sub(1, std::memory_order_relaxed);
        }
    }
};

// A QRhi resource owned by one renderer. Deleting it releases the native object (deferred by QRhi when the GPU may
// still use it).
template <class T>
using Owned = std::unique_ptr<T, ResourceDeleter>;

template <class T>
Owned<T> own(ResourceCounter& counter, T* resource)
{
    if (resource)
        counter.live.fetch_add(1, std::memory_order_relaxed);
    return Owned<T>(resource, ResourceDeleter{&counter});
}

// A baked shader from the resources compiled by qt_add_shaders (src/render/CMakeLists.txt), e.g. "mesh.vert".
// Returns an invalid QShader if it is missing.
QShader loadShader(const char* name);

// Theme colours are linear RGB with straight alpha (render.h). Shaders work in linear and encode sRGB at the end.
void toFloat4(const Color& c, float out[4]);
// A row-major 3x4 rigid transform as a 4x4 matrix.
QMatrix4x4 toMatrix(const Xform&);

} // namespace ray::render::rhi
