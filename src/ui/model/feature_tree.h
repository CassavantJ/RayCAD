// The `FeatureTree` QML singleton (task T-098): the document's features for the model tree (qml/panels/ModelTree.qml),
// the actions on them, the rollback bar, and the sketches shown in the 3D view outside the sketch mode.
//
//   Rows        one per feature in history order: name, type (with its icon), build status and message. A Failed
//               feature carries the rebuild's message; a NotBuilt one names the earlier feature it waits on.
//   Actions     rename (feature.edit), suppress (feature.suppress), delete (feature.remove), edit (the tool's panel
//               on the feature, FeatureTool.edit, or the sketch mode for a sketch), each one action and one undo step.
//               moveTo (feature.reorder) is checked first: the reorder is applied to a copy of the history and every
//               moved-past feature is checked as its add command checks it (the features and bodies it names must come
//               before it); then the reorder is rebuilt off to the side (Document::beginPreview) and refused with the
//               message of the first feature it would break. Only an order that builds as before is executed.
//   Rollback    setRollback(i) shows the model as it is after feature i (rebuilt off to the side by a rebuilder of its
//               own, shown in place of the scene's bodies); later features are greyed and new features are inserted
//               after i. It is a view state: the document does not change. -1 is the end of the history.
//   Sketches    every visible sketch is drawn in the view in the sketch colours (construction dashed). A sketch is
//               visible unless a later feature uses it (an extrude, revolve or hole) or it was hidden here; the
//               sketch a tool works on is always shown. The sketch being edited in the sketch mode is left to it.
#pragma once

#include "preview.h"

#include <ray/api.h>

#include <QtCore/QAbstractListModel>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtCore/QVariantMap>
#include <QtQml/QQmlEngine>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

namespace ray::ui {

class AppContext;
class FeatureToolModel;
class SceneController;
class Session;
class SketchModel;

class FeatureTreeModel : public QAbstractListModel {
    Q_OBJECT
    QML_NAMED_ELEMENT(FeatureTree)
    QML_SINGLETON

    Q_PROPERTY(int count READ count NOTIFY changed)
    Q_PROPERTY(int revision READ revision NOTIFY changed)
    // The last feature shown (rolled back), or -1 for the whole history.
    Q_PROPERTY(int rollbackIndex READ rollbackIndex WRITE setRollback NOTIFY rollbackChanged)
    Q_PROPERTY(int currentIndex READ currentIndex WRITE setCurrentIndex NOTIFY currentChanged)
    // Why the last tree action was refused (a blocked reorder, a failed rename, ...); "" when none.
    Q_PROPERTY(QString message READ message NOTIFY messageChanged)
    // A reorder is being checked (its side rebuild runs).
    Q_PROPERTY(bool checking READ checking NOTIFY messageChanged)

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        NameRole,
        TypeRole,
        TypeTitleRole,
        IconRole,
        StatusRole,     // "ok", "warning", "failed", "suppressed", "notBuilt", "building"
        StatusTextRole, // short: "Failed", "Waits on Extrude 1", ...
        MessageRole,    // the rebuild's message (failed, warning) or what it waits on
        SuppressedRole,
        RolledBackRole, // after the rollback bar
        IsSketchRole,
        SketchShownRole,
    };

