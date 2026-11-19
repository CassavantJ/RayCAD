// What the feature tools and the feature tree (task T-098) draw into the 3D view besides the scene's own bodies:
//
//   BodyOverlay   a set of bodies shown in place of some (or all) of the scene's bodies: the live preview of a feature
//                 tool, and the model rolled back to an earlier feature. Meshing runs on the job system, never on the
//                 UI thread; until a new set is meshed the previous one stays (a failed or slow preview keeps the last
//                 good view). The scene bodies it replaces are hidden in the viewport only (the model tree's
//                 visibility toggles are not touched) and shown again by clear().
//   LineOverlay   named sets of line segments (sketch curves outside sketch mode, picked regions, hole points), each
//                 one viewport lines item in a theme token colour.
// Neither draws anything by itself while nothing changes: items are added or removed only when their content does.
#pragma once

#include "../scene_controller.h"

#include <ray_api/viewer.h>
#include <ray_render/viewport.h>

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QString>

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <vector>

namespace ray::ui {

class BodyOverlay : public QObject {
    Q_OBJECT
public:
    explicit BodyOverlay(SceneController* scene, QObject* parent = nullptr);
    ~BodyOverlay() override;

    // Shows `bodies` with `style`, hiding the scene bodies whose ids are in `hide` (every scene body with hideAll).
    void show(std::vector<api::viewer::Body> bodies, std::set<BodyId> hide, bool hideAll, render::MeshStyle style);
    // Removes what is shown and shows the scene's bodies again; a mesh job still running is abandoned.
    void clear();
    bool showing() const { return !items_.empty() || active_; }
    bool meshing() const { return job_.valid(); }
    // Meshes shown (tests).
    int itemCount() const { return static_cast<int>(items_.size()); }
    const std::vector<std::shared_ptr<const kernel::Mesh>>& meshes() const { return meshes_; }

signals:
    void shown(); // a new set is on screen

private:
    using Meshes = std::shared_ptr<const std::vector<std::shared_ptr<const kernel::Mesh>>>;
    void applyHidden();
    void restoreScene();
    void removeItems();

    QPointer<SceneController> scene_;
    std::vector<render::ItemId> items_;
    std::vector<std::shared_ptr<const kernel::Mesh>> meshes_;
    std::set<BodyId> hide_;
    bool hideAll_ = false;
    bool active_ = false;
    render::MeshStyle style_{render::Token::ModelFace, render::Token::ModelEdge, 1.0f, true, false};
    std::uint64_t serial_ = 0;
    JobHandle<Meshes> job_;
    QList<QMetaObject::Connection> connections_;
};

class LineOverlay : public QObject {
    Q_OBJECT
public:
    explicit LineOverlay(SceneController* scene, QObject* parent = nullptr);
    ~LineOverlay() override;

    // Replaces set `key` with these segments (pairs of points). Empty segments remove it.
    void set(const QString& key, std::vector<Vec3> segments, render::Token colour, float widthPx, bool dashed = false);
    void remove(const QString& key);
    // Removes every set whose key starts with `prefix`.
    void removePrefix(const QString& prefix);
    void clear();
    bool has(const QString& key) const { return sets_.count(key) != 0; }
    int count() const { return static_cast<int>(sets_.size()); }
    std::vector<QString> keys() const
    {
        std::vector<QString> out;
        for (const auto& [key, s] : sets_)
            out.push_back(key);
        return out;
    }

private:
    struct Set {
        std::vector<Vec3> segments;
        render::Token colour;
        float width;
        bool dashed;
        render::ItemId item = 0;
        render::IRenderViewport* viewport = nullptr;
    };
    void push(Set& s);
    void drop(Set& s);

    QPointer<SceneController> scene_;
    std::map<QString, Set> sets_;
};

// A polyline as segments (a, b), (b, c), ... appended to `out`.
void appendPolyline(std::vector<Vec3>& out, const std::vector<Vec3>& polyline);

} // namespace ray::ui
