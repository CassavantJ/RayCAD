// The glass backdrop pass (task T-016). See backdrop_pass.h.
#include "backdrop_pass.h"

#include <QtGui/QColor>

#include <iterator>
#include <string>
#include <utility>

namespace ray::render::rhi {

namespace {

// std140; must match src/render/shaders/backdrop.glsl.
struct BlurBlock {
    float info[4];
    float taps[detail::kMaxBlurTaps][4];
};
static_assert(sizeof(BlurBlock) == 16 + 16 * detail::kMaxBlurTaps);

constexpr float kTriangle[6] = {-1, -1, 3, -1, -1, 3}; // covers the whole target
constexpr int kDownTaps = 4;
constexpr int kVariantCount = int(std::size(detail::kBlurTapVariants));

struct Shaders {
    QShader vs, down;
    QShader blur[kVariantCount]; // by index into kBlurTapVariants
    bool ok = false;
};

const Shaders& shaders()
{
    static const Shaders s = [] {
        Shaders out;
        out.vs = loadShader("backdrop.vert");
        out.down = loadShader("backdrop_down.frag");
        out.ok = out.vs.isValid() && out.down.isValid();
        for (int i = 0; i < kVariantCount; ++i) {
            const std::string name = "backdrop_blur" + std::to_string(detail::kBlurTapVariants[i]) + ".frag";
            out.blur[i] = loadShader(name.c_str());
            out.ok = out.ok && out.blur[i].isValid();
        }
        return out;
    }();
    return s;
}

// Index of the shortest shader variant that holds `taps` taps.
int variantFor(int taps)
{
    for (int i = 0; i < kVariantCount; ++i) {
        if (detail::kBlurTapVariants[i] >= taps)
            return i;
    }
    return kVariantCount - 1;
}

} // namespace

BackdropPass::BackdropPass(ResourceCounter& counter) : counter_(counter) {}

BackdropPass::~BackdropPass()
{
    releaseResources();
}

void BackdropPass::releaseResources()
{
    down_.reset();
    blur_.reset();
    blurVariant_ = 0;
    for (Stage& s : stages_) {
        s.bindings.reset();
        s.uniforms.reset();
        s.rt = nullptr;
        s.source = nullptr;
        s.sourceId = 0;
    }
    rtOut_.reset();
    rtTmp_.reset();
    renderPass_.reset();
    out_.reset();
    tmp_.reset();
    retired_.clear();
    sampler_.reset();
    triangle_.reset();
    triangleUploaded_ = false;
    size_ = sourceSize_ = QSize();
    stale_ = true;
    rhi_ = nullptr;
}

void BackdropPass::frameDone()
{
    retired_.clear();
}

IViewport::Backdrop BackdropPass::backdrop() const
{
    if (!out_ || stale_)
        return IViewport::Backdrop{nullptr, 0, 0, yUp_, generation_};
    return IViewport::Backdrop{out_.get(), size_.width(), size_.height(), yUp_, generation_};
}

void BackdropPass::dropTargets()
{
    for (Stage& s : stages_)
        s.rt = nullptr;
    rtOut_.reset();
    rtTmp_.reset();
    if (out_)
        retired_.push_back(std::move(out_)); // a scene graph node may still point at it for this frame
    tmp_.reset();
    size_ = QSize();
    stale_ = true;
}

bool BackdropPass::ensureTargets(const FrameContext& ctx)
{
    const QSize want(detail::backdropSize(ctx.pixelSize.width()), detail::backdropSize(ctx.pixelSize.height()));
    sourceSize_ = ctx.pixelSize;
    if (out_ && rtOut_ && rtTmp_ && want == size_)
        return true;
    dropTargets();
    out_ = own(counter_, rhi_->newTexture(QRhiTexture::RGBA8, want, 1,
                                          QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
    tmp_ = own(counter_, rhi_->newTexture(QRhiTexture::RGBA8, want, 1, QRhiTexture::RenderTarget));
    if (!out_->create() || !tmp_->create()) {
        out_.reset();
        tmp_.reset();
        return false;
    }
    rtOut_ = own(counter_, rhi_->newTextureRenderTarget(QRhiTextureRenderTargetDescription(out_.get())));
    rtTmp_ = own(counter_, rhi_->newTextureRenderTarget(QRhiTextureRenderTargetDescription(tmp_.get())));
    if (!renderPass_)
        renderPass_ = own(counter_, rtOut_->newCompatibleRenderPassDescriptor());
    rtOut_->setRenderPassDescriptor(renderPass_.get());
    rtTmp_->setRenderPassDescriptor(renderPass_.get());
    if (!rtOut_->create() || !rtTmp_->create()) {
        dropTargets();
        return false;
    }
    size_ = want;
    stages_[0].rt = rtOut_.get(); // colour -> out_ (4:1)
    stages_[1].rt = rtTmp_.get(); // out_ -> tmp_ (horizontal)
    stages_[2].rt = rtOut_.get(); // tmp_ -> out_ (vertical)
    stale_ = true;
    return true;
}

bool BackdropPass::ensureBindings(Stage& stage, QRhiTexture* source)
{
    if (!stage.uniforms) {
        stage.uniforms =
            own(counter_, rhi_->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(BlurBlock)));
        if (!stage.uniforms->create()) {
            stage.uniforms.reset();
            return false;
        }
    }
    if (!stage.bindings)
        stage.bindings = own(counter_, rhi_->newShaderResourceBindings());
    else if (stage.source == source && stage.sourceId == source->globalResourceId())
        return true;
    // Rebuilt in place, never replaced: the pipeline was created against the first stage's object.
    stage.bindings->setBindings(
        {QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::FragmentStage, stage.uniforms.get()),
         QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, source,
                                                   sampler_.get())});
    if (!stage.bindings->create()) {
        stage.source = nullptr;
        stage.sourceId = 0;
        return false;
    }
    stage.source = source;
    stage.sourceId = source->globalResourceId();
    return true;
}

