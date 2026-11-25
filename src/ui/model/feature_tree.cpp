#include "feature_tree.h"

#include "../app_context.h"
#include "../scene_controller.h"
#include "../session.h"
#include "../sketch/sketch_geom.h"
#include "../sketch/sketch_mode.h"
#include "../viewport_item.h"
#include "directedit_tool.h"
#include "feature_tools.h"
#include "model_util.h"

#include <ray_api/sketch_edit.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtGui/QMouseEvent>

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

namespace ray::ui {

using namespace modelui;

namespace {

constexpr const char* kTrContext = "ray::ui::FeatureTreeModel";

QString trt(const char* text)
{
    return QCoreApplication::translate(kTrContext, text);
}

QString typeTitle(const QString& type)
{
    static const std::pair<const char*, const char*> titles[] = {
        {"import", QT_TRANSLATE_NOOP("ray::ui::FeatureTreeModel", "Import")},
        {"sketch", QT_TRANSLATE_NOOP("ray::ui::FeatureTreeModel", "Sketch")},
        {"datum_plane", QT_TRANSLATE_NOOP("ray::ui::FeatureTreeModel", "Datum plane")},
        {"extrude", QT_TRANSLATE_NOOP("ray::ui::FeatureTreeModel", "Extrude")},
        {"revolve", QT_TRANSLATE_NOOP("ray::ui::FeatureTreeModel", "Revolve")},
        {"fillet", QT_TRANSLATE_NOOP("ray::ui::FeatureTreeModel", "Fillet")},
        {"chamfer", QT_TRANSLATE_NOOP("ray::ui::FeatureTreeModel", "Chamfer")},
        {"hole", QT_TRANSLATE_NOOP("ray::ui::FeatureTreeModel", "Hole")},
        {"transform_body", QT_TRANSLATE_NOOP("ray::ui::FeatureTreeModel", "Move body")}, // T-077
        {"delete_face", QT_TRANSLATE_NOOP("ray::ui::FeatureTreeModel", "Delete faces")}, // T-083
        {"offset_face", QT_TRANSLATE_NOOP("ray::ui::FeatureTreeModel", "Push/Pull")},
        {"resize_hole", QT_TRANSLATE_NOOP("ray::ui::FeatureTreeModel", "Resize hole")},
    };
    for (const auto& [t, title] : titles) {
        if (type == QLatin1String(t))
            return trt(title);
    }
    return type;
}

QString iconOf(const QString& type)
{
    if (type == QLatin1String("sketch"))
        return QStringLiteral("sketch");
    if (type == QLatin1String("datum_plane"))
        return QStringLiteral("datum");
    if (type == QLatin1String("extrude") || type == QLatin1String("revolve") || type == QLatin1String("fillet") ||
        type == QLatin1String("chamfer") || type == QLatin1String("hole"))
        return type == QLatin1String("fillet") ? QStringLiteral("fillet3d") : type;
    if (type == QLatin1String("import"))
        return QStringLiteral("cube");
    if (type == QLatin1String("transform_body")) // T-077: a fixture placed against the part
        return QStringLiteral("lock");
    if (type == QLatin1String("delete_face")) // T-083: the direct edits
        return QStringLiteral("deleteFace");
    if (type == QLatin1String("offset_face"))
        return QStringLiteral("pushPull");
    if (type == QLatin1String("resize_hole"))
        return QStringLiteral("resizeHole");
    return QStringLiteral("document");
}

QString statusKey(doc::FeatureStatus s)
{
    switch (s) {
    case doc::FeatureStatus::Ok:
        return QStringLiteral("ok");
    case doc::FeatureStatus::Warning:
        return QStringLiteral("warning");
    case doc::FeatureStatus::Failed:
        return QStringLiteral("failed");
    case doc::FeatureStatus::Suppressed:
        return QStringLiteral("suppressed");
    case doc::FeatureStatus::NotBuilt:
        return QStringLiteral("notBuilt");
    }
    return {};
}

// The bodies a feature's props say it reads (they must exist before it), and the ones it makes.
std::vector<QString> bodiesRead(const doc::Feature& f)
{
    std::vector<QString> out;
    const QJsonObject p = jsonObject(f.propsJson);
    const QString type = qs(f.type);
    if ((type == QLatin1String("extrude") || type == QLatin1String("revolve")) &&
        p.value(QStringLiteral("operation")).toString() != QLatin1String("new"))
        out.push_back(p.value(QStringLiteral("target")).toString());
    if (type == QLatin1String("hole"))
        out.push_back(p.value(QStringLiteral("target")).toString());
    if (type == QLatin1String("fillet") || type == QLatin1String("chamfer")) {
        for (const QJsonValue& e : p.value(QStringLiteral("edges")).toArray())
            out.push_back(e.toObject().value(QStringLiteral("body")).toString());
    }
    const QJsonObject plane =
        p.value(type == QLatin1String("datum_plane") ? QStringLiteral("base") : QStringLiteral("plane")).toObject();
    if (plane.contains(QStringLiteral("face")))
        out.push_back(plane.value(QStringLiteral("face")).toObject().value(QStringLiteral("body")).toString());
    out.erase(std::remove(out.begin(), out.end(), QString()), out.end());
    return out;
}

std::vector<QString> bodiesMade(const doc::Feature& f)
{
    std::vector<QString> out;
    const QJsonObject p = jsonObject(f.propsJson);
    const QString type = qs(f.type);
    if ((type == QLatin1String("extrude") || type == QLatin1String("revolve")) &&
        p.value(QStringLiteral("operation")).toString() == QLatin1String("new"))
        out.push_back(p.value(QStringLiteral("target")).toString());
    if (type == QLatin1String("import")) {
        for (const QJsonValue& b : p.value(QStringLiteral("bodies")).toArray())
            out.push_back(b.toObject().value(QStringLiteral("id")).toString());
    }
    return out;
}

} // namespace

FeatureTreeModel::FeatureTreeModel(SceneController* scene, Session* session, AppContext* context, QObject* parent)
    : QAbstractListModel(parent), scene_(scene), session_(session), context_(context),
      sketchLines_(std::make_unique<LineOverlay>(scene)), datumLines_(std::make_unique<LineOverlay>(scene))
{
    if (session_) {
        connect(session_, &Session::documentChanged, this, [this] {
            if (!session_)
                return;
            const std::string id = session_->documentId().toStdString();
            if (id != docId_) {
                rollback_ = -1;
                current_ = -1;
                sketchOverride_.clear();
                sketchLines_->clear();
                datumOverride_.clear();
                datums_.clear();
                datumLines_->clear();
                emit rollbackChanged();
                emit currentChanged();
                subscribe();
                sync();
            }
        });
    }
    if (scene_) {
        // The datum frames are sized to the model shown: drawn again when it changes.
        connect(scene_, &SceneController::stateChanged, this, [this] { drawDatums(); });
        connect(scene_, &SceneController::bodiesChanged, this, [this] { drawDatums(); });
    }
}

FeatureTreeModel::~FeatureTreeModel()
{
    sketchJob_.cancel();
    datumJob_.cancel();
    if (view_)
        view_->removeEventFilter(this);
    sketchLines_.reset();
    datumLines_.reset();
    unsubscribe();
}

void FeatureTreeModel::setFeatureTool(FeatureToolModel* tool)
{
    if (tool_)
        disconnect(tool_, nullptr, this, nullptr);
    tool_ = tool;
    if (tool_) {
        // The datum plane tool asks for a plane, and leaves the datum it edits to its own preview.
        connect(tool_, &FeatureToolModel::activeChanged, this, [this] { updateDatumLines(); });
    }
}

QObject* FeatureTreeModel::view() const
{
    return view_;
}

void FeatureTreeModel::setView(QObject* view)
{
    auto* item = qobject_cast<QQuickItem*>(view);
    if (item == view_)
        return;
    if (view_)
        view_->removeEventFilter(this);
    view_ = item;
    if (view_)
        view_->installEventFilter(this);
    emit viewChanged();
}

bool FeatureTreeModel::eventFilter(QObject* watched, QEvent* event)
{
    // A click (not a drag) on a datum plane where a plane is asked for picks it; the view still gets the click.
    if (watched == view_ && planeWanted()) {
        if (event->type() == QEvent::MouseButtonPress) {
            auto* m = static_cast<QMouseEvent*>(event);
            pressed_ = m->button() == Qt::LeftButton;
            pressAt_ = m->position();
        } else if (event->type() == QEvent::MouseButtonRelease && pressed_) {
            auto* m = static_cast<QMouseEvent*>(event);
            pressed_ = false;
            const QPointF d = m->position() - pressAt_;
            if (m->button() == Qt::LeftButton && std::hypot(d.x(), d.y()) < 4)
                pickDatumAt(m->position().x(), m->position().y());
        }
    }
    return QAbstractListModel::eventFilter(watched, event);
}

FeatureTreeModel* FeatureTreeModel::create(QQmlEngine* qml, QJSEngine*)
{
    SceneController* scene = nullptr;
    Session* session = nullptr;
    AppContext* context = nullptr;
    SketchModel* sketch = nullptr;
    if (qml != nullptr) {
        const QString uri = QStringLiteral("RayCad.Ui");
        scene = qml->singletonInstance<SceneController*>(uri, QStringLiteral("Scene"));
        session = qml->singletonInstance<Session*>(uri, QStringLiteral("Session"));
        context = qml->singletonInstance<AppContext*>(uri, QStringLiteral("AppContext"));
        sketch = qml->singletonInstance<SketchModel*>(uri, QStringLiteral("SketchTool"));
    }
    auto* model = new FeatureTreeModel(scene, session, context);
    model->engine_ = qml;
    model->setSketchTool(sketch);
    model->subscribe();
    model->sync();
    return model;
}

void FeatureTreeModel::setSketchTool(SketchModel* sketch)
{
    sketchTool_ = sketch;
    if (!sketch)
        return;
    const auto follow = [this] {
        std::optional<FeatureId> edited;
        if (sketchTool_ && sketchTool_->active())
            edited = sketchTool_->featureId();
        if (edited != editedSketch_) {
            editedSketch_ = edited;
            updateSketchLines();
        }
    };
    connect(sketch, &SketchModel::activeChanged, this, follow);
    connect(sketch, &SketchModel::modelChanged, this, follow);
    // The plane picker asks for a plane: the datum planes are drawn as pickable.
    connect(sketch, &SketchModel::placingChanged, this, [this] { drawDatums(); });
}

// ---------- document ----------
api::IApp* FeatureTreeModel::app() const
{
    return context_ ? context_->app() : nullptr;
}

api::OpenDocument* FeatureTreeModel::document() const
{
    api::IApp* a = app();
    return a == nullptr || docId_.empty() ? nullptr : a->findDocument(docId_);
}

doc::Document* FeatureTreeModel::liveDoc() const
{
    api::OpenDocument* od = document();
    return od != nullptr && od->document.get() == doc_ ? doc_ : nullptr;
}

void FeatureTreeModel::subscribe()
{
    unsubscribe();
    api::IApp* a = app();
    api::OpenDocument* od = a != nullptr ? a->active() : nullptr;
    if (od == nullptr || !od->document)
        return;
    docId_ = od->id;
    doc_ = od->document.get();
    const std::uint64_t generation = ++docGeneration_;
    QPointer<FeatureTreeModel> self(this);
    docListener_ = doc_->subscribe([self, generation](doc::DocStatePtr, doc::BuildResultPtr) {
        if (self && self->docGeneration_ == generation)
            self->sync();
    });
    previewListener_ = doc_->subscribePreview([self, generation](doc::DocStatePtr state, doc::BuildResultPtr build) {
        if (self && self->docGeneration_ == generation && self->checking_)
            self->finishReorderCheck(std::move(state), std::move(build));
    });
}

void FeatureTreeModel::unsubscribe()
{
    if (doc::Document* d = liveDoc(); d != nullptr) {
        if (docListener_ != 0)
            d->unsubscribe(docListener_);
        if (previewListener_ != 0)
            d->unsubscribePreview(previewListener_);
    }
    doc_ = nullptr;
    docListener_ = previewListener_ = 0;
    docId_.clear();
    ++docGeneration_;
    checking_ = false;
}

void FeatureTreeModel::sync()
{
    if (doc_ == nullptr && app() != nullptr && app()->active() != nullptr)
        subscribe();
    doc::Document* d = liveDoc();
    const doc::DocStatePtr state = d ? d->state() : nullptr;
    const doc::BuildResultPtr build = d ? d->build() : nullptr;
    shownState_ = state;
    shownBuild_ = build;
    // The document's rollback bar.
    int bar = -1;
    if (state && state->rollback) {
        for (std::size_t i = 0; i + 1 < state->features.size(); ++i) {
            if (state->features[i].id == *state->rollback)
                bar = static_cast<int>(i);
        }
    }
    std::vector<Row> rows;
    if (state) {
        std::map<FeatureId, const doc::FeatureReport*> reports;
        if (build) {
            for (const doc::FeatureReport& r : build->reports)
                reports[r.id] = &r;
        }
        const bool current = d->builtState() == state;
        QString lastFailed;
        for (const doc::Feature& f : state->features) {
            Row row;
            row.id = f.id;
            row.name = qs(f.name);
            row.type = qs(f.type);
            row.suppressed = f.suppressed;
            row.sketch = f.type == "sketch";
            row.datum = f.type == "datum_plane";
            const auto it = reports.find(f.id);
            if (bar >= 0 && static_cast<int>(rows.size()) > bar) {
                row.status = QStringLiteral("rolledBack");
                row.statusText = trt("Rolled back");
            } else if (f.suppressed) {
                row.status = QStringLiteral("suppressed");
                row.statusText = trt("Suppressed");
            } else if (it == reports.end()) {
                row.status = current ? QStringLiteral("notBuilt") : QStringLiteral("building");
                row.statusText = current ? trt("Not built") : trt("Building");
            } else {
                const doc::FeatureReport& r = *it->second;
                row.status = statusKey(r.status);
                row.message = qs(r.message);
                if (r.status == doc::FeatureStatus::Failed) {
                    row.statusText = trt("Failed");
                    lastFailed = row.name;
                } else if (r.status == doc::FeatureStatus::Warning) {
                    row.statusText = trt("Warning");
                } else if (r.status == doc::FeatureStatus::NotBuilt) {
                    row.statusText = lastFailed.isEmpty() ? trt("Not built") : trt("Waits on %1").arg(lastFailed);
                    if (row.message.isEmpty())
                        row.message = lastFailed.isEmpty()
                                          ? trt("An earlier feature it needs did not build")
                                          : trt("Waits on %1, which failed: a body or sketch it needs is not there")
                                                .arg(lastFailed);
                    else if (!lastFailed.isEmpty())
                        row.message = trt("Waits on %1: %2").arg(lastFailed, row.message);
                }
            }
            rows.push_back(std::move(row));
        }
    }
    // The same features in the same order (a rebuild finished, a rename, a suppress): the rows change in place, so the
    // tree keeps its delegates (and a name being edited).
    const bool sameRows =
        rows.size() == rows_.size() &&
        std::equal(rows.begin(), rows.end(), rows_.begin(), [](const Row& a, const Row& b) { return a.id == b.id; });
    if (sameRows) {
        rows_ = std::move(rows);
        if (!rows_.empty())
            emit dataChanged(index(0), index(count() - 1));
    } else {
        beginResetModel();
        rows_ = std::move(rows);
        endResetModel();
    }
    ++revision_;
    if (bar != rollback_) {
        rollback_ = bar;
        emit rollbackChanged();
        if (!rows_.empty())
            emit dataChanged(index(0), index(count() - 1), {RolledBackRole});
    }
    if (current_ >= count()) {
        current_ = -1;
        emit currentChanged();
    }
    emit changed();
    updateSketchLines();
    updateDatumLines();
}

int FeatureTreeModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant FeatureTreeModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= count())
        return {};
    const Row& r = rows_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case IdRole:
        return idText(r.id.value);
    case NameRole:
        return r.name;
    case TypeRole:
        return r.type;
    case TypeTitleRole:
        return typeTitle(r.type);
    case IconRole:
        return iconOf(r.type);
    case StatusRole:
        return r.status;
    case StatusTextRole:
        return r.statusText;
    case MessageRole:
        return r.message;
    case SuppressedRole:
        return r.suppressed;
    case RolledBackRole:
        return rollback_ >= 0 && index.row() > rollback_;
    case IsSketchRole:
        return r.sketch;
    case SketchShownRole:
        return r.sketch && sketchShown(r.id);
    case IsDatumRole:
        return r.datum;
    case DatumShownRole:
        return r.datum && datumShown(r.id);
    default:
        return {};
    }
}

