// The `References` QML singleton (task T-083): the reference review panel (qml/panels/ReferenceReview.qml, a QML type
// of that name, hence the singleton's other name) and what the status bar's badge counts.
//
//   List         every reference of the document that a model edit split, merged, moved (a likely match the user has
//                not confirmed) or removed (api::references::list: a feature's references where the feature stands,
//                a CAM operation's geometry and a setup's WCS references in the document's build), grouped by owner
//                (features in history order, then setups and operations in program order), each with its state in shop
//                words (directedit_logic.h). Worked out on the job system after every published build of the
//                document, a run superseded by the next; nothing polls. An empty list says so.
//   Toolpaths    staleCount: the operations whose toolpath is out of date, i.e. that had one and a change made it
//                stale (ICamEngine::statusAsync after every build and every engine notice, coalesced; an operation
//                never generated is "Not generated" in the CAM tree and is not counted). regenerateAll() (the command
//                toolpaths.regenerateStale, Ctrl+Shift+G, in the palette, the CAM tree's header and this panel)
//                generates every stale one (ICamEngine::generateAllStale).
//   Highlight    the current row shows in the view: what the reference resolves to now with the selection's highlight
//                (SelectionModel::setToolHighlight), and where the element lay when it was picked (its pick-time box
//                and points, dashed in the warning colour), when the reference has that evidence. A feature's reference
//                is shown in the model just before the feature (Session::showModel), where the feature resolves it.
//   Confirm      a likely match (Repaired, not confirmed) is confirmed through its owner's edit action
//                (api::references::confirmCommand): one undo step. Split and Merged have no confirmation in the
//                document (a feature uses them with a warning already; an operation needs a new pick to post).
//   Re-pick      repick(row) enters a pick mode for that kind of element (the selection filter picks only faces, edges
//                or points; in the model before the feature for a feature's reference); usePicks() makes references of
//                what is selected (model.reference) and puts them in its place through its owner's edit action
//                (api::references::replaceCommand): one undo step. Esc or cancelPick() leaves it.
//   Opening      the panel opens by itself (not modal, without taking the focus) after a model edit in the Design
//                workspace that leaves a reference to review that was not listed before (opened so, it closes when the
//                user leaves the Design workspace); otherwise from the command references.review (Ctrl+Shift+R), the
//                palette or the status bar's badge, in any workspace.
#pragma once

#include "directedit_logic.h"
#include "preview.h"

#include <ray/api.h>
#include <ray_api/references.h>

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtCore/QVariantList>
#include <QtQml/QQmlEngine>

#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <vector>

namespace ray::ui {

class AppContext;
class CommandCatalog;
class DirectEditModel;
class FeatureToolModel;
class SceneController;
class SelectionModel;
class Session;

class ReferenceReviewModel : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(References)
    QML_SINGLETON

    Q_PROPERTY(bool shown READ shown WRITE setShown NOTIFY shownChanged)
    // References to review (the rows that are not group headers).
    Q_PROPERTY(int count READ count NOTIFY listChanged)
    // Operations whose toolpath is out of date, and those generating now.
    Q_PROPERTY(int staleCount READ staleCount NOTIFY staleChanged)
    Q_PROPERTY(int generatingCount READ generatingCount NOTIFY staleChanged)
    // A listing is under way.
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    // The panel's rows: {kind: "group", title, owner ("feature" | "operation" | "setup")} or {kind: "ref", place,
    // words, consequence, reason, state (stateKey), stateLabel, canConfirm, canRepick, elements, owner}.
    Q_PROPERTY(QVariantList rows READ rows NOTIFY listChanged)
    // The row shown in the view (-1 none).
    Q_PROPERTY(int current READ current WRITE setCurrent NOTIFY currentChanged)
    // What the view shows for the current row ("The view shows the model just before Delete Faces 1").
    Q_PROPERTY(QString viewNote READ viewNote NOTIFY currentChanged)
    // Re-pick mode: on, what to do, how many fitting elements are selected.
    Q_PROPERTY(bool picking READ picking NOTIFY pickingChanged)
    Q_PROPERTY(QString pickHint READ pickHint NOTIFY pickingChanged)
    Q_PROPERTY(int pickCount READ pickCount NOTIFY pickingChanged)
    // The last action's result or why it failed; "" none.
    Q_PROPERTY(QString message READ message NOTIFY messageChanged)
    Q_PROPERTY(bool messageIsError READ messageIsError NOTIFY messageChanged)

public:
    ReferenceReviewModel(SceneController* scene, SelectionModel* selection, Session* session, AppContext* context,
                         QObject* parent = nullptr);
    ~ReferenceReviewModel() override;
    static ReferenceReviewModel* create(QQmlEngine* qml, QJSEngine* js);
    void setCommands(CommandCatalog* commands);
    // The tools that pick in the view: while one of them is open the review shows nothing in the view (they pick in
    // the document's build, not in a model before a feature) and a re-pick ends; a re-pick closes them.
    void setTools(FeatureToolModel* featureTool, DirectEditModel* directEdit);

