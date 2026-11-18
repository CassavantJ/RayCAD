#include "measure_model.h"

#include "palette_commands.h"
#include "scene_controller.h"
#include "selection_model.h"
#include "session.h"
#include "shortcuts.h"
#include "theme_object.h"
#include "units.h"
#include "viewport_item.h"
#include "viewport_navigation.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QPointF>
#include <QtCore/QVariantMap>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtQuick/QQuickWindow>

#include <algorithm>
#include <set>
#include <utility>

namespace ray::ui {

namespace measure = api::measure;

namespace {

constexpr const char* kPrefix = "measure.";
constexpr const char* kValueContext = "ray::api::measure"; // the api's English labels, translated here

QString qs(const std::string& s)
{
    return QString::fromStdString(s);
}

QString trValue(const std::string& english)
{
    return QCoreApplication::translate(kValueContext, english.c_str());
}

// The measurement a command id asks for ("" if it is not one of the measure actions).
QString kindOfCommand(const QString& id)
{
    static const std::pair<const char*, const char*> kKinds[] = {
        {"measure.distance", "distance"},       {"measure.angle", "angle"},
        {"measure.radius", "radius"},           {"measure.massProperties", "massProperties"},
        {"measure.boundingBox", "boundingBox"}, {"measure.selection", "auto"},
    };
    for (const auto& [command, kind] : kKinds) {
        if (id == QLatin1String(command))
            return QString::fromLatin1(kind);
    }
    return {};
}

QVariantList pointPair(QPointF a, QPointF b)
{
    return QVariantList{QVariant::fromValue(a), QVariant::fromValue(b)};
}

} // namespace

// ---------- MeasureListModel ----------
MeasureListModel::MeasureListModel(QList<QByteArray> roles, QObject* parent)
    : QAbstractListModel(parent), roles_(std::move(roles))
{
}

int MeasureListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(items_.size());
}

QVariant MeasureListModel::data(const QModelIndex& index, int role) const
{
    const int column = role - Qt::UserRole - 1;
    if (!index.isValid() || index.row() >= items_.size() || column < 0 || column >= roles_.size())
        return {};
    return items_.at(index.row()).toMap().value(QString::fromLatin1(roles_.at(column)));
}

QHash<int, QByteArray> MeasureListModel::roleNames() const
{
    QHash<int, QByteArray> names;
    for (int i = 0; i < roles_.size(); ++i)
        names.insert(Qt::UserRole + 1 + i, roles_.at(i));
    return names;
}

void MeasureListModel::setItems(const QVariantList& items)
{
    if (items.size() != items_.size()) {
        beginResetModel();
        items_ = items;
        endResetModel();
        return;
    }
    if (items == items_)
        return;
    items_ = items;
    if (!items_.isEmpty())
        emit dataChanged(index(0), index(static_cast<int>(items_.size()) - 1));
}

// ---------- MeasureModel ----------
MeasureModel::MeasureModel(SceneController* scene, SelectionModel* selection, Session* session, QObject* parent)
    : QObject(parent), scene_(scene), selection_(selection), session_(session),
      rowsModel_(new MeasureListModel({"key", "label", "value", "primary"}, this)),
      labelsModel_(new MeasureListModel({"px", "py", "text", "pinned", "primary"}, this)),
      pointsModel_(new MeasureListModel({"px", "py", "pinned"}, this))
{
    connect(this, &MeasureModel::currentChanged, this, [this] { rowsModel_->setItems(rows()); });
    if (selection_)
        connect(selection_, &SelectionModel::changed, this, &MeasureModel::selectionChanged);
    if (scene_) {
        connect(scene_, &SceneController::bodiesChanged, this, [this] {
            // A new body set: the current result belongs to the old one (pinned ones stay until the document changes).
            job_ = 0;
            current_.reset();
            dirty_ = true;
            emit busyChanged();
            emit currentChanged();
            redrawWitness();
        });
    }
    if (session_) {
        connect(session_, &Session::documentChanged, this, &MeasureModel::documentChanged);
        documentChanged();
    }
}

MeasureModel::~MeasureModel()
{
    if (commands_)
        commands_->setUiHandler(QString::fromLatin1(kPrefix), {});
    // The witness lines belong to the viewport; it may be gone already (the window goes before the singletons).
    if (view_ && view_->viewport() != nullptr) {
        clearItems(currentItems_);
        clearItems(pinnedItems_);
    }
}

