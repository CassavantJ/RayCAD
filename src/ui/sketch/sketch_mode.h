// The `SketchTool` QML singleton (task T-092): the sketch mode of the desktop app.
//
//   Entering     newSketch() shows the plane picker (principal planes, the document's datum planes, a planar face
//                selected in the view, an optional offset that makes a datum plane first: feature.datum_plane.add).
//                A face is turned into a persistent reference with model.reference and resolved on a worker.
//                editFeature() re-enters an existing sketch (the model tree's double-click). The view turns to look
//                along the plane's normal, sketch x to the right.
//                A NEW sketch is not added to the document until the first drawing step: that step is
//                feature.sketch.add with the geometry in it, so entering and leaving without drawing changes nothing
//                (the document hash stays). Undoing that step leaves the mode open on the same plane.
//   Drawing      the tools of sketch_tools.h. Every step is one action through the action registry (Caller::User):
//                sketch.entity.add with the inferred constraints, or sketch.trim / extend / offset / fillet /
//                construction / entity.remove, so undo and redo (edit.undo, edit.redo) go step by step. The tool's
//                own model is updated at once (optimistically) and replaced by the document's when the action has run.
//   Solving      after every change the model is solved on the job system (never on the UI thread) for the status
//                colours: under-constrained (SketchGeometry), fully constrained (SketchConstrained), over-constrained
//                or conflicting (SketchConflict), construction (SketchConstruction), selected (Selection), hovered
//                (Hover). The solved geometry is what is shown once it matches the model.
//   Dragging     pressing on a point (or a curve: a temporary point on it) and moving drags it through
//                ISketchSolver::beginDrag on the drag worker (sketch_drag.h), which always solves for the newest
//                pointer position. The release commits the result with sketch.set (one undo step).
//   Keyboard     typing a number while drawing fills the tool's first field (length, diameter, ...); Tab moves to
//                the next, Enter commits, Esc clears what was typed, then ends the chain, then goes back to Select.
//                Delete removes the selection. Tool keys are commands (sketch.tool.*: palette, shortcuts).
//   Rendering    SketchOverlay (sketch_overlay.h) draws overlay() on top of the view; nothing is drawn unless
//                something changed (an idle sketch renders no frames).
//
// Seams for T-093 (constraints and dimensions): selection() / selectedIds() are the selection of sketch entities;
// model() and solved() give the geometry; mapToView() places a glyph or dimension label; constraintEditRequested
// is where a dimension editor hooks in (emitted on a double-click of a dimension-holding entity); glyph layers go
// into SketchLayer.qml next to the drawing glyphs.
#pragma once

#include "sketch_drag.h"
#include "sketch_tools.h"

#include <ray/api.h>
#include <ray/render.h>

#include <QtCore/QObject>
#include <QtCore/QPointF>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVariantList>
#include <QtQml/QQmlEngine>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

class QKeyEvent;

namespace ray::ui {

class AppContext;
class CommandCatalog;
class SceneController;
class SelectionModel;
class Session;
class ThemeObject;
class UnitsObject;
class ViewportItem;

// What the overlay draws, in view item coordinates (logical pixels).
struct SketchOverlayScene {
    enum class Style { Geometry, Constrained, Conflict, Construction, Selected, Hovered, Preview, Guide, Highlight };
    struct Line {
        Style style;
        std::vector<QPointF> points;
        bool dashed = false;
    };
    struct Point {
        Style style;
        QPointF at;
    };
    std::vector<Line> lines;
    std::vector<Point> points;
    std::uint64_t revision = 0;
};

class SketchModel : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(SketchTool)
    QML_SINGLETON

    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    Q_PROPERTY(bool placing READ placing NOTIFY placingChanged)
    // [{key, title, kind}] for the plane picker: "xy", "xz", "yz", "datum:<id>", "face" (a selected planar face).
    Q_PROPERTY(QVariantList planes READ planes NOTIFY placingChanged)
    Q_PROPERTY(QString sketchName READ sketchName NOTIFY activeChanged)
    Q_PROPERTY(QString tool READ tool WRITE setTool NOTIFY toolChanged)
    Q_PROPERTY(bool construction READ construction WRITE setConstruction NOTIFY toolChanged)
    // "under", "full", "over", "conflict", "failed", "solving", or "" (no sketch).
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(int freeDof READ freeDof NOTIFY statusChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)
    Q_PROPERTY(QString hint READ hint NOTIFY hintChanged)
    Q_PROPERTY(int entityCount READ entityCount NOTIFY modelChanged)
    Q_PROPERTY(int constraintCount READ constraintCount NOTIFY modelChanged)
    Q_PROPERTY(bool dragging READ dragging NOTIFY draggingChanged)
    // Drawing glyphs over the view: [{px, py, kind}] (kind: sketch_tools.h glyphName).
    Q_PROPERTY(QVariantList glyphs READ glyphs NOTIFY overlayChanged)
    // The value chips while drawing: [{key, label, text, typed, current}]; at fieldsAt (view coordinates).
    Q_PROPERTY(QVariantList fields READ fields NOTIFY fieldsChanged)
    Q_PROPERTY(QPointF fieldsAt READ fieldsAt NOTIFY fieldsChanged)
    Q_PROPERTY(QStringList selection READ selection NOTIFY selectionChanged)
    // The ViewportItem the sketch lies in (SketchLayer.qml sets it).
    Q_PROPERTY(QObject* view READ view WRITE setView NOTIFY viewChanged)

public:
    SketchModel(SceneController* scene, SelectionModel* selection, Session* session, AppContext* context,
                QObject* parent = nullptr);
    ~SketchModel() override;
    static SketchModel* create(QQmlEngine* qml, QJSEngine* js);

