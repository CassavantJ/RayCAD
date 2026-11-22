#include "operation_view.h"

#include "../app_context.h"
#include "../scene_controller.h"
#include "../session.h"
#include "../ui_commands.h"
#include "setup_tool.h" // setup_tree.h's inline setSetupTool needs the whole type
#include "setup_tree.h"

#include <ray_api/cam_operation.h>

#include <algorithm>
#include <set>
#include <utility>

namespace ray::ui {

namespace {

render::MeshStyle styleOf(bool stale, bool dim)
{
    // The edge token is the path's: PathCut keeps the colours by move type, any other draws it all in that token.
    return render::MeshStyle{render::Token::ModelFace,
                             stale ? render::Token::PathStale : render::Token::PathCut,
                             dim ? 0.35f : 1.0f,
                             true,
                             false,
                             std::nullopt};
}

} // namespace

ToolpathDisplay::ToolpathDisplay(SceneController* scene, Session* session, AppContext* context, QObject* parent)
    : QObject(parent), scene_(scene), session_(session), context_(context)
{
    timer_.setSingleShot(true);
    timer_.setInterval(0);
    connect(&timer_, &QTimer::timeout, this, [this] { update(); });
    if (context_)
        connect(context_, &AppContext::workspaceChanged, this, [this] { scheduleUpdate(); });
    if (session_) {
        connect(session_, &Session::documentChanged, this, [this] {
            const std::string id = session_ ? session_->documentId().toStdString() : std::string();
            if (id != docId_) {
                docId_ = id;
                cache_.clear(); // another document: its toolpaths are other ones
                clearPreview();
            }
            scheduleUpdate();
        });
    }
}

ToolpathDisplay::~ToolpathDisplay()
{
    job_.cancel();
    removeAll();
}

ToolpathDisplay* ToolpathDisplay::create(QQmlEngine* qml, QJSEngine*)
{
    SceneController* scene = nullptr;
    Session* session = nullptr;
    AppContext* context = nullptr;
    SetupTreeModel* tree = nullptr;
    if (qml != nullptr) {
        const QString uri = QStringLiteral("RayCad.Ui");
        scene = qml->singletonInstance<SceneController*>(uri, QStringLiteral("Scene"));
        session = qml->singletonInstance<Session*>(uri, QStringLiteral("Session"));
        context = qml->singletonInstance<AppContext*>(uri, QStringLiteral("AppContext"));
        tree = qml->singletonInstance<SetupTreeModel*>(uri, QStringLiteral("CamTreeModel"));
    }
    auto* display = new ToolpathDisplay(scene, session, context);
    display->setTree(tree);
    return display; // owned by the engine
}

void ToolpathDisplay::setTree(SetupTreeModel* tree)
{
    if (tree_)
        disconnect(tree_, nullptr, this, nullptr);
    tree_ = tree;
    if (tree_) {
        connect(tree_, &SetupTreeModel::changed, this, [this] { scheduleUpdate(); });
        connect(tree_, &SetupTreeModel::currentChanged, this, [this] { scheduleUpdate(); });
    }
    scheduleUpdate();
}

void ToolpathDisplay::setPreview(const OperationId& op, const SetupId& setup,
                                 std::shared_ptr<const sim::Backplot> backplot)
{
    previewOp_ = op;
    previewSetup_ = setup;
    preview_ = std::move(backplot);
    update();
}

void ToolpathDisplay::clearPreview()
{
    if (!previewOp_ && !preview_)
        return;
    previewOp_.reset();
    preview_.reset();
    scheduleUpdate();
}

void ToolpathDisplay::scheduleUpdate()
{
    timer_.start();
}

doc::Document* ToolpathDisplay::liveDoc() const
{
    api::IApp* app = context_ ? context_->app() : nullptr;
    api::OpenDocument* od = app ? app->active() : nullptr;
    return od != nullptr ? od->document.get() : nullptr;
}

std::vector<ToolpathDisplay::Want> ToolpathDisplay::wanted() const
{
    std::vector<Want> out;
    if (!context_ || context_->workspace() != kCamWorkspace || !tree_)
        return out;
    doc::Document* d = liveDoc();
    const doc::DocStatePtr state = d ? d->state() : nullptr;
    if (!state || !state->cam)
        return out;
    // The setup of the current row, and the operation of it when it is an operation.
    std::optional<SetupId> setup = tree_->currentSetup();
    std::optional<OperationId> selected = tree_->currentOperation();
    if (previewOp_) {
        // The panel's operation is the one in view (a new one is in no row yet).
        setup = previewSetup_;
        selected = previewOp_;
    }
    if (!setup)
        return out;
    const auto s = std::find_if(state->cam->setups.begin(), state->cam->setups.end(),
                                [&](const cam::Setup& x) { return x.id == *setup; });
    std::vector<OperationId> ops;
    if (s != state->cam->setups.end())
        ops = s->operations;
    if (previewOp_ && std::find(ops.begin(), ops.end(), *previewOp_) == ops.end())
        ops.push_back(*previewOp_);
    for (const OperationId& op : ops) {
        Want w;
        w.op = op;
        w.setup = *setup;
        w.dim = selected && op != *selected;
        if (previewOp_ && op == *previewOp_) {
            if (!preview_)
                continue;
            w.preview = true;
            w.backplot = preview_;
            out.push_back(std::move(w));
            continue;
        }
        const cam::OpState* st = tree_->opState(op);
        if (st == nullptr || !st->toolpath || st->status == cam::OpStatus::Suppressed)
            continue;
        w.toolpath = st->toolpath;
        w.stale = st->status != cam::OpStatus::UpToDate;
        if (const auto c = cache_.find(st->toolpath.get()); c != cache_.end())
            w.backplot = c->second.second;
        out.push_back(std::move(w));
    }
    return out;
}

void ToolpathDisplay::update()
{
    std::vector<Want> wants = wanted();
    // The backplots still to build: on a worker, superseding a build of others.
    std::vector<std::pair<SetupId, std::shared_ptr<const cam::Toolpath>>> missing;
    std::vector<const cam::Toolpath*> keys;
    for (const Want& w : wants) {
        if (!w.backplot && w.toolpath) {
            missing.emplace_back(w.setup, w.toolpath);
            keys.push_back(w.toolpath.get());
        }
    }
    IJobSystem* jobs = scene_ ? scene_->jobs() : nullptr;
    doc::Document* d = liveDoc();
    if (!missing.empty() && jobs != nullptr && d != nullptr && (keys != building_ || !busy())) {
        if (busy() && job_.cancel())
            ++cancelled_;
        building_ = keys;
        const doc::DocStatePtr state = d->state();
        const doc::BuildResultPtr build = d->build();
        const std::uint64_t serial = ++serial_;
        job_ = jobs->submit<std::shared_ptr<const Built>>(
            JobInfo{"Toolpath display", JobPriority::Interactive, false},
            [state, build, missing](JobContext& ctx) -> Result<std::shared_ptr<const Built>> {
                auto out = std::make_shared<Built>();
                for (const auto& [setup, toolpath] : missing) {
                    if (auto st = ctx.cancel.check(); !st)
                        return tl::unexpected(st.error());
                    auto bp = api::cam_operation::backplot(state, build, setup, *toolpath, ctx);
                    if (bp)
                        out->emplace_back(toolpath.get(), std::move(*bp));
                    else if (bp.error().code == ErrorCode::Cancelled)
                        return tl::unexpected(bp.error());
                }
                return std::shared_ptr<const Built>(std::move(out));
            });
        QPointer<ToolpathDisplay> self(this);
        std::vector<std::shared_ptr<const cam::Toolpath>> held;
        for (const auto& m : missing)
            held.push_back(m.second);
        job_.then([self, serial, held](Result<std::shared_ptr<const Built>> r) {
            if (!self || serial != self->serial_)
                return;
            self->building_.clear();
            if (r) {
                for (const auto& [key, bp] : **r) {
                    for (const auto& t : held) {
                        if (t.get() == key)
                            self->cache_[key] = {t, bp};
                    }
                    ++self->built_;
                }
            }
            self->update();
        });
    } else if (missing.empty() && busy()) {
        if (job_.cancel())
            ++cancelled_;
        building_.clear();
    }
    // Keep the cache to what can still be shown again soon: every toolpath the tree holds now.
    if (cache_.size() > 256)
        cache_.clear();
    apply(wants);
    emit changed();
}

void ToolpathDisplay::apply(const std::vector<Want>& wants)
{
    render::IRenderViewport* viewport = scene_ ? scene_->viewport() : nullptr;
    for (const Want& w : wants) {
        if (!w.backplot)
            continue; // not built yet: the old one (if any) stays until it is
        auto it = items_.find(w.op);
        if (it != items_.end() && (it->second.backplot != w.backplot || it->second.viewport != viewport)) {
            if (it->second.viewport != nullptr && it->second.viewport == viewport && it->second.id != 0)
                viewport->remove(it->second.id);
            items_.erase(it);
            it = items_.end();
        }
        if (it == items_.end()) {
            if (viewport == nullptr)
                continue;
            Item item;
            item.id = viewport->addBackplot(w.backplot);
            item.viewport = viewport;
            item.backplot = w.backplot;
            item.toolpath = w.toolpath;
            item.stale = w.stale;
            item.dim = w.dim;
            item.preview = w.preview;
            viewport->setStyle(item.id, styleOf(w.stale, w.dim));
            items_.emplace(w.op, std::move(item));
            continue;
        }
        Item& item = it->second;
        item.toolpath = w.toolpath;
        item.preview = w.preview;
        if (item.stale != w.stale || item.dim != w.dim) {
            item.stale = w.stale;
            item.dim = w.dim;
            viewport->setStyle(item.id, styleOf(w.stale, w.dim));
        }
    }
    // What is no longer wanted at all goes (an operation still building keeps its old path meanwhile).
    std::set<OperationId> wantedOps;
    for (const Want& w : wants)
        wantedOps.insert(w.op);
    for (auto it = items_.begin(); it != items_.end();) {
        if (wantedOps.count(it->first) == 0) {
            if (it->second.viewport != nullptr && it->second.viewport == viewport && it->second.id != 0)
                viewport->remove(it->second.id);
            it = items_.erase(it);
        } else {
            ++it;
        }
    }
}

void ToolpathDisplay::removeAll()
{
    render::IRenderViewport* viewport = scene_ ? scene_->viewport() : nullptr;
    for (auto& [op, item] : items_) {
        if (item.viewport != nullptr && item.viewport == viewport && item.id != 0)
            viewport->remove(item.id);
    }
    items_.clear();
}

std::vector<ToolpathDisplay::Shown> ToolpathDisplay::shown() const
{
    std::vector<Shown> out;
    for (const auto& [op, item] : items_)
        out.push_back(Shown{op, item.id, item.stale, item.dim, item.preview, item.toolpath, item.backplot});
    return out;
}

} // namespace ray::ui
