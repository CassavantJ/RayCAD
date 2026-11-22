#include "session.h"

#include "app_context.h"
#include "scene_controller.h"

#include <ray_api/recent_files.h>

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QLocale>
#include <QtCore/QVariantMap>

#include <algorithm>
#include <utility>

namespace ray::ui {

namespace {

constexpr const char* kEngineKey = "_ray_ui_session";
constexpr int kRebuildPollMs = 100;

QString fromUtf8(const std::string& s)
{
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

// "Today, 14:05", "Yesterday, 09:12", or the date in the locale's short format.
QString whenText(qint64 ms)
{
    if (ms <= 0)
        return {};
    const QDateTime when = QDateTime::fromMSecsSinceEpoch(ms).toLocalTime();
    const QDate today = QDate::currentDate();
    const QLocale locale;
    const QString time = locale.toString(when.time(), QLocale::ShortFormat);
    if (when.date() == today)
        return QObject::tr("Today, %1").arg(time);
    if (when.date() == today.addDays(-1))
        return QObject::tr("Yesterday, %1").arg(time);
    return locale.toString(when, QLocale::ShortFormat);
}

} // namespace

// ---------- RecentFilesModel ----------
RecentFilesModel::RecentFilesModel(QObject* parent) : QAbstractListModel(parent) {}

int RecentFilesModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(items_.size());
}

QVariant RecentFilesModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount())
        return {};
    const Item& item = items_[static_cast<std::size_t>(index.row())];
    const QFileInfo info(item.path);
    switch (role) {
    case PathRole:
        return item.path;
    case NameRole:
        return info.fileName();
    case FolderRole:
        return QDir::toNativeSeparators(info.path());
    case OpenedRole:
        return whenText(item.openedMs);
    case OpenedMsRole:
        return item.openedMs;
    case MissingRole:
        return item.missing;
    case ThumbnailRole:
        return QString(); // thumbnails are not stored yet (IApp::RecentFile::thumbnailPng is empty in M1)
    default:
        return {};
    }
}

QHash<int, QByteArray> RecentFilesModel::roleNames() const
{
    return {{PathRole, "filePath"},      {NameRole, "fileName"},     {FolderRole, "folder"},
            {OpenedRole, "openedText"},  {OpenedMsRole, "openedMs"}, {MissingRole, "missing"},
            {ThumbnailRole, "thumbnail"}};
}

void RecentFilesModel::setItems(std::vector<Item> items)
{
    beginResetModel();
    items_ = std::move(items);
    endResetModel();
}

void RecentFilesModel::setMissing(const QString& path, bool missing)
{
    for (std::size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].path == path && items_[i].missing != missing) {
            items_[i].missing = missing;
            const QModelIndex at = index(static_cast<int>(i));
            emit dataChanged(at, at, {MissingRole});
        }
    }
}

// ---------- JobsModel ----------
JobsModel::JobsModel(QObject* parent) : QAbstractListModel(parent) {}

int JobsModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant JobsModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount())
        return {};
    const Row& row = rows_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case JobIdRole:
        return row.id;
    case TitleRole:
        return row.title;
    case StageRole:
        return row.stage;
    case ProgressRole:
        return row.progress;
    case KindRole:
        return row.kind;
    default:
        return {};
    }
}

QHash<int, QByteArray> JobsModel::roleNames() const
{
    return {{JobIdRole, "jobId"},
            {TitleRole, "jobTitle"},
            {StageRole, "jobStage"},
            {ProgressRole, "jobProgress"},
            {KindRole, "jobKind"}};
}

void JobsModel::setRows(std::vector<Row> rows)
{
    const bool sameIds =
        rows.size() == rows_.size() &&
        std::equal(rows.begin(), rows.end(), rows_.begin(), [](const Row& a, const Row& b) { return a.id == b.id; });
    if (!sameIds) {
        beginResetModel();
        rows_ = std::move(rows);
        endResetModel();
        return;
    }
    rows_ = std::move(rows);
    if (!rows_.empty())
        emit dataChanged(index(0), index(rowCount() - 1), {TitleRole, StageRole, ProgressRole});
}

