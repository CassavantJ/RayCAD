// Private to src/render/rhi: the glass backdrop pass (task T-016, PLAN.md 6.6), an IFramePass of SceneRenderer.
//
// While the backdrop is enabled (FrameSnapshot::backdropEnabled) and the frame's colour image changed
// (FrameContext::colourRedrawn), or the backdrop holds no blur of the current image yet (just enabled, resized), it
// records ONE blur sequence into a texture it owns:
//   1. down   the frame's colour -> out_, a quarter of the size (4 x 4 box filter: four bilinear taps)
//   2. across out_ -> tmp_, horizontal Gaussian (bilinear pairs, backdrop.h)
//   3. down   tmp_ -> out_, vertical Gaussian
// out_ is what IViewport::blurredBackdropTexture describes and every glass panel samples. Disabled means no
// texture and no pass at all. A blur sequence runs at most once per frame, however many panels sample it.
// Shaders: backdrop_down.frag for step 1, and for steps 2 and 3 the shortest backdrop_blurN.frag that holds the
// Gaussian's taps (constant-length loops, see shaders/backdrop.glsl). Separable Gaussian rather than dual Kawase:
// both cost the same on an Intel UHD 770 (0.37 ms each with a loop of uniform length), and the Gaussian is exact,
// so tests can rebuild it on the CPU.
//
// Lifetime: out_ is replaced only on a resize (or a new QRhi) and dropped when the backdrop is disabled. A replaced
// or dropped out_ is RETIRED, not deleted: it lives until frameDone() (the next frame's start in Qt Quick, after the
// offscreen frame's submission), so a scene graph node that was handed the old pointer for the frame being recorded
// never samples freed memory. It must be the last pass: anything a later pass drew into the colour would be missing
// from the blur, so SceneRenderer::addPass inserts new passes before it.
#pragma once

#include "frame_pass.h"
#include "rhi_util.h"

#include "../backdrop.h"

#include <cstdint>
#include <vector>

namespace ray::render::rhi {

class BackdropPass final : public IFramePass {
public:
    explicit BackdropPass(ResourceCounter& counter);
    ~BackdropPass() override;
    BackdropPass(const BackdropPass&) = delete;
    BackdropPass& operator=(const BackdropPass&) = delete;

    const char* name() const override { return "backdrop"; }
    void record(const FrameContext& ctx) override;
    void releaseResources() override;

    // After a frame was submitted: frees the textures retired before it.
    void frameDone();
    // The current blur (rhiTexture null when there is none). Same thread as record().
    IViewport::Backdrop backdrop() const;
    std::uint64_t blurs() const { return blurs_; }

private:
    struct Stage {
        QRhiTextureRenderTarget* rt = nullptr; // rtOut_ or rtTmp_
        Owned<QRhiBuffer> uniforms;
        Owned<QRhiShaderResourceBindings> bindings;
        QRhiTexture* source = nullptr;
        quint64 sourceId = 0; // QRhiResource::globalResourceId of `source` when the bindings were made
    };

    bool ensureTargets(const FrameContext& ctx);
    // Also the first stage's bindings, which the pipelines are made with; `blurTaps`: the Gaussian's tap count.
    bool ensurePipelines(QRhiTexture* colour, int blurTaps);
    bool ensureBindings(Stage& stage, QRhiTexture* source);
    void dropTargets(); // retires out_
    void writeUniforms(QRhiResourceUpdateBatch* u, const std::vector<detail::BlurTap>& taps);

    ResourceCounter& counter_;
    QRhi* rhi_ = nullptr;
    QSize size_, sourceSize_; // of out_ and tmp_; of the frame's colour
    Owned<QRhiTexture> out_, tmp_;
    Owned<QRhiTextureRenderTarget> rtOut_, rtTmp_;
    std::vector<Owned<QRhiTexture>> retired_;
    Owned<QRhiSampler> sampler_;
    Owned<QRhiBuffer> triangle_;
    bool triangleUploaded_ = false;
    Owned<QRhiRenderPassDescriptor> renderPass_;
    Owned<QRhiGraphicsPipeline> down_, blur_; // the 4:1 filter; the Gaussian with blurVariant_ taps
    int blurVariant_ = 0;
    Stage stages_[3];
    bool stale_ = true; // out_ holds no blur of the current colour image
    bool yUp_ = false;
    bool loggedFailure_ = false;
    std::uint64_t generation_ = 0;
    std::uint64_t blurs_ = 0;
};

} // namespace ray::render::rhi