MeasureModel* MeasureModel::create(QQmlEngine* qml, QJSEngine*)
{
    SceneController* scene = nullptr;
    SelectionModel* selection = nullptr;
    Session* session = nullptr;
    CommandCatalog* commands = nullptr;
    ShortcutManager* shortcuts = nullptr;
    if (qml != nullptr) {
        const QString uri = QStringLiteral("RayCad.Ui");
        scene = qml->singletonInstance<SceneController*>(uri, QStringLiteral("Scene"));
        selection = qml->singletonInstance<SelectionModel*>(uri, QStringLiteral("Selection"));
        session = qml->singletonInstance<Session*>(uri, QStringLiteral("Session"));
        commands = qml->singletonInstance<CommandCatalog*>(uri, QStringLiteral("Commands"));
        shortcuts = qml->singletonInstance<ShortcutManager*>(uri, QStringLiteral("Shortcuts"));
    }
    auto* model = new MeasureModel(scene, selection, session);
    model->attachCommands(commands, shortcuts);
    if (qml != nullptr) // T-024: the one source of the display units
        model->setUnitsSource(
            qml->singletonInstance<UnitsObject*>(QStringLiteral("RayCad.Ui"), QStringLiteral("Units")));
    return model; // owned by the engine
}

void MeasureModel::attachCommands(CommandCatalog* commands, ShortcutManager* shortcuts)
{
    commands_ = commands;
    if (commands_) {
        QPointer<MeasureModel> self(this);
        commands_->setUiHandler(
            QString::fromLatin1(kPrefix),
            CommandCatalog::UiHandler{[self](const QString& id) {
                                          return self ? self->commandReason(id)
                                                      : QCoreApplication::translate("ray::ui::MeasureModel",
                                                                                    "The measure tool is not "
                                                                                    "available");
                                      },
                                      [self](const QString& id) { return self && self->runCommand(id); }});
    }
    if (shortcuts) {
        // A key runs its command right after `triggered`: remember that this run came from a key.
        connect(shortcuts, &ShortcutManager::triggered, this, [this](const QString& id, const QString&) {
            if (!id.startsWith(QLatin1String(kPrefix)))
                return;
            fromShortcut_ = true;
            QMetaObject::invokeMethod(this, [this] { fromShortcut_ = false; }, Qt::QueuedConnection);
        });
    }
}

// ---------- units ----------
void MeasureModel::setUnitsSource(UnitsObject* units)
{
    if (units == units_)
        return;
    if (units_)
        disconnect(units_, nullptr, this, nullptr);
    units_ = units;
    if (!units_)
        return;
    connect(units_, &UnitsObject::changed, this, [this] {
        if (units_)
            applyFormat(units_->format());
    });
    applyFormat(units_->format());
}

void MeasureModel::setUnitFormat(UnitFormat format)
{
    // T-024: the Units singleton decides (document units, the session toggle, decimals from the settings); a request
    // here switches the display units there, and the new format comes back through Units::changed.
    if (units_) {
        units_->setDisplaySystem(format.system == UnitSystem::Metric ? QStringLiteral("metric")
                                                                     : QStringLiteral("inch"));
        return;
    }
    applyFormat(format);
}

void MeasureModel::applyFormat(UnitFormat format)
{
    format.decimals = std::clamp(format.decimals, 0, 8);
    if (format.system == format_.system && format.decimals == format_.decimals)
        return;
    format_ = format;
    emit unitsChanged();
    emit densityTextChanged();
    emit currentChanged();
    emit pinnedChanged();
    updateOverlay();
}

void MeasureModel::setUnits(const QString& system, int decimals)
{
    const QString s = system.trimmed().toLower();
    const bool mm = s == QLatin1String("mm") || s == QLatin1String("metric") || s == QLatin1String("millimetre") ||
                    s == QLatin1String("millimeter");
    setUnitFormat(UnitFormat{mm ? UnitSystem::Metric : UnitSystem::Inch, decimals >= 0 ? decimals : (mm ? 3 : 4)});
}

QString MeasureModel::unitSystem() const
{
    return metric() ? QStringLiteral("mm") : QStringLiteral("inch");
}

