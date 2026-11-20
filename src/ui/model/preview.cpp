#include "preview.h"

#include <algorithm>
#include <utility>

namespace ray::ui {

// ---------- BodyOverlay ----------
BodyOverlay::BodyOverlay(SceneController* scene, QObject* parent) : QObject(parent), scene_(scene)
{
    if (scene_) {
        // The scene makes new viewport items for a new body set (every document change): hide them again.
        connections_ << connect(scene_, &SceneController::stateChanged, this, [this] {
            if (active_)
                applyHidden();
        });
        connections_ << connect(scene_, &SceneController::bodiesChanged, this, [this] {
            if (active_)
                applyHidden();
        });
    }
}

BodyOverlay::~BodyOverlay()
{
    for (const auto& c : connections_)
        disconnect(c);
    job_.cancel();
    removeItems();
    if (active_)
        restoreScene();
}

void BodyOverlay::show(const QObject* owner, std::vector<api::viewer::Body> bodies, std::set<BodyId> hide, bool hideAll,
                       render::MeshStyle style)
{
    IJobSystem* jobs = scene_ ? scene_->jobs() : nullptr;
    if (jobs == nullptr)
        return;
    if (owner_ != nullptr && owner_ != owner) {
        const QObject* previous = owner_;
        reset();
        emit released(previous);
    }
    owner_ = owner;
    job_.cancel();
    const std::uint64_t serial = ++serial_;
    auto shared = std::make_shared<const std::vector<api::viewer::Body>>(std::move(bodies));
    job_ = jobs->submit<Meshes>(
        JobInfo{"Preview mesh", JobPriority::Interactive, false}, [shared](JobContext& ctx) -> Result<Meshes> {
            auto out = std::make_shared<std::vector<std::shared_ptr<const kernel::Mesh>>>();
            for (const api::viewer::Body& body : *shared) {
                if (auto ok = ctx.cancel.check(); !ok)
                    return tl::unexpected(ok.error());
                auto mesh =
                    api::viewer::meshBody(body.shape, api::viewer::kCoarseChordMm, api::viewer::kCoarseAngleDeg, ctx);
                if (!mesh && mesh.error().code == ErrorCode::Cancelled)
                    return tl::unexpected(mesh.error());
                out->push_back(mesh ? std::make_shared<const kernel::Mesh>(std::move(*mesh)) : nullptr);
            }
            return Meshes(std::move(out));
        });
    QPointer<BodyOverlay> self(this);
    job_.then([self, serial, hide = std::move(hide), hideAll, style](Result<Meshes> meshes) {
        if (!self || serial != self->serial_)
            return; // superseded or cleared
        self->job_ = {};
        if (!meshes)
            return; // keep the last good view
        self->removeItems();
        self->restoreScene();
        self->hide_ = hide;
        self->hideAll_ = hideAll;
        self->style_ = style;
        self->active_ = true;
        render::IRenderViewport* viewport = self->scene_ ? self->scene_->viewport() : nullptr;
        for (const auto& mesh : **meshes) {
            if (!mesh)
                continue;
            self->meshes_.push_back(mesh);
            if (viewport != nullptr) {
                const render::ItemId item = viewport->addMesh(mesh, Xform::identity(), style);
                if (item != 0)
                    viewport->setPickMode(item, render::PickMode::Never); // picks go to the model under it
                self->items_.push_back(item);
            }
        }
        self->applyHidden();
        emit self->shown();
    });
}

void BodyOverlay::clear(const QObject* owner)
{
    if (owner_ != nullptr && owner != owner_)
        return;
    reset();
}

void BodyOverlay::reset()
{
    job_.cancel();
    job_ = {};
    ++serial_;
    removeItems();
    if (active_)
        restoreScene();
    active_ = false;
    hide_.clear();
    hideAll_ = false;
    owner_ = nullptr;
}

void BodyOverlay::removeItems()
{
    render::IRenderViewport* viewport = scene_ ? scene_->viewport() : nullptr;
    if (viewport != nullptr) {
        for (render::ItemId item : items_) {
            if (item != 0)
                viewport->remove(item);
        }
    }
    items_.clear();
    meshes_.clear();
}

void BodyOverlay::applyHidden()
{
    render::IRenderViewport* viewport = scene_ ? scene_->viewport() : nullptr;
    if (viewport == nullptr)
        return;
    for (int i = 0; i < scene_->bodyCount(); ++i) {
        const api::viewer::Body* body = scene_->body(i);
        const render::ItemId item = scene_->itemForBody(i);
        if (body == nullptr || item == 0)
            continue;
        const bool hidden = hideAll_ || hide_.count(body->id) != 0;
        const bool shown = scene_->bodyVisible(i);
        viewport->setVisible(item, hidden ? false : shown);
        // A body the preview stands in for still answers picks (and shows what is picked on it), if the user shows it.
        viewport->setPickMode(item, hidden && shown ? render::PickMode::Always : render::PickMode::WhenVisible);
    }
}

void BodyOverlay::restoreScene()
{
    render::IRenderViewport* viewport = scene_ ? scene_->viewport() : nullptr;
    if (viewport == nullptr)
        return;
    for (int i = 0; i < scene_->bodyCount(); ++i) {
        const render::ItemId item = scene_->itemForBody(i);
        if (item == 0)
            continue;
        viewport->setVisible(item, scene_->bodyVisible(i));
        viewport->setPickMode(item, render::PickMode::WhenVisible);
    }
}

// ---------- LineOverlay ----------
LineOverlay::LineOverlay(SceneController* scene, QObject* parent) : QObject(parent), scene_(scene) {}

LineOverlay::~LineOverlay()
{
    clear();
}

void LineOverlay::push(Set& s)
{
    render::IRenderViewport* viewport = scene_ ? scene_->viewport() : nullptr;
    s.viewport = viewport;
    s.item = viewport != nullptr && !s.segments.empty()
                 ? viewport->addLines(std::span<const Vec3>(s.segments), s.colour, s.width, s.dashed)
                 : 0;
    if (s.item != 0)
        viewport->setPickMode(s.item, render::PickMode::Never); // guides: picks go to the model under them
}

void LineOverlay::drop(Set& s)
{
    render::IRenderViewport* viewport = scene_ ? scene_->viewport() : nullptr;
    if (s.item != 0 && viewport != nullptr && viewport == s.viewport)
        viewport->remove(s.item);
    s.item = 0;
}

void LineOverlay::set(const QString& key, std::vector<Vec3> segments, render::Token colour, float widthPx, bool dashed)
{
    if (segments.empty()) {
        remove(key);
        return;
    }
    auto it = sets_.find(key);
    if (it != sets_.end()) {
        Set& s = it->second;
        render::IRenderViewport* viewport = scene_ ? scene_->viewport() : nullptr;
        // Unchanged: nothing to draw (an idle view stays idle).
        if (s.colour == colour && s.width == widthPx && s.dashed == dashed && s.viewport == viewport &&
            s.segments.size() == segments.size() &&
            std::equal(s.segments.begin(), s.segments.end(), segments.begin(),
                       [](const Vec3& a, const Vec3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }))
            return;
        drop(s);
        sets_.erase(it);
    }
    Set s{std::move(segments), colour, widthPx, dashed, 0, nullptr};
    push(s);
    sets_.emplace(key, std::move(s));
}

void LineOverlay::remove(const QString& key)
{
    auto it = sets_.find(key);
    if (it == sets_.end())
        return;
    drop(it->second);
    sets_.erase(it);
}

void LineOverlay::removePrefix(const QString& prefix)
{
    for (auto it = sets_.begin(); it != sets_.end();) {
        if (it->first.startsWith(prefix)) {
            drop(it->second);
            it = sets_.erase(it);
        } else {
            ++it;
        }
    }
}

void LineOverlay::clear()
{
    for (auto& [key, s] : sets_)
        drop(s);
    sets_.clear();
}

void appendPolyline(std::vector<Vec3>& out, const std::vector<Vec3>& polyline)
{
    for (std::size_t i = 0; i + 1 < polyline.size(); ++i) {
        out.push_back(polyline[i]);
        out.push_back(polyline[i + 1]);
    }
}

} // namespace ray::ui