QHash<int, QByteArray> FeatureTreeModel::roleNames() const
{
    return {{IdRole, "featureId"},          {NameRole, "featureName"},     {TypeRole, "featureType"},
            {TypeTitleRole, "typeTitle"},   {IconRole, "iconName"},        {StatusRole, "status"},
            {StatusTextRole, "statusText"}, {MessageRole, "message"},      {SuppressedRole, "suppressed"},
            {RolledBackRole, "rolledBack"}, {IsSketchRole, "isSketch"},    {SketchShownRole, "sketchShown"},
            {IsDatumRole, "isDatum"},       {DatumShownRole, "datumShown"}};
}

QVariantMap FeatureTreeModel::get(int row) const
{
    QVariantMap out;
    if (row < 0 || row >= count())
        return out;
    const QHash<int, QByteArray> roles = roleNames();
    for (auto it = roles.begin(); it != roles.end(); ++it)
        out.insert(QString::fromLatin1(it.value()), data(index(row), it.key()));
    return out;
}

int FeatureTreeModel::rowOf(const QString& featureId) const
{
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        if (idText(rows_[i].id.value) == featureId)
            return static_cast<int>(i);
    }
    return -1;
}

std::optional<FeatureId> FeatureTreeModel::featureAt(int row) const
{
    if (row < 0 || row >= count())
        return std::nullopt;
    return rows_[static_cast<std::size_t>(row)].id;
}

