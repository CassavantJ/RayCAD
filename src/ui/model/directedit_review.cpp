#include "directedit_review.h"

#include "../app_context.h"
#include "../palette_commands.h"
#include "../scene_controller.h"
#include "../selection_model.h"
#include "../session.h"
#include "../ui_commands.h"
#include "directedit_tool.h"
#include "feature_tools.h"
#include "feature_tree.h" // feature_tools.h holds a QPointer to it
#include "model_util.h"

#include <ray_api/feature_edit.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QScopeGuard>

#include <algorithm>
#include <cmath>
#include <utility>

namespace ray::ui {

using namespace modelui;
namespace de = directedit;
namespace refs = api::references;

namespace {
// T-083: the reference review and the toolpaths a change made out of date. Both work in every workspace (the review
// lists CAM references too; a model edit makes toolpaths stale wherever it is made).
// clang-format off
constexpr UiCommand kReferenceCommands[] = {
    {"references.review", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Review References"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Model"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Lists the faces, edges and points that model edits split, merged, moved or removed under features and operations, to confirm or pick again"), "Ctrl+Shift+R", CommandContext::Global, ""},
    {"toolpaths.regenerateStale", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Regenerate Stale Toolpaths"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "CAM"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Generates every toolpath that a change made out of date, in every setup"), "Ctrl+Shift+G", CommandContext::Global, ""},
};
// clang-format on

QString trr(const char* text)
{
    return QCoreApplication::translate("ray::ui::ReferenceReview", text);
}

de::RefState stateOf(naming::ResolveState s)
{
    switch (s) {
    case naming::ResolveState::Exact:
        return de::RefState::Exact;
    case naming::ResolveState::Split:
        return de::RefState::Split;
    case naming::ResolveState::Merged:
        return de::RefState::Merged;
    case naming::ResolveState::Repaired:
        return de::RefState::Repaired;
    case naming::ResolveState::Broken:
        return de::RefState::Broken;
    }
    return de::RefState::Broken;
}

de::ElementKind kindOf(kernel::SubKind k)
{
    return k == kernel::SubKind::Edge     ? de::ElementKind::Edge
           : k == kernel::SubKind::Vertex ? de::ElementKind::Vertex
                                          : de::ElementKind::Face;
}

QString ownerKey(refs::Owner o)
{
    return o == refs::Owner::Feature     ? QStringLiteral("feature")
           : o == refs::Owner::Operation ? QStringLiteral("operation")
                                         : QStringLiteral("setup");
}

std::string keyOf(const refs::Reference& r)
{
    return r.ownerId + "|" + r.path + "|" + std::to_string(static_cast<int>(r.state));
}

de::ReviewItem itemOf(const refs::Reference& r)
{
    de::ReviewItem item;
    item.owner = r.owner == refs::Owner::Feature     ? de::OwnerKind::Feature
                 : r.owner == refs::Owner::Operation ? de::OwnerKind::Operation
                                                     : de::OwnerKind::Setup;
    item.ownerId = qs(r.ownerId);
    item.ownerName = qs(r.ownerName);
    item.setupName = qs(r.setupName);
    item.path = qs(r.path);
    item.ownerOrder = r.ownerOrder;
    item.index = r.index;
    item.of = r.of;
    item.state = stateOf(r.state);
    item.kind = kindOf(r.kind);
    item.elements = r.elements.size();
    item.confirmable = r.confirmable();
    item.reason = qs(r.reason);
    return item;
}

std::uint8_t filterOf(kernel::SubKind k)
{
    return static_cast<std::uint8_t>(k == kernel::SubKind::Edge     ? render::PickFilter::Edges
                                     : k == kernel::SubKind::Vertex ? render::PickFilter::Vertices
                                                                    : render::PickFilter::Faces);
}

// Whether the features (what a model edit changes) of two states differ.
bool featuresDiffer(const doc::DocState& a, const doc::DocState& b)
{
    if (a.features.size() != b.features.size() || a.rollback != b.rollback)
        return true;
    for (std::size_t i = 0; i < a.features.size(); ++i) {
        const doc::Feature& x = a.features[i];
        const doc::Feature& y = b.features[i];
        if (x.id != y.id || x.suppressed != y.suppressed || x.propsJson != y.propsJson)
            return true;
    }
    return false;
}

// Whether a state holds any reference at all (a cheap look before a listing job).
bool hasReferences(const doc::DocState& s)
{
    for (const doc::Feature& f : s.features) {
        if (f.type != "import" && f.propsJson.find("\"names\"") != std::string::npos)
            return true;
    }
    if (s.cam) {
        for (const cam::Setup& setup : s.cam->setups) {
            if (setup.wcs.zFace || setup.wcs.xEdge || setup.wcs.originVertex)
                return true;
        }
        for (const auto& [id, op] : s.cam->operations) {
            if (!op.geometry.empty())
                return true;
        }
    }
    return false;
}

} // namespace

std::span<const UiCommand> referenceUiCommands()
{
    return kReferenceCommands;
}

// ---------- construction ----------
ReferenceReviewModel::ReferenceReviewModel(SceneController* scene, SelectionModel* selection, Session* session,
                                           AppContext* context, QObject* parent)
    : QObject(parent), scene_(scene), selection_(selection), session_(session), context_(context),
      lines_(std::make_unique<LineOverlay>(scene))
{
    refreshTimer_.setSingleShot(true);
    refreshTimer_.setInterval(50); // one listing for the notifications of one change
    connect(&refreshTimer_, &QTimer::timeout, this, [this] { refresh(); });
    staleTimer_.setSingleShot(true);
    staleTimer_.setInterval(100); // the engine's notices come at up to 30 Hz while generating
    connect(&staleTimer_, &QTimer::timeout, this, [this] { refreshStale(); });
    if (session_) {
        connect(session_, &Session::documentChanged, this, [this] {
            if (!session_)
                return;
            if (session_->documentId().toStdString() != docId_) {
                subscribe();
                scheduleRefresh();
            }
        });
    }
    if (scene_) {
        const auto ready = [this] {
            if (highlightPending_ && scene_ && !scene_->meshing())
                applyHighlight();
        };
        connect(scene_, &SceneController::stateChanged, this, ready);
        connect(scene_, &SceneController::bodiesChanged, this, [this, ready] { QTimer::singleShot(0, this, ready); });
    }
    if (selection_) {
        connect(selection_, &SelectionModel::changed, this, [this] {
            if (picking())
                emit pickingChanged();
        });
    }
    if (context_) {
        connect(context_, &AppContext::workspaceChanged, this, [this] {
            if (autoOpened_ && context_ && context_->workspace() != 0)
                setShown(false);
        });
    }
}

ReferenceReviewModel::~ReferenceReviewModel()
{
    if (commands_) {
        commands_->setUiHandler(QStringLiteral("references."), {});
        commands_->setUiHandler(QStringLiteral("toolpaths."), {});
    }
    listJob_.cancel();
    staleJob_.cancel();
    if (session_)
        session_->clearShownModel(this);
    lines_.reset();
    if (engineListener_ != 0 && app() != nullptr && app()->cam() == engine_)
        engine_->unsubscribe(engineListener_);
    unsubscribe();
}

ReferenceReviewModel* ReferenceReviewModel::create(QQmlEngine* qml, QJSEngine*)
{
    SceneController* scene = nullptr;
    SelectionModel* selection = nullptr;
    Session* session = nullptr;
    AppContext* context = nullptr;
    CommandCatalog* commands = nullptr;
    if (qml != nullptr) {
        const QString uri = QStringLiteral("RayCad.Ui");
        scene = qml->singletonInstance<SceneController*>(uri, QStringLiteral("Scene"));
        selection = qml->singletonInstance<SelectionModel*>(uri, QStringLiteral("Selection"));
        session = qml->singletonInstance<Session*>(uri, QStringLiteral("Session"));
        context = qml->singletonInstance<AppContext*>(uri, QStringLiteral("AppContext"));
        commands = qml->singletonInstance<CommandCatalog*>(uri, QStringLiteral("Commands"));
    }
    auto* model = new ReferenceReviewModel(scene, selection, session, context);
    model->setCommands(commands);
    if (qml != nullptr) {
        const QString uri = QStringLiteral("RayCad.Ui");
        model->setTools(qml->singletonInstance<FeatureToolModel*>(uri, QStringLiteral("FeatureTool")),
                        qml->singletonInstance<DirectEditModel*>(uri, QStringLiteral("DirectEdit")));
    }
    model->subscribe();
    model->scheduleRefresh();
    return model; // owned by the engine
}

void ReferenceReviewModel::setCommands(CommandCatalog* commands)
{
    commands_ = commands;
    if (!commands_)
        return;
    QPointer<ReferenceReviewModel> self(this);
    const CommandCatalog::UiHandler handler{[self](const QString& id) {
                                                return self ? self->commandReason(id)
                                                            : trr("The reference review is not available");
                                            },
                                            [self](const QString& id) { return self && self->runCommand(id); }};
    commands_->setUiHandler(QStringLiteral("references."), handler);
    commands_->setUiHandler(QStringLiteral("toolpaths."), handler);
}

void ReferenceReviewModel::setTools(FeatureToolModel* featureTool, DirectEditModel* directEdit)
{
    if (featureTool_)
        disconnect(featureTool_, nullptr, this, nullptr);
    if (directEdit_)
        disconnect(directEdit_, nullptr, this, nullptr);
    featureTool_ = featureTool;
    directEdit_ = directEdit;
    const auto toolOpened = [this] {
        const bool open = (featureTool_ && featureTool_->active()) || (directEdit_ && directEdit_->active());
        if (!open)
            return;
        if (picking())
            cancelPick();
        setCurrent(-1);
    };
    if (featureTool_)
        connect(featureTool_, &FeatureToolModel::activeChanged, this, toolOpened);
    if (directEdit_)
        connect(directEdit_, &DirectEditModel::activeChanged, this, toolOpened);
}

// ---------- document ----------
api::IApp* ReferenceReviewModel::app() const
{
    return context_ ? context_->app() : nullptr;
}

doc::Document* ReferenceReviewModel::liveDoc() const
{
    api::IApp* a = app();
    if (a == nullptr || docId_.empty() || doc_ == nullptr)
        return nullptr;
    api::OpenDocument* od = a->findDocument(docId_);
    return od != nullptr && od->document.get() == doc_ ? doc_ : nullptr;
}

void ReferenceReviewModel::subscribe()
{
    unsubscribe();
    api::IApp* a = app();
    api::OpenDocument* od = a != nullptr ? a->active() : nullptr;
    if (od == nullptr || !od->document)
        return;
    docId_ = od->id;
    doc_ = od->document.get();
    const std::uint64_t generation = ++docGeneration_;
    QPointer<ReferenceReviewModel> self(this);
    docListener_ = doc_->subscribe([self, generation](doc::DocStatePtr, doc::BuildResultPtr) {
        if (self && self->docGeneration_ == generation)
            self->scheduleRefresh();
    });
    cam::ICamEngine* engine = a->cam();
    if (engine != engine_) {
        engine_ = engine;
        engineListener_ = 0;
        if (engine_ != nullptr) {
            engineListener_ = engine_->subscribe([self](const OperationId&) {
                if (self && !self->staleTimer_.isActive())
                    self->staleTimer_.start();
            });
        }
    }
}

void ReferenceReviewModel::unsubscribe()
{
    if (doc::Document* d = liveDoc(); d != nullptr && docListener_ != 0)
        d->unsubscribe(docListener_);
    doc_ = nullptr;
    docListener_ = 0;
    docId_.clear();
    ++docGeneration_;
    ++listSerial_;
    ++staleSerial_;
    listJob_.cancel();
    listJob_ = {};
    staleJob_.cancel();
    staleJob_ = {};
    if (picking())
        cancelPick();
    clearView();
    current_ = -1;
    listing_.reset();
    listedState_.reset();
    keys_.clear();
    items_.clear();
    rowItem_.clear();
    rows_.clear();
    staleCount_ = generatingCount_ = 0;
    emit listChanged();
    emit staleChanged();
    emit currentChanged();
}

bool ReferenceReviewModel::busy() const
{
    if (refreshTimer_.isActive() || (listJob_.valid() && !listJob_.done()))
        return true;
    doc::Document* d = liveDoc();
    return d != nullptr && d->build() && d->builtState() == d->state() && d->state() != listedState_;
}

void ReferenceReviewModel::scheduleRefresh()
{
    refreshTimer_.start();
    emit busyChanged();
}

void ReferenceReviewModel::refresh()
{
    // Whatever this run does, busy() may have changed (the timer that called it is over).
    const auto notify = qScopeGuard([this] { emit busyChanged(); });
    if (doc_ == nullptr && app() != nullptr && app()->active() != nullptr)
        subscribe();
    doc::Document* d = liveDoc();
    api::IApp* a = app();
    IJobSystem* jobs = scene_ ? scene_->jobs() : nullptr;
    if (d == nullptr || a == nullptr || jobs == nullptr)
        return;
    const doc::DocStatePtr state = d->state();
    const doc::BuildResultPtr build = d->build();
    // The references are judged on the build of the state shown; while it rebuilds, the next notification comes.
    if (!build || d->builtState() != state)
        return;
    refreshStale();
    if (state == listedState_)
        return;
    listJob_.cancel();
    const std::uint64_t serial = ++listSerial_;
    if (!hasReferences(*state)) {
        listJob_ = {};
        onListed(std::make_shared<const refs::Listing>(), state);
        return;
    }
    const std::string docId = docId_;
    listJob_ = jobs->submit<std::shared_ptr<const refs::Listing>>(
        JobInfo{"Reference review", JobPriority::Interactive, false},
        [a, docId, state, build](JobContext& ctx) -> Result<std::shared_ptr<const refs::Listing>> {
            auto listed = refs::list(*a, docId, state, build, ctx);
            if (!listed)
                return tl::unexpected(listed.error());
            return std::make_shared<const refs::Listing>(std::move(*listed));
        });
    emit busyChanged();
    QPointer<ReferenceReviewModel> self(this);
    listJob_.then([self, serial, state](Result<std::shared_ptr<const refs::Listing>> r) {
        if (!self || serial != self->listSerial_)
            return;
        self->listJob_ = {};
        emit self->busyChanged();
        if (r)
            self->onListed(*r, state);
    });
}

void ReferenceReviewModel::onListed(std::shared_ptr<const refs::Listing> listing, const doc::DocStatePtr& state)
{
    std::set<std::string> keys;
    for (const refs::Reference& r : listing->references) {
        if (r.needsReview())
            keys.insert(keyOf(r));
    }
    const bool modelEdit = listedState_ && state && featuresDiffer(*listedState_, *state);
    const bool fresh = std::any_of(keys.begin(), keys.end(), [&](const std::string& k) { return keys_.count(k) == 0; });
    // The row shown keeps its place when the same reference is listed again.
    std::string currentKey;
    if (const refs::Reference* r = referenceAt(current_))
        currentKey = keyOf(*r);
    if (picking())
        cancelPick();
    clearView();
    listing_ = std::move(listing);
    listedState_ = state;
    keys_ = std::move(keys);
    ++listings_;
    emit busyChanged();
    rebuildRows();
    current_ = -1;
    if (!currentKey.empty()) {
        for (int i = 0; i < static_cast<int>(rowItem_.size()); ++i) {
            const refs::Reference* r = referenceAt(i);
            if (r != nullptr && keyOf(*r) == currentKey)
                current_ = i;
        }
    }
    emit listChanged();
    emit currentChanged();
    if (current_ >= 0)
        applyCurrent();
    // After a model edit in the Design workspace that leaves something new to review: the panel opens (no focus).
    // Opened so, it closes again when the user leaves the Design workspace (the CAM panels have that corner).
    if (modelEdit && fresh && !shown_ && context_ && context_->workspace() == 0) {
        setShown(true);
        autoOpened_ = true;
    }
}

void ReferenceReviewModel::rebuildRows()
{
    items_.clear();
    rowItem_.clear();
    rows_.clear();
    if (!listing_)
        return;
    std::vector<de::ReviewItem> reviewItems;
    std::vector<const refs::Reference*> byItem;
    for (const refs::Reference& r : listing_->references) {
        if (!r.needsReview())
            continue;
        reviewItems.push_back(itemOf(r));
        byItem.push_back(&r);
    }
    for (const de::ReviewGroup& g : de::groupItems(reviewItems)) {
        rows_.append(QVariantMap{{QStringLiteral("kind"), QStringLiteral("group")},
                                 {QStringLiteral("title"), g.title},
                                 {QStringLiteral("owner"), ownerKey(byItem[g.items.front()]->owner)}});
        rowItem_.push_back(-1);
        for (const std::size_t i : g.items) {
            const de::ReviewItem& item = reviewItems[i];
            const refs::Reference& r = *byItem[i];
            rows_.append(QVariantMap{{QStringLiteral("kind"), QStringLiteral("ref")},
                                     {QStringLiteral("place"), de::placeWords(item)},
                                     {QStringLiteral("words"), de::stateWords(item)},
                                     {QStringLiteral("consequence"), de::consequence(item)},
                                     {QStringLiteral("reason"), item.reason},
                                     {QStringLiteral("state"), de::stateKey(item.state)},
                                     {QStringLiteral("stateLabel"), de::stateLabel(item.state)},
                                     {QStringLiteral("canConfirm"), r.confirmable()},
                                     {QStringLiteral("canRepick"), true},
                                     {QStringLiteral("elements"), static_cast<int>(r.elements.size())},
                                     {QStringLiteral("owner"), ownerKey(r.owner)}});
            rowItem_.push_back(static_cast<int>(items_.size()));
            items_.push_back(&r);
        }
    }
}

const refs::Reference* ReferenceReviewModel::referenceAt(int row) const
{
    if (row < 0 || row >= static_cast<int>(rowItem_.size()))
        return nullptr;
    const int item = rowItem_[static_cast<std::size_t>(row)];
    return item >= 0 ? items_[static_cast<std::size_t>(item)] : nullptr;
}

// ---------- toolpaths ----------
void ReferenceReviewModel::refreshStale()
{
    doc::Document* d = liveDoc();
    api::IApp* a = app();
    cam::ICamEngine* engine = a ? a->cam() : nullptr;
    if (d == nullptr || engine == nullptr)
        return;
    const doc::DocStatePtr state = d->state();
    const doc::BuildResultPtr build = d->build();
    if (!state || !state->cam || state->cam->operations.empty() || !build) {
        if (staleCount_ != 0 || generatingCount_ != 0) {
            staleCount_ = generatingCount_ = 0;
            emit staleChanged();
        }
        return;
    }
    staleJob_.cancel();
    const std::uint64_t serial = ++staleSerial_;
    staleJob_ = engine->statusAsync(state, build);
    if (!staleJob_.valid())
        return;
    QPointer<ReferenceReviewModel> self(this);
    staleJob_.then([self, serial, state](Result<std::map<OperationId, cam::OpState>> r) {
        if (!self || serial != self->staleSerial_ || !r)
            return;
        int stale = 0, generating = 0;
        for (const auto& [id, st] : *r) {
            const auto op = state->cam->operations.find(id);
            if (op == state->cam->operations.end() || op->second.suppressed)
                continue;
            // Out of date: it had a toolpath and a change made it stale (one never generated is "Not generated" in
            // the CAM tree, not stale).
            stale += st.status == cam::OpStatus::Stale && st.toolpath ? 1 : 0;
            generating += st.status == cam::OpStatus::Generating ? 1 : 0;
        }
        if (stale != self->staleCount_ || generating != self->generatingCount_) {
            self->staleCount_ = stale;
            self->generatingCount_ = generating;
            emit self->staleChanged();
        }
    });
}

bool ReferenceReviewModel::regenerateAll()
{
    doc::Document* d = liveDoc();
    api::IApp* a = app();
    cam::ICamEngine* engine = a ? a->cam() : nullptr;
    if (d == nullptr || engine == nullptr || !d->build()) {
        setMessage(trr("The model is not built yet"), true);
        return false;
    }
    if (const QString reason = commandReason(QStringLiteral("toolpaths.regenerateStale")); !reason.isEmpty()) {
        setMessage(reason, false);
        return false;
    }
    engine->generateAllStale(d->state(), d->build());
    setMessage(staleCount_ == 1 ? trr("Regenerating 1 toolpath") : trr("Regenerating %1 toolpaths").arg(staleCount_),
               false);
    staleTimer_.start();
    return true;
}

// ---------- the panel ----------
void ReferenceReviewModel::setShown(bool shown)
{
    autoOpened_ = false; // whoever calls this decides from now on
    if (shown == shown_)
        return;
    shown_ = shown;
    if (!shown_) {
        if (picking())
            cancelPick();
        clearView();
        current_ = -1;
        emit currentChanged();
    } else {
        scheduleRefresh();
    }
    emit shownChanged();
}

void ReferenceReviewModel::setCurrent(int row)
{
    if (row < -1 || row >= static_cast<int>(rowItem_.size()) || (row >= 0 && referenceAt(row) == nullptr))
        row = -1;
    if (row == current_)
        return;
    if (picking() && row != picking_)
        cancelPick();
    current_ = row;
    applyCurrent();
    emit currentChanged();
}

QString ReferenceReviewModel::viewNote() const
{
    const refs::Reference* r = referenceAt(current_);
    if (r == nullptr)
        return {};
    QString note;
    if (r->before)
        note = trr("The view shows the model just before %1, where it is resolved.").arg(qs(r->ownerName));
    if (r->pickedBox)
        note += (note.isEmpty() ? QString() : QStringLiteral(" ")) + trr("Dashed: where it was when it was picked.");
    return note;
}

int ReferenceReviewModel::sceneIndexOf(const BodyId& body) const
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

void ReferenceReviewModel::clearView()
{
    if (lines_)
        lines_->clear();
    if (selection_ && !highlighted_.empty())
        selection_->setToolHighlight({});
    highlighted_.clear();
    highlightPending_ = false;
    if (showingModel_ && session_)
        session_->clearShownModel(this);
    showingModel_ = false;
}

void ReferenceReviewModel::applyCurrent()
{
    if (lines_)
        lines_->clear();
    if (selection_ && !highlighted_.empty())
        selection_->setToolHighlight({});
    highlighted_.clear();
    const refs::Reference* r = referenceAt(current_);
    if (r == nullptr || !listing_ || !listedState_) {
        clearView();
        return;
    }
    // Where it was picked: its box and points, dashed in the warning colour.
    if (r->pickedBox && lines_) {
        const Box3& b = *r->pickedBox;
        const Vec3 c[8] = {{b.min.x, b.min.y, b.min.z}, {b.max.x, b.min.y, b.min.z}, {b.max.x, b.max.y, b.min.z},
                           {b.min.x, b.max.y, b.min.z}, {b.min.x, b.min.y, b.max.z}, {b.max.x, b.min.y, b.max.z},
                           {b.max.x, b.max.y, b.max.z}, {b.min.x, b.max.y, b.max.z}};
        const int edges[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6},
                                  {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
        std::vector<Vec3> segs;
        for (const auto& e : edges) {
            segs.push_back(c[e[0]]);
            segs.push_back(c[e[1]]);
        }
        lines_->set(QStringLiteral("review.before"), std::move(segs), render::Token::Warning, 2.0f, true);
        const double size = std::max(0.25, 0.03 * length(sub(b.max, b.min)));
        std::vector<Vec3> crosses;
        for (const Vec3& p : r->pickedPoints) {
            for (const Vec3 d : {Vec3{size, 0, 0}, Vec3{0, size, 0}, Vec3{0, 0, size}}) {
                crosses.push_back(sub(p, d));
                crosses.push_back(add(p, d));
            }
        }
        if (!crosses.empty())
            lines_->set(QStringLiteral("review.points"), std::move(crosses), render::Token::Warning, 2.0f);
    }
    // What it resolves to now, in the model it is resolved in.
    highlightPending_ = true;
    if (r->before && session_) {
        const auto built = listing_->before.find(*r->before);
        const doc::DocStatePtr before = api::feature_edit::stateBefore(*listedState_, *r->before);
        if (built != listing_->before.end() && before) {
            session_->showModel(this, before, built->second);
            showingModel_ = true;
        }
    } else if (showingModel_ && session_) {
        session_->clearShownModel(this);
        showingModel_ = false;
    }
    QTimer::singleShot(0, this, [this] {
        if (highlightPending_ && scene_ && !scene_->meshing())
            applyHighlight();
    });
}

void ReferenceReviewModel::applyHighlight()
{
    highlightPending_ = false;
    const refs::Reference* r = referenceAt(current_);
    if (r == nullptr || !selection_ || !scene_)
        return;
    const render::ItemId item = scene_->itemForBody(sceneIndexOf(r->body));
    std::vector<render::PickHit> hits;
    if (item != 0) {
        for (const kernel::SubId& e : r->elements)
            hits.push_back(render::PickHit{item, e.kind, e.index, Vec3{}, 0});
    }
    highlighted_ = hits;
    selection_->setToolHighlight(std::move(hits));
}

// ---------- confirm and re-pick ----------
void ReferenceReviewModel::setMessage(const QString& message, bool error)
{
    message_ = message;
    messageIsError_ = error;
    emit messageChanged();
}

void ReferenceReviewModel::dismissMessage()
{
    setMessage(QString(), false);
}

bool ReferenceReviewModel::runCommandOf(const doc::Command& command, const QString& done)
{
    api::IApp* a = app();
    if (a == nullptr)
        return false;
    JobHandle<api::ActionResult> job = a->actions().invoke(command.name, command.argsJson, api::Caller::User);
    if (!job.valid()) {
        setMessage(trr("The change could not be made"), true);
        return false;
    }
    QPointer<ReferenceReviewModel> self(this);
    job.then([self, done](Result<api::ActionResult> r) {
        if (!self)
            return;
        if (r)
            self->setMessage(done, false);
        else
            self->setMessage(qs(r.error().message), true);
    });
    return true;
}

bool ReferenceReviewModel::confirm(int row)
{
    const refs::Reference* r = referenceAt(row);
    doc::Document* d = liveDoc();
    if (r == nullptr || d == nullptr || !r->confirmable())
        return false;
    auto cmd = refs::confirmCommand(*d->state(), *r);
    if (!cmd) {
        setMessage(qs(cmd.error().message), true);
        return false;
    }
    return runCommandOf(*cmd, trr("Match confirmed for %1").arg(qs(r->ownerName)));
}

bool ReferenceReviewModel::repick(int row)
{
    const refs::Reference* r = referenceAt(row);
    api::OpenDocument* od = app() ? app()->findDocument(docId_) : nullptr;
    if (r == nullptr || !selection_ || od == nullptr)
        return false;
    if (od->readOnly) {
        setMessage(trr("The document is read-only"), true);
        return false;
    }
    if (picking())
        cancelPick();
    // The tools pick in the view too: one at a time.
    if (directEdit_ && directEdit_->active())
        directEdit_->cancel();
    if (featureTool_ && featureTool_->active())
        featureTool_->cancel();
    setCurrent(row);
    picking_ = row;
    savedFilter_ = selection_->filterMask();
    selection_->setFilterMask(filterOf(r->kind));
    selection_->clear();
    emit pickingChanged();
    return true;
}

void ReferenceReviewModel::cancelPick()
{
    if (!picking())
        return;
    picking_ = -1;
    ++pickSerial_;
    using_ = false;
    if (selection_)
        selection_->setFilterMask(savedFilter_);
    emit pickingChanged();
}

QString ReferenceReviewModel::pickHint() const
{
    const refs::Reference* r = referenceAt(picking_);
    if (r == nullptr)
        return {};
    const QString one = de::elementWord(kindOf(r->kind));
    return r->inList ? trr("Click the %1 to use now in the view (Shift adds more), then Use. Esc cancels.").arg(one)
                     : trr("Click the %1 to use now in the view, then Use. Esc cancels.").arg(one);
}

int ReferenceReviewModel::pickCount() const
{
    const refs::Reference* r = referenceAt(picking_);
    if (r == nullptr || !selection_)
        return 0;
    int n = 0;
    for (const SelectionEntry& e : selection_->entries())
        n += e.sub && e.sub->kind == r->kind ? 1 : 0;
    return n;
}

bool ReferenceReviewModel::usePicks()
{
    const refs::Reference* listed = referenceAt(picking_);
    api::IApp* a = app();
    if (listed == nullptr || a == nullptr || !selection_ || !scene_ || using_)
        return false;
    const refs::Reference ref = *listed; // the listing may be replaced while the references are made
    struct Want {
        BodyId body;
        std::uint32_t index;
    };
    std::vector<Want> wanted;
    for (const SelectionEntry& e : selection_->entries()) {
        if (!e.sub || e.sub->kind != ref.kind)
            continue;
        const api::viewer::Body* b = scene_->body(e.body);
        if (b != nullptr)
            wanted.push_back(Want{b->id, e.sub->index});
    }
    if (wanted.empty()) {
        setMessage(trr("Click the new %1 in the view first").arg(de::elementWord(kindOf(ref.kind))), true);
        return false;
    }
    if (!ref.inList && wanted.size() != 1) {
        setMessage(trr("Pick one %1 only for this reference").arg(de::elementWord(kindOf(ref.kind))), true);
        return false;
    }
    const QString kindName = ref.kind == kernel::SubKind::Edge     ? QStringLiteral("edge")
                             : ref.kind == kernel::SubKind::Vertex ? QStringLiteral("vertex")
                                                                   : QStringLiteral("face");
    using_ = true;
    const std::uint64_t serial = ++pickSerial_;
    auto made = std::make_shared<std::vector<std::string>>(wanted.size());
    auto left = std::make_shared<int>(static_cast<int>(wanted.size()));
    auto failed = std::make_shared<QString>();
    QPointer<ReferenceReviewModel> self(this);
    for (std::size_t i = 0; i < wanted.size(); ++i) {
        QJsonObject args{{QStringLiteral("body"), idText(wanted[i].body.value)},
                         {QStringLiteral("kind"), kindName},
                         {QStringLiteral("index"), static_cast<int>(wanted[i].index)}};
        if (ref.before)
            args.insert(QStringLiteral("before"), idText(ref.before->value)); // the model the view shows
        JobHandle<api::ActionResult> job = a->actions().invoke("model.reference", jsonText(args), api::Caller::User);
        if (!job.valid()) {
            using_ = false;
            setMessage(trr("The new reference could not be made"), true);
            return false;
        }
        job.then([self, serial, made, left, failed, i, ref](Result<api::ActionResult> r) {
            if (!self || serial != self->pickSerial_)
                return;
            if (r)
                (*made)[i] = jsonText(jsonObject(r->resultJson).value(QStringLiteral("ref")).toObject());
            else
                *failed = qs(r.error().message);
            if (--*left > 0)
                return;
            self->using_ = false;
            doc::Document* d = self->liveDoc();
            if (!failed->isEmpty() || d == nullptr) {
                self->setMessage(failed->isEmpty() ? trr("The document was closed") : *failed, true);
                return;
            }
            auto cmd = refs::replaceCommand(*d->state(), ref, *made);
            if (!cmd) {
                self->setMessage(qs(cmd.error().message), true);
                return;
            }
            self->cancelPick();
            self->runCommandOf(*cmd, trr("Picked again for %1").arg(qs(ref.ownerName)));
        });
    }
    return true;
}

// ---------- commands ----------
QString ReferenceReviewModel::commandReason(const QString& id) const
{
    api::IApp* a = app();
    if (a == nullptr)
        return trr("There is no session");
    if (a->active() == nullptr)
        return trr("Open or import a part first");
    if (id == QLatin1String("toolpaths.regenerateStale")) {
        const doc::DocStatePtr state = a->active()->document->state();
        if (!state->cam || state->cam->operations.empty())
            return trr("The document has no operations");
        if (staleCount_ == 0)
            return generatingCount_ > 0 ? trr("The toolpaths are being generated")
                                        : trr("Every toolpath is up to date");
        return {};
    }
    if (id == QLatin1String("references.review"))
        return {};
    return trr("Unknown command");
}

bool ReferenceReviewModel::runCommand(const QString& id)
{
    if (id == QLatin1String("references.review")) {
        setShown(!shown_);
        return true;
    }
    if (id == QLatin1String("toolpaths.regenerateStale"))
        return regenerateAll();
    return false;
}

} // namespace ray::ui
