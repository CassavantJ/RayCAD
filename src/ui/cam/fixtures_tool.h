// The `CamFixtures` QML singleton (task T-077): the fixtures panel of the CAM workspace (qml/cam/Fixtures.qml).
//
//   Open         open() (the Fixtures command, cam.fixtures.open, Shift+F in the CAM workspace, and the CAM tree's
//                Fixtures button) shows the panel: the document's fixtures (bodies with the role "fixture") and the
//                fixture models of the libraries (camui::libraryFixtures: each library's "fixtures" folder). One CAM
//                panel at a time: opening it cancels the setup and operation panels, and either of those opening
//                closes it.
//   Add          importFile(path) imports a STEP (or IGES) file as fixtures through the registry's file.import with the
//                role "fixture" (one undo step); addFromLibrary(i) does it with a library model. The new fixture bodies
//                are then added to the fixture bodies of the CAM tree's current setup (else the first setup; one more
//                undo step, cam.setup.edit), and the first is opened for placing. requestImport() asks the panel to
//                show its file dialog (importDialogRequested).
//   Place        place(bodyId) opens the placement of one fixture: its "transform_body" feature (the last one moving
//                that body), or a new one. pickMode "face": a click on a planar face of the fixture (the face that goes
//                against the part); "target": a click on a planar face of another body (the part's side, a parallel's
//                top). The two picks are the mate; gap, X, Y, Z (lengths in the display unit, numbers with units or
//                expressions) and the angle (degrees) adjust it (transform_body_feature.h says how). The face is
//                referenced as the fixture is just before its feature (model.reference "before"), the target as the
//                model is now.
//   Preview      every change previews feature.transform_body.add / .edit through Document::beginPreview; the moved
//                fixture is shown in the scene's preview slot in place of the fixture (the fixture under it stays
//                pickable). A command the document refuses or a mate that fails to build shows its message; the last
//                good preview stays.
//   Apply/Cancel commit() runs the command through the action registry as the user: one undo step. A new feature goes
//                before the first feature that mates another body to this fixture (camui::placementIndex), so a moving
//                jaw mated to the vise follows the vise. cancel() (Esc) ends the placement; the document is unchanged.
//                Esc with no placement open closes the panel. setInSetup(body, in) adds a fixture to (or takes it out
//                of) the fixture bodies of that setup.
//   On stock     (after T-077) placeOnStock() stands the setup's stock in a library vise the document has (one whose
//                library model has a description, ray_api/cam_fixtures.h: viseName, the one in the setup first): the
//                stock's bottom on the vise's floor, or on the parallels' top when the setup has the library's
//                parallels, centred along the jaws, the fixed jaw against one side, the moving jaw closed on the other
//                (its opening clamped to the vise's travel, said plainly when the stock is wider). grip ("shorter",
//                "x", "y") picks the two faces the jaws close on. The vise and parallels are placed by numbers, the jaw
//                by its mate to the fixed jaw with the opening as the gap: ordinary placements, editable with Place
//                afterwards. One cam.batch command (one undo step), which also makes them fixtures of the setup when
//                they were not. Worked out once: a later change of the stock does not move them (the panel says so; run
//                it again). The result or why not: stockMessage, stockStatus ("ok", "warning", "failed"). The placement
//                editor also shows tiltX and tiltY (degrees about model X and Y, without a mate), which this sets for a
//                setup whose Z is not model +Z.
// Fixtures are never machining geometry: the operation panel refuses picks on them, and so do the CAM commands and the
// CAM engine (cam_commands.cpp, cam::resolveSetupGeometry).
#pragma once

#include "../model/preview.h"
#include "fixtures_logic.h"
#include "sim_carried.h"

#include <ray/api.h>
#include <ray_api/cam_fixtures.h>

#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtCore/QVariantList>
#include <QtCore/QVariantMap>
#include <QtQml/QQmlEngine>

#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <vector>

namespace ray::ui {

class AppContext;
class CommandCatalog;
class OperationToolModel;
class SceneController;
class SelectionModel;
class Session;
class SetupToolModel;
class SetupTreeModel;
class UnitsObject;

class FixturesToolModel : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(CamFixtures)
    QML_SINGLETON