std::optional<FeatureId> FeatureTreeModel::currentFeatureId() const
{
    return featureAt(current_);
}

void FeatureTreeModel::setCurrentIndex(int index)
{
    if (index < -1 || index >= count())
        index = -1;
    if (index == current_)
        return;
    current_ = index;
    emit currentChanged();
}

void FeatureTreeModel::setMessage(const QString& message)
{
    if (message == message_)
        return;
    message_ = message;
    emit messageChanged();
}

void FeatureTreeModel::dismissMessage()
{
    setMessage(QString());
}

// ---------- actions ----------
bool FeatureTreeModel::run(const QString& actionId, const std::string& argsJson, const QString& label)
{
    api::IApp* a = app();
    api::OpenDocument* od = document();
    if (a == nullptr || od == nullptr)
        return false;
    if (od->readOnly) {
        setMessage(trt("The document is read-only"));
        return false;
    }
    const QByteArray id = actionId.toUtf8();
    JobHandle<api::ActionResult> job = a->actions().invoke(sv(id), argsJson, api::Caller::User);
    if (!job.valid())
        return false;
    QPointer<FeatureTreeModel> self(this);
    job.then([self, label](Result<api::ActionResult> r) {
        if (self && !r)
            self->setMessage(trt("%1 failed: %2").arg(label, qs(r.error().message)));
    });
    setMessage(QString());
    return true;
}