void MeasureModel::setDensity(double gPerCm3)
{
    if (!(gPerCm3 > 0) || gPerCm3 == density_)
        return;
    density_ = gPerCm3;
    emit densityChanged();
    emit densityTextChanged();
    // Only the mass depends on it: measure again what shows a mass.
    if (current_ && current_->find("mass") != nullptr)
        start(mode_);
}

QString MeasureModel::densityText() const
{
    measure::Value v;
    v.quantity = measure::Quantity::Density;
    v.number = density_;
    return format(v);
}

// ---------- the read-out ----------
QString MeasureModel::format(const measure::Value& v) const
{
    return qs(measure::formatValue(v, format_));
}

QVariantList MeasureModel::rowsOf(const measure::Measurement& m) const
{
    QVariantList rows;
    for (const measure::Value& v : m.values) {
        rows << QVariantMap{{QStringLiteral("key"), qs(v.key)},
                            {QStringLiteral("label"), trValue(v.label)},
                            {QStringLiteral("value"), format(v)},
                            {QStringLiteral("primary"), v.primary}};
    }
    return rows;
}

QString MeasureModel::title() const
{
    return current_ ? trValue(current_->title) : QString();
}

QVariantList MeasureModel::rows() const
{
    return current_ ? rowsOf(*current_) : QVariantList();
}

QStringList MeasureModel::notes() const
{
    QStringList out;
    if (current_) {
        for (const std::string& n : current_->notes)
            out << trValue(n);
    }
    return out;
}

QString MeasureModel::valueText(const QString& key) const
{
    if (!current_)
        return {};
    const measure::Value* v = current_->find(key.toStdString());
    return v != nullptr ? format(*v) : QString();
}

QVariantList MeasureModel::pinned() const
{
    QVariantList out;
    for (const Pinned& p : pinned_) {
        QStringList notes;
        for (const std::string& n : p.m.notes)
            notes << trValue(n);
        out << QVariantMap{{QStringLiteral("id"), p.id},
                           {QStringLiteral("title"), trValue(p.m.title)},
                           {QStringLiteral("rows"), rowsOf(p.m)},
                           {QStringLiteral("notes"), notes}};
    }
    return out;
}

void MeasureModel::setHint(const QString& text, bool error)
{
    hint_ = text;
    hintIsError_ = error;
}

// ---------- measuring ----------
void MeasureModel::setActive(bool on)
{
    if (on == active_)
        return;
    active_ = on;
    emit activeChanged();
    if (active_ && dirty_)
        start(mode_);
    else
        redrawWitness();
}

void MeasureModel::selectionChanged()
{
    // A new selection gets the automatic measurement again.
    mode_ = QStringLiteral("auto");
    if (!active_) {
        dirty_ = true;
        return;
    }
    start(mode_);
}

void MeasureModel::measure(const QString& kind)
{
    mode_ = kind.isEmpty() || kind == QLatin1String("selection") ? QStringLiteral("auto") : kind;
    start(mode_);
}

std::optional<MeasureModel::Request> MeasureModel::requestFor(const QString& kind, QString* why) const
{
    const auto fail = [why](const QString& text) {
        if (why != nullptr)
            *why = text;
        return std::nullopt;
    };
    if (!scene_ || scene_->bodyCount() == 0)
        return fail(tr("Open a part to measure it."));
    Request r;
    r.kind = kind;
    std::vector<std::pair<int, kernel::SubId>> picked;
    if (selection_) {
        for (const SelectionEntry& e : selection_->entries()) {
            if (e.pending || !e.sub)
                return fail(tr("Working out the selection\u2026"));
            picked.emplace_back(e.body, *e.sub);
        }
    }
    if (kind == QLatin1String("massProperties") || kind == QLatin1String("boundingBox")) {
        // The selected elements' bodies, else every body.
        std::set<int> bodies;
        for (const auto& [body, sub] : picked)
            bodies.insert(body);
        if (bodies.empty()) {
            r.wholeScene = true;
        } else {
            for (const int b : bodies)
                r.elements.emplace_back(b, kernel::SubId{kernel::SubKind::Solid, 0});
        }
        return r;
    }
    if (picked.empty())
        return fail(tr("Select a face, edge, vertex or body to see its measures, or two of them to measure between "
                       "them. Ctrl adds to the selection."));
    if ((kind == QLatin1String("distance") || kind == QLatin1String("angle")) && picked.size() < 2)
        return fail(tr("Select two elements (Ctrl adds to the selection)."));
    if (kind == QLatin1String("radius")) {
        const kernel::SubKind k = picked.back().second.kind;
        if (k != kernel::SubKind::Face && k != kernel::SubKind::Edge)
            return fail(tr("Select a round edge or face."));
        picked.erase(picked.begin(), picked.end() - 1);
    }
    r.elements = std::move(picked);
    return r;
}

