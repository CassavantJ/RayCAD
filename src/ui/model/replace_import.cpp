#include "replace_import.h"

#include "../app_context.h"
#include "../palette_commands.h"
#include "../scene_controller.h"
#include "../selection_model.h"
#include "../session.h"
#include "../ui_commands.h"
#include "feature_tree.h"
#include "model_util.h"

#include <ray_api/replace_import.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTimer>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <utility>

namespace ray::ui {

using namespace modelui;

namespace {
// T-084: the replace dialog (model/ReplaceImport.qml). In every workspace: a revision may come in while the CAM
// workspace is open.
// clang-format off
constexpr UiCommand kReplaceImportCommands[] = {
    {"replaceImport.open", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Replace with New Revision..."),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Model"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Replaces an imported part's STEP or IGES file with a new revision: unchanged faces keep their references, changed, new and removed faces are listed"), "", CommandContext::Global, "file.replaceImport"},
};
// clang-format on

QString trr(const char* text)
{
    return QCoreApplication::translate("ray::ui::ReplaceImport", text);
}
// One of two texts by count (English source texts; a translation file may give more forms).
QString count(int n, const char* one, const char* many)
{
    return n == 1 ? QCoreApplication::translate("ray::ui::ReplaceImport", one)
                  : QCoreApplication::translate("ray::ui::ReplaceImport", many).arg(n);
}

std::vector<Hash256> hashesOf(const QJsonArray& list)
{
    std::vector<Hash256> out;
    for (const QJsonValue& v : list) {
        const std::string hex = v.toString().toStdString();
        if (hex.size() != 64)
            continue;
        Hash256 h;
        bool ok = true;
        for (std::size_t i = 0; i < 32 && ok; ++i) {
            auto nibble = [&](char c) -> int {
                if (c >= '0' && c <= '9')
                    return c - '0';
                if (c >= 'a' && c <= 'f')
                    return c - 'a' + 10;
                ok = false;
                return 0;
            };
            h.bytes[i] = static_cast<std::uint8_t>(nibble(hex[2 * i]) * 16 + nibble(hex[2 * i + 1]));
        }
        if (ok)
            out.push_back(h);
    }
    return out;
}

// The 12 edges of a box, as segments.
std::vector<Vec3> boxSegments(const Box3& b)
{
    const Vec3 c[8] = {{b.min.x, b.min.y, b.min.z}, {b.max.x, b.min.y, b.min.z}, {b.max.x, b.max.y, b.min.z},
                       {b.min.x, b.max.y, b.min.z}, {b.min.x, b.min.y, b.max.z}, {b.max.x, b.min.y, b.max.z},
                       {b.max.x, b.max.y, b.max.z}, {b.min.x, b.max.y, b.max.z}};
    const int edges[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6},
                              {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    std::vector<Vec3> out;
    for (const auto& e : edges) {
        out.push_back(c[e[0]]);
        out.push_back(c[e[1]]);
    }
    return out;
}

// The triangles of `faces` of a display mesh, lifted off the surface along the normals by `lift` so they draw over it.
std::shared_ptr<const kernel::Mesh> tintOf(const kernel::Mesh& mesh, const std::set<std::uint32_t>& faces, double lift)
{
    auto out = std::make_shared<kernel::Mesh>();
    std::map<std::uint32_t, std::uint32_t> remap;
    for (const kernel::FaceRange& r : mesh.faces) {
        if (r.face.kind != kernel::SubKind::Face || faces.count(r.face.index) == 0)
            continue;
        const auto first = static_cast<std::uint32_t>(out->indices.size());
        for (std::uint32_t k = 0; k < r.indexCount; ++k) {
            const std::size_t at = static_cast<std::size_t>(r.firstIndex) + k;
            if (at >= mesh.indices.size())
                break;
            const std::uint32_t v = mesh.indices[at];
            auto [it, added] = remap.emplace(v, static_cast<std::uint32_t>(out->positions.size() / 3));
            if (added) {
                const std::size_t p = 3 * static_cast<std::size_t>(v);
                const bool hasNormal = p + 2 < mesh.normals.size();
                for (int c = 0; c < 3; ++c) {
                    const float n = hasNormal ? mesh.normals[p + c] : 0.0f;
                    out->positions.push_back(mesh.positions[p + c] + static_cast<float>(lift) * n);
                    out->normals.push_back(n);
                }
            }
            out->indices.push_back(it->second);
        }
        out->faces.push_back(kernel::FaceRange{r.face, first, static_cast<std::uint32_t>(out->indices.size()) - first});
    }
    return out;
}

} // namespace

std::span<const UiCommand> replaceUiCommands()
{
    return kReplaceImportCommands;
}

ReplaceImportModel::ReplaceImportModel(SceneController* scene, SelectionModel* selection, Session* session,
                                       AppContext* context, QObject* parent)
    : QObject(parent), scene_(scene), selection_(selection), session_(session), context_(context),
      lines_(std::make_unique<LineOverlay>(scene))
{
    if (session_) {
        connect(session_, &Session::documentChanged, this, [this] {
            refreshImports();
            if (session_ && session_->documentId().toStdString() != docId_ && hasResult_)
                clearResult();
        });
    }
    if (scene_) {
        const auto retint = [this] {
            if (hasResult_ && shown_)
                QTimer::singleShot(0, this, [this] { onBuild(); });
        };
        connect(scene_, &SceneController::stateChanged, this, retint);
        connect(scene_, &SceneController::bodiesChanged, this, retint);
    }
}

ReplaceImportModel::~ReplaceImportModel()
{
    if (commands_)
        commands_->setUiHandler(QStringLiteral("replaceImport."), {});
    clearTints();
    lines_.reset();
    unwatch();
}

ReplaceImportModel* ReplaceImportModel::create(QQmlEngine* qml, QJSEngine*)
{
    SceneController* scene = nullptr;
    SelectionModel* selection = nullptr;
    Session* session = nullptr;
    AppContext* context = nullptr;
    CommandCatalog* commands = nullptr;
    FeatureTreeModel* tree = nullptr;
    if (qml != nullptr) {
        const QString uri = QStringLiteral("RayCad.Ui");
        scene = qml->singletonInstance<SceneController*>(uri, QStringLiteral("Scene"));
        selection = qml->singletonInstance<SelectionModel*>(uri, QStringLiteral("Selection"));
        session = qml->singletonInstance<Session*>(uri, QStringLiteral("Session"));
        context = qml->singletonInstance<AppContext*>(uri, QStringLiteral("AppContext"));
        commands = qml->singletonInstance<CommandCatalog*>(uri, QStringLiteral("Commands"));
        tree = qml->singletonInstance<FeatureTreeModel*>(uri, QStringLiteral("FeatureTree"));
    }
    auto* model = new ReplaceImportModel(scene, selection, session, context);
    model->setCommands(commands);
    model->setFeatureTree(tree);
    return model; // owned by the engine
}

void ReplaceImportModel::setCommands(CommandCatalog* commands)
{
    commands_ = commands;
    if (!commands_)
        return;
    QPointer<ReplaceImportModel> self(this);
    commands_->setUiHandler(
        QStringLiteral("replaceImport."),
        CommandCatalog::UiHandler{
            [self](const QString& id) { return self ? self->commandReason(id) : trr("Replacing is not available"); },
            [self](const QString& id) { return self && self->runCommand(id); }});
}

void ReplaceImportModel::setFeatureTree(FeatureTreeModel* tree)
{
    tree_ = tree;
}

api::IApp* ReplaceImportModel::app() const
{
    return context_ ? context_->app() : nullptr;
}

doc::Document* ReplaceImportModel::liveDoc() const
{
    api::IApp* a = app();
    if (a == nullptr || docId_.empty() || doc_ == nullptr)
        return nullptr;
    api::OpenDocument* od = a->findDocument(docId_);
    return od != nullptr && od->document.get() == doc_ ? doc_ : nullptr;
}

// ---------- the dialog ----------
void ReplaceImportModel::setShown(bool shown)
{
    if (shown == shown_)
        return;
    shown_ = shown;
    if (!shown_) {
        clearTints();
        setCurrent(-1);
    } else {
        onBuild();
    }
    emit shownChanged();
}

void ReplaceImportModel::refreshImports()
{
    QVariantList list;
    api::OpenDocument* od = session_ ? session_->activeDocument() : nullptr;
    if (od != nullptr && od->document) {
        const doc::DocStatePtr state = od->document->state();
        for (const doc::Feature* f : api::replace_import::replaceableImports(*state)) {
            QVariantMap item;
            item.insert(QStringLiteral("id"), idText(f->id.value));
            item.insert(QStringLiteral("name"), qs(f->name));
            const auto props = QJsonDocument::fromJson(QByteArray::fromStdString(f->propsJson)).object();
            item.insert(QStringLiteral("file"),
                        QFileInfo(props.value(QStringLiteral("sourceFile")).toString()).fileName());
            list.push_back(item);
        }
    }
    if (list != imports_) {
        imports_ = list;
        emit importsChanged();
        emit featureChanged();
    }
}

void ReplaceImportModel::setFeature(const QString& id)
{
    if (id == feature_)
        return;
    feature_ = id;
    emit featureChanged();
    emit stateChanged();
}

QString ReplaceImportModel::featureName() const
{
    for (const QVariant& v : imports_)
        if (v.toMap().value(QStringLiteral("id")).toString() == feature_)
            return v.toMap().value(QStringLiteral("name")).toString();
    return {};
}

QString ReplaceImportModel::currentFile() const
{
    for (const QVariant& v : imports_)
        if (v.toMap().value(QStringLiteral("id")).toString() == feature_)
            return v.toMap().value(QStringLiteral("file")).toString();
    return {};
}

void ReplaceImportModel::setPath(const QString& pathOrUrl)
{
    QString path = pathOrUrl;
    if (path.startsWith(QStringLiteral("file:")))
        path = QUrl(path).toLocalFile();
    if (path == path_)
        return;
    path_ = path;
    emit pathChanged();
    emit stateChanged();
}

QString ReplaceImportModel::fileName() const
{
    return path_.isEmpty() ? QString() : QFileInfo(path_).fileName();
}

void ReplaceImportModel::open()
{
    refreshImports();
    QString pick;
    if (tree_) {
        if (const auto id = tree_->currentFeatureId()) {
            const QString text = idText(id->value);
            for (const QVariant& v : imports_)
                if (v.toMap().value(QStringLiteral("id")).toString() == text)
                    pick = text;
        }
    }
    if (pick.isEmpty() && imports_.size() == 1)
        pick = imports_.front().toMap().value(QStringLiteral("id")).toString();
    if (!busy_) {
        clearResult();
        setPath(QString());
        setMessage(QString(), false);
    }
    setFeature(pick);
    setShown(true);
}

void ReplaceImportModel::close()
{
    setShown(false);
}

void ReplaceImportModel::chooseFile()
{
    QUrl folder;
    for (const QString& p : {path_, currentFile()}) {
        if (!p.isEmpty() && QFileInfo(p).isAbsolute()) {
            folder = QUrl::fromLocalFile(QFileInfo(p).path());
            break;
        }
    }
    emit fileDialogRequested(folder);
}

QString ReplaceImportModel::commandReason(const QString&) const
{
    api::OpenDocument* od = session_ ? session_->activeDocument() : nullptr;
    if (od == nullptr || !od->document)
        return trr("Open a document with an imported part first");
    if (api::replace_import::replaceableImports(*od->document->state()).empty())
        return trr("The document has no imported part to replace");
    return {};
}

bool ReplaceImportModel::runCommand(const QString& id)
{
    if (id != QStringLiteral("replaceImport.open") || !commandReason(id).isEmpty())
        return false;
    open();
    return true;
}

void ReplaceImportModel::setBusy(bool busy)
{
    if (busy == busy_)
        return;
    busy_ = busy;
    emit busyChanged();
    emit stateChanged();
}

void ReplaceImportModel::setMessage(const QString& message, bool error)
{
    if (message == message_ && error == messageIsError_)
        return;
    message_ = message;
    messageIsError_ = error;
    emit messageChanged();
}

bool ReplaceImportModel::replace()
{
    api::IApp* a = app();
    api::OpenDocument* od = session_ ? session_->activeDocument() : nullptr;
    if (a == nullptr || od == nullptr || !canReplace())
        return false;
    QJsonObject args;
    args.insert(QStringLiteral("document"), qs(od->id));
    args.insert(QStringLiteral("feature"), feature_);
    args.insert(QStringLiteral("path"), path_);
    const QByteArray json = QJsonDocument(args).toJson(QJsonDocument::Compact);
    JobHandle<api::ActionResult> job = a->actions().invoke(api::replace_import::kAction, sv(json), api::Caller::User);
    if (!job.valid()) {
        setMessage(trr("The part could not be replaced."), true);
        return false;
    }
    clearResult();
    setMessage(trr("Reading %1 and comparing it with the part...").arg(fileName()), false);
    setBusy(true);
    if (session_)
        session_->refreshJobs();
    const std::uint64_t serial = ++serial_;
    QPointer<ReplaceImportModel> self(this);
    job.then([self, serial](Result<api::ActionResult> r) {
        if (!self || serial != self->serial_)
            return;
        self->setBusy(false);
        if (!r) {
            if (r.error().code == ErrorCode::Cancelled)
                self->setMessage(trr("Replacing was cancelled; nothing changed."), false);
            else
                self->setMessage(qs(r.error().message) + QLatin1Char(' ') + trr("Nothing was changed."), true);
            return;
        }
        self->setMessage(QString(), false);
        self->refreshImports(); // the import's file is the new one now
        self->takeResult(r->resultJson);
        emit self->replaced();
    });
    return true;
}

bool ReplaceImportModel::undo()
{
    api::IApp* a = app();
    doc::Document* d = liveDoc();
    if (a == nullptr || d == nullptr || !resultState_ || d->state() != resultState_)
        return false;
    QJsonObject args;
    args.insert(QStringLiteral("document"), qs(docId_));
    const QByteArray json = QJsonDocument(args).toJson(QJsonDocument::Compact);
    JobHandle<api::ActionResult> job = a->actions().invoke("edit.undo", sv(json), api::Caller::User);
    if (!job.valid())
        return false;
    QPointer<ReplaceImportModel> self(this);
    job.then([self](Result<api::ActionResult> r) {
        if (!self)
            return;
        if (!r) {
            self->setMessage(qs(r.error().message), true);
            return;
        }
        self->clearResult();
        self->refreshImports();
        self->setMessage(trr("The replace was undone: the part is back to its previous revision."), false);
    });
    return true;
}

// ---------- the result ----------
void ReplaceImportModel::watch()
{
    unwatch();
    api::IApp* a = app();
    api::OpenDocument* od = a != nullptr ? a->findDocument(docId_) : nullptr;
    if (od == nullptr || !od->document)
        return;
    doc_ = od->document.get();
    const std::uint64_t generation = ++generation_;
    QPointer<ReplaceImportModel> self(this);
    docListener_ = doc_->subscribe([self, generation](doc::DocStatePtr, doc::BuildResultPtr) {
        if (self && self->generation_ == generation)
            self->onBuild();
    });
}

void ReplaceImportModel::unwatch()
{
    if (doc::Document* d = liveDoc(); d != nullptr && docListener_ != 0)
        d->unsubscribe(docListener_);
    doc_ = nullptr;
    docListener_ = 0;
    ++generation_;
}

void ReplaceImportModel::clearResult()
{
    clearTints();
    unwatch();
    docId_.clear();
    resultState_.reset();
    resultJson_.clear();
    rows_.clear();
    rowList_.clear();
    current_ = -1;
    changed_ = added_ = removed_ = toolpaths_ = 0;
    summary_.clear();
    notes_.clear();
    if (built_) {
        built_ = false;
        emit builtChanged();
    }
    if (hasResult_) {
        hasResult_ = false;
        emit resultChanged();
        emit rowsChanged();
        emit currentChanged();
    }
}

void ReplaceImportModel::takeResult(const std::string& json)
{
    clearResult();
    const QJsonObject result = QJsonDocument::fromJson(QByteArray::fromStdString(json)).object();
    resultJson_ = json;
    docId_ = result.value(QStringLiteral("document")).toString().toStdString();
    watch();
    if (doc::Document* d = liveDoc())
        resultState_ = d->state();

    const QJsonObject counts = result.value(QStringLiteral("counts")).toObject();
    const QJsonObject faces = counts.value(QStringLiteral("faces")).toObject();
    changed_ = faces.value(QStringLiteral("changed")).toInt();
    added_ = faces.value(QStringLiteral("new")).toInt();
    removed_ = faces.value(QStringLiteral("removed")).toInt();
    toolpaths_ = result.value(QStringLiteral("toolpaths")).toInt();

    // "12 faces changed, 3 new, 1 removed"
    if (changed_ + added_ + removed_ == 0) {
        summary_ = trr("Every face is unchanged.");
    } else {
        QStringList parts;
        if (changed_ > 0)
            parts << count(changed_, "1 face changed", "%1 faces changed");
        if (added_ > 0)
            parts << (parts.isEmpty() ? count(added_, "1 new face", "%1 new faces") : trr("%1 new").arg(added_));
        if (removed_ > 0)
            parts << (parts.isEmpty() ? count(removed_, "1 face removed", "%1 faces removed")
                                      : trr("%1 removed").arg(removed_));
        summary_ = parts.join(QStringLiteral(", "));
    }
    int ec = 0, en = 0, er = 0;
    for (const char* kind : {"edges", "vertices"}) {
        const QJsonObject c = counts.value(QLatin1String(kind)).toObject();
        ec += c.value(QStringLiteral("changed")).toInt();
        en += c.value(QStringLiteral("new")).toInt();
        er += c.value(QStringLiteral("removed")).toInt();
    }
    if (ec + en + er > 0)
        notes_ << trr("Edges and corners: %1 changed, %2 new, %3 removed.").arg(ec).arg(en).arg(er);
    const QString oldUnits = result.value(QStringLiteral("oldUnits")).toString();
    const QString newUnits = result.value(QStringLiteral("newUnits")).toString();
    if (!oldUnits.isEmpty() && !newUnits.isEmpty() && oldUnits != newUnits) {
        const QString inch = trr("inches"), metric = trr("millimetres");
        notes_ << trr("The new file is in %1, the old one was in %2. Both are read in millimetres, so the change of "
                      "units by itself changes no size.")
                      .arg(newUnits == QStringLiteral("inch") ? inch : metric,
                           oldUnits == QStringLiteral("inch") ? inch : metric);
    }
    if (toolpaths_ > 0)
        notes_ << count(toolpaths_, "1 toolpath uses this part and needs regenerating.",
                        "%1 toolpaths use this part and need regenerating.");
    for (const QJsonValue& w : result.value(QStringLiteral("warnings")).toArray())
        notes_ << trr("From the new file: %1").arg(w.toString());

    for (const QJsonValue& bv : result.value(QStringLiteral("bodies")).toArray()) {
        const QJsonObject b = bv.toObject();
        const BodyId body{Uuid::fromString(b.value(QStringLiteral("body")).toString().toStdString())};
        for (const QJsonValue& fv : b.value(QStringLiteral("faces")).toArray()) {
            const QJsonObject f = fv.toObject();
            Row row;
            row.fate = f.value(QStringLiteral("fate")).toString();
            row.body = body;
            row.sub = kernel::SubId{kernel::SubKind::Face,
                                    static_cast<std::uint32_t>(f.value(QStringLiteral("index")).toInt())};
            row.names = hashesOf(f.value(QStringLiteral("names")).toArray());
            const QJsonArray box = f.value(QStringLiteral("box")).toArray();
            if (box.size() == 6)
                row.box = Box3{{box[0].toDouble(), box[1].toDouble(), box[2].toDouble()},
                               {box[3].toDouble(), box[4].toDouble(), box[5].toDouble()}};
            row.title = f.value(QStringLiteral("surface")).toString();
            row.detail = QString::number(f.value(QStringLiteral("radius")).toDouble(), 'g', 12);
            QVariantMap item;
            item.insert(QStringLiteral("fate"), row.fate);
            item.insert(QStringLiteral("surface"), row.title);
            item.insert(QStringLiteral("hole"), f.value(QStringLiteral("hole")).toBool());
            item.insert(QStringLiteral("diameter"), 2.0 * f.value(QStringLiteral("radius")).toDouble());
            item.insert(QStringLiteral("area"), f.value(QStringLiteral("area")).toDouble());
            item.insert(QStringLiteral("body"), b.value(QStringLiteral("name")).toString());
            item.insert(QStringLiteral("located"), row.fate == QStringLiteral("removed"));
            rows_.push_back(std::move(row));
            rowList_.push_back(item);
        }
    }
    hasResult_ = true;
    emit resultChanged();
    emit rowsChanged();
    emit currentChanged();
    onBuild();
}

int ReplaceImportModel::sceneIndexOf(const BodyId& body) const
{
    if (!scene_)
        return -1;
    for (int i = 0; i < scene_->bodyCount(); ++i) {
        const api::viewer::Body* b = scene_->body(i);
        if (b != nullptr && b->id == body)
            return i;
    }
    return -1;
}

// The build of the replaced state is the document's (and the scene shows it): find the rows' faces and tint them.
void ReplaceImportModel::onBuild()
{
    doc::Document* d = liveDoc();
    if (!hasResult_ || d == nullptr)
        return;
    if (d->state() != resultState_) {
        // Undone, or edited since: the summary no longer describes the view.
        clearTints();
        return;
    }
    const doc::BuildResultPtr build = d->build();
    if (!build || d->builtState() != resultState_)
        return;
    if (!built_) {
        built_ = true;
        emit builtChanged();
    }
    if (!shown_)
        return;
    bool changed = false;
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        Row& row = rows_[i];
        if (row.fate == QStringLiteral("removed"))
            continue;
        const auto located = api::replace_import::locate(*build, row.body, kernel::SubKind::Face, row.names);
        if (located != row.located) {
            row.located = located;
            QVariantMap item = rowList_[static_cast<int>(i)].toMap();
            item.insert(QStringLiteral("located"), located.has_value());
            rowList_[static_cast<int>(i)] = item;
            changed = true;
        }
    }
    if (scene_ && !scene_->meshing())
        applyTints();
    if (changed)
        emit rowsChanged();
}

void ReplaceImportModel::clearTints()
{
    render::IRenderViewport* viewport = scene_ ? scene_->viewport() : nullptr;
    if (viewport != nullptr && viewport == tintViewport_)
        for (const render::ItemId item : tintItems_)
            viewport->remove(item);
    const bool had = !tintItems_.empty() || (lines_ && lines_->count() > 0);
    tintItems_.clear();
    tintViewport_ = nullptr;
    tintChanged_ = tintNew_ = 0;
    if (lines_)
        lines_->clear();
    if (selection_ && !highlighted_.empty())
        selection_->setToolHighlight({});
    highlighted_.clear();
    if (had)
        emit rowsChanged();
}

void ReplaceImportModel::applyTints()
{
    clearTints();
    render::IRenderViewport* viewport = scene_ ? scene_->viewport() : nullptr;
    if (viewport == nullptr)
        return;
    tintViewport_ = viewport;
    // Per body and fate, the faces to tint.
    std::map<std::pair<int, bool>, std::set<std::uint32_t>> faces; // (scene index, new?) -> faces
    std::vector<Vec3> removedSegments;
    for (const Row& row : rows_) {
        if (row.fate == QStringLiteral("removed")) {
            if (row.box) {
                const std::vector<Vec3> segs = boxSegments(*row.box);
                removedSegments.insert(removedSegments.end(), segs.begin(), segs.end());
            }
            continue;
        }
        const int index = sceneIndexOf(row.body);
        if (index < 0 || !row.located)
            continue;
        faces[{index, row.fate == QStringLiteral("new")}].insert(row.located->index);
    }
    for (const auto& [key, set] : faces) {
        const auto mesh = scene_->meshOf(key.first);
        if (!mesh)
            continue;
        const api::viewer::Body* body = scene_->body(key.first);
        const Box3 b = body != nullptr ? kernel::bounds(body->shape) : Box3{};
        const double diagonal = length(sub(b.max, b.min));
        const auto tint = tintOf(*mesh, set, std::clamp(diagonal * 3e-4, 0.005, 0.2));
        if (tint->indices.empty())
            continue;
        const render::Token token = key.second ? render::Token::Success : render::Token::Warning;
        const render::ItemId item = viewport->addMesh(
            tint, Xform::identity(), render::MeshStyle{token, token, 1.0f, false, false, std::nullopt});
        if (item == 0)
            continue;
        viewport->setPickMode(item, render::PickMode::Never); // picks go to the model under it
        tintItems_.push_back(item);
        (key.second ? tintNew_ : tintChanged_) += static_cast<int>(set.size());
    }
    if (lines_ && !removedSegments.empty())
        lines_->set(QStringLiteral("replace.removed"), std::move(removedSegments), render::Token::Error, 1.5f, true);
    applyCurrent();
    emit rowsChanged();
}

void ReplaceImportModel::setCurrent(int row)
{
    if (row < -1 || row >= static_cast<int>(rows_.size()))
        row = -1;
    if (row == current_)
        return;
    current_ = row;
    applyCurrent();
    emit currentChanged();
}

void ReplaceImportModel::applyCurrent()
{
    if (lines_)
        lines_->remove(QStringLiteral("replace.current"));
    if (selection_ && !highlighted_.empty())
        selection_->setToolHighlight({});
    highlighted_.clear();
    if (current_ < 0 || current_ >= static_cast<int>(rows_.size()) || !tinted())
        return;
    const Row& row = rows_[static_cast<std::size_t>(current_)];
    if (row.fate == QStringLiteral("removed")) {
        if (row.box && lines_)
            lines_->set(QStringLiteral("replace.current"), boxSegments(*row.box), render::Token::Error, 3.0f, true);
        return;
    }
    if (!row.located || !scene_ || !selection_)
        return;
    const render::ItemId item = scene_->itemForBody(sceneIndexOf(row.body));
    if (item == 0)
        return;
    highlighted_.push_back(render::PickHit{item, kernel::SubKind::Face, row.located->index, Vec3{}, 0});
    selection_->setToolHighlight(highlighted_);
}

int ReplaceImportModel::tintedFaces(const QString& fate) const
{
    return fate == QStringLiteral("new") ? tintNew_ : fate == QStringLiteral("changed") ? tintChanged_ : 0;
}

int ReplaceImportModel::removedBoxes() const
{
    return lines_ && lines_->has(QStringLiteral("replace.removed")) ? removed_ : 0;
}

} // namespace ray::ui