bool FeatureTreeModel::rename(int row, const QString& name)
{
    const auto id = featureAt(row);
    const QString trimmed = name.trimmed();
    if (!id || trimmed.isEmpty() || trimmed == rows_[static_cast<std::size_t>(row)].name)
        return false;
    const QJsonObject args{{QStringLiteral("id"), idText(id->value)}, {QStringLiteral("name"), trimmed}};
    return run(QStringLiteral("feature.edit"), jsonText(args), trt("Rename"));
}

bool FeatureTreeModel::setSuppressed(int row, bool suppressed)
{
    const auto id = featureAt(row);
    if (!id || rows_[static_cast<std::size_t>(row)].suppressed == suppressed)
        return false;
    const QJsonObject args{{QStringLiteral("id"), idText(id->value)}, {QStringLiteral("suppressed"), suppressed}};
    return run(QStringLiteral("feature.suppress"), jsonText(args), suppressed ? trt("Suppress") : trt("Unsuppress"));
}

bool FeatureTreeModel::remove(int row)
{
    const auto id = featureAt(row);
    if (!id)
        return false;
    if (tool_ && tool_->active() && tool_->featureId() == id) {
        setMessage(trt("Close the feature's panel first"));
        return false;
    }
    const QJsonObject args{{QStringLiteral("id"), idText(id->value)}};
    return run(QStringLiteral("feature.remove"), jsonText(args), trt("Delete"));
}