// ---------- Session ----------
Session::Session(SceneController* scene, AppContext* context, QObject* parent)
    : QObject(parent), scene_(scene), context_(context), recent_(new RecentFilesModel(this)), jobs_(new JobsModel(this))
{
    rebuildPoll_.setInterval(kRebuildPollMs);
    connect(&rebuildPoll_, &QTimer::timeout, this, [this] {
        refreshDocument();
        syncScene(false);
        refreshJobs();
        updateRebuildPoll();
    });
    // The viewer's display meshing reports through the AppContext job slot.
    if (context_)
        connect(context_, &AppContext::jobChanged, this, &Session::refreshJobs);
}

Session::~Session()
{
    setApp(nullptr);
}

Session* Session::create(QQmlEngine* qml, QJSEngine*)
{
    if (qml != nullptr) {
        if (auto* session = qobject_cast<Session*>(qml->property(kEngineKey).value<QObject*>())) {
            QJSEngine::setObjectOwnership(session, QJSEngine::CppOwnership);
            return session;
        }
    }
    return new Session(nullptr, nullptr, nullptr); // owned by the engine; no session
}

void Session::provideFor(QQmlEngine* engine, Session* session)
{
    engine->setProperty(kEngineKey, QVariant::fromValue<QObject*>(session));
}

void Session::setApp(api::IApp* app)
{
    if (app == app_)
        return;
    detachDocument();
    if (app_ != nullptr && appListener_ != 0)
        app_->unsubscribe(appListener_);
    appListener_ = 0;
    app_ = app;
    abandoned_.clear();
    if (app_ != nullptr) {
        QPointer<Session> self(this);
        appListener_ = app_->subscribe([self](const api::AppEvent& event) {
            if (self)
                self->onAppEvent(event);
        });
    }
    attachActive();
    refreshRecent();
    refreshJobs();
    emit sessionChanged();
}

void Session::setRecoverySource(std::unique_ptr<IRecoverySource> source)
{
    recovery_ = std::move(source);
}

void Session::checkRecovery()
{
    pending_.clear();
    if (recovery_)
        pending_ = recovery_->pending();
    recoveryPrompt_ = !pending_.empty();
    emit recoveriesChanged();
}

bool Session::offerRecoveryFor(const QString& path)
{
    checkRecovery();
    const QString wanted = QFileInfo(path).absoluteFilePath();
#ifdef Q_OS_WIN
    const Qt::CaseSensitivity cs = Qt::CaseInsensitive;
#else
    const Qt::CaseSensitivity cs = Qt::CaseSensitive;
#endif
    const bool found = std::any_of(pending_.begin(), pending_.end(), [&](const PendingRecovery& p) {
        return QFileInfo(p.filePath).absoluteFilePath().compare(wanted, cs) == 0;
    });
    recoveryPrompt_ = found;
    emit recoveriesChanged();
    return found;
}

void Session::postponeRecovery()
{
    if (!recoveryPrompt_)
        return;
    recoveryPrompt_ = false;
    emit recoveriesChanged();
}

std::optional<PendingRecovery> Session::takeRecovery(const QString& id)
{
    auto it = std::find_if(pending_.begin(), pending_.end(), [&](const PendingRecovery& p) { return p.id == id; });
    if (it == pending_.end())
        return std::nullopt;
    PendingRecovery out = std::move(*it);
    pending_.erase(it);
    recoveryPrompt_ = recoveryPrompt_ && !pending_.empty();
    emit recoveriesChanged();
    return out;
}

void Session::discardRecovery(const QString& id)
{
    auto it = std::find_if(pending_.begin(), pending_.end(), [&](const PendingRecovery& p) { return p.id == id; });
    if (it == pending_.end() || !recovery_)
        return;
    if (auto st = recovery_->discard(*it); !st) {
        if (context_)
            context_->showMessage(tr("Could not discard the changes to %1: %2")
                                      .arg(it->title, QString::fromStdString(st.error().message)),
                                  true);
        return;
    }
    takeRecovery(id);
}

