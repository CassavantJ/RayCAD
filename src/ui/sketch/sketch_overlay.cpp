#include "sketch_overlay.h"

#include "../theme_object.h"
#include "sketch_mode.h"

#include <QtCore/QCoreApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtQuick/QSGFlatColorMaterial>
#include <QtQuick/QSGGeometryNode>

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

namespace ray::ui {

namespace {

using Style = SketchOverlayScene::Style;
using Vertex = QSGGeometry::Point2D;

// Line widths and sizes, derived from the theme's metrics (no sizes of their own).
struct Metrics {
    double curve = 2; // half of it either side of the curve
    double thin = 1.5;
    double point = 6; // side of a point's square
    double dash = 6, gap = 4;
};

Metrics metricsOf(const ThemeObject* theme)
{
    Metrics m;
    if (theme != nullptr) {
        m.thin = theme->lineThinPx();
        m.curve = theme->lineThinPx() * 4.0 / 3.0;
        m.point = theme->spacingS() * 0.75;
        m.dash = theme->spacingS() * 0.75;
        m.gap = theme->spacingXs();
    }
    return m;
}

QColor colourOf(const ThemeObject* theme, Style s)
{
    if (theme == nullptr)
        return QColor(Qt::transparent); // no theme yet: nothing to draw with
    switch (s) {
    case Style::Geometry:
        return theme->sketchGeometry();
    case Style::Constrained:
        return theme->sketchConstrained();
    case Style::Conflict:
        return theme->sketchConflict();
    case Style::Construction:
    case Style::Guide:
        return theme->sketchConstruction();
    case Style::Selected:
        return theme->selection();
    case Style::Hovered:
    case Style::Highlight:
        return theme->hover();
    case Style::Preview:
        return theme->accent();
    }
    return theme->sketchGeometry();
}

void quad(std::vector<Vertex>& out, QPointF a, QPointF b, double halfWidth)
{
    const double dx = b.x() - a.x(), dy = b.y() - a.y();
    const double len = std::hypot(dx, dy);
    if (!(len > 1e-6))
        return;
    const double nx = -dy / len * halfWidth, ny = dx / len * halfWidth;
    // Extend each end by half the width, so joints of a polyline overlap without gaps.
    const double ex = dx / len * halfWidth * 0.5, ey = dy / len * halfWidth * 0.5;
    const auto v = [](double x, double y) {
        Vertex p;
        p.set(static_cast<float>(x), static_cast<float>(y));
        return p;
    };
    const double ax = a.x() - ex, ay = a.y() - ey, bx = b.x() + ex, by = b.y() + ey;
    out.push_back(v(ax + nx, ay + ny));
    out.push_back(v(ax - nx, ay - ny));
    out.push_back(v(bx + nx, by + ny));
    out.push_back(v(bx + nx, by + ny));
    out.push_back(v(ax - nx, ay - ny));
    out.push_back(v(bx - nx, by - ny));
}

void strip(std::vector<Vertex>& out, const std::vector<QPointF>& pts, double halfWidth, bool dashed, double dash,
           double gap)
{
    if (!dashed) {
        for (std::size_t i = 0; i + 1 < pts.size(); ++i)
            quad(out, pts[i], pts[i + 1], halfWidth);
        return;
    }
    // Dashes measured along the whole polyline.
    double phase = 0; // distance into the current dash + gap period
    const double period = dash + gap;
    for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
        const QPointF a = pts[i], b = pts[i + 1];
        const double len = std::hypot(b.x() - a.x(), b.y() - a.y());
        double t = 0;
        while (t < len) {
            const double inPeriod = std::fmod(phase, period);
            const bool on = inPeriod < dash;
            const double step = std::min(len - t, on ? dash - inPeriod : period - inPeriod);
            if (on && step > 0) {
                const QPointF p = a + (b - a) * (t / len);
                const QPointF q = a + (b - a) * ((t + step) / len);
                quad(out, p, q, halfWidth);
            }
            t += step;
            phase += step;
            if (step <= 0)
                break;
        }
    }
}

void square(std::vector<Vertex>& out, QPointF c, double side)
{
    const float h = static_cast<float>(side / 2);
    const float x = static_cast<float>(c.x()), y = static_cast<float>(c.y());
    const auto v = [](float px, float py) {
        Vertex p;
        p.set(px, py);
        return p;
    };
    out.push_back(v(x - h, y - h));
    out.push_back(v(x + h, y - h));
    out.push_back(v(x - h, y + h));
    out.push_back(v(x - h, y + h));
    out.push_back(v(x + h, y - h));
    out.push_back(v(x + h, y + h));
}

} // namespace

SketchOverlay::SketchOverlay(QQuickItem* parent) : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    setAcceptedMouseButtons(Qt::LeftButton);
    setAcceptHoverEvents(true);
    setActiveFocusOnTab(false);
}

SketchOverlay::~SketchOverlay() = default;

QObject* SketchOverlay::tool() const
{
    return tool_.data();
}

void SketchOverlay::setTool(QObject* tool)
{
    auto* t = qobject_cast<SketchModel*>(tool);
    if (t == tool_)
        return;
    disconnect(toolConnection_);
    tool_ = t;
    if (tool_)
        toolConnection_ = connect(tool_, &SketchModel::overlayChanged, this, &QQuickItem::update);
    update();
    emit toolChanged();
}