bool FeatureTreeModel::edit(int row)
{
    const auto id = featureAt(row);
    if (!id)
        return false;
    const Row& r = rows_[static_cast<std::size_t>(row)];
    if (r.sketch)
        return sketchTool_ && sketchTool_->editFeature(idText(id->value));
    if (!tool_ && engine_ != nullptr)
        tool_ =
            engine_->singletonInstance<FeatureToolModel*>(QStringLiteral("RayCad.Ui"), QStringLiteral("FeatureTool"));
    if (r.type == QLatin1String("import")) {
        setMessage(trt("An imported body has no properties to edit"));
        return false;
    }
    if (r.type == QLatin1String("transform_body")) { // T-077
        setMessage(trt("%1 places a body: change it in the CAM workspace's Fixtures panel (Shift+F)").arg(r.name));
        return false;
    }
    if (rollback_ >= 0 && row > rollback_) {
        setMessage(trt("%1 is rolled back: move the rollback bar below it to edit it").arg(r.name));
        return false;
    }
    // T-083: the direct edits open in their own panel (model/directedit_tool.h); a face deletion has no value to
    // change.
    if (r.type == QLatin1String("delete_face")) {
        setMessage(trt("%1 removes faces and has no value to change: delete it and remove other faces with a new one")
                       .arg(r.name));
        return false;
    }
    if (r.type == QLatin1String("offset_face") || r.type == QLatin1String("resize_hole")) {
        auto* directEdit = engine_ != nullptr ? engine_->singletonInstance<DirectEditModel*>(
                                                    QStringLiteral("RayCad.Ui"), QStringLiteral("DirectEdit"))
                                              : nullptr;
        return directEdit != nullptr && directEdit->edit(idText(id->value));
    }
    return tool_ && tool_->edit(idText(id->value));
}

// ---------- reorder ----------
QString FeatureTreeModel::quickMoveProblem(const doc::DocState& state, int from, int to) const
{
    api::IApp* a = app();
    if (a == nullptr || from < 0 || from >= static_cast<int>(state.features.size()) || to < 0 ||
        to >= static_cast<int>(state.features.size()))
        return trt("There is no such place in the history");
    const doc::Feature& moved = state.features[static_cast<std::size_t>(from)];
    const QJsonObject args{{QStringLiteral("id"), idText(moved.id.value)}, {QStringLiteral("index"), to}};
    const doc::ICommandHandler* reorder = a->commandTypes().find("feature.reorder");
    if (reorder == nullptr)
        return trt("Reordering is not available");
    auto shared = std::make_shared<const doc::DocState>(state);
    auto next = reorder->apply(shared, jsonText(args));
    if (!next)
        return qs(next.error().message);
    const doc::DocState& s = **next;
    // Every feature names only what comes before it: the checks its own commands make.
    for (const doc::Feature& f : s.features) {
        const doc::ICommandHandler* edit = a->commandTypes().find("feature." + f.type + ".edit");
        if (edit == nullptr || f.name.empty())
            continue;
        const QJsonObject e{{QStringLiteral("id"), idText(f.id.value)}, {QStringLiteral("name"), qs(f.name)}};
        if (auto ok = edit->apply(*next, jsonText(e)); !ok)
            return qs(ok.error().message);
    }
    // The bodies a feature works on are made before it.
    std::map<QString, std::size_t> madeAt;
    for (std::size_t i = 0; i < s.features.size(); ++i) {
        for (const QString& b : bodiesMade(s.features[i]))
            madeAt.emplace(b, i);
    }
    for (std::size_t i = 0; i < s.features.size(); ++i) {
        for (const QString& b : bodiesRead(s.features[i])) {
            const auto it = madeAt.find(b);
            if (it != madeAt.end() && it->second > i) {
                return trt("%1 works on a body that %2 makes; it cannot come before it")
                    .arg(qs(s.features[i].name), qs(s.features[it->second].name));
            }
        }
    }
    return {};
}

QString FeatureTreeModel::moveProblem(int from, int to) const
{
    doc::Document* d = liveDoc();
    if (d == nullptr)
        return trt("No document is open");
    return quickMoveProblem(*d->state(), from, to);
}

bool FeatureTreeModel::moveTo(int from, int to)
{
    doc::Document* d = liveDoc();
    if (d == nullptr || from == to)
        return false;
    if (checking_) {
        setMessage(trt("Another reorder is being checked"));
        return false;
    }
    if (tool_ && tool_->active()) {
        setMessage(trt("Close the feature panel before reordering"));
        return false;
    }
    if (const QString problem = quickMoveProblem(*d->state(), from, to); !problem.isEmpty()) {
        setMessage(trt("Cannot move it there: %1").arg(problem));
        return false;
    }
    // The order is checked by building it off to the side: a feature that built before must still build.
    const doc::BuildResultPtr build = d->build();
    checkBefore_.clear();
    if (build) {
        for (const doc::FeatureReport& r : build->reports)
            checkBefore_[r.id] = r.status;
    }
    checkId_ = rows_[static_cast<std::size_t>(from)].id;
    checkTo_ = static_cast<std::size_t>(to);
    checkArgs_ = jsonText(QJsonObject{{QStringLiteral("id"), idText(checkId_.value)}, {QStringLiteral("index"), to}});
    checking_ = true;
    setMessage(QString());
    emit messageChanged();
    d->beginPreview(doc::Command{"feature.reorder", checkArgs_, "Reorder"});
    if (const auto error = d->previewError()) {
        checking_ = false;
        d->cancelPreview();
        setMessage(trt("Cannot move it there: %1").arg(qs(error->message)));
        return false;
    }
    return true;
}