    bool shown() const { return shown_; }
    void setShown(bool shown);
    int count() const { return static_cast<int>(items_.size()); }
    int staleCount() const { return staleCount_; }
    int generatingCount() const { return generatingCount_; }
    // A listing is scheduled or running, or the document has a build of a state not listed yet.
    bool busy() const;
    QVariantList rows() const { return rows_; }
    int current() const { return current_; }
    void setCurrent(int row);
    QString viewNote() const;
    bool picking() const { return picking_ >= 0; }
    QString pickHint() const;
    int pickCount() const;
    QString message() const { return message_; }
    bool messageIsError() const { return messageIsError_; }

    // ---- QML ----
    Q_INVOKABLE void show() { setShown(true); }
    Q_INVOKABLE void hide() { setShown(false); }
    Q_INVOKABLE bool confirm(int row);
    Q_INVOKABLE bool repick(int row);
    Q_INVOKABLE bool usePicks();
    Q_INVOKABLE void cancelPick();
    Q_INVOKABLE bool regenerateAll();
    Q_INVOKABLE void dismissMessage();

    // ---- the command catalog's hooks (references.*, toolpaths.*) ----
    QString commandReason(const QString& id) const;
    bool runCommand(const QString& id);

    // ---- C++ (tests) ----
    // The last listing (null before the first), its count of runs, and the row of each reference (rows index).
    const api::references::Listing* listing() const { return listing_.get(); }
    int listings() const { return listings_; }
    // The reference a "ref" row shows (null for a group row or out of range).
    const api::references::Reference* referenceAt(int row) const;
    // Lists again now (also done after every build).
    void refresh();
    // What the view shows for the current row: the elements highlighted, and the pick-time lines drawn.
    const std::vector<render::PickHit>& highlighted() const { return highlighted_; }
    bool beforeDrawn() const { return lines_ && lines_->has(QStringLiteral("review.before")); }
    bool showingModel() const { return showingModel_; }
    // A highlight is on its way (the model before a feature is being meshed).
    bool highlightPending() const { return highlightPending_; }

signals:
    void shownChanged();
    void listChanged();
    void staleChanged();
    void busyChanged();
    void currentChanged();
    void pickingChanged();
    void messageChanged();

private:
    api::IApp* app() const;
    doc::Document* liveDoc() const;
    void subscribe();
    void unsubscribe();
    void scheduleRefresh();
    void refreshStale();
    void onListed(std::shared_ptr<const api::references::Listing> listing, const doc::DocStatePtr& state);
    void rebuildRows();
    void applyCurrent();
    void applyHighlight();
    void clearView();
    void setMessage(const QString& message, bool error);
    bool runCommandOf(const doc::Command& command, const QString& done);
    int sceneIndexOf(const BodyId& body) const;

    QPointer<SceneController> scene_;
    QPointer<SelectionModel> selection_;
    QPointer<Session> session_;
    QPointer<AppContext> context_;
    QPointer<CommandCatalog> commands_;
    QPointer<FeatureToolModel> featureTool_;
    QPointer<DirectEditModel> directEdit_;
    std::unique_ptr<LineOverlay> lines_;

    std::string docId_;
    doc::Document* doc_ = nullptr;
    int docListener_ = 0;
    std::uint64_t docGeneration_ = 0;
    cam::ICamEngine* engine_ = nullptr;
    int engineListener_ = 0;

    bool shown_ = false;
    bool autoOpened_ = false; // opened by itself after an edit: leaving the Design workspace closes it
    // The listing and the rows made from it.
    std::shared_ptr<const api::references::Listing> listing_;
    doc::DocStatePtr listedState_;
    std::vector<const api::references::Reference*> items_; // the references needing review, in row order
    std::vector<int> rowItem_;                             // per row: index into items_, -1 for a group
    QVariantList rows_;
    std::set<std::string> keys_; // owner, path and state of every listed reference (what is new after an edit)
    int listings_ = 0;
    JobHandle<std::shared_ptr<const api::references::Listing>> listJob_;
    std::uint64_t listSerial_ = 0;
    QTimer refreshTimer_;

    // Toolpaths.
    int staleCount_ = 0, generatingCount_ = 0;
    JobHandle<std::map<OperationId, cam::OpState>> staleJob_;
    std::uint64_t staleSerial_ = 0;
    QTimer staleTimer_;

    // The current row in the view.
    int current_ = -1;
    std::vector<render::PickHit> highlighted_;
    bool showingModel_ = false;
    bool highlightPending_ = false;

    // Re-pick.
    int picking_ = -1; // the row
    std::uint8_t savedFilter_ = 0;
    std::uint64_t pickSerial_ = 0;
    bool using_ = false;

    QString message_;
    bool messageIsError_ = false;
};

} // namespace ray::ui
