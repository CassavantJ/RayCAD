// Private to src/render/rhi: the seam through which later passes plug into the viewport renderer (task T-014).
//
//   T-015 picking: an IFramePass that, when a pick is pending and FrameContext::sceneGeneration or
//          cameraGeneration moved since its last ID render, draws `draws` again into its own RGBA8 + depth target with
//          an id fragment shader (mesh.vert already passes the face id; line.vert and wideline.vert the edge id;
//          ItemBlock.ids.x holds the item id), then reads back a small rectangle and posts the answer through the
//          Outbox. It reuses the vertex buffers, the uniform buffer and the shader resource bindings below.
//   T-016 backdrop: BackdropPass (backdrop_pass.h) samples `colour` (the resolved frame), downsamples and blurs it
//          into a texture it owns, only when `colourRedrawn` (or it has no blur of the current image), and bumps a
//          generation each time. It is always the LAST pass: SceneRenderer::addPass inserts before it.
//   T-023 section: SceneRenderer::writeFrameBlock fills FrameBlock.misc.z / section / analysis; every face, edge,
//          line and ID shader discards beyond the plane. The caps (section.h) are drawn INSIDE the main pass, right
//          after the opaque faces, because they need its depth-stencil buffer (and MSAA); so the backdrop blurs them
//          with the rest. A section change marks the picture changed (colourRedrawn) and bumps sceneGeneration, so
//          glass and the ID buffer follow. The pick pass draws the caps into the ID buffer from `capQuad` and
//          `capped` (IdKind::Cap) with the same stencil technique.
#pragma once

#include "../viewport_frontend.h"

#include <rhi/qrhi.h>

#include <QtGui/QMatrix4x4>

#include <cstdint>
#include <span>
#include <vector>

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
    // Added by T-015.
    bool colourRedrawn = true; // false: the main pass was skipped, `colour` holds the last image
    std::vector<detail::PickRequest>* picks = nullptr; // pending picks; the pick pass takes them
    QRhiBuffer* lineCorners = nullptr;                 // quad template of the wide-line pipeline (wideline.vert)
    // Added by T-023: the section cap quad (6 world-space positions, capVertexLayout) and the indices into `draws`
    // of the bodies capped this frame; empty when nothing is capped.
    QRhiBuffer* capQuad = nullptr;
    std::span<const std::size_t> capped;
};

// Vertex layouts of the scene's buffers (GpuItem), for passes that draw them with their own shaders.
QRhiVertexInputLayout meshVertexLayout();     // mesh.vert: position, normal, face id
QRhiVertexInputLayout thinEdgeVertexLayout(); // line.vert: position, edge id
QRhiVertexInputLayout wideLineVertexLayout(); // wideline.vert: corner (lineCorners), then per instance p0 p1 edge id
QRhiVertexInputLayout capVertexLayout();      // cap.vert, pick_cap.vert: position (T-023)
// Section cap pipeline state (T-023, section.h): step 1 marks the inside of a body in stencil bit 0 (no colour into
// any of `colourTargets` attachments, no depth), step 2 draws the cap quad where it is marked and clears the mark.
void setCapStencilMarking(QRhiGraphicsPipeline& p, int colourTargets);
void setCapStencilFill(QRhiGraphicsPipeline& p);

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