    Q_PROPERTY(bool shown READ shown NOTIFY shownChanged)
    // [{id, name, placed (a transform_body feature moves it), mated (that feature has a mate), inSetup}]
    Q_PROPERTY(QVariantList fixtures READ fixtures NOTIFY fixturesChanged)
    // [{name, path, root}] the libraries' fixture models.
    Q_PROPERTY(QVariantList library READ library NOTIFY libraryChanged)
    // The setup new fixtures go into and inSetup refers to; "" when the document has none.
    Q_PROPERTY(QString setupName READ setupName NOTIFY fixturesChanged)
    // An import running (file.import).
    Q_PROPERTY(bool importing READ importing NOTIFY statusChanged)
    // The placement: the fixture's body id ("" when none is open) and name, and whether its feature exists already.
    Q_PROPERTY(QString placing READ placing NOTIFY placingChanged)
    Q_PROPERTY(QString placingName READ placingName NOTIFY placingChanged)
    Q_PROPERTY(bool placingExisting READ placingExisting NOTIFY placingChanged)
    // gap, x, y, z, angle as text.
    Q_PROPERTY(QVariantMap values READ values NOTIFY valuesChanged)
    // "face", "target" or "".
    Q_PROPERTY(QString pickMode READ pickMode NOTIFY picksChanged)
    Q_PROPERTY(QString faceText READ faceText NOTIFY picksChanged)
    Q_PROPERTY(QString targetText READ targetText NOTIFY picksChanged)
    Q_PROPERTY(bool hasFace READ hasFace NOTIFY picksChanged)
    Q_PROPERTY(bool hasTarget READ hasTarget NOTIFY picksChanged)
    // The message about the values or the last pick; messageField the field it is about.
    Q_PROPERTY(QString message READ message NOTIFY statusChanged)
    Q_PROPERTY(QString messageField READ messageField NOTIFY statusChanged)
    // "", "building", "ok", "warning", "failed".
    Q_PROPERTY(QString previewStatus READ previewStatus NOTIFY statusChanged)
    Q_PROPERTY(bool committing READ committing NOTIFY statusChanged)
    Q_PROPERTY(bool canCommit READ canCommit NOTIFY statusChanged)
    // On stock: the vise it places ("" when the document has none with a description), the grip, the run.
    Q_PROPERTY(QString viseName READ viseName NOTIFY fixturesChanged)
    Q_PROPERTY(QString grip READ grip WRITE setGrip NOTIFY gripChanged)
    Q_PROPERTY(bool placingOnStock READ placingOnStock NOTIFY stockStatusChanged)
    Q_PROPERTY(QString stockMessage READ stockMessage NOTIFY stockStatusChanged)
    Q_PROPERTY(QString stockStatus READ stockStatus NOTIFY stockStatusChanged)

public:
    FixturesToolModel(SceneController* scene, SelectionModel* selection, Session* session, AppContext* context,
                      QObject* parent = nullptr);
    ~FixturesToolModel() override;
    static FixturesToolModel* create(QQmlEngine* qml, QJSEngine* js);

    void setCommands(CommandCatalog* commands);
    void setUnits(UnitsObject* units);
    void setPanels(SetupToolModel* setup, OperationToolModel* operation, SetupTreeModel* tree);

    bool shown() const { return shown_; }
    QVariantList fixtures() const;
    QVariantList library() const { return library_; }
    QString setupName() const;
    bool importing() const { return importing_; }
    QString placing() const { return body_; }
    QString placingName() const;
    bool placingExisting() const { return existing_; }
    QVariantMap values() const { return values_; }
    QString pickMode() const { return pickMode_; }
    QString faceText() const;
    QString targetText() const;
    bool hasFace() const { return !face_.isEmpty(); }
    bool hasTarget() const { return !target_.isEmpty(); }
    QString message() const { return message_; }
    QString messageField() const { return messageField_; }
    QString previewStatus() const { return previewStatus_; }
    bool committing() const { return committing_; }
    bool canCommit() const;
    QString viseName() const;
    QString grip() const { return grip_; }
    void setGrip(const QString& grip);
    bool placingOnStock() const { return placingOnStock_; }
    QString stockMessage() const { return stockMessage_; }
    QString stockStatus() const { return stockStatus_; }

    // ---- QML ----
    Q_INVOKABLE bool open();
    Q_INVOKABLE void close();
    Q_INVOKABLE void escape(); // Esc: the placement if one is open, else the panel
    Q_INVOKABLE void requestImport();
    Q_INVOKABLE bool importFile(const QString& pathOrUrl);
    Q_INVOKABLE bool addFromLibrary(int index);
    Q_INVOKABLE bool place(const QString& bodyId);
    Q_INVOKABLE void setValue(const QString& key, const QVariant& value);
    Q_INVOKABLE void setPickMode(const QString& mode);
    Q_INVOKABLE void clearMate();
    Q_INVOKABLE bool commit();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE bool setInSetup(const QString& bodyId, bool in);
    Q_INVOKABLE bool placeOnStock();

