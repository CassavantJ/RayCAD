// The `CamToolpaths` QML singleton (task T-045): toolpaths in the 3D view of the CAM workspace.
//
//   What       the operation selected in the CAM tree, and the other operations of its setup dimmed; a setup row shows
//              all of its operations. Each is the toolpath the engine holds for it (ICamEngine status: the current
//              one, or the last one while it is stale, then drawn in the stale colour, Token::PathStale). While the
//              operation panel previews an operation, its preview is shown in that operation's place.
//   How        the backplot (api::cam_operation::backplot, sim::buildBackplot in the setup's frame) is built on the job
//              system, never on the UI thread; a build that is superseded is cancelled; a toolpath that is regenerated
//              is replaced; backplots are kept by toolpath, so going back to a row draws at once. Items are added to
//              the scene's viewport (IViewport::addBackplot) and restyled (setStyle) without a rebuild.
//   When       on every change of the tree's statuses or current row, and only in the CAM workspace (leaving it hides
//              them all).
#pragma once

#include <ray/api.h>
#include <ray_render/viewport.h>

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtQml/QQmlEngine>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace ray::ui {

class AppContext;
class SceneController;
class Session;
class SetupTreeModel;

class ToolpathDisplay : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(CamToolpaths)
    QML_SINGLETON

    Q_PROPERTY(int shownCount READ shownCount NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)

public:
    ToolpathDisplay(SceneController* scene, Session* session, AppContext* context, QObject* parent = nullptr);
    ~ToolpathDisplay() override;
    static ToolpathDisplay* create(QQmlEngine* qml, QJSEngine* js);
    void setTree(SetupTreeModel* tree);

    // The operation panel's preview of `op` (built on a worker already), shown in its place until clearPreview().
    // `setup` is the operation's setup (a new operation is in no tree row yet).
    void setPreview(const OperationId& op, const SetupId& setup, std::shared_ptr<const sim::Backplot> backplot);
    void clearPreview();

    // ---- C++ (tests) ----
    struct Shown {
        OperationId op;
        render::ItemId item = 0;
        bool stale = false, dim = false, preview = false;
        std::shared_ptr<const cam::Toolpath> toolpath; // null for a preview
        std::shared_ptr<const sim::Backplot> backplot;
    };
    std::vector<Shown> shown() const;
    int shownCount() const { return static_cast<int>(items_.size()); }
    bool busy() const { return job_.valid() && !job_.done(); }
    int backplotsBuilt() const { return built_; }
    int buildsCancelled() const { return cancelled_; }
    // Works out what to show now (also done after every change it hears of).
    void update();

signals:
    void changed();

private:
    struct Want {
        OperationId op;
        SetupId setup;
        std::shared_ptr<const cam::Toolpath> toolpath;
        std::shared_ptr<const sim::Backplot> backplot; // known: a preview or cached
        bool stale = false, dim = false, preview = false;
    };
    struct Item {
        render::ItemId id = 0;
        render::IRenderViewport* viewport = nullptr;
        std::shared_ptr<const sim::Backplot> backplot;
        std::shared_ptr<const cam::Toolpath> toolpath;
        bool stale = false, dim = false, preview = false;
    };
    using Built = std::vector<std::pair<const cam::Toolpath*, std::shared_ptr<const sim::Backplot>>>;
    std::vector<Want> wanted() const;
    void apply(const std::vector<Want>& wants);
    void removeAll();
    void scheduleUpdate();
    doc::Document* liveDoc() const;

    QPointer<SceneController> scene_;
    QPointer<Session> session_;
    QPointer<AppContext> context_;
    QPointer<SetupTreeModel> tree_;
    QTimer timer_;
    std::map<OperationId, Item> items_;
    // Backplots by toolpath (the toolpath is held too, so its address is not reused while it is a key).
    std::map<const cam::Toolpath*,
             std::pair<std::shared_ptr<const cam::Toolpath>, std::shared_ptr<const sim::Backplot>>>
        cache_;
    JobHandle<std::shared_ptr<const Built>> job_;
    std::uint64_t serial_ = 0;
    std::vector<const cam::Toolpath*> building_;
    std::optional<OperationId> previewOp_;
    SetupId previewSetup_;
    std::shared_ptr<const sim::Backplot> preview_;
    std::string docId_;
    int built_ = 0;
    int cancelled_ = 0;
};

} // namespace ray::ui