    void setCommands(CommandCatalog* commands);
    void setUnits(UnitsObject* units);
    void setTheme(ThemeObject* theme);

    bool active() const { return active_; }
    bool placing() const { return placing_; }
    QVariantList planes() const;
    QString sketchName() const { return name_; }
    QString tool() const;
    void setTool(const QString& name);
    bool construction() const { return engine_.construction(); }
    void setConstruction(bool on);
    QString status() const { return status_; }
    int freeDof() const { return freeDof_; }
    QString statusText() const;
    QString hint() const;
    int entityCount() const { return static_cast<int>(model_.entities.size()); }
    int constraintCount() const { return static_cast<int>(model_.constraints.size()); }
    bool dragging() const { return dragging_; }
    QVariantList glyphs() const { return glyphs_; }
    QVariantList fields() const;
    QPointF fieldsAt() const { return fieldsAt_; }
    QStringList selection() const;
    QObject* view() const;
    void setView(QObject* view);

    // ---- entering and leaving ----
    Q_INVOKABLE void newSketch();
    // "xy", "xz", "yz", "datum:<id>" or "face"; offset (display units text, "" or "0": none) makes a datum plane.
    Q_INVOKABLE void placeOn(const QString& key, const QString& offsetText = QString());
    Q_INVOKABLE void cancelPlacing();
    Q_INVOKABLE bool editFeature(const QString& featureId);
    Q_INVOKABLE bool editFeatureAt(int index); // index into the document's features (the model tree's rows)
    Q_INVOKABLE void finish();

    // ---- editing ----
    Q_INVOKABLE void deleteSelection();
    Q_INVOKABLE void toggleConstruction(); // the selection's flag, or the drawing mode without a selection
    Q_INVOKABLE void clearSelection();
    Q_INVOKABLE QPointF mapToView(double u, double v) const; // a sketch point in view coordinates (T-093 seam)

    // ---- input from the overlay (view item coordinates) ----
    void pointerMove(QPointF pos, Qt::KeyboardModifiers mods);
    void pointerPress(QPointF pos, Qt::KeyboardModifiers mods);
    void pointerRelease(QPointF pos, Qt::KeyboardModifiers mods);
    void pointerDoubleClick(QPointF pos, Qt::KeyboardModifiers mods);
    void pointerLeave();
    // A key for the sketch (true: handled). wantsKey: claim it before the shortcuts (ShortcutOverride).
    bool keyPress(QKeyEvent* event);
    bool wantsKey(const QKeyEvent* event) const;

    // ---- the command catalog's hooks (sketch.tool.*, sketch.mode.*, feature.sketch.add) ----
    QString commandReason(const QString& id) const;
    bool runCommand(const QString& id);

    // ---- C++ (overlay, tests) ----
    const SketchOverlayScene& overlay() const { return overlay_; }
    const sketch::Sketch& model() const { return model_; }
    std::optional<sketch::Sketch> solved() const;
    const sketchui::SketchView& displayView() const { return display_; }
    std::optional<FeatureId> featureId() const;
    bool featureInDocument() const { return active_ && !pending_; }
    const kernel::Plane& plane() const { return plane_; }
    std::vector<SketchEntId> selectedIds() const { return {selected_.begin(), selected_.end()}; }
    int inFlight() const { return inFlight_; }
    bool solving() const { return solvedSerial_ != modelSerial_; }
    // Plane point under a view point; nullopt when the ray misses the plane.
    std::optional<Vec2> toPlane(QPointF viewPos) const;
    std::optional<QPointF> toView(Vec2 planePoint) const;
    // Drag statistics (tests): steps solved, the slowest step, the last step's status.
    int dragSteps() const { return dragSteps_; }
    double slowestDragStepMs() const { return slowestStepMs_; }

