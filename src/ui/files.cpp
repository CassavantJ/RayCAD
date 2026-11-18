#include "files.h"

#include "app_context.h"
#include "session.h"
#include "session_recovery.h"

#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QVariant>

#include <algorithm>
#include <utility>

namespace ray::ui {

namespace {

constexpr const char* kEngineKey = "_ray_ui_files";

enum class Kind { Unknown, Document, Import };

Kind kindOf(const QString& path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QLatin1String("raycad"))
        return Kind::Document;
    if (suffix == QLatin1String("step") || suffix == QLatin1String("stp") || suffix == QLatin1String("p21") ||
        suffix == QLatin1String("iges") || suffix == QLatin1String("igs"))
        return Kind::Import;
    return Kind::Unknown;
}

QString fromUtf8(const std::string& s)
{
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

std::string toUtf8(const QString& s)
{
    const QByteArray bytes = s.toUtf8();
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

QString errorText(const Error& e)
{
    return fromUtf8(e.message);
}

QString exportAction(const QString& kind)
{
    if (kind == QLatin1String("iges"))
        return QStringLiteral("file.exportIges");
    if (kind == QLatin1String("stl"))
        return QStringLiteral("file.exportStl");
    return QStringLiteral("file.exportStep");
}

QString exportSuffix(const QString& kind)
{
    if (kind == QLatin1String("iges"))
        return QStringLiteral(".igs");
    if (kind == QLatin1String("stl"))
        return QStringLiteral(".stl");
    return QStringLiteral(".step");
}

// The new job of the app that a call just started (IApp registers it before returning), or 0.
qint64 newJobId(const std::vector<qint64>& before, const std::vector<qint64>& after)
{
    for (qint64 id : after) {
        if (std::find(before.begin(), before.end(), id) == before.end())
            return id;
    }
    return 0;
}

} // namespace

Files::Files(Session* session, AppContext* context, QObject* parent)
    : QObject(parent), session_(session), context_(context)
{
    if (session_) {
        connect(session_, &Session::sessionChanged, this, &Files::availableChanged);
        connect(session_, &Session::jobAbandoned, this, &Files::onJobAbandoned);
    }
}

Files::~Files()
{
    openJob_.cancel();
}

Files* Files::create(QQmlEngine* qml, QJSEngine*)
{
    if (qml != nullptr) {
        if (auto* files = qobject_cast<Files*>(qml->property(kEngineKey).value<QObject*>())) {
            QJSEngine::setObjectOwnership(files, QJSEngine::CppOwnership);
            return files;
        }
    }
    return new Files(nullptr, nullptr, nullptr);
}

void Files::provideFor(QQmlEngine* engine, Files* files)
{
    engine->setProperty(kEngineKey, QVariant::fromValue<QObject*>(files));
}

api::IApp* Files::app() const
{
    return session_ ? session_->app() : nullptr;
}

bool Files::available() const
{
    return app() != nullptr;
}

QStringList Files::openFilters() const
{
    return {tr("RayCAD, STEP and IGES files (*.raycad *.step *.stp *.p21 *.iges *.igs)"),
            tr("RayCAD documents (*.raycad)"), tr("STEP files (*.step *.stp *.p21)"), tr("IGES files (*.iges *.igs)"),
            tr("All files (*)")};
}

QStringList Files::importFilters() const
{
    return {tr("STEP and IGES files (*.step *.stp *.p21 *.iges *.igs)"), tr("All files (*)")};
}

QStringList Files::documentFilters() const
{
    return {tr("RayCAD documents (*.raycad)")};
}

QStringList Files::exportFilters(const QString& kind) const
{
    if (kind == QLatin1String("iges"))
        return {tr("IGES files (*.igs *.iges)")};
    if (kind == QLatin1String("stl"))
        return {tr("STL files (*.stl)")};
    return {tr("STEP files (*.step *.stp)")};
}

void Files::setUseNativeDialogs(bool on)
{
    if (on == nativeDialogs_)
        return;
    nativeDialogs_ = on;
    emit useNativeDialogsChanged();
}

QString Files::promptDocument() const
{
    return prompt_ ? prompt_->title : QString();
}

QString Files::promptReason() const
{
    if (!prompt_)
        return {};
    switch (prompt_->reason) {
    case Reason::Close:
        return QStringLiteral("close");
    case Reason::Quit:
        return QStringLiteral("quit");
    case Reason::Open:
        return QStringLiteral("open");
    case Reason::New:
        return QStringLiteral("new");
    }
    return {};
}

QString Files::localPath(const QString& pathOrUrl)
{
    // "C:/x.step" parses as a URL with the scheme "c": only file:// URLs are converted.
    if (pathOrUrl.startsWith(QLatin1String("file:"), Qt::CaseInsensitive)) {
        const QUrl url(pathOrUrl);
        if (url.isLocalFile())
            return url.toLocalFile();
    }
    return pathOrUrl;
}

void Files::showError(const QString& message)
{
    if (context_)
        context_->showMessage(message, true);
}

void Files::setStatus(const QString& text)
{
    if (context_)
        context_->setStatusText(text);
}

bool Files::hasUnsavedChanges() const
{
    api::IApp* a = app();
    if (a == nullptr)
        return false;
    for (api::OpenDocument* od : a->documents()) {
        if (od->modified || (od->document && od->document->dirty()))
            return true;
    }
    return false;
}

// ---------- the action hook ----------
bool Files::handleAction(const QString& actionId, const QString& argsJson)
{
    if (app() == nullptr)
        return false;
    const QJsonObject args = QJsonDocument::fromJson(argsJson.toUtf8()).object();
    const QString path = args.value(QStringLiteral("path")).toString();
    if (actionId == QLatin1String("file.new")) {
        newPart();
    } else if (actionId == QLatin1String("file.open")) {
        if (path.isEmpty())
            requestOpen();
        else
            open(path);
    } else if (actionId == QLatin1String("file.save")) {
        save();
    } else if (actionId == QLatin1String("file.saveAs")) {
        if (path.isEmpty())
            requestSaveAs();
        else
            saveAs(path, args.value(QStringLiteral("overwrite")).toBool());
    } else if (actionId == QLatin1String("file.close")) {
        close();
    } else if (actionId == QLatin1String("file.import")) {
        if (path.isEmpty())
            requestImport();
        else
            importFile(path);
    } else if (actionId.startsWith(QLatin1String("file.export"))) {
        const QString kind = actionId == QLatin1String("file.exportIges")  ? QStringLiteral("iges")
                             : actionId == QLatin1String("file.exportStl") ? QStringLiteral("stl")
                                                                           : QStringLiteral("step");
        if (path.isEmpty())
            requestExport(kind);
        else
            exportTo(kind, path);
    } else if (actionId == QLatin1String("edit.undo") || actionId == QLatin1String("edit.redo")) {
        // Through the registry as usual, but an empty stack is a status note, not an error banner.
        const bool redo = actionId == QLatin1String("edit.redo");
        if (!session_ || !session_->hasDocument() || (redo ? !session_->canRedo() : !session_->canUndo())) {
            setStatus(redo ? tr("Nothing to redo") : tr("Nothing to undo"));
            return true;
        }
        return false;
    } else {
        return false;
    }
    return true;
}

// ---------- the unsaved-changes prompt ----------
void Files::guard(Reason reason, std::function<void()> then)
{
    api::OpenDocument* od = session_ ? session_->activeDocument() : nullptr;
    if (od == nullptr || !od->document || !(od->modified || od->document->dirty())) {
        then();
        return;
    }
    prompt_ = Prompt{reason, od->id, fromUtf8(od->title), std::move(then)};
    emit promptChanged();
}

void Files::answerPrompt(const QString& choice)
{
    if (!prompt_)
        return;
    Prompt p = std::move(*prompt_);
    prompt_.reset();
    emit promptChanged();
    if (choice == QLatin1String("discard")) {
        p.then();
        return;
    }
    if (choice != QLatin1String("save"))
        return; // cancel: nothing happens
    api::IApp* a = app();
    api::OpenDocument* od = a != nullptr ? a->findDocument(p.documentId) : nullptr;
    if (od == nullptr) {
        p.then();
        return;
    }
    if (od->path.empty() || od->readOnly) {
        // Save As first; the operation continues once that save succeeded.
        a->setActive(od);
        afterSaveAs_ = std::move(p.then);
        requestSaveAs();
        return;
    }
    startSave(*od, std::nullopt, false, std::move(p.then));
}

// ---------- open ----------
bool Files::canOpen(const QUrl& url) const
{
    return url.isLocalFile() && kindOf(url.toLocalFile()) != Kind::Unknown && available();
}

bool Files::canOpenAny(const QList<QUrl>& urls) const
{
    return std::any_of(urls.begin(), urls.end(), [this](const QUrl& u) { return canOpen(u); });
}

void Files::requestOpen()
{
    if (available())
        emit openDialogRequested();
}

bool Files::open(const QString& pathOrUrl)
{
    return openPaths({localPath(pathOrUrl)});
}

bool Files::openUrls(const QList<QUrl>& urls)
{
    QStringList paths;
    for (const QUrl& url : urls) {
        if (url.isLocalFile())
            paths << url.toLocalFile();
    }
    if (paths.isEmpty()) {
        showError(tr("Only files on this computer or a network share can be opened."));
        return false;
    }
    return openPaths(paths);
}

bool Files::openPaths(const QStringList& rawPaths)
{
    if (app() == nullptr || rawPaths.isEmpty())
        return false;
    QStringList paths;
    for (const QString& p : rawPaths)
        paths << localPath(p);
    // The first file that opens becomes the document; further STEP/IGES files are imported into it.
    QString first;
    QStringList imports, skipped;
    for (const QString& p : paths) {
        const Kind kind = kindOf(p);
        if (kind == Kind::Unknown) {
            skipped << QFileInfo(p).fileName();
        } else if (first.isEmpty()) {
            first = p;
        } else if (kind == Kind::Import) {
            imports << p;
        } else {
            skipped << QFileInfo(p).fileName();
        }
    }
    if (first.isEmpty()) {
        const QString name = QFileInfo(paths.constFirst()).fileName();
        showError(paths.size() == 1 ? tr("%1 is not a RayCAD, STEP or IGES file.").arg(name)
                                    : tr("None of these files is a RayCAD, STEP or IGES file."));
        return false;
    }
    if (context_)
        context_->dismissMessage();
    if (!skipped.isEmpty())
        showError(tr("Not opened (one document at a time, STEP and IGES files are imported into it): %1")
                      .arg(skipped.join(QStringLiteral(", "))));
    api::OpenDocument* current = session_->activeDocument();
    const std::string replaceId = current != nullptr ? current->id : std::string();
    guard(Reason::Open, [this, first, imports, replaceId] { startOpen(first, imports, replaceId); });
    return true;
}

void Files::startOpen(const QString& path, QStringList importAfter, std::string replaceId, Opened onOpened,
                      MakeJob make)
{
    api::IApp* a = app();
    if (a == nullptr)
        return;
    if (openJob_.valid()) {
        // A newer open supersedes one still running (its result is dropped).
        if (openJobId_ > 0)
            session_->cancelJob(openJobId_);
        else
            openJob_.cancel();
    }
    const QString name = QFileInfo(path).fileName();
    const std::vector<qint64> before = session_->appJobIds();
    openJob_ = make ? make() : a->open(toUtf8(path));
    openJobId_ = newJobId(before, session_->appJobIds());
    openingName_ = name;
    setStatus(make ? tr("Recovering %1…").arg(name) : tr("Opening %1…").arg(name));
    const std::uint64_t serial = ++openSerial_;
    session_->refreshJobs();
    emit busyChanged();
    QPointer<Files> self(this);
    const bool recovering = static_cast<bool>(make);
    openJob_.then(
        [self, serial, name, path, recovering, importAfter, replaceId, onOpened](Result<api::OpenDocument*> result) {
            if (!self || serial != self->openSerial_)
                return; // cancelled (Esc) or superseded: the late result is dropped
            self->openFinished();
            if (!result) {
                if (result.error().code == ErrorCode::Cancelled) {
                    self->setStatus(tr("Opening cancelled"));
                    return;
                }
                // A file with journalled changes from a session that ended unexpectedly opens only after they were
                // recovered or discarded: offer exactly that.
                if (!recovering && result.error().code == ErrorCode::Locked && self->session_ &&
                    self->session_->offerRecoveryFor(path)) {
                    self->setStatus(tr("%1 has unsaved changes from a session that ended unexpectedly").arg(name));
                    return;
                }
                const QString message = recovering ? tr("Could not recover %1: %2").arg(name, errorText(result.error()))
                                                   : tr("Could not open %1: %2").arg(name, errorText(result.error()));
                self->showError(message);
                emit self->openFailed(message);
                return;
            }
            api::OpenDocument* od = *result;
            // The shell shows one document: the one this open replaces goes now that the new one is there.
            if (!replaceId.empty() && replaceId != od->id)
                self->closeDocument(replaceId);
            if (!importAfter.isEmpty())
                self->importChain(od->id, importAfter);
            const QString title = fromUtf8(od->title);
            emit self->opened(title);
            if (onOpened)
                onOpened(od);
        });
}

void Files::openFinished()
{
    openJob_ = {};
    openJobId_ = 0;
    openingName_.clear();
    emit busyChanged();
}

void Files::onJobAbandoned(qint64 id)
{
    if (id == 0 || id != openJobId_)
        return;
    ++openSerial_; // drop the late result
    openFinished();
    setStatus(tr("Opening cancelled"));
}

void Files::importChain(std::string documentId, QStringList paths)
{
    api::IApp* a = app();
    api::OpenDocument* od = a != nullptr ? a->findDocument(documentId) : nullptr;
    if (od == nullptr || paths.isEmpty())
        return;
    const QString path = paths.takeFirst();
    const QString name = QFileInfo(path).fileName();
    JobHandle<std::monostate> job = a->importFile(*od, toUtf8(path));
    session_->refreshJobs();
    QPointer<Files> self(this);
    job.then([self, documentId, paths, name](Result<std::monostate> done) {
        if (!self)
            return;
        if (!done && done.error().code != ErrorCode::Cancelled)
            self->showError(tr("Could not import %1: %2").arg(name, errorText(done.error())));
        else if (done)
            self->setStatus(tr("Imported %1").arg(name));
        if (done)
            self->importChain(documentId, paths);
    });
}

// ---------- the current document ----------
void Files::newPart()
{
    if (app() == nullptr)
        return;
    api::OpenDocument* current = session_->activeDocument();
    const std::string replaceId = current != nullptr ? current->id : std::string();
    guard(Reason::New, [this, replaceId] {
        api::IApp* a = app();
        if (a == nullptr)
            return;
        auto made = a->newDocument("");
        if (!made) {
            showError(tr("Could not create a document: %1").arg(errorText(made.error())));
            return;
        }
        if (!replaceId.empty() && replaceId != (*made)->id)
            closeDocument(replaceId);
    });
}

void Files::save()
{
    api::OpenDocument* od = session_ ? session_->activeDocument() : nullptr;
    if (od == nullptr)
        return;
    if (od->path.empty() || od->readOnly) {
        requestSaveAs();
        return;
    }
    startSave(*od, std::nullopt, false, {});
}

void Files::requestSaveAs()
{
    api::OpenDocument* od = session_ ? session_->activeDocument() : nullptr;
    if (od == nullptr)
        return;
    const QString title = fromUtf8(od->title);
    const QUrl folder =
        od->path.empty() || od->readOnly ? QUrl() : QUrl::fromLocalFile(QFileInfo(fromUtf8(od->path)).path());
    emit saveAsDialogRequested(title + QStringLiteral(".raycad"), folder);
}

void Files::saveAsCancelled()
{
    afterSaveAs_ = nullptr;
}

void Files::saveAs(const QString& pathOrUrl, bool confirmedOverwrite)
{
    api::OpenDocument* od = session_ ? session_->activeDocument() : nullptr;
    std::function<void()> then = std::move(afterSaveAs_);
    afterSaveAs_ = nullptr;
    if (od == nullptr)
        return;
    QString path = localPath(pathOrUrl);
    if (QFileInfo(path).suffix().isEmpty())
        path += QStringLiteral(".raycad");
    startSave(*od, toUtf8(path), confirmedOverwrite, std::move(then));
}

void Files::startSave(api::OpenDocument& od, std::optional<std::string> path, bool overwrite,
                      std::function<void()> then)
{
    api::IApp* a = app();
    if (a == nullptr)
        return;
    const std::string id = od.id;
    JobHandle<std::monostate> job = a->save(od, std::move(path), overwrite);
    session_->refreshJobs();
    QPointer<Files> self(this);
    job.then([self, id, then = std::move(then)](Result<std::monostate> done) {
        if (!self)
            return;
        api::IApp* a = self->app();
        api::OpenDocument* now = a != nullptr ? a->findDocument(id) : nullptr;
        const QString title = now != nullptr ? fromUtf8(now->title) : QString();
        if (!done) {
            if (done.error().code == ErrorCode::Cancelled)
                self->setStatus(tr("Save cancelled"));
            else
                self->showError(tr("Could not save %1: %2").arg(title, errorText(done.error())));
            return; // whatever waited for the save (close, quit, open) does not happen
        }
        self->setStatus(tr("Saved %1").arg(title));
        emit self->saved(title);
        if (then)
            then();
    });
}

void Files::requestImport()
{
    if (available())
        emit importDialogRequested();
}

void Files::importFile(const QString& pathOrUrl)
{
    const QString path = localPath(pathOrUrl);
    api::OpenDocument* od = session_ ? session_->activeDocument() : nullptr;
    if (od == nullptr) {
        openPaths({path}); // nothing open: the file opens as a new document
        return;
    }
    if (kindOf(path) != Kind::Import) {
        showError(tr("%1 is not a STEP or IGES file.").arg(QFileInfo(path).fileName()));
        return;
    }
    importChain(od->id, {path});
}

void Files::requestExport(const QString& kind)
{
    api::OpenDocument* od = session_ ? session_->activeDocument() : nullptr;
    if (od == nullptr) {
        showError(tr("Open a document to export it."));
        return;
    }
    const QUrl folder = od->path.empty() ? QUrl() : QUrl::fromLocalFile(QFileInfo(fromUtf8(od->path)).path());
    emit exportDialogRequested(kind, fromUtf8(od->title) + exportSuffix(kind), folder);
}

void Files::exportTo(const QString& kind, const QString& pathOrUrl)
{
    api::IApp* a = app();
    api::OpenDocument* od = session_ ? session_->activeDocument() : nullptr;
    if (a == nullptr || od == nullptr)
        return;
    QString path = localPath(pathOrUrl);
    if (QFileInfo(path).suffix().isEmpty())
        path += exportSuffix(kind);
    QJsonObject args;
    args.insert(QStringLiteral("path"), path);
    args.insert(QStringLiteral("document"), fromUtf8(od->id));
    const QByteArray json = QJsonDocument(args).toJson(QJsonDocument::Compact);
    const std::string id = toUtf8(exportAction(kind));
    JobHandle<api::ActionResult> job = a->actions().invoke(
        id, std::string_view(json.constData(), static_cast<std::size_t>(json.size())), api::Caller::User);
    session_->refreshJobs();
    QPointer<Files> self(this);
    const QString name = QFileInfo(path).fileName();
    job.then([self, name, path](Result<api::ActionResult> done) {
        if (!self)
            return;
        if (!done) {
            if (done.error().code == ErrorCode::Cancelled)
                self->setStatus(tr("Export cancelled"));
            else
                self->showError(tr("Could not export %1: %2").arg(name, errorText(done.error())));
            return;
        }
        self->setStatus(tr("Exported %1").arg(name));
        emit self->exported(path);
    });
}

bool Files::closeDocument(const std::string& id)
{
    api::IApp* a = app();
    api::OpenDocument* od = a != nullptr ? a->findDocument(id) : nullptr;
    if (od == nullptr)
        return true;
    if (auto st = a->close(*od); !st) {
        showError(errorText(st.error()));
        return false;
    }
    return true;
}

void Files::close()
{
    api::OpenDocument* od = session_ ? session_->activeDocument() : nullptr;
    if (od == nullptr)
        return;
    const std::string id = od->id;
    guard(Reason::Close, [this, id] {
        if (closeDocument(id))
            setStatus(tr("Ready"));
    });
}

void Files::requestQuit()
{
    api::IApp* a = app();
    api::OpenDocument* dirty = nullptr;
    if (a != nullptr) {
        for (api::OpenDocument* od : a->documents()) {
            if (od->modified || (od->document && od->document->dirty())) {
                dirty = od;
                break;
            }
        }
    }
    if (dirty == nullptr) {
        emit quitApproved();
        return;
    }
    // One document at a time; after each answer the next one with unsaved changes is asked about.
    a->setActive(dirty);
    const std::string id = dirty->id;
    guard(Reason::Quit, [this, id] {
        if (closeDocument(id))
            requestQuit();
    });
}

// ---------- crash recovery ----------
void Files::restoreRecovery(const QString& id)
{
    if (app() == nullptr || !session_ || session_->recoverySource() == nullptr)
        return;
    std::optional<PendingRecovery> taken = session_->takeRecovery(id);
    if (!taken)
        return;
    auto recovery = std::make_shared<const PendingRecovery>(std::move(*taken));
    api::OpenDocument* current = session_->activeDocument();
    const std::string replaceId = current != nullptr ? current->id : std::string();
    QPointer<Files> self(this);
    guard(Reason::Open, [this, self, recovery, replaceId] {
        IRecoverySource* source = session_ ? session_->recoverySource() : nullptr;
        if (source == nullptr)
            return;
        // Shown like an open (progress, Esc); the recovered document becomes the active one.
        startOpen(
            recovery->title, {}, replaceId,
            [self, recovery](api::OpenDocument* od) {
                if (!self)
                    return;
                QString status = tr("Recovered %1").arg(fromUtf8(od->title));
                if (recovery->damagedTail)
                    status += tr(" (the last change was damaged and is lost)");
                self->setStatus(status);
                // What the replay reported (also delivered as Warning events): in the banner.
                QStringList warnings;
                for (const std::string& w : od->warnings)
                    warnings << fromUtf8(w);
                if (!warnings.isEmpty() && self->context_)
                    self->context_->showMessage(warnings.join(QLatin1Char('\n')), false);
                emit self->recoveryRestored(fromUtf8(od->title));
                // Never saved by the user: offer Save As so it gets a name.
                if (recovery->unsaved || od->path.empty())
                    self->requestSaveAs();
            },
            [source, recovery] { return source->recover(*recovery); });
    });
}

} // namespace ray::ui
