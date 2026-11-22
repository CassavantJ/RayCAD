// The `Selection` QML singleton (task T-020): what is selected and hovered in the viewport, the selection filter,
// and the properties read-out of the current element.
//
// An entry is (body, kernel::SubId) plus the naming::TopoRef made through IResolver::makeRef when the body has an
// element map. The renderer's PickHit is kept too, because that is what the viewport highlights. Faces, edges and
// bodies know their SubId at once; a vertex hit names an edge end (render::vertexCode), so its kernel vertex and
// every TopoRef are worked out on the job system (kernel queries never run on the UI thread): until then the entry
// is `pending`. The properties read-out (SelectionInfo.qml) is computed off the UI thread too.
#pragma once

#include "scene_controller.h"

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QVariantList>
#include <QtQml/QQmlEngine>

#include <optional>
#include <vector>

namespace ray::ui {

struct SelectionEntry {
    render::PickHit hit{};              // what the viewport highlights
    int body = -1;                      // index into the scene's bodies
    std::optional<kernel::SubId> sub;   // the kernel element; a vertex has it once `pending` clears
    std::optional<naming::TopoRef> ref; // when the body has an element map
    bool pending = false;               // sub / ref still being worked out
    std::uint64_t serial = 0;
};

class SelectionModel : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(Selection)
    QML_SINGLETON

    // Selection filter: which kinds a click, a box and the hover pick. The renderer prefers the most specific
    // enabled kind inside its pick tolerance (vertex > edge > face > body).
    Q_PROPERTY(bool faces READ faces WRITE setFaces NOTIFY filterChanged)
    Q_PROPERTY(bool edges READ edges WRITE setEdges NOTIFY filterChanged)
    Q_PROPERTY(bool vertices READ vertices WRITE setVertices NOTIFY filterChanged)
    Q_PROPERTY(bool bodies READ bodies WRITE setBodies NOTIFY filterChanged)
    Q_PROPERTY(int count READ count NOTIFY changed)
    Q_PROPERTY(bool pending READ pending NOTIFY changed)
    Q_PROPERTY(QString summary READ summary NOTIFY changed)
    // Properties read-out of the last selected entry: rows of { label, value }.
    Q_PROPERTY(QString infoTitle READ infoTitle NOTIFY infoChanged)
    Q_PROPERTY(QVariantList info READ info NOTIFY infoChanged)
    Q_PROPERTY(bool infoBusy READ infoBusy NOTIFY infoChanged)

public:
    explicit SelectionModel(SceneController* scene, QObject* parent = nullptr);
    ~SelectionModel() override;

    static SelectionModel* create(QQmlEngine* qml, QJSEngine* js);
    static void provideFor(QQmlEngine* engine, SelectionModel* selection);

    bool faces() const { return (mask_ & kFaces) != 0; }
    bool edges() const { return (mask_ & kEdges) != 0; }
    bool vertices() const { return (mask_ & kVertices) != 0; }
    bool bodies() const { return (mask_ & kBodies) != 0; }
    void setFaces(bool on) { setMaskBit(kFaces, on); }
    void setEdges(bool on) { setMaskBit(kEdges, on); }
    void setVertices(bool on) { setMaskBit(kVertices, on); }
    void setBodies(bool on) { setMaskBit(kBodies, on); }
    // render::PickFilter bits of the enabled kinds.
    std::uint8_t filterMask() const
    {
        return pathPicking_ ? std::uint8_t(mask_ | std::uint8_t(render::PickFilter::Toolpath)) : mask_;
    }
    void setFilterMask(std::uint8_t mask);

    int count() const { return static_cast<int>(entries_.size()); }
    bool pending() const;
    QString summary() const;
    QString infoTitle() const { return infoTitle_; }
    QVariantList info() const { return info_; }
    bool infoBusy() const { return infoJob_ != 0; }