QVariantList Session::recoveries() const
{
    QVariantList out;
    for (const PendingRecovery& p : pending_) {
        QVariantMap row;
        row.insert(QStringLiteral("id"), p.id);
        row.insert(QStringLiteral("title"), p.title);
        row.insert(QStringLiteral("folder"), p.unsaved || p.filePath.isEmpty()
                                                 ? tr("Never saved")
                                                 : QDir::toNativeSeparators(QFileInfo(p.filePath).path()));
        row.insert(QStringLiteral("time"), whenText(p.lastWriteMs));
        row.insert(QStringLiteral("changes"),
                   p.steps == 1 ? tr("1 unsaved change") : tr("%1 unsaved changes").arg(p.steps));
        row.insert(QStringLiteral("damaged"), p.damagedTail);
        out << row;
    }
    return out;
}
api::OpenDocument* Session::activeDocument() const
{
    return app_ != nullptr ? app_->active() : nullptr;
}

// ---------- events and the active document ----------
void Session::onAppEvent(const api::AppEvent& event)
{
    switch (event.kind) {
    case api::AppEvent::Kind::DocumentOpened:
    case api::AppEvent::Kind::DocumentClosed:
    case api::AppEvent::Kind::ActiveDocumentChanged:
    case api::AppEvent::Kind::DocumentSaved:
        attachActive();
        refreshDocument();
        break;
    case api::AppEvent::Kind::JobsChanged:
        refreshJobs();
        break;
    case api::AppEvent::Kind::RecentFilesChanged:
        refreshRecent();
        break;
    case api::AppEvent::Kind::Warning:
        // E.g. crash recovery is off for a document, or only part of a journal could be replayed.
        if (context_ && !event.message.empty())
            context_->showMessage(fromUtf8(event.message), false);
        break;
    }
}

doc::Document* Session::liveDoc()
{
    if (doc_ == nullptr)
        return nullptr;
    api::OpenDocument* od = app_ != nullptr ? app_->findDocument(docId_) : nullptr;
    if (od != nullptr && od->document.get() == doc_)
        return doc_;
    attachActive(); // it was closed: follow the active document now
    return doc_;
}

void Session::detachDocument()
{
    if (doc_ != nullptr && app_ != nullptr) {
        // A closed document is gone, and so are its listeners: unsubscribe only from one that is still open.
        api::OpenDocument* od = app_->findDocument(docId_);
        if (od != nullptr && od->document.get() == doc_ && docListener_ != 0)
            doc_->unsubscribe(docListener_);
    }
    doc_ = nullptr;
    docListener_ = 0;
    docId_.clear();
    shownBuild_.reset();
    if (modelOwner_ != nullptr && scene_)
        scene_->setFramingHeld(false);
    modelOwner_ = nullptr; // a shown model belongs to the document it was made from
    modelState_.reset();
    modelBuild_.reset();
    ++docGeneration_;
    rebuildPoll_.stop();
}

void Session::showModel(const QObject* owner, doc::DocStatePtr state, doc::BuildResultPtr build)
{
    if (owner == nullptr || !state || !build || liveDoc() == nullptr)
        return;
    if (owner == modelOwner_ && state == modelState_ && build == modelBuild_)
        return;
    modelOwner_ = owner;
    modelState_ = std::move(state);
    modelBuild_ = std::move(build);
    if (scene_)
        scene_->setFramingHeld(true); // a passing model: the view stays where it is
    syncScene(true);
}

void Session::noteInStatus(const std::string& documentId, const QString& note)
{
    if (note.isEmpty())
        return;
    if (documentId == docId_ && shownBuild_ && modelOwner_ == nullptr && context_) {
        context_->setStatusText(context_->statusText() + tr(" · %1").arg(note));
        return;
    }
    statusNote_ = note;
    statusNoteDoc_ = documentId;
}

