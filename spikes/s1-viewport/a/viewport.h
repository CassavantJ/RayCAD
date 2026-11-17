// Spike S1, variant A: own renderer on Qt RHI inside a QQuickRhiItem.
#pragma once
#include <QQuickRhiItem>
#include <QSGTextureProvider>
#include <atomic>

class BackdropProvider : public QSGTextureProvider
{
public:
    QSGTexture *texture() const override { return tex; }
    void set(QSGTexture *t) { tex = t; emit textureChanged(); }
    QSGTexture *tex = nullptr;
};

class ViewportItem : public QQuickRhiItem
{
    Q_OBJECT
    Q_PROPERTY(bool backdropEnabled READ backdropEnabled WRITE setBackdropEnabled NOTIFY backdropEnabledChanged)
public:
    QQuickRhiItemRenderer *createRenderer() override;
    bool backdropEnabled() const { return m_backdrop; }
    void setBackdropEnabled(bool on) { if (on != m_backdrop) { m_backdrop = on; emit backdropEnabledChanged(); update(); } }
    Q_INVOKABLE void pick(qreal x, qreal y);          // item coordinates; answer arrives through picked()
    BackdropProvider *backdropProvider();             // render thread only
signals:
    void backdropEnabledChanged();
    void picked(int faceId, double latencyMs);
private:
    friend class ViewportRenderer;
    bool m_backdrop = true;
    bool m_pickPending = false;
    QPoint m_pickPx; qint64 m_pickStampNs = 0;
    BackdropProvider *m_provider = nullptr;
};

// Invisible item that exposes the viewport's shared blurred backdrop texture to ShaderEffect panels.
class BackdropItem : public QQuickItem
{
    Q_OBJECT
    Q_PROPERTY(ViewportItem *viewport MEMBER m_viewport NOTIFY viewportChanged)
public:
    bool isTextureProvider() const override { return true; }
    QSGTextureProvider *textureProvider() const override { return m_viewport ? m_viewport->backdropProvider() : nullptr; }
signals:
    void viewportChanged();
private:
    ViewportItem *m_viewport = nullptr;
};