    // ---- the command catalog's hooks (cam.fixtures.*) ----
    QString commandReason(const QString& id) const;
    bool runCommand(const QString& id);

    // ---- C++ (tests) ----
    bool busy() const; // an import, a pick, a preview or a commit under way
    int previewsShown() const { return previewsShown_; }
    // The props Apply would store now, or why it cannot.
    camui::PlacementArgs arguments() const;
    std::optional<FeatureId> featureId() const { return feature_; }

signals:
    void shownChanged();
    void fixturesChanged();
    void libraryChanged();
    void placingChanged();
    void valuesChanged();
    void picksChanged();
    void statusChanged();
    void importDialogRequested();
    void committed(const QString& bodyId);
    void gripChanged();
    void stockStatusChanged();

private:
    // The library vise placeOnStock uses, its jaw and the setup's parallels.
    struct ViseChoice {
        api::cam_fixtures::ViseModel model;
        BodyId vise, jaw;
        std::optional<api::cam_fixtures::ParallelsModel> parallelsModel;
        std::vector<BodyId> parallels;
        QString name;
    };
    std::optional<ViseChoice> chooseVise(const doc::DocState& state) const;
    void setStockStatus(const QString& status, const QString& message);
    void planOnStock(ViseChoice choice, SetupId setup, doc::DocStatePtr state, doc::BuildResultPtr build,
                     std::shared_ptr<const cam::CarriedStock> carried);
    void applyOnStock(const api::cam_fixtures::PlacePlan& plan, const ViseChoice& choice, const QString& setupName);

    api::IApp* app() const;
    doc::Document* liveDoc() const;
    void subscribe();
    void unsubscribe();
    void onDocumentChanged();
    void loadLibrary();
    std::optional<SetupId> targetSetup() const;
    void closePlacement();
    void applyPickMode();
    void onSelectionChanged();
    void takePick(int bodyIndex, kernel::SubId sub);
    void setStatus(const QString& status, const QString& message, const QString& field = QString());
    void schedulePreview();
    void startPreview();
    void onPreviewBuilt(doc::DocStatePtr state, doc::BuildResultPtr build);
    QString bodyName(const QString& id) const;
    doc::Command command() const;

    QPointer<SceneController> scene_;
    QPointer<SelectionModel> selection_;
    QPointer<Session> session_;
    QPointer<AppContext> context_;
    QPointer<CommandCatalog> commands_;
    QPointer<UnitsObject> units_;
    QPointer<SetupToolModel> setupTool_;
    QPointer<OperationToolModel> operationTool_;
    QPointer<SetupTreeModel> tree_;

    std::string docId_;
    doc::Document* doc_ = nullptr;
    int docListener_ = 0;
    int previewListener_ = 0;
    std::uint64_t docGeneration_ = 0;

    bool shown_ = false;
    QVariantList library_;
    bool importing_ = false;

    // The placement.
    QString body_;
    std::optional<FeatureId> feature_;
    bool existing_ = false;
    QVariantMap values_;
    QJsonObject face_, target_;
    QString targetBody_;
    QString pickMode_;
    QString message_, messageField_, previewStatus_;
    bool committing_ = false;
    bool rejected_ = false;
    std::uint8_t savedFilter_ = 0;
    bool filterSaved_ = false;
    std::uint64_t pickSerial_ = 0;
    bool pickPending_ = false;

    QTimer previewTimer_;
    bool previewBusy_ = false;
    bool previewPending_ = false;
    doc::DocStatePtr previewBase_;
    int previewsShown_ = 0;

    // On stock.
    std::vector<api::cam_fixtures::FixtureModel> models_; // the libraries' fixture descriptions
    QString grip_ = QStringLiteral("shorter");
    bool placingOnStock_ = false;
    QString stockMessage_, stockStatus_;
    std::uint64_t stockSerial_ = 0;
    std::unique_ptr<camui::CarriedStockRequest> carried_;
    JobHandle<std::shared_ptr<const api::cam_fixtures::PlacePlan>> stockJob_;
};

} // namespace ray::ui
