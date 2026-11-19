// The `Scene` QML singleton (task T-020): the set of bodies the viewport shows, their display meshes, and the
// model tree.
//
// ONE entry point feeds it: setBodies(). The session (session.h, T-019) feeds it from the active document through
// syncFromDocument(), which turns a build result into the body list (api::viewer::bodiesFromDocument) and also
// lists the features in the tree.
//
// Meshing never runs on the UI thread. Each new body set is meshed coarse first (0.2 mm / 20 degrees, PLAN.md 9)
// on the job system and shown as soon as that finishes, then refined in the background to the tier preset's
// tolerance. Both steps show in the status bar's job slot (AppContext::setJob) and Esc / Cancel stops them; a
// cancelled or superseded mesh job is abandoned (its result is dropped when it arrives, PLAN.md 7).
#pragma once

#include "app_context.h"
#include "scene_jobs.h"

#include <ray_api/viewer.h>
#include <ray_render/viewport.h>

#include <QtCore/QAbstractListModel>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtQml/QQmlEngine>

#include <memory>
#include <optional>
#include <vector>

namespace ray::ui {

class AppContext;
class SceneController;

// An immutable view of the current bodies, shared with jobs (selection references, the properties read-out).
struct SceneSnapshot {
    std::uint64_t generation = 0;
    std::vector<api::viewer::Body> bodies;
    std::shared_ptr<const api::viewer::SelectionResolver> resolver;
};

// The model tree's rows: section headers, features (when a document is attached) and bodies, flattened.
class SceneTreeModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role {
        KindRole = Qt::UserRole + 1, // "header", "feature" or "body"
        NameRole,
        DepthRole,
        ExpandableRole,
        ExpandedRole,
        VisibleRole,  // bodies: shown in the viewport
        SelectedRole, // bodies: the selection holds the body or one of its elements
        BodyIndexRole,
        FeatureIndexRole,
        DetailRole, // secondary text: feature type and status, body face count
        SectionRole,
    };
    explicit SceneTreeModel(SceneController* scene);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void toggleSection(const QString& section);
    Q_INVOKABLE int rowOfBody(int bodyIndex) const; // -1 if collapsed or unknown

    void rebuild();
    void refreshBodies(); // visibility and selection changed: same rows

private:
    struct Row {
        QString kind, section;
        int body = -1, feature = -1;
    };
    SceneController* scene_;
    std::vector<Row> rows_;
    bool featuresExpanded_ = true;
    bool bodiesExpanded_ = true;
};

class SceneController : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(Scene)
    QML_SINGLETON

    Q_PROPERTY(int bodyCount READ bodyCount NOTIFY bodiesChanged)
    Q_PROPERTY(QString sourceName READ sourceName NOTIFY bodiesChanged)
    Q_PROPERTY(bool hasDocument READ hasDocument NOTIFY bodiesChanged)
    Q_PROPERTY(QAbstractItemModel* tree READ tree CONSTANT)
    // A coarse mesh job runs (nothing of the current bodies is shown yet).
    Q_PROPERTY(bool meshing READ meshing NOTIFY stateChanged)
    // The background refinement to the tier tolerance runs.
    Q_PROPERTY(bool refining READ refining NOTIFY stateChanged)
    // Every body of the current set has a display mesh (coarse or fine).
    Q_PROPERTY(bool ready READ ready NOTIFY stateChanged)
    Q_PROPERTY(QString tierName READ tierName NOTIFY presetChanged)