    // Viewport input. A click: Ctrl toggles, Shift adds, no modifier replaces; a click on nothing without a
    // modifier clears. A box: the same rules for every hit in it.
    void applyClick(std::optional<render::PickHit> hit, Qt::KeyboardModifiers modifiers);
    void applyBox(const std::vector<render::PickHit>& hits, Qt::KeyboardModifiers modifiers);
    void setHover(std::optional<render::PickHit> hit);
    std::optional<render::PickHit> hover() const { return hover_; }

    Q_INVOKABLE void clear();
    // From the model tree. `modifiers` -1: the keyboard's state now (Qt::KeyboardModifiers otherwise).
    Q_INVOKABLE void selectBody(int bodyIndex, int modifiers = -1);
    Q_INVOKABLE bool isBodySelected(int bodyIndex) const;

    const std::vector<SelectionEntry>& entries() const { return entries_; }
    // What the viewport highlights: the entries' hits, then the path highlight and the tool highlight (below).
    std::vector<render::PickHit> hits() const;

    // T-047, toolpath moves (the G-code viewer, cam/post_view.h). While path picking is on, clicks and the hover pick
    // toolpaths too (render::PickFilter::Toolpath in filterMask()); a click on a toolpath move does not change the
    // selection: it becomes the path highlight and is announced (pathPicked). The path highlight is drawn with the
    // selection (render.h setHighlight: a hit on a backplot item highlights its move).
    void setPathPicking(bool on);
    bool pathPicking() const { return pathPicking_; }
    void setPathHighlight(std::optional<render::PickHit> hit);
    std::optional<render::PickHit> pathHighlight() const { return pathHighlight_; }

    // What a tool panel shows as picked (the operation panel's holes, faces or edges): drawn with the selection's
    // highlight but not part of the selection (a click, Esc or clear() leave it). `emphasis` (the pick a list row under
    // the pointer names) is drawn with the hover highlight while the view has no hover of its own. Empty: none.
    void setToolHighlight(std::vector<render::PickHit> hits, std::optional<render::PickHit> emphasis = std::nullopt);
    const std::vector<render::PickHit>& toolHighlight() const { return toolHighlight_; }
    std::optional<render::PickHit> toolEmphasis() const { return toolEmphasis_; }
    // What the viewport draws with the hover highlight: the element under the pointer, else the tool's emphasis.
    std::optional<render::PickHit> hoverHighlight() const { return hover_ ? hover_ : toolEmphasis_; }

signals:
    void changed();
    void highlightChanged(); // selection or hover: the viewport updates its highlight
    void filterChanged();
    void infoChanged();
    void pathPicked(const ray::render::PickHit& hit); // T-047

private:
    static constexpr std::uint8_t kFaces = static_cast<std::uint8_t>(render::PickFilter::Faces);
    static constexpr std::uint8_t kEdges = static_cast<std::uint8_t>(render::PickFilter::Edges);
    static constexpr std::uint8_t kVertices = static_cast<std::uint8_t>(render::PickFilter::Vertices);
    static constexpr std::uint8_t kBodies = static_cast<std::uint8_t>(render::PickFilter::Bodies);

    void setMaskBit(std::uint8_t bit, bool on);
    std::optional<SelectionEntry> entryFor(const render::PickHit& hit);
    int find(const render::PickHit& hit) const;
    void add(const render::PickHit& hit);
    void toggle(const render::PickHit& hit);
    void resolveLater(SelectionEntry& entry);
    void selectionChanged();
    void updateInfo();
    void resetForNewScene();

    QPointer<SceneController> scene_;
    std::vector<SelectionEntry> entries_;
    std::optional<render::PickHit> hover_;
    std::uint8_t mask_ = kFaces | kEdges | kVertices;
    std::uint64_t nextSerial_ = 1;
    std::uint64_t infoJob_ = 0; // serial of the read-out job in flight, 0 if none
    std::uint64_t infoFor_ = 0; // entry serial the read-out shows
    QString infoTitle_;
    QVariantList info_;
    bool pathPicking_ = false;                     // T-047
    std::optional<render::PickHit> pathHighlight_; // T-047
    std::vector<render::PickHit> toolHighlight_;
    std::optional<render::PickHit> toolEmphasis_;
};

} // namespace ray::ui