bool BackdropPass::ensurePipelines(QRhiTexture* colour, int blurTaps)
{
    const Shaders& sh = shaders();
    if (!sh.ok)
        return false;
    if (!sampler_) {
        sampler_ = own(counter_, rhi_->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                                                  QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
        if (!sampler_->create()) {
            sampler_.reset();
            return false;
        }
    }
    if (!triangle_) {
        triangle_ = own(counter_, rhi_->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof(kTriangle)));
        triangleUploaded_ = false;
        if (!triangle_->create()) {
            triangle_.reset();
            return false;
        }
    }
    if (!ensureBindings(stages_[0], colour))
        return false;
    const auto make = [&](const QShader& fs) {
        Owned<QRhiGraphicsPipeline> p = own(counter_, rhi_->newGraphicsPipeline());
        p->setShaderStages({{QRhiShaderStage::Vertex, sh.vs}, {QRhiShaderStage::Fragment, fs}});
        QRhiVertexInputLayout layout;
        layout.setBindings({QRhiVertexInputBinding(2 * sizeof(float))});
        layout.setAttributes({QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0)});
        p->setVertexInputLayout(layout);
        p->setTopology(QRhiGraphicsPipeline::Triangles);
        p->setCullMode(QRhiGraphicsPipeline::None);
        // Every stage's bindings have this layout.
        p->setShaderResourceBindings(stages_[0].bindings.get());
        p->setRenderPassDescriptor(renderPass_.get());
        if (!p->create())
            p.reset();
        return p;
    };
    if (!down_)
        down_ = make(sh.down);
    const int variant = variantFor(blurTaps);
    if (!blur_ || blurVariant_ != variant) {
        blur_ = make(sh.blur[variant]); // a theme or pixel-ratio change may need another length
        blurVariant_ = variant;
    }
    return down_ && blur_;
}