QObject* SketchOverlay::theme() const
{
    return theme_.data();
}

void SketchOverlay::setTheme(QObject* theme)
{
    auto* t = qobject_cast<ThemeObject*>(theme);
    if (t == theme_)
        return;
    disconnect(themeConnection_);
    theme_ = t;
    if (theme_)
        themeConnection_ = connect(theme_, &ThemeObject::themeChanged, this, &QQuickItem::update);
    update();
    emit themeChanged();
}

QSGNode* SketchOverlay::updatePaintNode(QSGNode* old, UpdatePaintNodeData*)
{
    ++paints_;
    QSGNode* root = old != nullptr ? old : new QSGNode;
    while (QSGNode* child = root->firstChild()) {
        root->removeChildNode(child);
        delete child;
    }
    if (!tool_ || !tool_->active())
        return root;
    const SketchOverlayScene& scene = tool_->overlay();
    const Metrics m = metricsOf(theme_);
    // Back to front: guides, construction, geometry, highlights, preview, then the points.
    const Style order[] = {Style::Guide,     Style::Construction, Style::Geometry, Style::Constrained, Style::Conflict,
                           Style::Highlight, Style::Hovered,      Style::Selected, Style::Preview};
    std::map<int, std::vector<Vertex>> byStyle;
    for (const SketchOverlayScene::Line& line : scene.lines) {
        const bool thinLine = line.style == Style::Guide || line.style == Style::Construction;
        const double width =
            thinLine ? m.thin
            : (line.style == Style::Hovered || line.style == Style::Selected || line.style == Style::Highlight)
                ? m.curve * 1.5
                : m.curve;
        strip(byStyle[static_cast<int>(line.style)], line.points, width / 2, line.dashed, m.dash, m.gap);
    }
    std::map<int, std::vector<Vertex>> points;
    for (const SketchOverlayScene::Point& p : scene.points) {
        const double size = p.style == Style::Hovered || p.style == Style::Selected ? m.point * 1.4 : m.point;
        square(points[static_cast<int>(p.style)], p.at, size);
    }
    const auto addNode = [&](Style s, const std::vector<Vertex>& v) {
        if (v.empty())
            return;
        auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), static_cast<int>(v.size()));
        geometry->setDrawingMode(QSGGeometry::DrawTriangles);
        std::copy(v.begin(), v.end(), geometry->vertexDataAsPoint2D());
        auto* material = new QSGFlatColorMaterial;
        material->setColor(colourOf(theme_, s));
        auto* node = new QSGGeometryNode;
        node->setGeometry(geometry);
        node->setFlag(QSGNode::OwnsGeometry);
        node->setMaterial(material);
        node->setFlag(QSGNode::OwnsMaterial);
        root->appendChildNode(node);
    };
    for (const Style s : order)
        addNode(s, byStyle[static_cast<int>(s)]);
    for (const Style s : order)
        addNode(s, points[static_cast<int>(s)]);
    return root;
}

void SketchOverlay::mousePressEvent(QMouseEvent* event)
{
    if (!tool_ || !tool_->active() || event->button() != Qt::LeftButton || (event->modifiers() & Qt::AltModifier)) {
        event->ignore();
        return;
    }
    forceActiveFocus(Qt::MouseFocusReason);
    pressed_ = true;
    tool_->pointerPress(event->position(), event->modifiers());
    event->accept();
}

void SketchOverlay::mouseMoveEvent(QMouseEvent* event)
{
    if (tool_)
        tool_->pointerMove(event->position(), event->modifiers());
    event->accept();
}

void SketchOverlay::mouseReleaseEvent(QMouseEvent* event)
{
    pressed_ = false;
    if (tool_)
        tool_->pointerRelease(event->position(), event->modifiers());
    event->accept();
}

void SketchOverlay::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (!tool_ || !tool_->active() || event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    tool_->pointerDoubleClick(event->position(), event->modifiers());
    event->accept();
}

void SketchOverlay::hoverMoveEvent(QHoverEvent* event)
{
    if (tool_ && tool_->active() && !pressed_)
        tool_->pointerMove(event->position(), event->modifiers());
    event->accept();
}

void SketchOverlay::hoverLeaveEvent(QHoverEvent* event)
{
    if (tool_)
        tool_->pointerLeave();
    event->accept();
}

void SketchOverlay::keyPressEvent(QKeyEvent* event)
{
    if (tool_ && tool_->keyPress(event)) {
        event->accept();
        return;
    }
    // The view's own keys (fit, standard views) still work while the sketch has the focus.
    if (tool_) {
        if (auto* view = qobject_cast<QQuickItem*>(tool_->view())) {
            QKeyEvent copy(event->type(), event->key(), event->modifiers(), event->text(), event->isAutoRepeat(),
                           static_cast<ushort>(event->count()));
            QCoreApplication::sendEvent(view, &copy);
            event->setAccepted(copy.isAccepted());
            return;
        }
    }
    event->ignore();
}

bool SketchOverlay::event(QEvent* event)
{
    if (event->type() == QEvent::ShortcutOverride && tool_ && tool_->wantsKey(static_cast<QKeyEvent*>(event))) {
        event->accept();
        return true;
    }
    return QQuickItem::event(event);
}

} // namespace ray::ui
