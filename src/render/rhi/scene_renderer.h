// Private to src/render: draws one viewport's FrameSnapshot with a QRhi into a colour texture someone else owns
// (task T-014). Used on the render thread by the Qt Quick seam and on the UI thread by the offscreen viewport, in
// the same order: setRhi / setTarget when they change, setSnapshot, render. It owns every RHI resource it creates
// (counted in liveResources) and brings its own MSAA and depth buffers, so the host only provides the texture.
#pragma once

#include "frame_pass.h"
#include "rhi_util.h"

#include "../pick.h"
#include "../viewport_frontend.h"

#include <QtCore/QElapsedTimer>

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

namespace ray::render::rhi {

// GPU copy of one item's geometry.
struct GpuItem {
    std::uint64_t generation = 0;
    Owned<QRhiBuffer> vertices;  // mesh: position, normal, face id (28 bytes per vertex)
    Owned<QRhiBuffer> indices;   // mesh: uint32 triangles
    Owned<QRhiBuffer> thinEdges; // mesh: line list, position + edge id (16 bytes per vertex)
    Owned<QRhiBuffer> wideEdges; // mesh edges or line items: one instance per segment, p0 p1 edge id (28 bytes)
    std::uint32_t indexCount = 0, thinEdgeVertexCount = 0, segmentCount = 0;
    std::shared_ptr<const detail::MeshIndex> index; // mesh items: face, edge and vertex lookups (T-015)
};

class PickPass;

class SceneRenderer {
public:
    SceneRenderer();
    ~SceneRenderer();
    SceneRenderer(const SceneRenderer&) = delete;
    SceneRenderer& operator=(const SceneRenderer&) = delete;

    // A different QRhi releases everything made with the old one first.
    void setRhi(QRhi* rhi);
    QRhi* rhiInUse() const { return rhi_; }
    // The texture the frame ends up in (created with the RenderTarget flag). The render target is rebuilt for every
    // new texture object (told apart by QRhiResource::globalResourceId, since a successor may reuse a freed one's
    // address) and kept when the same texture is handed in again.
    void setTarget(QRhiTexture* colour);
    void setSnapshot(detail::FrameSnapshot&& snapshot);
    // Where results and stats go; the front of the viewport owns both.
    void setSinks(std::shared_ptr<detail::Outbox> outbox, std::shared_ptr<detail::SharedStatus> status);

    // Records one frame into `cb` (inside beginFrame/endFrame or beginOffscreenFrame/endOffscreenFrame). Returns
    // true when another frame is needed soon (read-backs in flight).
    bool render(QRhiCommandBuffer* cb);
    // After the frame was submitted: frees capture targets whose read-back completed, and stores the GPU time of
    // the last completed frame. `wallMs` > 0 replaces cpuMs (offscreen: the whole frame on the calling thread).
    void frameDone(QRhiCommandBuffer* cb, double wallMs = 0);
    bool busy() const;

    void releaseResources();
    void addPass(std::unique_ptr<IFramePass> pass);

    ResourceCounter& counter() { return counter_; }
    std::int64_t liveResources() const { return counter_.live.load(); }
    std::uint64_t framesRendered() const { return frames_; }
    std::uint64_t pickBufferRenders() const; // ID renders of the picking pass (T-015)

private:
    struct PipelineSet;
    struct Target;
    struct Capture;
    struct HighlightGroup;

    void ensureStatic();
    PipelineSet* pipelinesFor(Target& target);
    bool buildTarget(Target& target, QRhiTexture* colour, QSize size, int samples);
    void syncItems(QRhiResourceUpdateBatch* u);
    void uploadItem(const detail::SceneItem& item, GpuItem& gpu, bool wantWide, QRhiResourceUpdateBatch* u);
    void ensureWide(const detail::SceneItem& item, GpuItem& gpu, QRhiResourceUpdateBatch* u);
    bool ensureUniformCapacity(quint32 bytes);
    void writeFrameBlock(QRhiResourceUpdateBatch* u, quint32 offset, QSize size, QMatrix4x4& view,
                         QMatrix4x4& projection);
    void drawScene(QRhiCommandBuffer* cb, PipelineSet& set, QSize size, quint32 frameOffset);
    int effectiveSamples(int wanted) const;
    void publishStats(double cpuMs);
    // Hover and selection (T-015): the items whose edges need the wide-line buffer, and the highlight draw list with
    // its ItemBlocks (from `base` on) and vertex markers.
    void collectHighlightItems();
    void prepareHighlights(QRhiResourceUpdateBatch* u, const Theme& theme, quint32 base, quint32 slot);
    void drawHighlights(QRhiCommandBuffer* cb, PipelineSet& set, const QRhiViewport& viewport, quint32 frameOffset,
                        std::uint64_t& drawCalls);

    QRhi* rhi_ = nullptr;
    ResourceCounter counter_;
    QRhiTexture* colour_ = nullptr;
    quint64 colourId_ = 0;
    bool targetDirty_ = true;
    detail::FrameSnapshot snap_;
    std::shared_ptr<detail::Outbox> outbox_;
    std::shared_ptr<detail::SharedStatus> status_;

    std::vector<std::unique_ptr<PipelineSet>> pipelineSets_;
    std::unique_ptr<Target> main_;
    std::vector<std::unique_ptr<Capture>> captures_;
    std::map<ItemId, GpuItem> gpu_;
    std::vector<DrawItem> draws_;
    std::vector<std::unique_ptr<IFramePass>> passes_;

    Owned<QRhiBuffer> quad_;    // full-screen triangle
    Owned<QRhiBuffer> corners_; // wide-line quad template
    Owned<QRhiBuffer> uniforms_;
    quint32 uniformCapacity_ = 0;
    Owned<QRhiShaderResourceBindings> bindings_;
    quint32 frameBlockSize_ = 0, itemBlockSize_ = 0;

    // T-015: picking pass (owned by passes_), highlights, and frames that only pick (main pass skipped).
    PickPass* pick_ = nullptr;
    std::vector<HighlightGroup> highlights_;
    std::vector<ItemId> highlightWide_; // sorted
    Owned<QRhiBuffer> markers_;         // vertex markers, wide-line instances
    quint32 markerCapacity_ = 0;
    bool needRedraw_ = true, lastDrew_ = true;

    bool smoothEdges_ = false;
    int samples_ = 1;
    std::uint64_t frames_ = 0;
    IViewport::FrameStats stats_{0, 0, 0, 0};
    bool loggedNoTheme_ = false, loggedPipelineFailure_ = false;
};

} // namespace ray::render::rhi