void Session::clearShownModel(const QObject* owner)
{
    if (modelOwner_ == nullptr || owner != modelOwner_)
        return;
    modelOwner_ = nullptr;
    modelState_.reset();
    modelBuild_.reset();
    if (scene_)
        scene_->setFramingHeld(false);
    syncScene(true);
}

void Session::attachActive()
{
    api::OpenDocument* od = activeDocument();
    doc::Document* d = od != nullptr ? od->document.get() : nullptr;
    const std::string id = od != nullptr ? od->id : std::string();
    if (id == docId_ && d == doc_)
        return;
    detachDocument();
    docId_ = id;
    doc_ = d;
    if (doc_ != nullptr) {
        QPointer<Session> self(this);
        const std::uint64_t generation = docGeneration_;
        docListener_ = doc_->subscribe([self, generation](doc::DocStatePtr, doc::BuildResultPtr) {
            if (self)
                self->onDocumentChanged(generation);
        });
    }
    refreshDocument();
    // A different document: the scene starts from nothing, labelled with the new title.
    if (scene_) {
        scene_->clear();
        if (doc_ != nullptr)
            scene_->setBodies({}, title_);
    }
    syncScene(true);
    updateRebuildPoll();
    refreshJobs();
}

void Session::onDocumentChanged(std::uint64_t generation)
{
    if (generation != docGeneration_ || liveDoc() == nullptr)
        return;
    refreshDocument();
    syncScene(false);
    updateRebuildPoll();
    refreshJobs();
}

void Session::refreshDocument()
{
    doc::Document* d = liveDoc();
    api::OpenDocument* od = d != nullptr ? app_->findDocument(docId_) : nullptr;
    QString title, path, holder, since, undo, redo;
    bool dirty = false, readOnly = false, stale = false, canUndo = false, canRedo = false;
    if (od != nullptr) {
        title = fromUtf8(od->title);
        path = fromUtf8(od->path);
        readOnly = od->readOnly;
        if (od->lock) {
            const store::LockInfo& lock = *od->lock;
            holder = lock.user.empty() ? tr("another user") : fromUtf8(lock.user);
            if (!lock.host.empty())
                holder = tr("%1 on %2").arg(holder, fromUtf8(lock.host));
            if (lock.sinceUnixMs > 0)
                since = QLocale().toString(QDateTime::fromMSecsSinceEpoch(lock.sinceUnixMs).toLocalTime(),
                                           QLocale::ShortFormat);
            stale = lock.stale;
        }
        dirty = od->modified || d->dirty(); // modified: a recovery left it unlike the file's head
        canUndo = d->canUndo();
        canRedo = d->canRedo();
        if (const auto c = d->undoCommand())
            undo = fromUtf8(c->label);
        if (const auto c = d->redoCommand())
            redo = fromUtf8(c->label);
    }
    const bool changed = title != title_ || path != path_ || holder != lockHolder_ || since != lockSince_ ||
                         undo != undoText_ || redo != redoText_ || dirty != dirty_ || readOnly != readOnly_ ||
                         stale != lockStale_ || canUndo != canUndo_ || canRedo != canRedo_;
    title_ = title;
    path_ = path;
    lockHolder_ = holder;
    lockSince_ = since;
    undoText_ = undo;
    redoText_ = redo;
    dirty_ = dirty;
    readOnly_ = readOnly;
    lockStale_ = stale;
    canUndo_ = canUndo;
    canRedo_ = canRedo;
    if (changed)
        emit documentChanged();
}

