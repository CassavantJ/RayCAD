// Small helpers shared by the model environment's C++ (task T-098): vectors, sketch-plane frames, strings and JSON.
// Display and input only (src/ui is not deterministic code, PLAN.md 8).
#pragma once

#include <ray/api.h>

#include <QtCore/QByteArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QString>

#include <cmath>
#include <string>
#include <string_view>

namespace ray::ui::modelui {

inline Vec3 add(Vec3 a, Vec3 b)
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
inline Vec3 sub(Vec3 a, Vec3 b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
inline Vec3 mul(Vec3 a, double s)
{
    return {a.x * s, a.y * s, a.z * s};
}
inline double dot(Vec3 a, Vec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline Vec3 cross(Vec3 a, Vec3 b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline double length(Vec3 a)
{
    return std::sqrt(dot(a, a));
}
inline Vec3 unit(Vec3 a)
{
    const double l = length(a);
    return l > 1e-12 ? mul(a, 1.0 / l) : Vec3{0, 0, 0};
}
inline Vec3 yDirOf(const kernel::Plane& p)
{
    return cross(p.normal, p.xDir);
}
inline Vec3 worldOf(const kernel::Plane& p, Vec2 uv)
{
    return add(p.origin, add(mul(p.xDir, uv.x), mul(yDirOf(p), uv.y)));
}
inline Vec2 planeOf(const kernel::Plane& p, Vec3 w)
{
    const Vec3 d = sub(w, p.origin);
    return {dot(d, p.xDir), dot(d, yDirOf(p))};
}
// `v` turned by `angle` (radians) about the unit axis `axis` (Rodrigues).
inline Vec3 rotate(Vec3 v, Vec3 axis, double angle)
{
    const double c = std::cos(angle), s = std::sin(angle);
    return add(add(mul(v, c), mul(cross(axis, v), s)), mul(axis, dot(axis, v) * (1 - c)));
}

inline QString qs(const std::string& s)
{
    return QString::fromStdString(s);
}
inline std::string ss(const QString& s)
{
    return s.toStdString();
}
inline std::string_view sv(const QByteArray& b)
{
    return {b.constData(), static_cast<std::size_t>(b.size())};
}
inline QString idText(const Uuid& id)
{
    return QString::fromStdString(id.toString());
}
inline std::string jsonText(const QJsonObject& o)
{
    return QJsonDocument(o).toJson(QJsonDocument::Compact).toStdString();
}
inline QJsonObject jsonObject(std::string_view text)
{
    const QJsonDocument d = QJsonDocument::fromJson(QByteArray(text.data(), static_cast<qsizetype>(text.size())));
    return d.isObject() ? d.object() : QJsonObject();
}

// The point of polygon `poly` (closed implicitly) test, even-odd.
template <class P>
bool insidePolygon(const P& poly, Vec2 q)
{
    bool in = false;
    const std::size_t n = poly.size();
    if (n < 3)
        return false;
    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
        const Vec2 a = poly[i], b = poly[j];
        if ((a.y > q.y) != (b.y > q.y) && q.x < (b.x - a.x) * (q.y - a.y) / (b.y - a.y) + a.x)
            in = !in;
    }
    return in;
}

} // namespace ray::ui::modelui
