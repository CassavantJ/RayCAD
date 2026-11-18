// Helpers shared by the RHI code (task T-014).
#include "rhi_util.h"

#include <QtCore/QFile>
#include <QtCore/QString>

namespace ray::render::rhi {

QShader loadShader(const char* name)
{
    QFile file(QStringLiteral(":/ray/render/shaders/%1.qsb").arg(QLatin1String(name)));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QShader::fromSerialized(file.readAll());
}

void toFloat4(const Color& c, float out[4])
{
    out[0] = c.r;
    out[1] = c.g;
    out[2] = c.b;
    out[3] = c.a;
}

QMatrix4x4 toMatrix(const Xform& xf)
{
    const double* m = xf.m;
    return QMatrix4x4(static_cast<float>(m[0]), static_cast<float>(m[1]), static_cast<float>(m[2]),
                      static_cast<float>(m[3]), static_cast<float>(m[4]), static_cast<float>(m[5]),
                      static_cast<float>(m[6]), static_cast<float>(m[7]), static_cast<float>(m[8]),
                      static_cast<float>(m[9]), static_cast<float>(m[10]), static_cast<float>(m[11]), 0.0f, 0.0f, 0.0f,
                      1.0f);
}

} // namespace ray::render::rhi