void Session::syncScene(bool force)
{
    doc::Document* d = liveDoc();
    if (!scene_ || d == nullptr)
        return;
    if (modelOwner_ != nullptr) {
        // A tool's model (showModel) stands in for the document's builds until it is cleared.
        if (!force)
            return;
        shownBuild_.reset(); // the document's build is shown again (forced) when the tool's model goes
        scene_->syncFromDocument(modelState_, modelBuild_);
        return;
    }
    const doc::BuildResultPtr build = d->build();
    if (!force && build == shownBuild_)
        return;
    shownBuild_ = build;
    // The tree and the bodies come from the snapshot the build was made from, so they always agree.
    const doc::DocStatePtr state = build ? d->builtState() : d->state();
    scene_->syncFromDocument(state, build);
    if (build && context_) {
        int failed = 0;
        for (const doc::FeatureReport& report : build->reports) {
            if (report.status == doc::FeatureStatus::Failed)
                ++failed;
        }
        const int bodies = scene_->bodyCount();
        QString status = bodies == 1 ? tr("%1: 1 body").arg(title_) : tr("%1: %2 bodies").arg(title_).arg(bodies);
        if (failed > 0)
            status += failed == 1 ? tr(", 1 feature failed") : tr(", %1 features failed").arg(failed);
        if (!statusNote_.isEmpty() && statusNoteDoc_ == docId_) {
            status += tr(" · %1").arg(statusNote_);
            statusNote_.clear();
        }
        context_->setStatusText(status);
    }
    if (build)
        emit buildShown();
}

void Session::updateRebuildPoll()
{
    // The document notifies when a rebuild publishes, not when one fails or is cancelled: poll while one runs.
    doc::Document* d = liveDoc();
    const bool pending = d != nullptr && d->rebuildPending();
    if (pending && !rebuildPoll_.isActive())
        rebuildPoll_.start();
    else if (!pending && rebuildPoll_.isActive())
        rebuildPoll_.stop();
}

QString Session::windowTitle() const
{
    if (!hasDocument())
        return tr("RayCAD");
    if (readOnly_)
        return tr("%1 (read-only) - RayCAD").arg(title_);
    return dirty_ ? tr("%1* - RayCAD").arg(title_) : tr("%1 - RayCAD").arg(title_);
}

// ---------- jobs ----------
std::vector<qint64> Session::appJobIds() const
{
    std::vector<qint64> out;
    if (app_ != nullptr) {
        for (const api::JobStatus& job : app_->activeJobs())
            out.push_back(static_cast<qint64>(job.id));
    }
    return out;
}

void Session::refreshJobs()
{
    std::vector<JobsModel::Row> rows;
    std::set<qint64> stillRunning;
    if (app_ != nullptr) {
        for (const api::JobStatus& job : app_->activeJobs()) {
            const auto id = static_cast<qint64>(job.id);
            stillRunning.insert(id);
            if (abandoned_.count(id) != 0)
                continue;
            rows.push_back(
                JobsModel::Row{id, fromUtf8(job.title), fromUtf8(job.stage), QStringLiteral("app"), job.progress});
        }
    }
    // Forget abandoned jobs that have ended.
    for (auto it = abandoned_.begin(); it != abandoned_.end();)
        it = stillRunning.count(*it) != 0 ? std::next(it) : abandoned_.erase(it);
    if (doc::Document* d = liveDoc(); d != nullptr && d->rebuildPending())
        rows.push_back(
            JobsModel::Row{kRebuildJobId, tr("Rebuilding %1").arg(title_), {}, QStringLiteral("rebuild"), -1});
    if (context_ && context_->jobActive())
        rows.push_back(JobsModel::Row{
            kDisplayJobId, context_->jobTitle(), {}, QStringLiteral("display"), context_->jobProgress()});

    // Foreground: the newest app job, else the rebuild, else the display meshing.
    qint64 foreground = -1;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].kind == QLatin1String("app"))
            foreground = static_cast<qint64>(i);
    }
    if (foreground < 0 && !rows.empty())
        foreground = 0;
    foreground_ = foreground;
    jobs_->setRows(std::move(rows));
    emit jobsChanged();
}