void FeatureTreeModel::finishReorderCheck(doc::DocStatePtr state, doc::BuildResultPtr build)
{
    checking_ = false;
    doc::Document* d = liveDoc();
    if (d != nullptr)
        d->cancelPreview();
    if (!state || !build) {
        setMessage(trt("Cannot move it there: the new order could not be built"));
        return;
    }
    for (const doc::FeatureReport& r : build->reports) {
        const auto before = checkBefore_.find(r.id);
        const bool wasGood = before == checkBefore_.end() || before->second == doc::FeatureStatus::Ok ||
                             before->second == doc::FeatureStatus::Warning;
        const bool bad = r.status == doc::FeatureStatus::Failed || r.status == doc::FeatureStatus::NotBuilt;
        if (wasGood && bad && before != checkBefore_.end()) {
            QString name;
            for (const doc::Feature& f : state->features) {
                if (f.id == r.id)
                    name = qs(f.name);
            }
            setMessage(trt("Cannot move it there: %1 would not build (%2)")
                           .arg(name, r.message.empty() ? trt("an earlier feature it needs fails") : qs(r.message)));
            return;
        }
    }
    run(QStringLiteral("feature.reorder"), checkArgs_, trt("Reorder"));
}

// ---------- rollback ----------
bool FeatureTreeModel::rollTo(int index)
{
    if (index < -1 || index >= count() - 1)
        index = -1;
    if (index == rollback_)
        return false;
    if (tool_ && tool_->active()) {
        setMessage(trt("Close the feature panel before moving the rollback bar"));
        return false;
    }
    QJsonObject args;
    if (const auto id = featureAt(index))
        args.insert(QStringLiteral("to"), idText(id->value));
    return run(QStringLiteral("feature.rollback"), jsonText(args), index < 0 ? trt("Roll forward") : trt("Roll back"));
}

// ---------- sketches in the view ----------
bool FeatureTreeModel::sketchShown(const FeatureId& id) const
{
    if (toolSketch_ && *toolSketch_ == id)
        return true;
    const auto o = sketchOverride_.find(id);
    if (o != sketchOverride_.end())
        return o->second;
    // Hidden once a later feature uses it.
    if (!shownState_)
        return true;
    bool after = false;
    const QString text = idText(id.value);
    for (const doc::Feature& f : shownState_->features) {
        if (f.id == id) {
            after = true;
            continue;
        }
        if (after && !f.suppressed && jsonObject(f.propsJson).value(QStringLiteral("sketch")).toString() == text &&
            f.type != "sketch")
            return false;
    }
    return true;
}

void FeatureTreeModel::setSketchShown(int row, bool shown)
{
    const auto id = featureAt(row);
    if (!id)
        return;
    sketchOverride_[*id] = shown;
    emit dataChanged(index(row), index(row), {SketchShownRole});
    updateSketchLines();
}

void FeatureTreeModel::setToolSketch(std::optional<FeatureId> sketch)
{
    if (sketch == toolSketch_)
        return;
    toolSketch_ = sketch;
    if (!rows_.empty())
        emit dataChanged(index(0), index(count() - 1), {SketchShownRole});
    updateSketchLines();
}