void MeasureModel::start(const QString& kind)
{
    dirty_ = false;
    QString why;
    const std::optional<Request> request = requestFor(kind, &why);
    IJobSystem* jobs = scene_ ? scene_->jobs() : nullptr;
    if (!request || jobs == nullptr) {
        job_ = 0;
        current_.reset();
        setHint(why, false);
        emit busyChanged();
        emit currentChanged();
        redrawWitness();
        return;
    }
    const std::shared_ptr<const SceneSnapshot> snap = scene_->snapshot();
    const std::uint64_t serial = nextSerial_++;
    job_ = serial;
    const double density = density_;
    const Request req = *request;
    JobHandle<measure::Measurement> job = jobs->submit<measure::Measurement>(
        JobInfo{"Measure", JobPriority::Interactive, false},
        [snap, req, density](JobContext& ctx) -> Result<measure::Measurement> {
            std::vector<measure::Element> elements;
            for (const auto& [body, sub] : req.elements) {
                if (body < 0 || static_cast<std::size_t>(body) >= snap->bodies.size())
                    return tl::unexpected(Error{ErrorCode::NotFound, "The selected body is gone.", {}});
                const api::viewer::Body& b = snap->bodies[static_cast<std::size_t>(body)];
                elements.push_back(measure::Element{b.shape, sub, b.name});
            }
            std::vector<kernel::Shape> shapes;
            if (req.wholeScene) {
                for (const api::viewer::Body& b : snap->bodies)
                    shapes.push_back(b.shape);
            } else {
                for (const measure::Element& e : elements)
                    shapes.push_back(e.shape);
            }
            const QString& k = req.kind;
            if (k == QLatin1String("distance"))
                return measure::distance(elements[elements.size() - 2], elements.back(), ctx);
            if (k == QLatin1String("angle"))
                return measure::angle(elements[elements.size() - 2], elements.back(), ctx);
            if (k == QLatin1String("radius"))
                return measure::radius(elements.back(), ctx);
            if (k == QLatin1String("massProperties"))
                return measure::massProperties(shapes, density, ctx);
            if (k == QLatin1String("boundingBox"))
                return measure::boundingBox(shapes, ctx);
            return measure::selection(elements, density, ctx);
        });
    QPointer<MeasureModel> self(this);
    const std::uint64_t generation = snap->generation;
    job.then([self, serial, generation](Result<measure::Measurement> r) {
        if (!self || self->job_ != serial)
            return; // superseded
        self->job_ = 0;
        emit self->busyChanged();
        if (!self->scene_ || self->scene_->snapshot()->generation != generation) {
            self->dirty_ = true;
            emit self->currentChanged();
            return;
        }
        if (r) {
            self->current_ = std::move(*r);
            self->setHint({}, false);
        } else {
            self->current_.reset();
            self->setHint(QString::fromStdString(r.error().message), r.error().code != ErrorCode::Cancelled);
        }
        emit self->currentChanged();
        self->redrawWitness();
    });
    emit busyChanged();
    redrawWitness(); // the previous result's witness goes while this one is worked out
}

void MeasureModel::documentChanged()
{
    // Pinned results belong to the document they were measured in. (The units are not decided here: Units does.)
    api::OpenDocument* od = session_ ? session_->activeDocument() : nullptr;
    const std::string id = od != nullptr ? od->id : std::string();
    if (id != documentId_) {
        documentId_ = id;
        clearPinned();
    }
}

// ---------- pinned ----------
bool MeasureModel::pin()
{
    if (!current_)
        return false;
    pinned_.push_back(Pinned{nextPinId_++, *current_});
    emit pinnedChanged();
    redrawWitness();
    return true;
}

