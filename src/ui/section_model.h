// The `SectionView` QML singleton (task T-023): the section view and draft analysis of the 3D view, behind
// panels/Section.qml and the view.section.* / view.draft.* commands.
//
// Section: a plane XY, YZ or ZX (normal +Z, +X, +Y), or the plane of a selected planar face (its outward normal).
// `offset` is the plane's signed distance from the origin along that normal, in mm; its range is the extent of what
// is shown along the normal (the viewport's scene bounds), and it follows the model. Flip cuts away the other side
// at the same place. Cap asks for a solid cap on the cut; the tier preset decides whether caps are drawn
// (TierPreset::sectionCaps, PLAN.md 6.2), and capReason says why not. Dragging the offset only changes the
// viewport's uniforms: nothing is meshed or uploaded again.
// Draft analysis: faces coloured by the angle between their normal and the pull direction (+-X, +-Y, +-Z or a
// selected planar face's outward normal), with a threshold angle (ray_render/viewport.h, DraftAnalysis).
//
// Lengths are shown and typed in the display units of the Units singleton (units.h, T-024: the document's units,
// the status bar's toggle, the decimals from the settings) and follow a switch live; values are kept in mm.
//
// The state lives here and is pushed into the window's viewport on every change and whenever a viewport appears;
// the renderer keeps it across theme and preset switches.
#pragma once

#include <ray_render/viewport.h>

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtQml/QQmlEngine>

#include <functional>
#include <optional>

namespace ray::ui {

class SceneController;
class SelectionModel;
class UnitsObject;
class ViewportItem;

class SectionModel : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(SectionView)
    QML_SINGLETON

    // The Section panel is shown.
    Q_PROPERTY(bool panelOpen READ panelOpen WRITE setPanelOpen NOTIFY panelOpenChanged)
    // ---- section ----
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY changed)
    // "xy", "yz", "zx" or "face" (set through planeFromSelectedFace).
    Q_PROPERTY(QString plane READ plane WRITE setPlane NOTIFY changed)
    Q_PROPERTY(double offset READ offset WRITE setOffset NOTIFY changed) // mm along the plane's normal
    Q_PROPERTY(double offsetMin READ offsetMin NOTIFY rangeChanged)
    Q_PROPERTY(double offsetMax READ offsetMax NOTIFY rangeChanged)
    Q_PROPERTY(bool hasModel READ hasModel NOTIFY rangeChanged) // something is shown to cut
    Q_PROPERTY(bool flipped READ flipped WRITE setFlipped NOTIFY changed)
    Q_PROPERTY(bool cap READ cap WRITE setCap NOTIFY changed)
    Q_PROPERTY(bool capAvailable READ capAvailable NOTIFY availabilityChanged)
    Q_PROPERTY(QString capReason READ capReason NOTIFY availabilityChanged)
    // ---- draft analysis ----
    Q_PROPERTY(bool draft READ draft WRITE setDraft NOTIFY changed)
    // "+x", "-x", "+y", "-y", "+z", "-z" or "face" (set through pullFromSelectedFace).
    Q_PROPERTY(QString pull READ pull WRITE setPull NOTIFY changed)
    Q_PROPERTY(double draftThreshold READ draftThreshold WRITE setDraftThreshold NOTIFY changed) // degrees
    // ---- the selected face ----
    Q_PROPERTY(bool faceAvailable READ faceAvailable NOTIFY availabilityChanged)
    Q_PROPERTY(QString faceReason READ faceReason NOTIFY availabilityChanged)
    // ---- display units (the Units singleton) ----
    Q_PROPERTY(QString unit READ unit NOTIFY unitChanged)
    Q_PROPERTY(int unitDecimals READ unitDecimals NOTIFY unitChanged)
    // The offset formatted in the display units ("0.5000 in"); also changes with the offset.
    Q_PROPERTY(QString offsetText READ offsetText NOTIFY offsetTextChanged)

