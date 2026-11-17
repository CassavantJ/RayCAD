// Spike S1, variant B: OCCT V3d_View rendering into a QQuickFramebufferObject (Qt Quick on OpenGL).
// Everything OCCT lives on the Qt Quick render thread, where the OpenGL context of Qt is current.
#include "occtview.h"
#include "bench.h"
#include "scene.h"
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QQuickOpenGLUtils>
#include <QQuickWindow>

#include <AIS_InteractiveContext.hxx>
#include <AIS_InteractiveObject.hxx>
#include <Aspect_DisplayConnection.hxx>
#include <Aspect_NeutralWindow.hxx>
#include <Graphic3d_ArrayOfSegments.hxx>
#include <Graphic3d_ArrayOfTriangles.hxx>
#include <Graphic3d_AspectLine3d.hxx>
#include <OpenGl_Context.hxx>
#include <OpenGl_FrameBuffer.hxx>
#include <OpenGl_GraphicDriver.hxx>
#include <OpenGl_View.hxx>
#include <OpenGl_Window.hxx>
#include <Prs3d_ShadingAspect.hxx>
#include <Select3D_SensitivePrimitiveArray.hxx>
#include <SelectMgr_EntityOwner.hxx>
#include <V3d_View.hxx>
#include <V3d_Viewer.hxx>

namespace {

class FaceOwner : public SelectMgr_EntityOwner
{
    DEFINE_STANDARD_RTTI_INLINE(FaceOwner, SelectMgr_EntityOwner)
public:
    FaceOwner(const Handle(SelectMgr_SelectableObject) &obj, int id) : SelectMgr_EntityOwner(obj, 5), faceId(id) {}
    int faceId;
};

// One presentation per body: one indexed triangle array with normals, one segment array for the edges.
// This is what AIS_Shape hands to the driver for a meshed solid; it is built directly so that A and B
// draw exactly the same triangles.
class BodyObject : public AIS_InteractiveObject
{
    DEFINE_STANDARD_RTTI_INLINE(BodyObject, AIS_InteractiveObject)
public:
    BodyObject(const s1::Mesh &m, int bodyIndex) : m_body(bodyIndex)
    {
        const s1::Body &b = m.bodies[bodyIndex];
        m_tris = new Graphic3d_ArrayOfTriangles(int(b.vertexCount), int(b.indexCount), Graphic3d_ArrayFlags_VertexNormal);
        for (std::uint32_t i = 0; i < b.vertexCount; ++i) {
            const s1::Vertex &v = m.verts[b.firstVertex + i];
            m_tris->AddVertex(v.px, v.py, v.pz, v.nx, v.ny, v.nz);
        }
        for (std::uint32_t i = 0; i < b.indexCount; ++i)
            m_tris->AddEdge(int(m.indices[b.firstIndex + i] - b.firstVertex) + 1);
        m_segs = new Graphic3d_ArrayOfSegments(int(b.edgeVertexCount));
        for (std::uint32_t i = 0; i < b.edgeVertexCount; ++i) {
            const float *p = &m.edgeVerts[(b.firstEdgeVertex + i) * 3];
            m_segs->AddVertex(p[0], p[1], p[2]);
        }
    }

private:
    bool AcceptDisplayMode(const int mode) const override { return mode == 0; }
    void Compute(const Handle(PrsMgr_PresentationManager) &, const Handle(Prs3d_Presentation) &prs, const int) override
    {
        Handle(Prs3d_ShadingAspect) shading = new Prs3d_ShadingAspect();
        shading->SetColor(Quantity_Color(0.62, 0.66, 0.72, Quantity_TOC_sRGB));
        Handle(Graphic3d_Group) faces = prs->NewGroup();
        faces->SetGroupPrimitivesAspect(shading->Aspect());
        faces->AddPrimitiveArray(m_tris);
        Handle(Graphic3d_Group) edges = prs->NewGroup();
        edges->SetGroupPrimitivesAspect(new Graphic3d_AspectLine3d(Quantity_Color(0.05, 0.05, 0.06, Quantity_TOC_sRGB), Aspect_TOL_SOLID, 1.0));
        edges->AddPrimitiveArray(m_segs);
    }
    void ComputeSelection(const Handle(SelectMgr_Selection) &sel, const int) override
    {
        const int perFace = s1::kRows * s1::kSegments * 6;
        for (int f = 0; f < s1::kFaces; ++f) {
            Handle(FaceOwner) owner = new FaceOwner(this, int(s1::faceId(m_body, f)));
            Handle(Select3D_SensitivePrimitiveArray) sens = new Select3D_SensitivePrimitiveArray(owner);
            sens->InitTriangulation(m_tris->Attributes(), m_tris->Indices(), TopLoc_Location(), f * perFace, (f + 1) * perFace - 1);
            sel->Add(sens);
        }
    }
    int m_body;
    Handle(Graphic3d_ArrayOfTriangles) m_tris;
    Handle(Graphic3d_ArrayOfSegments) m_segs;
};

} // namespace

