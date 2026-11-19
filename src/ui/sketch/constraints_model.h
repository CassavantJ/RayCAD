// The `SketchConstraints` QML singleton (task T-093): constraints and dimensions of the sketch being edited, on top of
// SketchTool (sketch_mode.h, T-092).
//
//   Dimensions   every dimension of the model is drawn over the view (SketchDimensionOverlay, dimensions_overlay.h:
//                extension lines, the dimension line or arc, arrowheads) with its value label (labels rows, in the
//                document's display units; R and the diameter sign for radii and diameters; driven ones in
//                parentheses and their own colour). A label can be dragged (sketch.dimension.place on release, one
//                step); a double-click on it, or on a dimensioned entity, opens the value field over it.
//   Value field  takes a length or angle as shown ("12.5", "1/2 in", "30 deg"), an expression over the document's
//                parameters ("width / 2 + 3 mm"), or a parameter's name: the feature props' expression language
//                (ray_api/sketch_edit.h evaluateExpression). Enter commits with sketch.constraint.set (one step);
//                what does not evaluate is shown under the field, which stays open. Esc closes it.
//   Smart dimension   the tool (D, sketch.constrain.dimension): click one or two entities (or start with them
//                selected); the dimension they make follows the pointer (dimensions.h pickDimension); a click on empty
//                space places it and opens the value field. Enter adds it (sketch.constraint.add with its label, one
//                step). Before that a trial solve on a worker checks it: a dimension that would over-constrain the
//                sketch is offered as a driven (reference) dimension instead.
//   Geometric constraints   coincident, horizontal, vertical, parallel, perpendicular, tangent, equal, midpoint,
//                concentric, collinear, symmetric and fix apply to the selection (sketch.constrain.<kind>: the
//                toolbar, the palette, the keys); each is enabled only when the selection fits (reason() says what
//                to select). Existing ones show a glyph by their entities (glyphs rows); hovering a glyph highlights
//                its entities, a click selects it, Delete removes it (sketch.constraint.remove).
//   Solver feedback   constraints the last solve names conflicting or redundant draw in the conflict colour and are
//                listed (offenders); selecting one selects it in the view. While a solve fails the view keeps the last
//                good shape (SketchTool). The remaining degrees of freedom are in SketchTool.statusText.
// Nothing here changes the document except through actions; no solve runs on the UI thread.
#pragma once

#include "constraints.h"
#include "constraints_rows.h"
#include "dimensions.h"

#include <ray/api.h>

#include <QtCore/QObject>
#include <QtCore/QPointF>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtCore/QVariantList>
#include <QtQml/QQmlEngine>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <vector>

class QKeyEvent;

namespace ray::ui {

class AppContext;
class CommandCatalog;
class SceneController;
class SketchModel;
class ThemeObject;
class UnitsObject;

// What SketchDimensionOverlay draws, in view item coordinates.
struct DimensionScene {
    enum class Style { Normal, Driven, Bad, Hovered, Selected, Preview };
    struct Line {
        Style style;
        std::vector<QPointF> points;
        bool operator==(const Line&) const = default;
    };
    struct Arrow {
        Style style;
        QPointF tip;
        QPointF dir; // unit, in view coordinates
        bool operator==(const Arrow&) const = default;
    };
    std::vector<Line> lines;
    std::vector<Arrow> arrows;
    bool operator==(const DimensionScene&) const = default;
};

class SketchConstraintsModel : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(SketchConstraints)
    QML_SINGLETON

