// Spike S1, variant B: OCCT V3d_View rendering into a QQuickFramebufferObject (Qt Quick on OpenGL).
#pragma once
#include <QQuickFramebufferObject>

class OcctView : public QQuickFramebufferObject
{
    Q_OBJECT
public:
    OcctView() { setMirrorVertically(true); }
    Renderer *createRenderer() const override;
    Q_INVOKABLE void pick(qreal x, qreal y);   // item coordinates; answer arrives through picked()
signals:
    void picked(int faceId, double latencyMs);
private:
    friend class OcctRenderer;
    bool m_pickPending = false;
    QPoint m_pickPx; qint64 m_pickStampNs = 0;
};
