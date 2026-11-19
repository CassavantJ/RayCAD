#include "dimensions_overlay.h"

#include "../theme_object.h"
#include "constraints_model.h"

#include <QtQuick/QSGFlatColorMaterial>
#include <QtQuick/QSGGeometryNode>

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

namespace ray::ui {

namespace {

using Style = DimensionScene::Style;
using Vertex = QSGGeometry::Point2D;

QColor colourOf(const ThemeObject* theme, Style s)
{
    if (theme == nullptr)
        return QColor(Qt::transparent);
    switch (s) {
    case Style::Normal:
        return theme->accentText();
    case Style::Driven:
        return theme->sketchConstruction();
    case Style::Bad:
        return theme->sketchConflict();
    case Style::Hovered:
        return theme->hover();
    case Style::Selected:
        return theme->selection();
    case Style::Preview:
        return theme->accent();
    }
    return theme->accentText();
}

Vertex vertex(double x, double y)
{
    Vertex p;
    p.set(static_cast<float>(x), static_cast<float>(y));
    return p;
}

void segment(std::vector<Vertex>& out, QPointF a, QPointF b, double halfWidth)
{
    const double dx = b.x() - a.x(), dy = b.y() - a.y();
    const double len = std::hypot(dx, dy);
    if (!(len > 1e-6))
        return;
    const double nx = -dy / len * halfWidth, ny = dx / len * halfWidth;
    out.push_back(vertex(a.x() + nx, a.y() + ny));
    out.push_back(vertex(a.x() - nx, a.y() - ny));
    out.push_back(vertex(b.x() + nx, b.y() + ny));
    out.push_back(vertex(b.x() + nx, b.y() + ny));
    out.push_back(vertex(a.x() - nx, a.y() - ny));
    out.push_back(vertex(b.x() - nx, b.y() - ny));
}

void arrowhead(std::vector<Vertex>& out, QPointF tip, QPointF dir, double length, double halfWidth)
{
    const QPointF back = tip - dir * length;
    const QPointF n(-dir.y() * halfWidth, dir.x() * halfWidth);
    out.push_back(vertex(tip.x(), tip.y()));
    out.push_back(vertex(back.x() + n.x(), back.y() + n.y()));
    out.push_back(vertex(back.x() - n.x(), back.y() - n.y()));
}

} // namespace

SketchDimensionOverlay::SketchDimensionOverlay(QQuickItem* parent) : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    setAcceptedMouseButtons(Qt::NoButton);
}

SketchDimensionOverlay::~SketchDimensionOverlay() = default;

QObject* SketchDimensionOverlay::constraints() const
{
    return model_.data();
}

void SketchDimensionOverlay::setConstraints(QObject* constraints)
{
    auto* m = qobject_cast<SketchConstraintsModel*>(constraints);
    if (m == model_)
        return;
    disconnect(modelConnection_);
    model_ = m;
    if (model_)
        modelConnection_ = connect(model_, &SketchConstraintsModel::sceneChanged, this, &QQuickItem::update);
    update();
    emit constraintsChanged();
}

QObject* SketchDimensionOverlay::theme() const
{
    return theme_.data();
}

void SketchDimensionOverlay::setTheme(QObject* theme)
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

QSGNode* SketchDimensionOverlay::updatePaintNode(QSGNode* old, UpdatePaintNodeData*)
{
    ++paints_;
    QSGNode* root = old != nullptr ? old : new QSGNode;
    while (QSGNode* child = root->firstChild()) {
        root->removeChildNode(child);
        delete child;
    }
    if (!model_)
        return root;
    const DimensionScene& scene = model_->scene();
    const double thin = theme_ ? theme_->lineThinPx() : 1.0;
    const double arrowLength = theme_ ? theme_->spacingS() * 1.25 : 10.0;
    const double arrowHalf = arrowLength * 0.3;
    std::map<int, std::vector<Vertex>> byStyle;
    for (const DimensionScene::Line& line : scene.lines) {
        const double w = line.style == Style::Hovered || line.style == Style::Selected ? thin * 2 : thin;
        for (std::size_t i = 0; i + 1 < line.points.size(); ++i)
            segment(byStyle[static_cast<int>(line.style)], line.points[i], line.points[i + 1], w / 2);
    }
    for (const DimensionScene::Arrow& a : scene.arrows)
        arrowhead(byStyle[static_cast<int>(a.style)], a.tip, a.dir, arrowLength, arrowHalf);
    // Back to front: plain ones first, the one being placed last.
    for (const Style s : {Style::Driven, Style::Normal, Style::Bad, Style::Hovered, Style::Selected, Style::Preview}) {
        const std::vector<Vertex>& v = byStyle[static_cast<int>(s)];
        if (v.empty())
            continue;
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
    }
    return root;
}

} // namespace ray::ui