QString Session::jobTitle() const
{
    if (foreground_ < 0 || foreground_ >= jobCount())
        return {};
    const JobsModel::Row& row = jobs_->rows()[static_cast<std::size_t>(foreground_)];
    return row.stage.isEmpty() ? row.title : tr("%1: %2").arg(row.title, row.stage);
}

qreal Session::jobProgress() const
{
    if (foreground_ < 0 || foreground_ >= jobCount())
        return -1;
    return jobs_->rows()[static_cast<std::size_t>(foreground_)].progress;
}

void Session::cancelJob(qint64 id)
{
    if (id == kDisplayJobId) {
        if (context_)
            context_->cancelJob(); // the scene cancels its mesh jobs
        return;
    }
    if (id == kRebuildJobId) {
        if (doc::Document* d = liveDoc(); d != nullptr && d->rebuildPending()) {
            d->cancelRebuild();
            if (context_)
                context_->setStatusText(tr("Rebuild cancelled"));
        }
        refreshJobs();
        return;
    }
    if (app_ == nullptr || id <= 0)
        return;
    if (!app_->cancelJob(static_cast<std::uint64_t>(id))) {
        // Too late: the job is past its point of no return (or has ended) and reports what it did, so it is not
        // abandoned and its result is shown as usual.
        if (context_)
            context_->setStatusText(tr("Too late to cancel: finishing"));
        refreshJobs();
        return;
    }
    // The UI returns at once (PLAN.md 7): the job leaves the list now, its late result is dropped.
    abandoned_.insert(id);
    refreshJobs();
    emit jobAbandoned(id);
}

void Session::cancelForegroundJob()
{
    if (foreground_ < 0 || foreground_ >= jobCount())
        return;
    cancelJob(jobs_->rows()[static_cast<std::size_t>(foreground_)].id);
}

// ---------- recent files ----------
void Session::refreshRecent()
{
    std::vector<RecentFilesModel::Item> items;
    if (app_ != nullptr) {
        for (const api::IApp::RecentFile& f : app_->recentFiles())
            items.push_back(RecentFilesModel::Item{fromUtf8(f.path), f.openedMs, false});
    }
    // Keep what is known about existence until the check below answers.
    for (RecentFilesModel::Item& item : items) {
        for (const RecentFilesModel::Item& old : recent_->items()) {
            if (old.path == item.path)
                item.missing = old.missing;
        }
    }
    recent_->setItems(std::move(items));
    emit recentChanged();
    checkRecentExistence();
}

void Session::checkRecentExistence()
{
    if (app_ == nullptr || recent_->items().empty())
        return;
    std::vector<std::string> paths;
    for (const RecentFilesModel::Item& item : recent_->items())
        paths.push_back(item.path.toStdString());
    using Found = std::vector<std::pair<std::string, bool>>;
    const std::uint64_t serial = ++recentSerial_;
    JobHandle<Found> job = app_->jobs().submit<Found>(JobInfo{"Checking recent files", JobPriority::Background, false},
                                                      [paths](JobContext& ctx) -> Result<Found> {
                                                          Found out;
                                                          for (const std::string& p : paths) {
                                                              if (auto st = ctx.cancel.check(); !st)
                                                                  return tl::unexpected(st.error());
                                                              const QFileInfo info(QString::fromStdString(p));
                                                              out.emplace_back(p, !info.isFile());
                                                          }
                                                          return out;
                                                      });
    QPointer<Session> self(this);
    job.then([self, serial](Result<Found> found) {
        if (!self || serial != self->recentSerial_ || !found)
            return;
        for (const auto& [path, missing] : *found)
            self->recent_->setMissing(QString::fromStdString(path), missing);
    });
}

bool Session::removeRecent(const QString& path)
{
    if (app_ == nullptr)
        return false;
    return api::forgetRecentFile(*app_, path.toStdString());
}

// ---------- actions ----------
QString Session::actionTitle(const QString& id) const
{
    if (app_ == nullptr)
        return {};
    const auto info = app_->actions().find(id.toStdString());
    return info ? fromUtf8(info->title) : QString();
}

} // namespace ray::ui