    // Rows {cid, slot, kind, title, px, py, stack, bad, hovered, selected}: one glyph per place of a constraint.
    Q_PROPERTY(QObject* glyphs READ glyphs CONSTANT)
    // Rows {cid, slot, type, title, text, expression, px, py, driven, bad, hovered, selected}: the dimension labels.
    Q_PROPERTY(QObject* labels READ labels CONSTANT)
    // [{name, title, icon, enabled, reason}] for the toolbar: the geometric constraints in order.
    Q_PROPERTY(QVariantList actions READ actions NOTIFY actionsChanged)
    Q_PROPERTY(bool dimensionTool READ dimensionTool NOTIFY toolChanged)
    Q_PROPERTY(QString selected READ selected NOTIFY selectionChanged)
    Q_PROPERTY(bool selectedIsDimension READ selectedIsDimension NOTIFY selectionChanged)
    Q_PROPERTY(bool selectedDriven READ selectedDriven NOTIFY selectionChanged)
    Q_PROPERTY(QString hovered READ hovered NOTIFY hoverChanged)
    // [{cid, title, why}] the constraints the last solve named conflicting or redundant.
    Q_PROPERTY(QVariantList offenders READ offenders NOTIFY offendersChanged)
    // The value field.
    Q_PROPERTY(bool editing READ editing NOTIFY editChanged)
    Q_PROPERTY(bool editNew READ editNew NOTIFY editChanged)
    Q_PROPERTY(QString editId READ editId NOTIFY editChanged)
    Q_PROPERTY(QString editTitle READ editTitle NOTIFY editChanged)
    Q_PROPERTY(QString editText READ editText NOTIFY editChanged)
    Q_PROPERTY(QPointF editAt READ editAt NOTIFY editPlaceChanged)
    Q_PROPERTY(QString editError READ editError NOTIFY editChanged)
    Q_PROPERTY(bool editChecking READ editChecking NOTIFY editChanged)
    Q_PROPERTY(bool editOverConstrains READ editOverConstrains NOTIFY editChanged)
    // The smart dimension tool's live value next to the pointer.
    Q_PROPERTY(QString previewText READ previewText NOTIFY previewChanged)
    Q_PROPERTY(QPointF previewAt READ previewAt NOTIFY previewChanged)

public:
    SketchConstraintsModel(SketchModel* tool, SceneController* scene, AppContext* context, QObject* parent = nullptr);
    ~SketchConstraintsModel() override;
    static SketchConstraintsModel* create(QQmlEngine* qml, QJSEngine* js);

    void setCommands(CommandCatalog* commands);
    void setUnits(UnitsObject* units);
    void setTheme(ThemeObject* theme);

    QObject* glyphs() { return &glyphs_; }
    QObject* labels() { return &labels_; }
    QVariantList actions() const { return actions_; }
    bool dimensionTool() const { return dimensionTool_; }
    QString selected() const;
    bool selectedIsDimension() const;
    bool selectedDriven() const;
    QString hovered() const;
    QVariantList offenders() const { return offenders_; }
    bool editing() const { return edit_.has_value(); }
    bool editNew() const { return edit_ && edit_->isNew; }
    QString editId() const;
    QString editTitle() const;
    QString editText() const { return edit_ ? edit_->text : QString(); }
    QPointF editAt() const { return editAt_; }
    QString editError() const { return edit_ ? edit_->error : QString(); }
    bool editChecking() const { return edit_ && edit_->checking; }
    bool editOverConstrains() const { return edit_ && edit_->overConstrains; }
    QString previewText() const { return previewText_; }
    QPointF previewAt() const { return previewAt_; }

    // ---- geometric constraints ----
    Q_INVOKABLE QString reason(const QString& kind) const; // "" when the selection fits
    Q_INVOKABLE bool apply(const QString& kind);
    // ---- constraints in the view ----
    Q_INVOKABLE void hoverConstraint(const QString& cid); // "" clears
    Q_INVOKABLE void selectConstraint(const QString& cid);
    Q_INVOKABLE void removeSelected();
    Q_INVOKABLE void toggleDriven();
    Q_INVOKABLE void dragLabel(const QString& cid, double viewX, double viewY); // the label's centre
    Q_INVOKABLE void dropLabel(const QString& cid);
    // ---- the smart dimension tool ----
    Q_INVOKABLE void setDimensionTool(bool on);
    // ---- the value field ----
    Q_INVOKABLE void beginEdit(const QString& cid);
    Q_INVOKABLE QString check(const QString& text) const; // "" when it reads, else why not
    Q_INVOKABLE bool commitEdit(const QString& text);     // false: editError says why (the field stays)
    Q_INVOKABLE void addAsDriven();                       // a new dimension that would over-constrain
    Q_INVOKABLE void cancelEdit();

