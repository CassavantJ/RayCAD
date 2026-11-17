// Spike S1, variant A: own renderer on Qt RHI inside a QQuickRhiItem.
#include "viewport.h"
#include "bench.h"
#include "scene.h"
#include <QFile>
#include <QQuickWindow>
#include <QSGTexture>
#include <rhi/qrhi.h>
#include <memory>

namespace {

QShader loadShader(const char *name)
{
    QFile f(QString::fromLatin1(":/shaders/%1.qsb").arg(QLatin1String(name)));
    if (!f.open(QIODevice::ReadOnly))
        qFatal("missing shader %s", name);
    return QShader::fromSerialized(f.readAll());
}

struct SceneUniforms { float mvp[16], mv[16], light[4], face[4], edge[4], highlight[4]; };
struct BlurUniforms { float dir[2], ySign, pad; };

} // namespace

class ViewportRenderer : public QQuickRhiItemRenderer
{
public:
    ~ViewportRenderer() override
    {
        m_rtA.reset();
        releaseBackdropTexture();
    }

protected:
    void initialize(QRhiCommandBuffer *) override
    {
        if (m_rhi != rhi()) {
            m_rhi = rhi();
            m_facePipe.reset(); m_vbuf.reset();
        }
        if (!m_vbuf)
            createStatic();
        QRhiRenderTarget *rt = renderTarget();
        const auto rpFormat = rt->renderPassDescriptor()->serializedFormat();
        if (!m_facePipe || rpFormat != m_rpFormat || rt->sampleCount() != m_samples) {
            m_rpFormat = rpFormat;
            m_samples = rt->sampleCount();
            createScenePipelines();
        }
        QRhiTexture *src = resolveTexture() ? resolveTexture() : colorTexture();
        if (rt->pixelSize() != m_size || src != m_src) {
            m_size = rt->pixelSize();
            m_src = src;
            createSized();
        }
    }

    void synchronize(QQuickRhiItem *item) override
    {
        auto *vp = static_cast<ViewportItem *>(item);
        m_item = vp;
        m_backdrop = vp->m_backdrop;
        if (vp->m_pickPending) {
            vp->m_pickPending = false;
            m_pick = true; m_pickPx = vp->m_pickPx; m_pickStampNs = vp->m_pickStampNs;
        }
        if (m_sgDirty && m_texA) {
            m_sgTex = vp->window()->createTextureFromRhiTexture(m_texA);
            m_sgTex->setFiltering(QSGTexture::Linear);
            vp->backdropProvider()->set(m_sgTex);
            m_sgDirty = false;
        }
    }

    void render(QRhiCommandBuffer *cb) override
    {
        QRhiResourceUpdateBatch *u = m_rhi->nextResourceUpdateBatch();
        if (m_pending) { u->merge(m_pending); m_pending->release(); m_pending = nullptr; }

        const double t = s1::orbitSeconds();
        const QMatrix4x4 view = s1::viewMatrix(t);
        const QMatrix4x4 mvp = m_rhi->clipSpaceCorrMatrix() * s1::projMatrix(float(m_size.width()) / m_size.height()) * view;
        SceneUniforms ub{};
        memcpy(ub.mvp, mvp.constData(), 64);
        memcpy(ub.mv, view.constData(), 64);
        const float light[4] = {0.25f, 0.45f, 0.86f, 0}, face[4] = {0.62f, 0.66f, 0.72f, 1}, edge[4] = {0.05f, 0.05f, 0.06f, 1};
        memcpy(ub.light, light, 16); memcpy(ub.face, face, 16); memcpy(ub.edge, edge, 16);
        for (int i = 0; i < 4; ++i) ub.highlight[i] = float((m_highlight >> (8 * i)) & 255) / 255.0f;
        if (m_highlight == 0) ub.highlight[3] = 2.0f;   // matches nothing
        u->updateDynamicBuffer(m_sceneUbuf.get(), 0, sizeof(ub), &ub);

        const QRhiViewport vpFull(0, 0, float(m_size.width()), float(m_size.height()));
        const QRhiCommandBuffer::VertexInput vin(m_vbuf.get(), 0), ein(m_ebuf.get(), 0), qin(m_quadBuf.get(), 0);

        // 1. shaded faces, then edge lines
        cb->beginPass(renderTarget(), QColor::fromRgbF(0.16f, 0.17f, 0.19f), {1.0f, 0}, u);
        cb->setGraphicsPipeline(m_facePipe.get());
        cb->setViewport(vpFull);
        cb->setShaderResources(m_sceneSrb.get());
        cb->setVertexInput(0, 1, &vin, m_ibuf.get(), 0, QRhiCommandBuffer::IndexUInt32);
        for (const s1::Body &b : m_bodies) cb->drawIndexed(b.indexCount, 1, b.firstIndex);
        cb->setGraphicsPipeline(m_edgePipe.get());
        cb->setShaderResources(m_sceneSrb.get());
        cb->setVertexInput(0, 1, &ein);
        for (const s1::Body &b : m_bodies) cb->draw(b.edgeVertexCount, 1, b.firstEdgeVertex);
        cb->endPass();

        // 2. ID buffer, only when a pick was requested
        if (m_pick) {
            m_pick = false;
            m_pickInFlight = true;
            cb->beginPass(m_idRt.get(), QColor(0, 0, 0, 0), {1.0f, 0});
            cb->setGraphicsPipeline(m_idPipe.get());
            cb->setViewport(vpFull);
            cb->setShaderResources(m_sceneSrb.get());
            cb->setVertexInput(0, 1, &vin, m_ibuf.get(), 0, QRhiCommandBuffer::IndexUInt32);
            for (const s1::Body &b : m_bodies) cb->drawIndexed(b.indexCount, 1, b.firstIndex);
            QRhiResourceUpdateBatch *rb = m_rhi->nextResourceUpdateBatch();
            const int px = qBound(0, m_pickPx.x(), m_size.width() - 1);
            int py = qBound(0, m_pickPx.y(), m_size.height() - 1);
            if (m_rhi->isYUpInFramebuffer()) py = m_size.height() - 1 - py;
            QRhiReadbackDescription desc(m_idTex.get());
            desc.setRect(QRect(px, py, 1, 1));
            const qint64 stamp = m_pickStampNs;
            m_readback.completed = [this, stamp] {
                const auto *p = reinterpret_cast<const uchar *>(m_readback.data.constData());
                const int id = m_readback.data.size() >= 4 ? int(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24)) : -1;
                const double ms = (s1::clock().nsecsElapsed() - stamp) * 1e-6;
                m_highlight = id > 0 ? quint32(id) : 0;
                m_pickInFlight = false;
                ViewportItem *item = m_item;
                QMetaObject::invokeMethod(item, [item, id, ms] { emit item->picked(id, ms); }, Qt::QueuedConnection);
            };
            rb->readBackTexture(desc, &m_readback);
            cb->endPass(rb);
        }