void BackdropPass::writeUniforms(QRhiResourceUpdateBatch* u, const std::vector<detail::BlurTap>& taps)
{
    const float tx = 1.0f / float(size_.width());
    const float ty = 1.0f / float(size_.height());
    // 1. 4:1 box filter: four bilinear taps one source pixel from the target texel's centre, each the mean of 2 x 2.
    BlurBlock down{};
    down.info[0] = tx;
    down.info[1] = ty;
    const float sx = 1.0f / float(sourceSize_.width());
    const float sy = 1.0f / float(sourceSize_.height());
    const float corners[kDownTaps][2] = {{-sx, -sy}, {sx, -sy}, {-sx, sy}, {sx, sy}};
    for (int i = 0; i < kDownTaps; ++i) {
        down.taps[i][0] = corners[i][0];
        down.taps[i][1] = corners[i][1];
        down.taps[i][2] = 1.0f / kDownTaps;
    }
    u->updateDynamicBuffer(stages_[0].uniforms.get(), 0, sizeof(down), &down);

    // 2. and 3. the Gaussian, across then down. Taps beyond `taps` keep weight 0 (shader variant padding).
    for (int pass = 0; pass < 2; ++pass) {
        BlurBlock block{};
        block.info[0] = tx;
        block.info[1] = ty;
        for (std::size_t i = 0; i < taps.size(); ++i) {
            block.taps[i][pass] = taps[i].offset * (pass == 0 ? tx : ty);
            block.taps[i][2] = taps[i].weight;
        }
        u->updateDynamicBuffer(stages_[1 + pass].uniforms.get(), 0, sizeof(block), &block);
    }
}

void BackdropPass::record(const FrameContext& ctx)
{
    if (!ctx.snapshot || !ctx.snapshot->backdropEnabled) {
        // Off is zero cost: no pass, and no resource beyond a texture retired this frame.
        if (rhi_) {
            std::vector<Owned<QRhiTexture>> keep = std::move(retired_);
            dropTargets();
            for (auto& t : retired_)
                keep.push_back(std::move(t));
            releaseResources();
            retired_ = std::move(keep);
        }
        return;
    }
    if (!ctx.rhi || !ctx.cb || !ctx.colour || ctx.pixelSize.isEmpty())
        return;
    if (ctx.rhi != rhi_) {
        releaseResources();
        rhi_ = ctx.rhi;
    }
    yUp_ = rhi_->isYUpInFramebuffer();
    const std::vector<detail::BlurTap> taps =
        detail::gaussianTaps(detail::backdropSigma(ctx.snapshot->theme.get(), ctx.snapshot->devicePixelRatio));
    const bool ok = ensureTargets(ctx) && ensurePipelines(ctx.colour, int(taps.size())) &&
                    ensureBindings(stages_[1], out_.get()) && ensureBindings(stages_[2], tmp_.get());
    if (!ok) {
        if (!loggedFailure_) {
            ray::log(LogLevel::Error, "render", "glass backdrop could not be created; glass panels show no blur");
            loggedFailure_ = true;
        }
        return;
    }
    // Only a changed image (or none blurred yet) is blurred again; panels keep sampling the last blur.
    if (!ctx.colourRedrawn && !stale_)
        return;

    QRhiCommandBuffer* cb = ctx.cb;
    QRhiResourceUpdateBatch* u = rhi_->nextResourceUpdateBatch();
    if (!triangleUploaded_) {
        u->uploadStaticBuffer(triangle_.get(), kTriangle);
        triangleUploaded_ = true;
    }
    writeUniforms(u, taps);
    const QRhiViewport viewport(0, 0, float(size_.width()), float(size_.height()));
    const QRhiCommandBuffer::VertexInput vin(triangle_.get(), 0);
    for (int i = 0; i < 3; ++i) {
        cb->beginPass(stages_[i].rt, QColor(Qt::black), {1.0f, 0}, i == 0 ? u : nullptr);
        cb->setGraphicsPipeline(i == 0 ? down_.get() : blur_.get());
        cb->setViewport(viewport);
        cb->setShaderResources(stages_[i].bindings.get());
        cb->setVertexInput(0, 1, &vin);
        cb->draw(3);
        cb->endPass();
    }
    stale_ = false;
    ++generation_;
    ++blurs_;
}

} // namespace ray::render::rhi