class OcctRenderer : public QQuickFramebufferObject::Renderer
{
public:
    ~OcctRenderer() override
    {
        // Runs on the render thread with the context current. Release OCCT GL resources here.
        if (!m_ctx.IsNull()) m_ctx->RemoveAll(false);
        if (!m_view.IsNull()) m_view->Remove();
        m_ctx.Nullify(); m_view.Nullify(); m_viewer.Nullify();
    }

protected:
    QOpenGLFramebufferObject *createFramebufferObject(const QSize &size) override
    {
        QOpenGLFramebufferObjectFormat f;
        f.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
        s1::g_rtW = size.width(); s1::g_rtH = size.height();
        ++s1::g_rtReallocs;
        return new QOpenGLFramebufferObject(size, f);
    }

    void synchronize(QQuickFramebufferObject *item) override
    {
        auto *v = static_cast<OcctView *>(item);
        m_item = v;
        m_hwnd = reinterpret_cast<void *>(v->window()->winId());
        if (v->m_pickPending) {
            v->m_pickPending = false;
            m_pick = true; m_pickPx = v->m_pickPx; m_pickStampNs = v->m_pickStampNs;
        }
    }

    void render() override
    {
        const QSize size = framebufferObject()->size();
        if (m_view.IsNull() && !init(size)) {
            if (!m_failed) { m_failed = true; qWarning("OCCT init failed"); }
            return;
        }
        Handle(OpenGl_Context) gl = Handle(OpenGl_View)::DownCast(m_view->View())->GlWindow()->GetGlContext();
        // Make OCCT treat the FBO of this item as its window back buffer. Qt binds it before render(), but
        // OCCT's own initialisation (first frame) rebinds framebuffer 0, so bind it again.
        framebufferObject()->bind();
        Handle(OpenGl_FrameBuffer) target = gl->DefaultFrameBuffer();
        if (target.IsNull()) {
            target = new OpenGl_FrameBuffer();
            gl->SetDefaultFrameBuffer(target);
        }
        if (!target->InitWrapper(gl)) {
            if (!m_failed) { m_failed = true; qWarning("OCCT could not wrap the Qt framebuffer object"); }
            return;
        }
        if (size != m_size) {
            m_size = size;
            Handle(Aspect_NeutralWindow)::DownCast(m_view->Window())->SetSize(size.width(), size.height());
            m_view->MustBeResized();
        }
        const s1::Cam c = s1::orbitCamera(s1::orbitSeconds());
        const Handle(Graphic3d_Camera) &cam = m_view->Camera();
        cam->SetEyeAndCenter(gp_Pnt(c.eye[0], c.eye[1], c.eye[2]), gp_Pnt(c.target[0], c.target[1], c.target[2]));
        cam->SetUp(gp_Dir(0, 0, 1));
        cam->OrthogonalizeUp();
        m_view->Invalidate();
        m_view->Redraw();

        if (m_pick) {
            m_pick = false;
            m_ctx->MoveTo(m_pickPx.x(), m_pickPx.y(), m_view, false);
            int id = 0;
            if (m_ctx->HasDetected()) {
                Handle(FaceOwner) o = Handle(FaceOwner)::DownCast(m_ctx->DetectedOwner());
                if (!o.IsNull()) id = o->faceId;
            }
            const double ms = (s1::clock().nsecsElapsed() - m_pickStampNs) * 1e-6;
            OcctView *item = m_item;
            QMetaObject::invokeMethod(item, [item, id, ms] { emit item->picked(id, ms); }, Qt::QueuedConnection);
        }

        QQuickOpenGLUtils::resetOpenGLState();
        if (s1::g_animate.load())
            update();
    }

private:
    bool init(const QSize &size)
    {
        const bool core = QOpenGLContext::currentContext()->format().profile() == QSurfaceFormat::CoreProfile;
        Handle(Aspect_DisplayConnection) disp = new Aspect_DisplayConnection();
        Handle(OpenGl_GraphicDriver) driver = new OpenGl_GraphicDriver(disp, false);
        driver->ChangeOptions().buffersNoSwap = true;        // Qt swaps
        driver->ChangeOptions().buffersOpaqueAlpha = true;   // do not write alpha
        // true: draw straight into the Qt FBO (fastest). The OCCT Qt samples use false (own FBO plus a blit).
        driver->ChangeOptions().useSystemBuffer = qEnvironmentVariableIsSet("S1_OCCT_SYSTEM_BUFFER")
            ? qEnvironmentVariableIntValue("S1_OCCT_SYSTEM_BUFFER") != 0 : true;
        driver->ChangeOptions().sRGBDisable = true;          // same colour handling as A
        driver->ChangeOptions().swapInterval = 0;

        m_viewer = new V3d_Viewer(driver);
        m_viewer->SetDefaultLights();
        m_viewer->SetLightOn();
        m_ctx = new AIS_InteractiveContext(m_viewer);
        m_view = m_viewer->CreateView();
        m_view->SetImmediateUpdate(false);
        m_view->SetBackgroundColor(Quantity_Color(0.16, 0.17, 0.19, Quantity_TOC_sRGB));
        m_view->SetShadingModel(Graphic3d_TypeOfShadingModel_Phong);
        m_view->ChangeRenderingParams().NbMsaaSamples = 0;
        m_view->Camera()->SetProjectionType(Graphic3d_Camera::Projection_Perspective);
        m_view->Camera()->SetFOVy(s1::orbitCamera(0).fovYDeg);

        Handle(OpenGl_Context) gl = new OpenGl_Context();
        if (!gl->Init(core)) return false;
        Handle(Aspect_NeutralWindow) win = new Aspect_NeutralWindow();
        win->SetVirtual(true);
        win->SetNativeHandle(Aspect_Drawable(m_hwnd));
        win->SetSize(size.width(), size.height());
        m_view->SetWindow(win, gl->RenderingContext());
        m_size = size;

        const s1::Mesh mesh = s1::buildMesh();
        for (int b = 0; b < int(mesh.bodies.size()); ++b)
            m_ctx->Display(new BodyObject(mesh, b), 0, 0, false);
        return true;
    }

    Handle(V3d_Viewer) m_viewer;
    Handle(V3d_View) m_view;
    Handle(AIS_InteractiveContext) m_ctx;
    QSize m_size;
    void *m_hwnd = nullptr;
    OcctView *m_item = nullptr;
    bool m_failed = false, m_pick = false;
    QPoint m_pickPx; qint64 m_pickStampNs = 0;
};

QQuickFramebufferObject::Renderer *OcctView::createRenderer() const { return new OcctRenderer; }

void OcctView::pick(qreal x, qreal y)
{
    const qreal dpr = window() ? window()->effectiveDevicePixelRatio() : 1.0;
    m_pickPx = QPoint(int(x * dpr), int(y * dpr));
    m_pickStampNs = s1::clock().nsecsElapsed();
    m_pickPending = true;
    update();
}