        // 3. shared backdrop: 4:1 downsample, then separable blur. Final image is in m_texA.
        if (m_backdrop) {
            const QRhiViewport vpQ(0, 0, float(m_qsize.width()), float(m_qsize.height()));
            cb->beginPass(m_rtA.get(), Qt::black, {1.0f, 0});
            cb->setGraphicsPipeline(m_downPipe.get());
            cb->setViewport(vpQ);
            cb->setShaderResources(m_downSrb.get());
            cb->setVertexInput(0, 1, &qin);
            cb->draw(3);
            cb->endPass();
            cb->beginPass(m_rtB.get(), Qt::black, {1.0f, 0});
            cb->setGraphicsPipeline(m_blurPipe.get());
            cb->setViewport(vpQ);
            cb->setShaderResources(m_blurHSrb.get());
            cb->setVertexInput(0, 1, &qin);
            cb->draw(3);
            cb->endPass();
            cb->beginPass(m_rtA.get(), Qt::black, {1.0f, 0});
            cb->setGraphicsPipeline(m_blurPipe.get());
            cb->setViewport(vpQ);
            cb->setShaderResources(m_blurVSrb.get());
            cb->setVertexInput(0, 1, &qin);
            cb->draw(3);
            cb->endPass();
        }

        if (s1::g_animate.load() || m_pickInFlight)
            update();
    }

