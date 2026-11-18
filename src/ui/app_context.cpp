#include "app_context.h"

#include <ray/api.h>

#include <QtCore/QVariant>

namespace ray::ui {
namespace {
constexpr const char* kEngineKey = "_ray_ui_app_context";
}

AppContext::AppContext(QObject* parent) : QObject(parent) {}

AppContext::~AppContext() = default;

AppContext* AppContext::create(QQmlEngine* qml, QJSEngine*)
{
    if (qml != nullptr) {
        if (auto* context = qobject_cast<AppContext*>(qml->property(kEngineKey).value<QObject*>())) {
            QJSEngine::setObjectOwnership(context, QJSEngine::CppOwnership);
            return context;
        }
    }
    return new AppContext(nullptr); // owned by the engine
}

void AppContext::provideFor(QQmlEngine* engine, AppContext* context)
{
    engine->setProperty(kEngineKey, QVariant::fromValue<QObject*>(context));
}

void AppContext::setApp(api::IApp* app)
{
    if (app == app_) {
        return;
    }
    app_ = app;
    emit sessionChanged();
}

void AppContext::setGlass(bool on)
{
    if (on == glass_) {
        return;
    }
    glass_ = on;
    emit glassChanged();
}

void AppContext::setBackdropSource(QObject* source)
{
    if (source == backdropSource_) {
        return;
    }
    backdropSource_ = source;
    emit backdropSourceChanged();
}

QStringList AppContext::workspaceNames() const
{
    return {tr("Design"), tr("CAM"), tr("Simulate")};
}

void AppContext::setWorkspace(int index)
{
    if (index < 0 || index >= workspaceNames().size() || index == workspace_) {
        return;
    }
    workspace_ = index;
    emit workspaceChanged();
}

void AppContext::setStatusText(const QString& text)
{
    if (text == statusText_) {
        return;
    }
    statusText_ = text;
    emit statusTextChanged();
}

void AppContext::setJob(const QString& title, qreal progress)
{
    jobActive_ = true;
    jobTitle_ = title;
    jobProgress_ = progress;
    emit jobChanged();
}

void AppContext::clearJob()
{
    if (!jobActive_) {
        return;
    }
    jobActive_ = false;
    jobTitle_.clear();
    jobProgress_ = -1;
    emit jobChanged();
}

QString AppContext::version() const
{
    return QStringLiteral(RAY_VERSION_STRING);
}

bool AppContext::invoke(const QString& actionId, const QString& argsJson)
{
    if (app_ == nullptr) {
        return false;
    }
    const QByteArray id = actionId.toUtf8();
    const QByteArray args = argsJson.toUtf8();
    auto job = app_->actions().invoke(std::string_view(id.constData(), static_cast<std::size_t>(id.size())),
                                      std::string_view(args.constData(), static_cast<std::size_t>(args.size())),
                                      api::Caller::User);
    return job.valid();
}

void AppContext::cancelJob()
{
    emit jobCancelRequested();
}

} // namespace ray::ui