public:
    struct Feature {
        QString name, type;
        bool suppressed = false;
        QString status;
    };

    // `jobs` returns the job system to use; `context` gets the job slot. Both outlive this object.
    SceneController(JobsProvider jobs, AppContext* context, QObject* parent = nullptr);
    ~SceneController() override;

    static SceneController* create(QQmlEngine* qml, QJSEngine* js);
    static void provideFor(QQmlEngine* engine, SceneController* scene);

    // ---- the one entry point ----
    // Replaces everything shown. `sourceName` is a label for the tree and the status bar ("part.step").
    void setBodies(std::vector<api::viewer::Body> bodies, const QString& sourceName = {});
    // Adapter for T-017/T-019: the bodies of the document's latest build, plus its features for the tree.
    void syncFromDocument(const doc::DocStatePtr& state, const doc::BuildResultPtr& build);
    Q_INVOKABLE void clear();

    // ---- the viewport ----
    // The scene pushes its meshes into the attached viewport (one at a time; the viewport item attaches itself).
    void attachViewport(render::IRenderViewport* viewport);
    void detachViewport(render::IRenderViewport* viewport);
    render::IRenderViewport* viewport() const { return viewport_; }
    // Display tier: fed to the viewport and used as the refinement tolerance. Detected on first use.
    render::TierPreset preset() const { return preset_; }
    void setPreset(const render::TierPreset& preset);
    QString tierName() const;

    // ---- queries ----
    int bodyCount() const { return static_cast<int>(slots_.size()); }
    QString sourceName() const { return sourceName_; }
    bool hasDocument() const { return !features_.empty() || hasDocument_; }
    QAbstractItemModel* tree() const { return tree_; }
    bool meshing() const { return coarse_.valid(); }
    bool refining() const { return fine_.valid(); }
    bool ready() const;
    const api::viewer::Body* body(int index) const;
    QString bodyName(int index) const;
    bool bodyVisible(int index) const;
    Q_INVOKABLE void setBodyVisible(int index, bool visible);
    int bodyIndexForItem(render::ItemId item) const; // -1 if unknown
    render::ItemId itemForBody(int index) const;     // 0 if not shown
    std::shared_ptr<const kernel::Mesh> meshOf(int index) const;
    bool meshIsFine(int index) const;
    const std::vector<Feature>& features() const { return features_; }
    std::shared_ptr<const SceneSnapshot> snapshot() const { return snapshot_; }
    IJobSystem* jobs() const { return jobs_ ? jobs_() : nullptr; }
    AppContext* context() const { return context_; }

    // Bounds of the given selection hits in world coordinates (from the display meshes); nullopt if none.
    std::optional<Box3> boundsOf(std::span<const render::PickHit> hits) const;

    // Cancels the running mesh jobs (the job slot's Cancel and Esc). `quiet`: no status message.
    void cancelJobs(bool quiet = false);
    // The selection tells the tree which bodies hold selected elements.
    void setSelectedBodies(std::vector<int> bodies);
    bool bodySelected(int index) const;

signals:
    void bodiesChanged(); // a new body set (the selection clears itself)
    void bodyVisibilityChanged(int index);
    void stateChanged();
    void presetChanged();
    void firstMeshesShown(); // a new body set became visible: the viewport fits it (not for the same bodies again)

private:
    struct Slot {
        std::shared_ptr<const kernel::Mesh> mesh;
        bool fine = false;
        render::ItemId item = 0;
        bool visible = true;
    };
    // Shared, so JobHandle hands the result over without copying the meshes; a null entry is a body that failed.
    using Meshes = std::shared_ptr<const std::vector<std::shared_ptr<const kernel::Mesh>>>;

    void startCoarse();
    void startFine();
    void ensurePreset();
    void showMeshes(const Meshes& meshes, bool fine);
    void pushToViewport(int index);
    void updateJobSlot();
    void setFeatures(std::vector<Feature> features);

    JobsProvider jobs_;
    QPointer<AppContext> context_;
    render::IRenderViewport* viewport_ = nullptr;
    std::vector<Slot> slots_;          // parallel to snapshot_->bodies
    std::vector<int> selectedBodies_;  // sorted
    std::vector<BodyId> framedBodies_; // the body set the view last framed (sorted ids), T-092
    std::vector<Feature> features_;
    bool hasDocument_ = false;
    QString sourceName_;
    std::shared_ptr<const SceneSnapshot> snapshot_;
    std::uint64_t generation_ = 0;
    std::uint64_t jobSerial_ = 0, coarseSerial_ = 0, fineSerial_ = 0, presetSerial_ = 0;
    JobHandle<Meshes> coarse_;
    JobHandle<Meshes> fine_;
    QString jobTitle_;
    double jobProgress_ = -1;
    render::TierPreset preset_;
    bool presetKnown_ = false;
    bool presetPending_ = false;
    bool fineWanted_ = false;
    SceneTreeModel* tree_ = nullptr;
};

} // namespace ray::ui