void MeasureModel::unpin(int id)
{
    const auto it = std::find_if(pinned_.begin(), pinned_.end(), [id](const Pinned& p) { return p.id == id; });
    if (it == pinned_.end())
        return;
    pinned_.erase(it);
    emit pinnedChanged();
    redrawWitness();
}

void MeasureModel::clearPinned()
{
    if (pinned_.empty())
        return;
    pinned_.clear();
    emit pinnedChanged();
    redrawWitness();
}

// ---------- clipboard ----------
QString MeasureModel::textOf(const measure::Measurement& m) const
{
    QStringList lines{trValue(m.title)};
    for (const measure::Value& v : m.values)
        lines << QStringLiteral("%1: %2").arg(trValue(v.label), format(v));
    for (const std::string& n : m.notes)
        lines << trValue(n);
    return lines.join(QLatin1Char('\n'));
}

void MeasureModel::toClipboard(const QString& text)
{
    if (text.isEmpty())
        return;
    if (QClipboard* clipboard = QGuiApplication::clipboard())
        clipboard->setText(text);
    emit copied(text);
}

void MeasureModel::copyValue(int row)
{
    if (!current_ || row < 0 || row >= static_cast<int>(current_->values.size()))
        return;
    toClipboard(format(current_->values[static_cast<std::size_t>(row)]));
}

void MeasureModel::copyPinnedValue(int id, int row)
{
    for (const Pinned& p : pinned_) {
        if (p.id == id && row >= 0 && row < static_cast<int>(p.m.values.size()))
            toClipboard(format(p.m.values[static_cast<std::size_t>(row)]));
    }
}

void MeasureModel::copyAll()
{
    QStringList parts;
    if (current_)
        parts << textOf(*current_);
    for (const Pinned& p : pinned_)
        parts << textOf(p.m);
    toClipboard(parts.join(QStringLiteral("\n\n")));
}

// ---------- commands ----------
QString MeasureModel::commandReason(const QString& id) const
{
    if (id == QLatin1String("measure.minInternalCornerRadius"))
        return tr("Coming later: it arrives with the corner analysis for CAM (task T-050)");
    if (id == QLatin1String("measure.tool"))
        return {};
    if (id == QLatin1String("measure.pin"))
        return current_ ? QString() : tr("Nothing is measured yet");
    if (id == QLatin1String("measure.clearPinned"))
        return pinned_.empty() ? tr("Nothing is pinned") : QString();
    if (id == QLatin1String("measure.copyAll"))
        return current_ || !pinned_.empty() ? QString() : tr("Nothing is measured yet");
    const QString kind = kindOfCommand(id);
    if (kind.isEmpty())
        return tr("Unknown measure command");
    QString why;
    if (!requestFor(kind, &why) && !(selection_ && selection_->pending()))
        return why;
    return {};
}

bool MeasureModel::runCommand(const QString& id)
{
    const bool key = fromShortcut_;
    fromShortcut_ = false;
    if (key) {
        // The M key works while the 3D view has the focus; elsewhere it does nothing.
        QQuickWindow* window = view_ ? view_->window() : nullptr;
        if (window == nullptr || qobject_cast<ViewportItem*>(window->activeFocusItem()) == nullptr)
            return true;
    }
    if (id == QLatin1String("measure.tool")) {
        emit showRequested();
        setActive(true);
        measure(QStringLiteral("auto"));
        return true;
    }
    if (id == QLatin1String("measure.pin"))
        return pin();
    if (id == QLatin1String("measure.clearPinned")) {
        clearPinned();
        return true;
    }
    if (id == QLatin1String("measure.copyAll")) {
        copyAll();
        return true;
    }
    const QString kind = kindOfCommand(id);
    if (kind.isEmpty())
        return false;
    emit showRequested();
    setActive(true);
    measure(kind);
    return true;
}

// ---------- the viewport ----------
QObject* MeasureModel::view() const
{
    return view_.data();
}