    // ---- the command catalog's hooks (sketch.constrain.*) ----
    QString commandReason(const QString& id) const;
    bool runCommand(const QString& id);

    // ---- C++ (overlay, tests) ----
    const DimensionScene& scene() const { return dimScene_; }
    std::uint64_t sceneRevision() const { return sceneRevision_; }
    // The constraint ids the last solve named (conflicting, redundant, partially redundant).
    const std::set<Uuid>& badIds() const { return bad_; }
    // The smart dimension tool's picks and the dimension they make now.
    const std::vector<SketchEntId>& picks() const { return picks_; }
    std::optional<sketchui::DimensionPick> candidate() const { return candidate_; }
    int pending() const { return pendingSteps_; } // steps sent and not back yet

signals:
    void actionsChanged();
    void toolChanged();
    void selectionChanged();
    void hoverChanged();
    void offendersChanged();
    void editChanged();
    void editPlaceChanged();
    void previewChanged();
    void sceneChanged();

private:
    struct Edit {
        bool isNew = false;
        Uuid id;                      // the dimension (a new one's id to be)
        sketchui::DimensionPick pick; // a new dimension: what it measures
        Vec2 offset{};                // a new dimension: its label offset
        QString text;                 // as the field opened
        QString error;
        bool checking = false;
        bool overConstrains = false;
        std::uint64_t serial = 0;
    };
    struct Parsed {
        double value = 0; // mm or rad, as shown (not negative)
        std::optional<std::string> expression;
        QString error;
    };

    api::IApp* app() const;
    void installHooks();
    void onActiveChanged();
    void onModelChanged();
    void refresh();
    void refreshActions();
    void updateHighlight();
    void runStep(const QString& actionId, const std::string& argsJson, const QString& label,
                 std::function<void()> after = {});
    const sketch::Constraint* constraintById(const Uuid& id) const;
    std::optional<Vec2> offsetOf(const Uuid& id) const;
    double gapMm() const;
    double tolMm(QPointF viewPos) const;
    QString formatValue(sketch::ConstraintType type, double value) const;
    QString labelText(const sketch::Constraint& c, double value) const;
    Parsed parse(const QString& text, bool angle) const;
    void startTrial();
    void openEdit(Edit edit, QPointF at);
    void closeEdit();
    bool commitByClick();

    // Input from SketchTool (true: used).
    bool pointerMove(QPointF pos);
    bool pointerPress(QPointF pos);
    bool wantsKey(const QKeyEvent* e) const;
    bool keyPress(QKeyEvent* e);

    QPointer<SketchModel> tool_;
    QPointer<SceneController> scene_;
    QPointer<AppContext> context_;
    QPointer<CommandCatalog> commands_;
    QPointer<UnitsObject> units_;
    QPointer<ThemeObject> theme_;

    ConstraintRows glyphs_;
    ConstraintRows labels_;
    QVariantList actions_;
    QVariantList offenders_;
    DimensionScene dimScene_;
    std::uint64_t sceneRevision_ = 0;

    sketch::Sketch evaluated_;          // the model with its expressions evaluated now
    std::map<Uuid, Vec2> storedLabels_; // the feature's "labels"
    std::map<Uuid, Vec2> dragged_;      // labels moved and not stored yet
    std::set<Uuid> bad_;
    std::optional<Uuid> selected_;
    std::optional<Uuid> hovered_;

    bool dimensionTool_ = false;
    bool settingTool_ = false;
    std::vector<SketchEntId> picks_;
    sketchui::SketchView::Hit hover_;
    Vec2 cursor_{};
    bool cursorIn_ = false;
    std::optional<sketchui::DimensionPick> candidate_;
    QString previewText_;
    QPointF previewAt_;

    std::optional<Edit> edit_;
    QPointF editAt_;
    std::uint64_t editSerial_ = 0;
    int pendingSteps_ = 0;
};

} // namespace ray::ui
