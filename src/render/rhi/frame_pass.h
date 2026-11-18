// Private to src/render/rhi: the seam through which later passes plug into the viewport renderer (task T-014).
//
//   T-015 picking: an IFramePass that, when a pick is pending and FrameContext::sceneGeneration or
//          cameraGeneration moved since its last ID render, draws `draws` again into its own RGBA8 + depth target with
//          an id fragment shader (mesh.vert already passes the face id; line.vert and wideline.vert the edge id;
//          ItemBlock.ids.x holds the item id), then reads back a small rectangle and posts the answer through the
//          Outbox. It reuses the vertex buffers, the uniform buffer and the shader resource bindings below.
//   T-016 backdrop: an IFramePass that samples `colour` (the resolved frame), downsamples and blurs it into a
//          texture it owns, and bumps a generation only when the frame changed.
//   T-023 section: fills FrameBlock.misc.z / FrameBlock.section (see SceneRenderer::writeFrameBlock) and adds a
//          cap pass; the face shaders then discard on the far side of the plane.
#pragma once

#include "../viewport_frontend.h"

#include <rhi/qrhi.h>

#include <QtGui/QMatrix4x4>

#include <cstdint>
#include <span>

namespace ray::render::rhi {

struct GpuItem;

// One visible item of this frame, with where its ItemBlock sits in the uniform buffer.
struct DrawItem {
    const detail::SceneItem* item = nullptr;
    const GpuItem* gpu = nullptr;
    quint32 itemUniformOffset = 0;
};

struct FrameContext {
    QRhi* rhi = nullptr;
    QRhiCommandBuffer* cb = nullptr;
    QRhiTexture* colour = nullptr; // the frame's final single-sample colour; sampleable
    QSize pixelSize;
    const detail::FrameSnapshot* snapshot = nullptr;
    QMatrix4x4 view, projection; // projection includes QRhi's clip-space correction
    std::span<const DrawItem> draws;
    QRhiBuffer* uniforms = nullptr;                 // dynamic buffer holding the FrameBlock and the ItemBlocks
    QRhiShaderResourceBindings* bindings = nullptr; // binding 0: FrameBlock, binding 1: ItemBlock (dynamic offsets)
    quint32 frameUniformOffset = 0;                 // the main pass's FrameBlock
    std::uint64_t sceneGeneration = 0, cameraGeneration = 0;
    detail::Outbox* outbox = nullptr; // to deliver results on the UI thread
};

class IFramePass {
public:
    virtual ~IFramePass() = default;
    virtual const char* name() const = 0;
    // After the main colour pass of every frame, outside any render pass, in the order the passes were added.
    virtual void record(const FrameContext&) = 0;
    // True while the pass waits for the GPU, so the host schedules another frame.
    virtual bool busy() const { return false; }
    // The QRhi is going away or the renderer drops its resources; recreate lazily in the next record().
    virtual void releaseResources() = 0;
};

} // namespace ray::render::rhi