private:
    void releaseBackdropTexture()
    {
        if (m_sgTex) {
            if (m_item && m_item->m_provider) m_item->m_provider->set(nullptr);
            delete m_sgTex;   // also deletes m_texA
        } else {
            delete m_texA;
        }
        m_sgTex = nullptr; m_texA = nullptr;
    }

    QRhiResourceUpdateBatch *pending()
    {
        if (!m_pending) m_pending = m_rhi->nextResourceUpdateBatch();
        return m_pending;
    }

    void createStatic()
    {
        const s1::Mesh mesh = s1::buildMesh();
        m_bodies = mesh.bodies;
        m_vbuf.reset(m_rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, quint32(mesh.verts.size() * sizeof(s1::Vertex))));
        m_ibuf.reset(m_rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::IndexBuffer, quint32(mesh.indices.size() * 4)));
        m_ebuf.reset(m_rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, quint32(mesh.edgeVerts.size() * 4)));
        static const float quad[6] = {-1, -1, 3, -1, -1, 3};
        m_quadBuf.reset(m_rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof(quad)));
        m_vbuf->create(); m_ibuf->create(); m_ebuf->create(); m_quadBuf->create();
        pending()->uploadStaticBuffer(m_vbuf.get(), mesh.verts.data());
        pending()->uploadStaticBuffer(m_ibuf.get(), mesh.indices.data());
        pending()->uploadStaticBuffer(m_ebuf.get(), mesh.edgeVerts.data());
        pending()->uploadStaticBuffer(m_quadBuf.get(), quad);

        m_sceneUbuf.reset(m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(SceneUniforms)));
        m_sceneUbuf->create();
        m_sceneSrb.reset(m_rhi->newShaderResourceBindings());
        m_sceneSrb->setBindings({QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, m_sceneUbuf.get())});
        m_sceneSrb->create();
        m_sampler.reset(m_rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                                          QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
        m_sampler->create();
        for (auto *b : {&m_downUbuf, &m_blurHUbuf, &m_blurVUbuf}) {
            b->reset(m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(BlurUniforms)));
            (*b)->create();
        }
    }

    QRhiGraphicsPipeline *meshPipeline(const char *vs, const char *fs, bool lines, bool biased,
                                       QRhiRenderPassDescriptor *rp, int samples)
    {
        QRhiGraphicsPipeline *p = m_rhi->newGraphicsPipeline();
        p->setShaderStages({{QRhiShaderStage::Vertex, loadShader(vs)}, {QRhiShaderStage::Fragment, loadShader(fs)}});
        QRhiVertexInputLayout layout;
        if (lines) {
            layout.setBindings({{3 * sizeof(float)}});
            layout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float3, 0}});
            p->setTopology(QRhiGraphicsPipeline::Lines);
            p->setDepthOp(QRhiGraphicsPipeline::LessOrEqual);
        } else {
            layout.setBindings({{sizeof(s1::Vertex)}});
            layout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float3, 0},
                                  {0, 1, QRhiVertexInputAttribute::Float3, 12},
                                  {0, 2, QRhiVertexInputAttribute::UNormByte4, 24}});
        }
        if (biased) { p->setDepthBias(2); p->setSlopeScaledDepthBias(1.5f); }   // faces sit behind their edges
        p->setVertexInputLayout(layout);
        p->setDepthTest(true);
        p->setDepthWrite(true);
        p->setCullMode(QRhiGraphicsPipeline::None);
        p->setSampleCount(samples);
        p->setShaderResourceBindings(m_sceneSrb.get());
        p->setRenderPassDescriptor(rp);
        if (!p->create()) qFatal("pipeline %s/%s failed", vs, fs);
        return p;
    }

    void createScenePipelines()
    {
        QRhiRenderPassDescriptor *rp = renderTarget()->renderPassDescriptor();
        m_facePipe.reset(meshPipeline("scene.vert", "scene.frag", false, true, rp, m_samples));
        m_edgePipe.reset(meshPipeline("line.vert", "line.frag", true, false, rp, m_samples));
    }

    QRhiGraphicsPipeline *quadPipeline(const char *fs, QRhiShaderResourceBindings *srb)
    {
        QRhiGraphicsPipeline *p = m_rhi->newGraphicsPipeline();
        p->setShaderStages({{QRhiShaderStage::Vertex, loadShader("quad.vert")}, {QRhiShaderStage::Fragment, loadShader(fs)}});
        QRhiVertexInputLayout layout;
        layout.setBindings({{2 * sizeof(float)}});
        layout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float2, 0}});
        p->setVertexInputLayout(layout);
        p->setShaderResourceBindings(srb);
        p->setRenderPassDescriptor(m_blurRp.get());
        if (!p->create()) qFatal("pipeline %s failed", fs);
        return p;
    }

    void createSized()
    {
        s1::g_rtW = m_size.width(); s1::g_rtH = m_size.height();
        ++s1::g_rtReallocs;
        // ID buffer
        m_idTex.reset(m_rhi->newTexture(QRhiTexture::RGBA8, m_size, 1, QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
        m_idTex->create();
        m_idDepth.reset(m_rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil, m_size, 1));
        m_idDepth->create();
        QRhiTextureRenderTargetDescription idDesc{QRhiColorAttachment(m_idTex.get())};
        idDesc.setDepthStencilBuffer(m_idDepth.get());
        m_idRt.reset(m_rhi->newTextureRenderTarget(idDesc));
        m_idRp.reset(m_idRt->newCompatibleRenderPassDescriptor());
        m_idRt->setRenderPassDescriptor(m_idRp.get());
        m_idRt->create();
        m_idPipe.reset(meshPipeline("scene.vert", "id.frag", false, false, m_idRp.get(), 1));

        // quarter-resolution blur targets. Finding: the QSGTexture made by createTextureFromRhiTexture()
        // deletes the QRhiTexture it wraps, so once wrapped, m_texA belongs to m_sgTex.
        m_rtA.reset();
        releaseBackdropTexture();
        m_qsize = QSize(qMax(1, m_size.width() / 4), qMax(1, m_size.height() / 4));
        m_texA = m_rhi->newTexture(QRhiTexture::RGBA8, m_qsize, 1, QRhiTexture::RenderTarget);
        m_texB.reset(m_rhi->newTexture(QRhiTexture::RGBA8, m_qsize, 1, QRhiTexture::RenderTarget));
        m_texA->create(); m_texB->create();
        m_rtA.reset(m_rhi->newTextureRenderTarget({QRhiColorAttachment(m_texA)}));
        m_blurRp.reset(m_rtA->newCompatibleRenderPassDescriptor());
        m_rtA->setRenderPassDescriptor(m_blurRp.get());
        m_rtA->create();
        m_rtB.reset(m_rhi->newTextureRenderTarget({QRhiColorAttachment(m_texB.get())}));
        m_rtB->setRenderPassDescriptor(m_blurRp.get());
        m_rtB->create();

        const auto bind = [this](std::unique_ptr<QRhiShaderResourceBindings> &srb, QRhiBuffer *ubuf, QRhiTexture *tex) {
            srb.reset(m_rhi->newShaderResourceBindings());
            srb->setBindings({QRhiShaderResourceBinding::uniformBuffer(
                                  0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, ubuf),
                              QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, tex, m_sampler.get())});
            srb->create();
        };
        bind(m_downSrb, m_downUbuf.get(), m_src);
        bind(m_blurHSrb, m_blurHUbuf.get(), m_texA);
        bind(m_blurVSrb, m_blurVUbuf.get(), m_texB.get());
        m_downPipe.reset(quadPipeline("down.frag", m_downSrb.get()));
        m_blurPipe.reset(quadPipeline("blur.frag", m_blurHSrb.get()));

        const float ySign = m_rhi->isYUpInFramebuffer() ? 1.0f : -1.0f;
        const BlurUniforms down{{1.0f / m_size.width(), 1.0f / m_size.height()}, ySign, 0};
        const BlurUniforms bh{{1.0f / m_qsize.width(), 0}, ySign, 0}, bv{{0, 1.0f / m_qsize.height()}, ySign, 0};
        pending()->updateDynamicBuffer(m_downUbuf.get(), 0, sizeof(down), &down);
        pending()->updateDynamicBuffer(m_blurHUbuf.get(), 0, sizeof(bh), &bh);
        pending()->updateDynamicBuffer(m_blurVUbuf.get(), 0, sizeof(bv), &bv);
        m_sgDirty = true;
    }

    QRhi *m_rhi = nullptr;
    QRhiResourceUpdateBatch *m_pending = nullptr;
    std::vector<s1::Body> m_bodies;
    std::unique_ptr<QRhiBuffer> m_vbuf, m_ibuf, m_ebuf, m_quadBuf, m_sceneUbuf, m_downUbuf, m_blurHUbuf, m_blurVUbuf;
    std::unique_ptr<QRhiSampler> m_sampler;
    std::unique_ptr<QRhiShaderResourceBindings> m_sceneSrb, m_downSrb, m_blurHSrb, m_blurVSrb;
    std::unique_ptr<QRhiGraphicsPipeline> m_facePipe, m_edgePipe, m_idPipe, m_downPipe, m_blurPipe;
    std::unique_ptr<QRhiTexture> m_idTex, m_texB;
    QRhiTexture *m_texA = nullptr;
    std::unique_ptr<QRhiRenderBuffer> m_idDepth;
    std::unique_ptr<QRhiTextureRenderTarget> m_idRt, m_rtA, m_rtB;
    std::unique_ptr<QRhiRenderPassDescriptor> m_idRp, m_blurRp;
    QVector<quint32> m_rpFormat;
    int m_samples = 0;
    QSize m_size, m_qsize;
    QRhiTexture *m_src = nullptr;
    QSGTexture *m_sgTex = nullptr;
    bool m_sgDirty = false, m_backdrop = true, m_pick = false, m_pickInFlight = false;
    QPoint m_pickPx; qint64 m_pickStampNs = 0;
    quint32 m_highlight = 0;
    QRhiReadbackResult m_readback;
    ViewportItem *m_item = nullptr;
};

QQuickRhiItemRenderer *ViewportItem::createRenderer() { return new ViewportRenderer; }

void ViewportItem::pick(qreal x, qreal y)
{
    const qreal dpr = window() ? window()->effectiveDevicePixelRatio() : 1.0;
    m_pickPx = QPoint(int(x * dpr), int(y * dpr));
    m_pickStampNs = s1::clock().nsecsElapsed();
    m_pickPending = true;
    update();
}

BackdropProvider *ViewportItem::backdropProvider()
{
    if (!m_provider) m_provider = new BackdropProvider;
    return m_provider;
}
