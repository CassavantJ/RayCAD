#include "dev_open.h"

#include "app_context.h"

#include <QtCore/QFileInfo>
#include <QtCore/QVariant>

#include <utility>

namespace ray::ui {

namespace {
constexpr const char* kEngineKey = "_ray_ui_dev_open";
using ModelPtr = std::shared_ptr<api::viewer::ImportedModel>;
} // namespace

DevOpen::DevOpen(SceneController* scene, AppContext* context, QObject* parent)
    : QObject(parent), scene_(scene), context_(context)
{
    if (context_)
        connect(context_, &AppContext::jobCancelRequested, this, &DevOpen::cancel);
}

DevOpen::~DevOpen()
{
    job_.cancel();
}

DevOpen* DevOpen::create(QQmlEngine* qml, QJSEngine*)
{
    if (qml != nullptr) {
        if (auto* opener = qobject_cast<DevOpen*>(qml->property(kEngineKey).value<QObject*>())) {
            QJSEngine::setObjectOwnership(opener, QJSEngine::CppOwnership);
            return opener;
        }
    }
    return new DevOpen(nullptr, nullptr, nullptr);
}

void DevOpen::provideFor(QQmlEngine* engine, DevOpen* opener)
{
    engine->setProperty(kEngineKey, QVariant::fromValue<QObject*>(opener));
}

QStringList DevOpen::nameFilters() const
{
    return {tr("STEP and IGES files (*.step *.stp *.igs *.iges)"), tr("All files (*)")};
}

QString DevOpen::localPath(const QString& pathOrUrl)
{
    const QUrl url(pathOrUrl);
    if (url.isLocalFile())
        return url.toLocalFile();
    return pathOrUrl;
}

bool DevOpen::canOpen(const QUrl& url) const
{
    if (!url.isLocalFile())
        return false;
    const QString path = url.toLocalFile();
    return api::viewer::isImportable(path.toStdString()) && QFileInfo(path).isFile();
}

bool DevOpen::open(const QString& pathOrUrl)
{
    const QString path = localPath(pathOrUrl);
    const QFileInfo file(path);
    const QString name = file.fileName();
    auto fail = [&](const QString& message) {
        if (context_)
            context_->showMessage(message, true);
        emit failed(message);
        return false;
    };
    if (!api::viewer::isImportable(path.toStdString()))
        return fail(tr("%1 is not a STEP or IGES file.").arg(name.isEmpty() ? path : name));
    if (!file.isFile() || !file.isReadable())
        return fail(tr("%1 cannot be read.").arg(name));
    IJobSystem* jobs = scene_ ? scene_->jobs() : nullptr;
    if (jobs == nullptr)
        return fail(tr("Files cannot be opened in this window."));

    if (busy())
        cancel();
    scene_->cancelJobs(true); // a refinement of the previous model is pointless now
    if (context_) {
        context_->dismissMessage();
        context_->setJob(tr("Opening %1").arg(name), -1);
    }
    fileName_ = name;
    const std::uint64_t serial = ++serial_;
    const std::string utf8 = file.absoluteFilePath().toStdString();
    job_ = jobs->submit<ModelPtr>(JobInfo{"Open " + name.toStdString(), JobPriority::Interactive, true},
                                  [utf8](JobContext& ctx) -> Result<ModelPtr> {
                                      Result<api::viewer::ImportedModel> model = api::viewer::importModel(utf8, ctx);
                                      if (!model)
                                          return tl::unexpected(model.error());
                                      return std::make_shared<api::viewer::ImportedModel>(std::move(*model));
                                  });
    QPointer<DevOpen> self(this);
    job_.onProgress([self, serial, name](double fraction, const std::string& stage) {
        if (!self || serial != self->serial_ || !self->busy() || !self->context_)
            return;
        const QString title =
            stage.empty() ? tr("Opening %1").arg(name) : tr("Opening %1: %2").arg(name, QString::fromStdString(stage));
        self->context_->setJob(title, fraction);
    });
    job_.then([self, serial, name](Result<ModelPtr> model) {
        if (!self || serial != self->serial_)
            return; // abandoned (Esc) or superseded
        self->job_ = {};
        if (self->context_)
            self->context_->clearJob();
        emit self->busyChanged();
        if (!model) {
            if (model.error().code == ErrorCode::Cancelled) {
                if (self->context_)
                    self->context_->setStatusText(tr("Opening cancelled"));
                return;
            }
            const QString message =
                tr("Could not open %1: %2").arg(name, QString::fromStdString(model.error().message));
            if (self->context_)
                self->context_->showMessage(message, true);
            emit self->failed(message);
            return;
        }
        const int count = static_cast<int>((*model)->bodies.size());
        if (count == 0) {
            const QString message = tr("%1 contains no bodies that can be shown.").arg(name);
            if (self->context_)
                self->context_->showMessage(message, true);
            emit self->failed(message);
            return;
        }
        if (self->scene_)
            self->scene_->setBodies(std::move((*model)->bodies), name);
        if (self->context_) {
            QString status = count == 1 ? tr("%1: 1 body").arg(name) : tr("%1: %2 bodies").arg(name).arg(count);
            const int warnings = static_cast<int>((*model)->warnings.size());
            if (warnings > 0)
                status += warnings == 1 ? tr(" (1 import warning)") : tr(" (%1 import warnings)").arg(warnings);
            self->context_->setStatusText(status);
        }
        emit self->opened(name, count);
    });
    emit busyChanged();
    return true;
}

void DevOpen::cancel()
{
    if (!busy())
        return;
    job_.cancel();
    job_ = {};
    ++serial_; // the result that arrives later is dropped: the UI returns at once (PLAN.md 7)
    if (context_) {
        context_->clearJob();
        context_->setStatusText(tr("Opening cancelled"));
    }
    emit busyChanged();
}

} // namespace ray::ui