public:
    using ViewportProvider = std::function<ViewportItem*()>;

    // All may be null; they must outlive this object.
    SectionModel(SceneController* scene, SelectionModel* selection, ViewportProvider viewport,
                 QObject* parent = nullptr);
    ~SectionModel() override;

    static SectionModel* create(QQmlEngine* qml, QJSEngine* js);
    static void provideFor(QQmlEngine* engine, SectionModel* section);

    bool panelOpen() const { return panelOpen_; }
    void setPanelOpen(bool open);
    bool active() const { return active_; }
    void setActive(bool on);
    QString plane() const { return plane_; }
    void setPlane(const QString& plane);
    double offset() const { return offset_; }
    void setOffset(double mm);
    double offsetMin() const { return rangeMin_; }
    double offsetMax() const { return rangeMax_; }
    bool hasModel() const { return hasRange_; }
    bool flipped() const { return flipped_; }
    void setFlipped(bool on);
    bool cap() const { return cap_; }
    void setCap(bool on);
    bool capAvailable() const;
    QString capReason() const;
    bool draft() const { return draft_; }
    void setDraft(bool on);
    QString pull() const { return pull_; }
    void setPull(const QString& pull);
    double draftThreshold() const { return threshold_; }
    void setDraftThreshold(double degrees);
    bool faceAvailable() const { return selectedFace().has_value(); }
    QString faceReason() const;
    // The display units (null: millimetres, 3 decimals). AppHost hands in the Units singleton.
    void setUnits(UnitsObject* units);
    QString unit() const;
    int unitDecimals() const;
    QString offsetText() const;

    // Millimetres to and from the document's length unit (the seam above).
    Q_INVOKABLE double toUnits(double mm) const;
    Q_INVOKABLE double fromUnits(double value) const;
    // What a person typed ("0.5", "1/2 in", "12.7 mm") as mm, read in the display units; NaN if it does not read.
    Q_INVOKABLE double parseLength(const QString& text) const;
    // Cut parallel to the selected planar face (its outward normal), through the middle of the model. False (and
    // nothing changes) when no single planar face is selected.
    Q_INVOKABLE bool planeFromSelectedFace();
    // Pull along the selected planar face's outward normal.
    Q_INVOKABLE bool pullFromSelectedFace();

    // What the renderer is given now (tests, the commands).
    std::optional<render::SectionPlane> sectionPlane() const;
    std::optional<render::DraftAnalysis> draftAnalysis() const;
    Vec3 planeNormal() const; // unit, before flipping

    // ---- commands (palette_commands.cpp routes view.section.* and view.draft.* here) ----
    static bool handles(const QString& id);
    QString commandReason(const QString& id) const; // "" when it can run
    bool runCommand(const QString& id);
    bool commandMarked(const QString& id) const; // a toggle that is on

signals:
    void panelOpenChanged();
    void changed();
    void rangeChanged();
    void availabilityChanged();
    void unitChanged();
    void offsetTextChanged();

private:
    struct Face {
        Vec3 normal, centroid;
    };
    std::optional<Face> selectedFace() const;
    ViewportItem* view() const;
    void apply();
    void updateRange(bool recentre);
    bool inch() const;

    QPointer<SceneController> scene_;
    QPointer<SelectionModel> selection_;
    ViewportProvider viewportProvider_;
    mutable QPointer<ViewportItem> view_;
    QPointer<UnitsObject> units_;

    bool panelOpen_ = false;
    bool active_ = false;
    QString plane_ = QStringLiteral("xy");
    Vec3 faceNormal_{0, 0, 1}; // plane "face"
    double offset_ = 0;
    bool offsetSet_ = false; // the user (or a plane change) placed it; otherwise it follows the model's middle
    double rangeMin_ = 0, rangeMax_ = 0;
    bool hasRange_ = false;
    bool flipped_ = false;
    bool cap_ = true;
    bool draft_ = false;
    QString pull_ = QStringLiteral("+z");
    Vec3 pullFace_{0, 0, 1}; // pull "face"
    double threshold_ = 1.0;
};

} // namespace ray::ui