    FeatureTreeModel(SceneController* scene, Session* session, AppContext* context, QObject* parent = nullptr);
    ~FeatureTreeModel() override;
    static FeatureTreeModel* create(QQmlEngine* qml, QJSEngine* js);
    void setSketchTool(SketchModel* sketch);
    void setFeatureTool(FeatureToolModel* tool) { tool_ = tool; }

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return static_cast<int>(rows_.size()); }
    int revision() const { return revision_; }
    int rollbackIndex() const { return rollback_; }
    void setRollback(int index);
    int currentIndex() const { return current_; }
    void setCurrentIndex(int index);
    QString message() const { return message_; }
    bool checking() const { return checking_; }

    // ---- QML ----
    Q_INVOKABLE QVariantMap get(int row) const;
    Q_INVOKABLE int rowOf(const QString& featureId) const;
    Q_INVOKABLE bool rename(int row, const QString& name);
    Q_INVOKABLE bool setSuppressed(int row, bool suppressed);
    Q_INVOKABLE bool remove(int row);
    Q_INVOKABLE bool edit(int row);
    // Moves row `from` to index `to` of the resulting history, after the checks above. False when refused at once
    // (message() says why); true when the check started (the move happens if it passes).
    Q_INVOKABLE bool moveTo(int from, int to);
    // Why moving `from` to `to` is refused, from the quick checks only ("" when they pass).
    Q_INVOKABLE QString moveProblem(int from, int to) const;
    Q_INVOKABLE void setSketchShown(int row, bool shown);
    Q_INVOKABLE void dismissMessage();

    // ---- C++ ----
    std::optional<FeatureId> currentFeatureId() const;
    std::optional<FeatureId> featureAt(int row) const;
    // The sketch a tool works on: always drawn (nullopt: none).
    void setToolSketch(std::optional<FeatureId> sketch);
    // A tool previews the rolled-back model itself: the rollback view steps aside meanwhile.
    void setRollbackOverlaySuspended(bool suspended);
    const BodyOverlay& rollbackOverlay() const { return *rollbackOverlay_; }
    const LineOverlay& sketchLines() const { return *sketchLines_; }
    bool sketchShown(const FeatureId& id) const;
    // Sketch-line jobs and the rollback build still running (tests).
    bool busy() const { return sketchJob_.valid() || rollbackJob_.valid(); }

signals:
    void changed();
    void rollbackChanged();
    void currentChanged();
    void messageChanged();
    // The Rename Feature command (F2): the tree opens the name of `row` for editing.
    void renameRequested(int row);
    // The Parameters command: the model panel shows the parameter table.
    void parametersRequested();

private:
    struct Row {
        FeatureId id;
        QString name, type, status, statusText, message;
        bool suppressed = false;
        bool sketch = false;
    };
    api::IApp* app() const;
    api::OpenDocument* document() const;
    doc::Document* liveDoc() const;
    void subscribe();
    void unsubscribe();
    void sync();
    void setMessage(const QString& message);
    bool run(const QString& actionId, const std::string& argsJson, const QString& label);
    void updateSketchLines();
    void updateRollbackView();
    void finishReorderCheck(doc::DocStatePtr state, doc::BuildResultPtr build);
    QString quickMoveProblem(const doc::DocState& state, int from, int to) const;

    QPointer<SceneController> scene_;
    QPointer<Session> session_;
    QPointer<AppContext> context_;
    QPointer<SketchModel> sketchTool_;
    QPointer<FeatureToolModel> tool_;
    QQmlEngine* engine_ = nullptr;
    std::unique_ptr<BodyOverlay> rollbackOverlay_;
    std::unique_ptr<LineOverlay> sketchLines_;

    std::string docId_;
    doc::Document* doc_ = nullptr;
    int docListener_ = 0;
    int previewListener_ = 0;
    std::uint64_t docGeneration_ = 0;
    doc::DocStatePtr shownState_;
    doc::BuildResultPtr shownBuild_;

    std::vector<Row> rows_;
    int revision_ = 0;
    int rollback_ = -1;
    int current_ = -1;
    QString message_;

    // Reorder check.
    bool checking_ = false;
    FeatureId checkId_;
    std::size_t checkTo_ = 0;
    std::string checkArgs_;
    std::map<FeatureId, doc::FeatureStatus> checkBefore_;

    // Rollback view.
    bool rollbackSuspended_ = false;
    std::shared_ptr<doc::IRebuilder> sideRebuilder_;
    std::string sideRebuilderDoc_;
    JobHandle<doc::BuildResultPtr> rollbackJob_;
    std::uint64_t rollbackSerial_ = 0;

    // Sketch lines.
    std::map<FeatureId, bool> sketchOverride_; // user's show / hide
    std::optional<FeatureId> toolSketch_;
    std::optional<FeatureId> editedSketch_; // open in the sketch mode
    struct SketchLines {
        FeatureId id;
        std::vector<Vec3> geometry, construction, points; // segments in world coordinates
    };
    JobHandle<std::shared_ptr<const std::vector<SketchLines>>> sketchJob_;
    std::uint64_t sketchSerial_ = 0;
};

} // namespace ray::ui