    // ---- T-093 hooks (constraints and dimensions: sketch/constraints_model.h) ----
    // The constraints layer sees the pointer and keys first while it has a tool of its own (true: it used them).
    struct InputHook {
        std::function<bool(QPointF, Qt::KeyboardModifiers)> move, press, release, doubleClick;
        std::function<bool(const QKeyEvent*)> wantsKey;
        std::function<bool(QKeyEvent*)> keyPress;
    };
    void setInputHook(InputHook hook) { inputHook_ = std::move(hook); }
    // The finished solve of the current model (its diagnosis); nullptr while solving or when it failed outright.
    const sketch::SolveResult* currentSolve() const;
    // The solved shape shown for the current model's entities (this solve, or the last good one while solving or
    // after a failure); nullopt when there is none. Commands send it so the stored shape is the one shown.
    std::optional<sketch::Sketch> shownSolved() const;
    // Runs a sketch command as one step (its arguments include "id"); `done` hears whether it ran.
    void runStep(const QString& actionId, std::string argsJson, const QString& label,
                 std::function<void(bool)> done = {});
    void selectEntities(const std::vector<SketchEntId>& ids);
    // Entities drawn in the highlight colour (a hovered constraint's, the dimension tool's picks).
    void setHighlight(const std::vector<SketchEntId>& ids);
    // A hint that replaces the tool's while set ("" gives it back).
    void setHintOverride(const QString& hint);

signals:
    void activeChanged();
    void placingChanged();
    void toolChanged();
    void statusChanged();
    void hintChanged();
    void modelChanged();
    void draggingChanged();
    void overlayChanged();
    void fieldsChanged();
    void selectionChanged();
    void viewChanged();
    // T-093: a double-click on an entity that carries a dimension (the dimension editor hooks in here).
    void constraintEditRequested(const QString& constraintId);
    // T-093: a solve of the model finished (currentSolve() has its diagnosis), or failed.
    void solveFinished();

private:
    enum class Press { None, Select, Drag };
    struct Pending {
        bool down = false;
        QPointF at;
        Qt::KeyboardModifiers mods;
        sketchui::SketchView::Hit hit;
        bool moved = false;
    };

    api::IApp* app() const;
    api::OpenDocument* document() const;
    doc::Document* liveDoc() const;
    void subscribe();
    void unsubscribe();
    void syncFromDocument();
    void enter(FeatureId id, QString name, std::string planeJson, kernel::Plane plane, bool pending);
    void leave();
    void turnView();
    void setModel(sketch::Sketch model);
    void refreshDisplay();
    void requestSolve();
    void run(const QString& actionId, std::string argsJson, const QString& label, std::function<void(bool)> done = {});
    void apply(const sketchui::Action& action);
    void afterToolInput();
    void updateOverlay();
    void updateHint();
    double snapTolerance(QPointF pos) const;
    double snapPixels() const;
    void startDrag(const sketchui::SketchView::Hit& hit, Vec2 at);
    void onDrag(const sketchui::SketchDrag::Update& update);
    std::optional<double> parseField(const sketchui::FieldSpec& spec, const QString& text) const;
    void applyTyped();
    void setStatus(const QString& status, int dof);
    QString nextName(const char* prefix, const char* type) const;
    void placeResolved(std::string planeJson);

    QPointer<SceneController> scene_;
    QPointer<SelectionModel> selection_;
    QPointer<Session> session_;
    QPointer<AppContext> context_;
    QPointer<CommandCatalog> commands_;
    QPointer<UnitsObject> units_;
    QPointer<ThemeObject> theme_;
    QPointer<ViewportItem> view_;
    QList<QMetaObject::Connection> viewConnections_;

    // The document observed.
    std::string docId_;
    doc::Document* doc_ = nullptr;
    int docListener_ = 0;
    std::uint64_t docGeneration_ = 0;

    bool active_ = false;
    bool placing_ = false;
    bool pending_ = false; // the feature is not in the document (yet, or any more after an undo)
    bool created_ = false; // this session made the feature (an undo past it keeps the mode open)
    FeatureId feature_;
    QString name_;
    std::string planeJson_;
    kernel::Plane plane_{};
    sketch::Sketch model_;
    std::uint64_t modelSerial_ = 0;
    std::optional<sketch::SolveResult> solved_;
    std::uint64_t solveSerial_ = 0, solvedSerial_ = 0;
    QString status_;
    int freeDof_ = 0;
    std::set<SketchEntId> conflictEntities_;
    sketchui::SketchView display_;
    sketchui::ToolEngine engine_;
    int inFlight_ = 0;
    std::uint64_t placeSerial_ = 0;

    // Pointer state.
    QPointF pointer_;
    bool pointerIn_ = false;
    Pending press_;
    sketchui::SketchView::Hit hover_;
    std::set<SketchEntId> selected_;

    // Dragging.
    std::unique_ptr<sketchui::SketchDrag> drag_;
    std::uint64_t dragSerial_ = 0;
    bool dragging_ = false;
    bool dragFinishing_ = false;
    std::optional<sketch::Sketch> dragSketch_;
    std::optional<SketchEntId> dragTempPoint_;
    std::optional<Uuid> dragTempConstraint_;
    Vec2 dragGrabOffset_{};
    int dragSteps_ = 0;
    double slowestStepMs_ = 0;

    // Typed values.
    std::map<std::string, QString> typedText_;
    int activeField_ = -1;

    SketchOverlayScene overlay_;
    QVariantList glyphs_;
    QPointF fieldsAt_;

    // T-093.
    InputHook inputHook_;
    std::optional<sketch::Sketch> lastGood_; // the last solve that worked: shown while solving and after a failure
    std::set<SketchEntId> externalHighlight_;
    QString hintOverride_;
};

} // namespace ray::ui