void FeatureTreeModel::updateSketchLines()
{
    sketchJob_.cancel();
    sketchJob_ = {};
    const std::uint64_t serial = ++sketchSerial_;
    doc::Document* d = liveDoc();
    IJobSystem* jobs = scene_ ? scene_->jobs() : nullptr;
    if (d == nullptr || jobs == nullptr) {
        sketchLines_->clear();
        return;
    }
    const doc::DocStatePtr state = d->state();
    const doc::BuildResultPtr build = d->build();
    std::vector<FeatureId> shown;
    for (std::size_t i = 0; i < state->features.size(); ++i) {
        const doc::Feature& f = state->features[i];
        if (f.type != "sketch" || f.suppressed)
            continue;
        if (rollback_ >= 0 && static_cast<int>(i) > rollback_)
            continue;
        if (editedSketch_ && *editedSketch_ == f.id)
            continue;
        if (sketchShown(f.id))
            shown.push_back(f.id);
    }
    if (shown.empty()) {
        sketchLines_->clear();
        return;
    }
    sketchJob_ = jobs->submit<std::shared_ptr<const std::vector<SketchLines>>>(
        JobInfo{"Sketch lines", JobPriority::Interactive, false},
        [state, build, shown](JobContext& ctx) -> Result<std::shared_ptr<const std::vector<SketchLines>>> {
            auto out = std::make_shared<std::vector<SketchLines>>();
            for (const FeatureId& id : shown) {
                if (auto ok = ctx.cancel.check(); !ok)
                    return tl::unexpected(ok.error());
                auto model = api::sketch_edit::modelOf(*state, id);
                auto planeJson = api::sketch_edit::planeJsonOf(*state, id);
                if (!model || !planeJson)
                    continue;
                auto plane = api::sketch_edit::resolvePlane(*state, build.get(), *planeJson, ctx);
                if (!plane)
                    continue;
                SketchLines lines;
                lines.id = id;
                const sketchui::SketchView view(*model);
                for (const sketchui::ViewCurve& c : view.curves()) {
                    std::vector<Vec3> w;
                    for (const Vec2& p : c.poly)
                        w.push_back(worldOf(*plane, p));
                    appendPolyline(c.construction ? lines.construction : lines.geometry, w);
                }
                const auto bounds = view.bounds();
                double size = 0.5;
                if (bounds) {
                    const double dx = bounds->second.x - bounds->first.x, dy = bounds->second.y - bounds->first.y;
                    size = std::max(0.5, std::hypot(dx, dy) * 0.01);
                }
                for (const sketchui::ViewPoint& p : view.points()) {
                    if (p.used)
                        continue;
                    const Vec2 c = p.p;
                    for (const auto& [a, b] : {std::pair<Vec2, Vec2>{{c.x - size, c.y}, {c.x + size, c.y}},
                                               std::pair<Vec2, Vec2>{{c.x, c.y - size}, {c.x, c.y + size}}}) {
                        lines.points.push_back(worldOf(*plane, a));
                        lines.points.push_back(worldOf(*plane, b));
                    }
                }
                out->push_back(std::move(lines));
            }
            return std::shared_ptr<const std::vector<SketchLines>>(std::move(out));
        });
    QPointer<FeatureTreeModel> self(this);
    sketchJob_.then([self, serial](Result<std::shared_ptr<const std::vector<SketchLines>>> result) {
        if (!self || serial != self->sketchSerial_)
            return;
        self->sketchJob_ = {};
        if (!result)
            return;
        std::set<QString> keys;
        for (const SketchLines& s : **result) {
            const QString base = QStringLiteral("sketch:") + idText(s.id.value);
            self->sketchLines_->set(base + QStringLiteral(":g"), s.geometry, render::Token::SketchGeometry, 1.5f);
            self->sketchLines_->set(base + QStringLiteral(":c"), s.construction, render::Token::SketchConstruction,
                                    1.0f, true);
            self->sketchLines_->set(base + QStringLiteral(":p"), s.points, render::Token::SketchGeometry, 2.0f);
            keys.insert(base);
        }
        // Sketches no longer shown (hidden, deleted, undone).
        for (const QString& key : self->sketchLines_->keys()) {
            if (keys.count(key.section(QLatin1Char(':'), 0, 1)) == 0)
                self->sketchLines_->remove(key);
        }
    });
}

// ---------- datum planes in the view ----------
bool FeatureTreeModel::datumShown(const FeatureId& id) const
{
    const auto o = datumOverride_.find(id);
    return o == datumOverride_.end() || o->second;
}

void FeatureTreeModel::setDatumShown(int row, bool shown)
{
    const auto id = featureAt(row);
    if (!id)
        return;
    datumOverride_[*id] = shown;
    emit dataChanged(index(row), index(row), {DatumShownRole});
    updateDatumLines();
}

bool FeatureTreeModel::planeWanted() const
{
    return (sketchTool_ && sketchTool_->placing()) ||
           (tool_ && tool_->active() && tool_->type() == QLatin1String("datum_plane"));
}

void FeatureTreeModel::updateDatumLines()
{
    datumJob_.cancel();
    datumJob_ = {};
    const std::uint64_t serial = ++datumSerial_;
    doc::Document* d = liveDoc();
    IJobSystem* jobs = scene_ ? scene_->jobs() : nullptr;
    if (d == nullptr || jobs == nullptr) {
        datums_.clear();
        datumLines_->clear();
        return;
    }
    const doc::DocStatePtr state = d->state();
    const doc::BuildResultPtr build = d->build();
    // The datum plane tool's own datum is drawn by its preview.
    std::optional<FeatureId> edited;
    if (tool_ && tool_->active() && tool_->editing() && tool_->type() == QLatin1String("datum_plane"))
        edited = tool_->featureId();
    std::vector<FeatureId> shown;
    for (std::size_t i = 0; i < state->features.size(); ++i) {
        const doc::Feature& f = state->features[i];
        if (f.type != "datum_plane" || f.suppressed)
            continue;
        if (rollback_ >= 0 && static_cast<int>(i) > rollback_)
            continue; // not built
        if ((edited && *edited == f.id) || !datumShown(f.id))
            continue;
        shown.push_back(f.id);
    }
    if (shown.empty()) {
        datums_.clear();
        drawDatums();
        return;
    }
    // Resolving a plane on a face reads the kernel: on a worker.
    datumJob_ = jobs->submit<std::shared_ptr<const std::vector<DatumPlane>>>(
        JobInfo{"Datum planes", JobPriority::Interactive, false},
        [state, build, shown](JobContext& ctx) -> Result<std::shared_ptr<const std::vector<DatumPlane>>> {
            auto out = std::make_shared<std::vector<DatumPlane>>();
            for (const FeatureId& id : shown) {
                if (auto ok = ctx.cancel.check(); !ok)
                    return tl::unexpected(ok.error());
                const std::string planeJson = "{\"datum\":\"" + id.value.toString() + "\"}";
                auto plane = api::sketch_edit::resolvePlane(*state, build.get(), planeJson, ctx);
                if (plane)
                    out->push_back(DatumPlane{id, *plane});
            }
            return std::shared_ptr<const std::vector<DatumPlane>>(std::move(out));
        });
    QPointer<FeatureTreeModel> self(this);
    datumJob_.then([self, serial](Result<std::shared_ptr<const std::vector<DatumPlane>>> result) {
        if (!self || serial != self->datumSerial_)
            return;
        self->datumJob_ = {};
        if (!result)
            return;
        self->datums_ = **result;
        self->drawDatums();
    });
}

