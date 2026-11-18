// Private to src/render/rhi: the picking pass (task T-015), an IFramePass of SceneRenderer.
//
// When picks are pending it makes sure its ID buffer matches the frame (drawn again only when the scene or camera
// generation, the size or the QRhi changed since the last ID render), then reads back one small window that covers
// every pending request and answers them on the UI thread through the Outbox. The CPU side of the answer is
// detail::resolvePick / resolvePickRect (pick.h).
//
// ID buffer: three RGBA8 colour attachments plus a depth buffer, single-sample, at the frame's size:
//   0  ids    rgb = kernel face / edge index + 1 (or addLines segment + 1), little-endian; a = IdKind
//   1  item   rgb = ItemId, little-endian
//   2  depth  rgb = round((0.5 * NDC z + 0.5) * (2^24 - 1)), little-endian
// The faces are pushed back with the main pass's depth bias, so visible edges win over the faces they border.
#pragma once

#include "frame_pass.h"
#include "rhi_util.h"

#include "../pick.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace ray::render::rhi {

class PickPass final : public IFramePass {
public:
    explicit PickPass(ResourceCounter& counter);
    ~PickPass() override;
    PickPass(const PickPass&) = delete;
    PickPass& operator=(const PickPass&) = delete;

    const char* name() const override { return "pick"; }
    void record(const FrameContext& ctx) override;
    bool busy() const override;
    void releaseResources() override;

    std::uint64_t idRenders() const { return renders_; }

private:
    struct Job;

    bool ensureTarget(const FrameContext& ctx);
    bool ensurePipelines(const FrameContext& ctx);
    void drawIds(const FrameContext& ctx);
    std::shared_ptr<const detail::PickView> makeView(const FrameContext& ctx) const;
    static void answerMisses(std::vector<detail::PickRequest>& requests, detail::Outbox* outbox);

    ResourceCounter& counter_;
    QRhi* rhi_ = nullptr;
    QSize size_;
    Owned<QRhiTexture> ids_, items_, depths_;
    Owned<QRhiRenderBuffer> depthBuffer_;
    Owned<QRhiTextureRenderTarget> rt_;
    Owned<QRhiRenderPassDescriptor> rp_;
    Owned<QRhiGraphicsPipeline> faces_, thinEdges_, segments_;
    QRhiShaderResourceBindings* pipelineBindings_ = nullptr; // what the pipelines were made against
    bool pipelinesTried_ = false;

    // The scene the ID buffer shows.
    std::shared_ptr<const detail::PickView> view_;
    std::uint64_t sceneGeneration_ = 0, cameraGeneration_ = 0;
    std::uint64_t renders_ = 0;
    bool loggedFailure_ = false;

    std::vector<std::unique_ptr<Job>> jobs_;
};

} // namespace ray::render::rhi