void MeasureModel::setView(QObject* view)
{
    auto* item = qobject_cast<ViewportItem*>(view);
    if (item == view_.data())
        return;
    if (view_ && view_->viewport() != nullptr) {
        clearItems(currentItems_);
        clearItems(pinnedItems_);
    }
    currentItems_.clear();
    pinnedItems_.clear();
    for (const QMetaObject::Connection& c : viewConnections_)
        disconnect(c);
    viewConnections_.clear();
    view_ = item;
    if (view_) {
        viewConnections_ << connect(view_, &ViewportItem::cameraChanged, this, &MeasureModel::updateOverlay);
        viewConnections_ << connect(view_, &QQuickItem::widthChanged, this, &MeasureModel::updateOverlay);
        viewConnections_ << connect(view_, &QQuickItem::heightChanged, this, &MeasureModel::updateOverlay);
        viewConnections_ << connect(view_, &ViewportItem::themeChanged, this, &MeasureModel::redrawWitness);
    }
    emit viewChanged();
    redrawWitness();
}

void MeasureModel::clearItems(std::vector<render::ItemId>& items)
{
    render::IRenderViewport* viewport = view_ ? view_->viewport() : nullptr;
    if (viewport != nullptr) {
        for (const render::ItemId id : items)
            viewport->remove(id);
    }
    items.clear();
}

void MeasureModel::addWitness(const measure::Witness& witness, bool pinned, std::vector<render::ItemId>& items)
{
    render::IRenderViewport* viewport = view_ ? view_->viewport() : nullptr;
    if (viewport == nullptr)
        return;
    std::vector<Vec3> solid, dashed;
    for (const measure::Segment& s : witness.lines) {
        std::vector<Vec3>& to = s.construction ? dashed : solid;
        to.push_back(s.a);
        to.push_back(s.b);
    }
    // Widths from the theme: the current result's dimension lines as heavy as a focus ring, the rest thin.
    const ThemeObject* theme = view_->theme();
    const float thin = theme != nullptr ? float(theme->lineThinPx()) : 1.0f;
    const float strong = theme != nullptr ? float(theme->focusRingWidth()) : 2.0f;
    if (!solid.empty())
        items.push_back(viewport->addLines(solid, render::Token::AccentThinLine, pinned ? thin : strong, false));
    if (!dashed.empty())
        items.push_back(viewport->addLines(dashed, render::Token::AccentThinLine, thin, true));
}

void MeasureModel::redrawWitness()
{
    clearItems(currentItems_);
    clearItems(pinnedItems_);
    if (active_ && current_ && job_ == 0)
        addWitness(current_->witness, false, currentItems_);
    for (const Pinned& p : pinned_)
        addWitness(p.m.witness, true, pinnedItems_);
    if (view_)
        view_->update();
    updateOverlay();
}

void MeasureModel::updateOverlay()
{
    labels_.clear();
    points_.clear();
    currentPaths_.clear();
    pinnedPaths_.clear();
    if (view_ && view_->width() > 0 && view_->height() > 0) {
        const render::Camera camera = view_->camera();
        const double w = view_->width();
        const double h = view_->height();
        const auto project = [&](Vec3 p) {
            return nav::project(camera, p, w, h);
        };
        const auto add = [&](const measure::Measurement& m, bool pinned, QVariantList& paths) {
            for (const measure::Segment& s : m.witness.lines) {
                if (s.construction)
                    continue;
                const auto a = project(s.a);
                const auto b = project(s.b);
                if (a && b)
                    paths << QVariant(pointPair(*a, *b));
            }
            for (const Vec3& p : m.witness.points) {
                if (const auto at = project(p))
                    points_ << QVariantMap{{QStringLiteral("px"), at->x()},
                                           {QStringLiteral("py"), at->y()},
                                           {QStringLiteral("pinned"), pinned}};
            }
            for (const measure::Label& l : m.witness.labels) {
                const measure::Value* v = m.find(l.key);
                const auto at = project(l.at);
                if (v == nullptr || !at)
                    continue;
                labels_ << QVariantMap{{QStringLiteral("px"), at->x()},
                                       {QStringLiteral("py"), at->y()},
                                       {QStringLiteral("text"), format(*v)},
                                       {QStringLiteral("pinned"), pinned},
                                       {QStringLiteral("primary"), v->primary}};
            }
        };
        for (const Pinned& p : pinned_)
            add(p.m, true, pinnedPaths_);
        if (active_ && current_ && job_ == 0)
            add(*current_, false, currentPaths_);
    }
    labelsModel_->setItems(labels_);
    pointsModel_->setItems(points_);
    emit overlayChanged();
}

} // namespace ray::ui