namespace {

// The bodies shown, as one box (from their display meshes); nullopt when none is meshed.
std::optional<Box3> modelBox(const SceneController& scene)
{
    std::vector<render::PickHit> hits;
    for (int i = 0; i < scene.bodyCount(); ++i) {
        if (const render::ItemId item = scene.itemForBody(i); item != 0 && scene.bodyVisible(i))
            hits.push_back(render::PickHit{item, kernel::SubKind::Solid, 0, Vec3{}, 0});
    }
    return hits.empty() ? std::nullopt : scene.boundsOf(hits);
}

// The centre of a datum's frame: the model's centre projected onto the plane (the plane's origin without a model).
Vec3 frameCentre(const kernel::Plane& plane, const std::optional<Box3>& box)
{
    if (!box)
        return plane.origin;
    const Vec3 c{(box->min.x + box->max.x) / 2, (box->min.y + box->max.y) / 2, (box->min.z + box->max.z) / 2};
    return sub(c, mul(plane.normal, dot(sub(c, plane.origin), plane.normal)));
}

} // namespace

void FeatureTreeModel::drawDatums()
{
    if (!datumLines_)
        return;
    const std::optional<Box3> box = scene_ ? modelBox(*scene_) : std::nullopt;
    // Half the frame's side: a little more than half the model's diagonal, so the frame shows around the model.
    datumHalf_ = box ? std::max(5.0, 0.6 * length(sub(box->max, box->min))) : 25.0;
    const bool wanted = planeWanted();
    const render::Token colour = wanted ? render::Token::Accent : render::Token::AccentThinLine;
    std::set<QString> keys;
    for (const DatumPlane& dp : datums_) {
        const kernel::Plane& p = dp.plane;
        const Vec3 c = frameCentre(p, box);
        const Vec3 x = p.xDir;
        const Vec3 y = yDirOf(p);
        const auto at = [&](double u, double v) {
            return add(c, add(mul(x, u), mul(y, v)));
        };
        const auto square = [&](double h) {
            std::vector<Vec3> segs;
            appendPolyline(segs, {at(-h, -h), at(h, -h), at(h, h), at(-h, h), at(-h, -h)});
            return segs;
        };
        const double h = datumHalf_;
        const QString base = QStringLiteral("datum:") + idText(dp.id.value);
        datumLines_->set(base + QStringLiteral(":frame"), square(h), colour, wanted ? 2.0f : 1.25f);
        datumLines_->set(base + QStringLiteral(":inner"), square(h * 0.85), colour, 1.0f, true);
        datumLines_->set(base + QStringLiteral(":normal"), {c, add(c, mul(p.normal, h * 0.2))}, colour,
                         wanted ? 2.0f : 1.25f);
        keys.insert(base);
    }
    for (const QString& key : datumLines_->keys()) {
        if (keys.count(key.section(QLatin1Char(':'), 0, 1)) == 0)
            datumLines_->remove(key);
    }
}

std::optional<FeatureId> FeatureTreeModel::datumAt(const render::Ray& ray) const
{
    const std::optional<Box3> box = scene_ ? modelBox(*scene_) : std::nullopt;
    std::optional<FeatureId> best;
    double bestT = 0;
    for (const DatumPlane& dp : datums_) {
        const kernel::Plane& p = dp.plane;
        const double denom = dot(ray.dir, p.normal);
        if (std::abs(denom) < 1e-9)
            continue; // edge-on
        const double t = dot(sub(p.origin, ray.origin), p.normal) / denom;
        if (t <= 0)
            continue;
        const Vec3 hit = add(ray.origin, mul(ray.dir, t));
        const Vec3 d = sub(hit, frameCentre(p, box));
        if (std::abs(dot(d, p.xDir)) > datumHalf_ || std::abs(dot(d, yDirOf(p))) > datumHalf_)
            continue;
        if (!best || t < bestT) {
            best = dp.id;
            bestT = t;
        }
    }
    return best;
}

bool FeatureTreeModel::pickDatumAt(qreal x, qreal y)
{
    auto* view = qobject_cast<ViewportItem*>(view_.data());
    if (view == nullptr || view->width() <= 0 || view->height() <= 0 || !planeWanted())
        return false;
    const render::Ray ray =
        render::cameraRay(view->camera(), x, y, static_cast<int>(view->width()), static_cast<int>(view->height()));
    const std::optional<FeatureId> datum = datumAt(ray);
    if (!datum)
        return false;
    const QString key = QStringLiteral("datum:") + idText(datum->value);
    if (sketchTool_ && sketchTool_->placing()) {
        sketchTool_->placeOn(key); // the new sketch goes on it
        return true;
    }
    if (tool_ && tool_->active() && tool_->type() == QLatin1String("datum_plane")) {
        tool_->setValue(QStringLiteral("base"), key); // the new plane is offset from it
        return true;
    }
    return false;
}

} // namespace ray::ui
