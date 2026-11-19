#include "constraints_model.h"

#include "../app_context.h"
#include "../palette_commands.h"
#include "../scene_controller.h"
#include "../theme_object.h"
#include "../units.h"
#include "sketch_mode.h"

#include <ray_api/sketch_edit.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtGui/QKeyEvent>
#include <QtQuick/QQuickItem>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace ray::ui {

namespace su = sketchui;
using sketch::ConstraintType;

namespace {

constexpr const char* kTrContext = "ray::ui::SketchConstraintsModel";
constexpr const char* kPrefix = "sketch.constrain.";

QString trs(const char* text)
{
    return QCoreApplication::translate(kTrContext, text);
}

QString qs(const std::string& s)
{
    return QString::fromStdString(s);
}

std::optional<Uuid> uuidOf(const QString& text)
{
    if (text.isEmpty())
        return std::nullopt;
    const Uuid id = Uuid::fromString(text.toStdString());
    if (id == Uuid{} || qs(id.toString()) != text)
        return std::nullopt;
    return id;
}

std::string json(const QJsonObject& o)
{
    return QJsonDocument(o).toJson(QJsonDocument::Compact).toStdString();
}

// The shape shown, as the "solved" argument of the constraint commands (none: nothing is added).
void addSolved(QJsonObject& args, const std::optional<sketch::Sketch>& shown)
{
    if (shown) {
        const std::string text = api::sketch_edit::sketchJson(*shown);
        args.insert(QStringLiteral("solved"), QJsonDocument::fromJson(QByteArray::fromStdString(text)).object());
    }
}

bool signedType(ConstraintType t)
{
    return t == ConstraintType::DistanceX || t == ConstraintType::DistanceY || t == ConstraintType::Angle;
}

// A stored expression of a signed dimension whose value is negative is "-(<as typed>)"; the field shows it as typed.
QString unwrapNegative(const QString& expression)
{
    if (expression.startsWith(QLatin1String("-(")) && expression.endsWith(QLatin1Char(')')))
        return expression.mid(2, expression.size() - 3);
    return expression;
}

// The evaluation error without the "The dimension: 'value': " lead-in.
QString shortError(const std::string& message)
{
    QString m = qs(message);
    const QString lead = QStringLiteral("The dimension: 'value': ");
    if (m.startsWith(lead))
        m = m.mid(lead.size());
    return m;
}

const char* kGeoTitles[] = {
    QT_TRANSLATE_NOOP("ray::ui::SketchConstraintsModel", "Coincident"),
    QT_TRANSLATE_NOOP("ray::ui::SketchConstraintsModel", "Horizontal"),
    QT_TRANSLATE_NOOP("ray::ui::SketchConstraintsModel", "Vertical"),
    QT_TRANSLATE_NOOP("ray::ui::SketchConstraintsModel", "Parallel"),
    QT_TRANSLATE_NOOP("ray::ui::SketchConstraintsModel", "Perpendicular"),
    QT_TRANSLATE_NOOP("ray::ui::SketchConstraintsModel", "Tangent"),
    QT_TRANSLATE_NOOP("ray::ui::SketchConstraintsModel", "Equal"),
    QT_TRANSLATE_NOOP("ray::ui::SketchConstraintsModel", "Midpoint"),
    QT_TRANSLATE_NOOP("ray::ui::SketchConstraintsModel", "Concentric"),
    QT_TRANSLATE_NOOP("ray::ui::SketchConstraintsModel", "Collinear"),
    QT_TRANSLATE_NOOP("ray::ui::SketchConstraintsModel", "Symmetric"),
    QT_TRANSLATE_NOOP("ray::ui::SketchConstraintsModel", "Fix"),
    QT_TRANSLATE_NOOP("ray::ui::SketchConstraintsModel", "Select two points, or a point and a curve"),
    QT_TRANSLATE_NOOP("ray::ui::SketchConstraintsModel", "Select one or more lines, or two points"),
    QT_TRANSLATE_NOOP("ray::ui::SketchConstraintsModel", "Select two or more lines"),
    QT_TRANSLATE_NOOP("ray::ui::SketchConstraintsModel", "Select two lines"),
    QT_TRANSLATE_NOOP("ray::ui::SketchConstraintsModel", "Select an arc or circle and a line, arc or circle"),
    QT_TRANSLATE_NOOP("ray::ui::SketchConstraintsModel", "Select two or more lines, or two or more arcs and circles"),
    QT_TRANSLATE_NOOP("ray::ui::SketchConstraintsModel", "Select a point and a line"),
    QT_TRANSLATE_NOOP("ray::ui::SketchConstraintsModel", "Select two arcs or circles"),
    QT_TRANSLATE_NOOP("ray::ui::SketchConstraintsModel", "Select two points and a line, or three points"),
    QT_TRANSLATE_NOOP("ray::ui::SketchConstraintsModel", "Select the points or curves to fix"),
    QT_TRANSLATE_NOOP("ray::ui::SketchConstraintsModel", "Point on curve"),
    QT_TRANSLATE_NOOP("ray::ui::SketchConstraintsModel", "Distance"),
    QT_TRANSLATE_NOOP("ray::ui::SketchConstraintsModel", "Horizontal distance"),
    QT_TRANSLATE_NOOP("ray::ui::SketchConstraintsModel", "Vertical distance"),
    QT_TRANSLATE_NOOP("ray::ui::SketchConstraintsModel", "Radius"),
    QT_TRANSLATE_NOOP("ray::ui::SketchConstraintsModel", "Diameter"),
    QT_TRANSLATE_NOOP("ray::ui::SketchConstraintsModel", "Angle"),
};

} // namespace

SketchConstraintsModel::SketchConstraintsModel(SketchModel* tool, SceneController* scene, AppContext* context,
                                               QObject* parent)
    : QObject(parent), tool_(tool), scene_(scene), context_(context),
      glyphs_({QStringLiteral("cid"), QStringLiteral("slot"), QStringLiteral("kind"), QStringLiteral("title"),
               QStringLiteral("px"), QStringLiteral("py"), QStringLiteral("stack"), QStringLiteral("bad"),
               QStringLiteral("hovered"), QStringLiteral("selected")}),
      labels_({QStringLiteral("cid"), QStringLiteral("slot"), QStringLiteral("type"), QStringLiteral("title"),
               QStringLiteral("text"), QStringLiteral("expression"), QStringLiteral("px"), QStringLiteral("py"),
               QStringLiteral("driven"), QStringLiteral("bad"), QStringLiteral("hovered"), QStringLiteral("selected")})
{
    (void)kGeoTitles;
    QQmlEngine::setObjectOwnership(&glyphs_, QQmlEngine::CppOwnership);
    QQmlEngine::setObjectOwnership(&labels_, QQmlEngine::CppOwnership);
    if (!tool_)
        return;
    connect(tool_, &SketchModel::activeChanged, this, &SketchConstraintsModel::onActiveChanged);
    connect(tool_, &SketchModel::modelChanged, this, &SketchConstraintsModel::onModelChanged);
    connect(tool_, &SketchModel::overlayChanged, this, &SketchConstraintsModel::refresh);
    connect(tool_, &SketchModel::solveFinished, this, &SketchConstraintsModel::refresh);
    connect(tool_, &SketchModel::statusChanged, this, &SketchConstraintsModel::refresh);
    connect(tool_, &SketchModel::selectionChanged, this, [this] {
        if (tool_ && !tool_->selectedIds().empty() && selected_) {
            selected_.reset();
            emit selectionChanged();
            updateHighlight();
            refresh();
        }
        refreshActions();
    });
    connect(tool_, &SketchModel::toolChanged, this, [this] {
        if (!settingTool_ && dimensionTool_)
            setDimensionTool(false);
    });
    connect(tool_, &SketchModel::constraintEditRequested, this, &SketchConstraintsModel::beginEdit);
    installHooks();
}

SketchConstraintsModel::~SketchConstraintsModel()
{
    if (commands_)
        commands_->setUiHandler(QString::fromLatin1(kPrefix), {});
    if (tool_)
        tool_->setInputHook({});
}

SketchConstraintsModel* SketchConstraintsModel::create(QQmlEngine* qml, QJSEngine*)
{
    SketchModel* tool = nullptr;
    SceneController* scene = nullptr;
    AppContext* context = nullptr;
    CommandCatalog* commands = nullptr;
    UnitsObject* units = nullptr;
    ThemeObject* theme = nullptr;
    if (qml != nullptr) {
        const QString uri = QStringLiteral("RayCad.Ui");
        tool = qml->singletonInstance<SketchModel*>(uri, QStringLiteral("SketchTool"));
        scene = qml->singletonInstance<SceneController*>(uri, QStringLiteral("Scene"));
        context = qml->singletonInstance<AppContext*>(uri, QStringLiteral("AppContext"));
        commands = qml->singletonInstance<CommandCatalog*>(uri, QStringLiteral("Commands"));
        units = qml->singletonInstance<UnitsObject*>(uri, QStringLiteral("Units"));
        theme = qml->singletonInstance<ThemeObject*>(uri, QStringLiteral("Theme"));
    }
    auto* model = new SketchConstraintsModel(tool, scene, context);
    model->setCommands(commands);
    model->setUnits(units);
    model->setTheme(theme);
    return model; // owned by the engine
}

void SketchConstraintsModel::setCommands(CommandCatalog* commands)
{
    commands_ = commands;
    if (!commands_)
        return;
    QPointer<SketchConstraintsModel> self(this);
    commands_->setUiHandler(
        QString::fromLatin1(kPrefix),
        CommandCatalog::UiHandler{[self](const QString& id) {
                                      return self ? self->commandReason(id) : trs("The sketch tools are not available");
                                  },
                                  [self](const QString& id) { return self && self->runCommand(id); }});
}

void SketchConstraintsModel::setUnits(UnitsObject* units)
{
    units_ = units;
    if (units_)
        connect(units_, &UnitsObject::changed, this, &SketchConstraintsModel::refresh);
}

void SketchConstraintsModel::setTheme(ThemeObject* theme)
{
    theme_ = theme;
}

api::IApp* SketchConstraintsModel::app() const
{
    return context_ ? context_->app() : nullptr;
}

void SketchConstraintsModel::installHooks()
{
    QPointer<SketchConstraintsModel> self(this);
    SketchModel::InputHook hook;
    hook.move = [self](QPointF pos, Qt::KeyboardModifiers) {
        return self && self->pointerMove(pos);
    };
    hook.press = [self](QPointF pos, Qt::KeyboardModifiers) {
        return self && self->pointerPress(pos);
    };
    hook.release = [self](QPointF, Qt::KeyboardModifiers) {
        return self && self->dimensionTool_;
    };
    hook.doubleClick = [self](QPointF, Qt::KeyboardModifiers) {
        return self && self->dimensionTool_;
    };
    hook.wantsKey = [self](const QKeyEvent* e) {
        return self && self->wantsKey(e);
    };
    hook.keyPress = [self](QKeyEvent* e) {
        return self && self->keyPress(e);
    };
    tool_->setInputHook(std::move(hook));
}

// ---------- following SketchTool ----------
void SketchConstraintsModel::onActiveChanged()
{
    if (!tool_ || !tool_->active()) {
        if (dimensionTool_) {
            dimensionTool_ = false;
            emit toolChanged();
        }
        picks_.clear();
        candidate_.reset();
        hover_ = {};
        selected_.reset();
        hovered_.reset();
        dragged_.clear();
        storedLabels_.clear();
        evaluated_ = {};
        bad_.clear();
        if (edit_) {
            edit_.reset();
            emit editChanged();
        }
        emit selectionChanged();
        emit hoverChanged();
    }
    onModelChanged();
}

void SketchConstraintsModel::onModelChanged()
{
    storedLabels_.clear();
    evaluated_ = {};
    if (tool_ && tool_->active()) {
        evaluated_ = tool_->model();
        api::IApp* a = app();
        api::OpenDocument* od = a != nullptr ? a->active() : nullptr;
        const auto feature = tool_->featureId();
        if (od != nullptr && od->document && feature && tool_->featureInDocument()) {
            const doc::DocStatePtr state = od->document->state();
            evaluated_ = api::sketch_edit::withEvaluatedDimensions(*state, std::move(evaluated_));
            storedLabels_ = api::sketch_edit::labelsOf(*state, *feature);
        }
    }
    // Forget what is gone.
    const auto gone = [this](const std::optional<Uuid>& id) {
        return id && constraintById(*id) == nullptr;
    };
    if (gone(selected_)) {
        selected_.reset();
        emit selectionChanged();
    }
    if (gone(hovered_)) {
        hovered_.reset();
        emit hoverChanged();
    }
    if (edit_ && !edit_->isNew && constraintById(edit_->id) == nullptr) {
        edit_.reset();
        emit editChanged();
    }
    if (tool_) {
        const su::SketchView& view = tool_->displayView();
        picks_.erase(std::remove_if(picks_.begin(), picks_.end(),
                                    [&](const SketchEntId& id) {
                                        return view.pointEntry(id) == nullptr && view.curve(id) == nullptr;
                                    }),
                     picks_.end());
    }
    emit selectionChanged(); // driven flag may have changed
    updateHighlight();
    refreshActions();
    refresh();
}

const sketch::Constraint* SketchConstraintsModel::constraintById(const Uuid& id) const
{
    if (!tool_)
        return nullptr;
    for (const sketch::Constraint& c : tool_->model().constraints) {
        if (c.id == id)
            return &c;
    }
    return nullptr;
}

std::optional<Vec2> SketchConstraintsModel::offsetOf(const Uuid& id) const
{
    if (const auto it = dragged_.find(id); it != dragged_.end())
        return it->second;
    if (const auto it = storedLabels_.find(id); it != storedLabels_.end())
        return it->second;
    return std::nullopt;
}

double SketchConstraintsModel::tolMm(QPointF viewPos) const
{
    if (!tool_)
        return 1;
    const double px = theme_ ? theme_->spacingS() : 8.0;
    const auto a = tool_->toPlane(viewPos);
    const auto b = tool_->toPlane(viewPos + QPointF(px, 0));
    if (!a || !b)
        return 1;
    return std::max(1e-9, su::distance(*a, *b));
}

double SketchConstraintsModel::gapMm() const
{
    QObject* v = tool_ ? tool_->view() : nullptr;
    auto* item = qobject_cast<QQuickItem*>(v);
    const QPointF centre = item != nullptr ? QPointF(item->width() / 2, item->height() / 2) : QPointF();
    const double px = theme_ ? theme_->spacingL() : 16.0;
    return tolMm(centre) * px / (theme_ ? theme_->spacingS() : 8.0);
}

QString SketchConstraintsModel::formatValue(ConstraintType type, double value) const
{
    const double shown = su::shownValue(type, value);
    if (type == ConstraintType::Angle)
        return QString::number(shown * 180 / su::kPi, 'f', 2) + QStringLiteral("°");
    return units_ ? units_->formatLength(shown) : QString::number(shown, 'f', 3);
}

QString SketchConstraintsModel::labelText(const sketch::Constraint& c, double value) const
{
    QString text = formatValue(c.type, value);
    if (c.type == ConstraintType::Radius)
        text = QStringLiteral("R ") + text;
    else if (c.type == ConstraintType::Diameter)
        text = QStringLiteral("⌀ ") + text;
    if (c.driven)
        text = QLatin1Char('(') + text + QLatin1Char(')');
    return text;
}

// ---------- the scene ----------
void SketchConstraintsModel::refresh()
{
    if (!tool_ || !tool_->active()) {
        glyphs_.setRows({});
        labels_.setRows({});
        if (!dimScene_.lines.empty() || !dimScene_.arrows.empty()) {
            dimScene_ = {};
            ++sceneRevision_;
            emit sceneChanged();
        }
        if (!offenders_.isEmpty()) {
            offenders_.clear();
            emit offendersChanged();
        }
        return;
    }
    // What the last solve named; kept while the next one runs, so the colours do not flicker.
    if (const sketch::SolveResult* solve = tool_->currentSolve()) {
        bad_.clear();
        bad_.insert(solve->conflicting.begin(), solve->conflicting.end());
        bad_.insert(solve->redundant.begin(), solve->redundant.end());
        bad_.insert(solve->partiallyRedundant.begin(), solve->partiallyRedundant.end());
    } else if (tool_->status() != QLatin1String("solving")) {
        bad_.clear();
    }

    const su::SketchView& view = tool_->displayView();
    const sketch::Sketch& model = tool_->model();
    const double gap = gapMm();
    const QPointF oldEditAt = editAt_;
    using Style = DimensionScene::Style;
    DimensionScene scene;
    QVector<QVariantMap> glyphRows, labelRows;
    std::map<std::pair<long, long>, int> stacks;

    const auto addShape = [&](const su::DimensionShape& shape, Style style) {
        for (const std::vector<Vec2>& line : shape.lines) {
            DimensionScene::Line out{style, {}};
            for (const Vec2& p : line) {
                const auto q = tool_->toView(p);
                if (!q)
                    break;
                out.points.push_back(*q);
            }
            if (out.points.size() == line.size() && out.points.size() >= 2)
                scene.lines.push_back(std::move(out));
        }
        for (const su::DimensionShape::Arrow& a : shape.arrows) {
            const auto tip = tool_->toView(a.tip);
            const auto back = tool_->toView(su::sub(a.tip, su::mul(a.dir, gap * 0.1)));
            if (!tip || !back)
                continue;
            const QPointF d = *tip - *back;
            const double len = std::hypot(d.x(), d.y());
            if (len > 1e-9)
                scene.arrows.push_back({style, *tip, d / len});
        }
    };
    // A shape with its label at `label` (mm).
    const auto shapeAt = [&](const sketch::Constraint& c, Vec2 label) -> std::optional<su::DimensionShape> {
        const auto base = su::layoutDimension(view, c, std::nullopt, gap);
        if (!base)
            return std::nullopt;
        return su::layoutDimension(view, c, su::sub(label, base->anchor), gap);
    };

    for (const sketch::Constraint& c : model.constraints) {
        const bool isSelected = selected_ && *selected_ == c.id;
        const bool isHovered = hovered_ && *hovered_ == c.id;
        const bool isBad = bad_.count(c.id) > 0;
        const QString cid = qs(c.id.toString());
        if (su::isDimension(c.type)) {
            const auto shape = su::layoutDimension(view, c, offsetOf(c.id), gap);
            if (!shape)
                continue;
            const Style style = isSelected  ? Style::Selected
                                : isHovered ? Style::Hovered
                                : isBad     ? Style::Bad
                                : c.driven  ? Style::Driven
                                            : Style::Normal;
            addShape(*shape, style);
            const auto at = tool_->toView(shape->label);
            if (!at)
                continue;
            double value = c.value;
            if (c.driven) {
                value = su::measureSigned(view, c).value_or(c.value);
            } else {
                for (const sketch::Constraint& e : evaluated_.constraints) {
                    if (e.id == c.id)
                        value = e.value;
                }
            }
            QVariantMap row;
            row.insert(QStringLiteral("cid"), cid);
            row.insert(QStringLiteral("slot"), 0);
            row.insert(QStringLiteral("type"), QString::fromLatin1(su::glyphOf(c.type)));
            row.insert(QStringLiteral("title"), trs(su::typeTitle(c.type)));
            row.insert(QStringLiteral("text"), labelText(c, value));
            row.insert(QStringLiteral("expression"), c.expression ? unwrapNegative(qs(*c.expression)) : QString());
            row.insert(QStringLiteral("px"), at->x());
            row.insert(QStringLiteral("py"), at->y());
            row.insert(QStringLiteral("driven"), c.driven);
            row.insert(QStringLiteral("bad"), isBad);
            row.insert(QStringLiteral("hovered"), isHovered);
            row.insert(QStringLiteral("selected"), isSelected);
            labelRows << row;
            if (edit_ && !edit_->isNew && edit_->id == c.id)
                editAt_ = *at;
            continue;
        }
        const std::vector<Vec2> places = su::glyphPlaces(view, c);
        for (int i = 0; i < static_cast<int>(places.size()); ++i) {
            const auto at = tool_->toView(places[static_cast<std::size_t>(i)]);
            if (!at)
                continue;
            const std::pair<long, long> key{std::lround(at->x() / 6), std::lround(at->y() / 6)};
            QVariantMap row;
            row.insert(QStringLiteral("cid"), cid);
            row.insert(QStringLiteral("slot"), i);
            row.insert(QStringLiteral("kind"), QString::fromLatin1(su::glyphOf(c.type)));
            row.insert(QStringLiteral("title"), trs(su::typeTitle(c.type)));
            row.insert(QStringLiteral("px"), at->x());
            row.insert(QStringLiteral("py"), at->y());
            row.insert(QStringLiteral("stack"), stacks[key]++);
            row.insert(QStringLiteral("bad"), isBad);
            row.insert(QStringLiteral("hovered"), isHovered);
            row.insert(QStringLiteral("selected"), isSelected);
            glyphRows << row;
        }
    }

    // The smart dimension tool's live dimension, or the new one whose value is being typed.
    QString previewText;
    QPointF previewAt = previewAt_;
    if (edit_ && edit_->isNew) {
        sketch::Constraint c;
        c.type = edit_->pick.type;
        c.refs = edit_->pick.refs;
        if (const auto shape = su::layoutDimension(view, c, edit_->offset, gap)) {
            addShape(*shape, Style::Preview);
            if (const auto at = tool_->toView(shape->label))
                editAt_ = *at;
        }
    } else if (dimensionTool_ && candidate_ && cursorIn_) {
        sketch::Constraint c;
        c.type = candidate_->type;
        c.refs = candidate_->refs;
        if (const auto shape = shapeAt(c, cursor_)) {
            addShape(*shape, Style::Preview);
            if (const auto at = tool_->toView(shape->label)) {
                previewAt = *at;
                previewText = labelText(c, candidate_->value);
            }
        }
    }
    if (previewText != previewText_ || previewAt != previewAt_) {
        previewText_ = previewText;
        previewAt_ = previewAt;
        emit previewChanged();
    }
    if (editAt_ != oldEditAt)
        emit editPlaceChanged();

    if (!(scene == dimScene_)) {
        dimScene_ = std::move(scene);
        ++sceneRevision_;
        emit sceneChanged();
    }
    glyphs_.setRows(std::move(glyphRows));
    labels_.setRows(std::move(labelRows));

    // The offenders list.
    QVariantList offenders;
    const sketch::SolveResult* solve = tool_->currentSolve();
    for (const sketch::Constraint& c : model.constraints) {
        if (!bad_.count(c.id))
            continue;
        QString why = trs("Redundant");
        if (solve != nullptr &&
            std::find(solve->conflicting.begin(), solve->conflicting.end(), c.id) != solve->conflicting.end())
            why = trs("Conflicting");
        else if (solve != nullptr && std::find(solve->partiallyRedundant.begin(), solve->partiallyRedundant.end(),
                                               c.id) != solve->partiallyRedundant.end())
            why = trs("Partly redundant");
        QString title = trs(su::typeTitle(c.type));
        if (su::isDimension(c.type)) {
            double value = c.value;
            for (const sketch::Constraint& e : evaluated_.constraints) {
                if (e.id == c.id)
                    value = e.value;
            }
            title += QLatin1Char(' ') + labelText(c, value);
        }
        QVariantMap m;
        m.insert(QStringLiteral("cid"), qs(c.id.toString()));
        m.insert(QStringLiteral("title"), title);
        m.insert(QStringLiteral("why"), why);
        m.insert(QStringLiteral("selected"), selected_ && *selected_ == c.id);
        offenders << m;
    }
    if (offenders != offenders_) {
        offenders_ = offenders;
        emit offendersChanged();
    }
}

void SketchConstraintsModel::refreshActions()
{
    QVariantList out;
    for (const su::GeoKind k : su::kGeoKinds) {
        const QString name = QString::fromLatin1(su::geoName(k));
        const QString why = reason(name);
        QVariantMap m;
        m.insert(QStringLiteral("name"), name);
        m.insert(QStringLiteral("title"), trs(su::geoTitle(k)));
        m.insert(QStringLiteral("icon"), QStringLiteral("glyph-") + name);
        m.insert(QStringLiteral("enabled"), why.isEmpty());
        m.insert(QStringLiteral("reason"), why);
        out << m;
    }
    if (out != actions_) {
        actions_ = out;
        emit actionsChanged();
    }
}

void SketchConstraintsModel::updateHighlight()
{
    if (!tool_)
        return;
    std::vector<SketchEntId> ids;
    for (const std::optional<Uuid>& id : {hovered_, selected_}) {
        if (const sketch::Constraint* c = id ? constraintById(*id) : nullptr)
            ids.insert(ids.end(), c->refs.begin(), c->refs.end());
    }
    if (dimensionTool_) {
        ids.insert(ids.end(), picks_.begin(), picks_.end());
        if (hover_.kind != su::SketchView::Hit::Kind::None)
            ids.push_back(hover_.id);
    }
    tool_->setHighlight(ids);
}

void SketchConstraintsModel::runStep(const QString& actionId, const std::string& argsJson, const QString& label,
                                     std::function<void()> after)
{
    if (!tool_)
        return;
    ++pendingSteps_;
    QPointer<SketchConstraintsModel> self(this);
    tool_->runStep(actionId, argsJson, label, [self, after = std::move(after)](bool) {
        if (!self)
            return;
        --self->pendingSteps_;
        if (after)
            after();
    });
}

// ---------- geometric constraints ----------
QString SketchConstraintsModel::reason(const QString& kind) const
{
    if (!tool_ || !tool_->active())
        return trs("Open a sketch first (New Sketch)");
    const auto k = su::geoFromName(kind.toStdString());
    if (!k)
        return trs("Unknown constraint");
    if (!tool_->featureInDocument())
        return trs("Draw something first");
    bool already = false;
    const auto plan = su::planConstraint(*k, tool_->displayView(), tool_->model(), tool_->selectedIds(), &already);
    if (!plan.empty())
        return {};
    if (already)
        return trs("The selection has this constraint already");
    return trs(su::geoRequirement(*k));
}

bool SketchConstraintsModel::apply(const QString& kind)
{
    const QString why = reason(kind);
    const auto k = su::geoFromName(kind.toStdString());
    const auto feature = tool_ ? tool_->featureId() : std::nullopt;
    if (!why.isEmpty() || !k || !feature) {
        if (context_)
            context_->showMessage(why.isEmpty() ? trs("Nothing to constrain") : why, true);
        return false;
    }
    const auto plan = su::planConstraint(*k, tool_->displayView(), tool_->model(), tool_->selectedIds());
    const auto shown = tool_->shownSolved();
    runStep(QStringLiteral("sketch.constraint.add"),
            api::sketch_edit::constraintAddArgs(*feature, plan, {}, shown ? &*shown : nullptr), trs(su::geoTitle(*k)));
    return true;
}

// ---------- constraints in the view ----------
void SketchConstraintsModel::hoverConstraint(const QString& cid)
{
    const auto id = uuidOf(cid);
    if (id == hovered_)
        return;
    hovered_ = id && constraintById(*id) != nullptr ? id : std::nullopt;
    emit hoverChanged();
    updateHighlight();
    refresh();
}

void SketchConstraintsModel::selectConstraint(const QString& cid)
{
    const auto id = uuidOf(cid);
    const std::optional<Uuid> next = id && constraintById(*id) != nullptr ? id : std::nullopt;
    if (next && tool_)
        tool_->clearSelection(); // a constraint or entities are selected, not both
    if (next == selected_)
        return;
    selected_ = next;
    emit selectionChanged();
    updateHighlight();
    refresh();
}

QString SketchConstraintsModel::selected() const
{
    return selected_ ? qs(selected_->toString()) : QString();
}

bool SketchConstraintsModel::selectedIsDimension() const
{
    const sketch::Constraint* c = selected_ ? constraintById(*selected_) : nullptr;
    return c != nullptr && su::isDimension(c->type);
}

bool SketchConstraintsModel::selectedDriven() const
{
    const sketch::Constraint* c = selected_ ? constraintById(*selected_) : nullptr;
    return c != nullptr && c->driven;
}

QString SketchConstraintsModel::hovered() const
{
    return hovered_ ? qs(hovered_->toString()) : QString();
}

void SketchConstraintsModel::removeSelected()
{
    const auto feature = tool_ ? tool_->featureId() : std::nullopt;
    if (!selected_ || !feature || constraintById(*selected_) == nullptr)
        return;
    QJsonObject args;
    args.insert(QStringLiteral("id"), qs(feature->value.toString()));
    args.insert(QStringLiteral("constraints"), QJsonArray{qs(selected_->toString())});
    addSolved(args, tool_->shownSolved());
    runStep(QStringLiteral("sketch.constraint.remove"), json(args), trs("Delete constraint"));
    selected_.reset();
    hovered_.reset();
    emit selectionChanged();
    emit hoverChanged();
    updateHighlight();
    refresh();
}

void SketchConstraintsModel::toggleDriven()
{
    const sketch::Constraint* c = selected_ ? constraintById(*selected_) : nullptr;
    const auto feature = tool_ ? tool_->featureId() : std::nullopt;
    if (c == nullptr || !feature || !su::isDimension(c->type)) {
        if (context_)
            context_->showMessage(trs("Select a dimension first"), true);
        return;
    }
    QJsonObject args;
    args.insert(QStringLiteral("id"), qs(feature->value.toString()));
    args.insert(QStringLiteral("constraint"), qs(c->id.toString()));
    args.insert(QStringLiteral("driven"), !c->driven);
    if (c->driven && !c->expression) {
        // Driving again at the value it shows, so nothing moves.
        if (const auto v = su::measureSigned(tool_->displayView(), *c))
            args.insert(QStringLiteral("value"), *v);
    }
    addSolved(args, tool_->shownSolved());
    runStep(QStringLiteral("sketch.constraint.set"), json(args),
            c->driven ? trs("Driving dimension") : trs("Driven dimension"));
}

void SketchConstraintsModel::dragLabel(const QString& cid, double viewX, double viewY)
{
    const auto id = uuidOf(cid);
    const sketch::Constraint* c = id ? constraintById(*id) : nullptr;
    if (c == nullptr || !tool_)
        return;
    const auto at = tool_->toPlane(QPointF(viewX, viewY));
    const auto base = su::layoutDimension(tool_->displayView(), *c, std::nullopt, gapMm());
    if (!at || !base)
        return;
    dragged_[*id] = su::sub(*at, base->anchor);
    refresh();
}

void SketchConstraintsModel::dropLabel(const QString& cid)
{
    const auto id = uuidOf(cid);
    const auto feature = tool_ ? tool_->featureId() : std::nullopt;
    if (!id || !feature || !dragged_.count(*id))
        return;
    const Vec2 offset = dragged_[*id];
    QJsonObject args;
    args.insert(QStringLiteral("id"), qs(feature->value.toString()));
    args.insert(QStringLiteral("constraint"), cid);
    args.insert(QStringLiteral("offset"),
                QJsonObject{{QStringLiteral("x"), offset.x}, {QStringLiteral("y"), offset.y}});
    QPointer<SketchConstraintsModel> self(this);
    const Uuid key = *id;
    runStep(QStringLiteral("sketch.dimension.place"), json(args), trs("Move dimension"), [self, key] {
        if (!self)
            return;
        self->dragged_.erase(key);
        self->onModelChanged(); // the stored labels again
    });
}

// ---------- the smart dimension tool ----------
void SketchConstraintsModel::setDimensionTool(bool on)
{
    if (!tool_)
        return;
    if (on && !tool_->active())
        return;
    if (on == dimensionTool_) {
        if (on && !tool_->selectedIds().empty()) {
            picks_ = tool_->selectedIds();
            if (picks_.size() > 2)
                picks_.resize(2);
            tool_->clearSelection();
            updateHighlight();
            refresh();
        }
        return;
    }
    if (on) {
        settingTool_ = true;
        tool_->setTool(QStringLiteral("select"));
        settingTool_ = false;
        picks_ = tool_->selectedIds();
        if (picks_.size() > 2)
            picks_.resize(2);
        tool_->clearSelection();
        selected_.reset();
        emit selectionChanged();
        dimensionTool_ = true;
        candidate_ = su::pickDimension(tool_->displayView(), picks_, cursor_);
        tool_->setHintOverride(trs("Click a line, circle or arc, or two entities; then click where the dimension "
                                   "goes. Esc ends"));
    } else {
        dimensionTool_ = false;
        picks_.clear();
        candidate_.reset();
        hover_ = {};
        tool_->setHintOverride(QString());
    }
    updateHighlight();
    emit toolChanged();
    refresh();
}

bool SketchConstraintsModel::pointerMove(QPointF pos)
{
    if (!dimensionTool_ || !tool_)
        return false;
    if (edit_)
        return true;
    const auto uv = tool_->toPlane(pos);
    if (!uv)
        return true;
    cursor_ = *uv;
    cursorIn_ = true;
    const su::SketchView::Hit h = tool_->displayView().hit(*uv, tolMm(pos));
    if (h.kind != hover_.kind || h.id != hover_.id) {
        hover_ = h;
        updateHighlight();
    }
    candidate_ = su::pickDimension(tool_->displayView(), picks_, cursor_);
    refresh();
    return true;
}

bool SketchConstraintsModel::pointerPress(QPointF pos)
{
    if (edit_) {
        commitByClick();
        return true;
    }
    if (!dimensionTool_ || !tool_)
        return false;
    const auto uv = tool_->toPlane(pos);
    if (!uv)
        return true;
    cursor_ = *uv;
    cursorIn_ = true;
    const su::SketchView& view = tool_->displayView();
    const su::SketchView::Hit h = view.hit(*uv, tolMm(pos));
    if (h.kind != su::SketchView::Hit::Kind::None) {
        const auto it = std::find(picks_.begin(), picks_.end(), h.id);
        if (it != picks_.end())
            picks_.erase(it);
        else if (picks_.size() >= 2)
            picks_ = {h.id};
        else
            picks_.push_back(h.id);
        candidate_ = su::pickDimension(view, picks_, cursor_);
        updateHighlight();
        refresh();
        return true;
    }
    candidate_ = su::pickDimension(view, picks_, cursor_);
    if (!candidate_)
        return true;
    // Place it: the label where the pointer is, the value field open on it.
    sketch::Constraint c;
    c.type = candidate_->type;
    c.refs = candidate_->refs;
    const auto base = su::layoutDimension(view, c, std::nullopt, gapMm());
    if (!base)
        return true;
    Edit e;
    e.isNew = true;
    e.id = Uuid::generate();
    e.pick = *candidate_;
    e.offset = su::sub(cursor_, base->anchor);
    e.text = formatValue(c.type, candidate_->value);
    picks_.clear();
    candidate_.reset();
    hover_ = {};
    updateHighlight();
    openEdit(std::move(e), pos);
    startTrial();
    return true;
}

bool SketchConstraintsModel::wantsKey(const QKeyEvent* e) const
{
    const int key = e->key();
    if (key == Qt::Key_Escape)
        return dimensionTool_ || selected_.has_value();
    if ((key == Qt::Key_Delete || key == Qt::Key_Backspace) && selected_ && tool_ && tool_->selectedIds().empty())
        return true;
    return false;
}

bool SketchConstraintsModel::keyPress(QKeyEvent* e)
{
    const int key = e->key();
    if (key == Qt::Key_Escape) {
        if (dimensionTool_) {
            if (!picks_.empty()) {
                picks_.clear();
                candidate_.reset();
                updateHighlight();
                refresh();
            } else {
                setDimensionTool(false);
            }
            return true;
        }
        if (selected_) {
            selectConstraint(QString());
            return true;
        }
        return false;
    }
    if ((key == Qt::Key_Delete || key == Qt::Key_Backspace) && selected_ && tool_ && tool_->selectedIds().empty()) {
        removeSelected();
        return true;
    }
    return false;
}

// ---------- the value field ----------
QString SketchConstraintsModel::editId() const
{
    return edit_ ? qs(edit_->id.toString()) : QString();
}

QString SketchConstraintsModel::editTitle() const
{
    if (!edit_)
        return {};
    if (edit_->isNew)
        return trs(su::typeTitle(edit_->pick.type));
    const sketch::Constraint* c = constraintById(edit_->id);
    return c != nullptr ? trs(su::typeTitle(c->type)) : QString();
}

void SketchConstraintsModel::beginEdit(const QString& cid)
{
    const auto id = uuidOf(cid);
    const sketch::Constraint* c = id ? constraintById(*id) : nullptr;
    if (c == nullptr || !su::isDimension(c->type) || !tool_ || !tool_->featureInDocument())
        return;
    if (c->driven) {
        if (context_)
            context_->showMessage(trs("A driven dimension shows a measurement; make it driving to change it"), false);
        selectConstraint(cid);
        return;
    }
    double value = c->value;
    for (const sketch::Constraint& e : evaluated_.constraints) {
        if (e.id == c->id)
            value = e.value;
    }
    Edit e;
    e.isNew = false;
    e.id = c->id;
    e.text = c->expression ? unwrapNegative(qs(*c->expression)) : formatValue(c->type, value);
    QPointF at;
    if (const QVariantMap row = labels_.find(cid); !row.isEmpty())
        at = QPointF(row.value(QStringLiteral("px")).toDouble(), row.value(QStringLiteral("py")).toDouble());
    openEdit(std::move(e), at);
}

void SketchConstraintsModel::openEdit(Edit edit, QPointF at)
{
    edit.serial = ++editSerial_;
    edit_ = std::move(edit);
    editAt_ = at;
    emit editChanged();
    emit editPlaceChanged();
    refresh();
}

void SketchConstraintsModel::closeEdit()
{
    if (!edit_)
        return;
    edit_.reset();
    ++editSerial_;
    emit editChanged();
    refresh();
}

void SketchConstraintsModel::cancelEdit()
{
    closeEdit();
}

void SketchConstraintsModel::startTrial()
{
    if (!edit_ || !edit_->isNew || !tool_)
        return;
    IJobSystem* jobs = scene_ ? scene_->jobs() : nullptr;
    if (jobs == nullptr)
        return;
    sketch::Sketch trial = evaluated_;
    if (const auto shown = tool_->shownSolved())
        trial.entities = shown->entities; // from the shape shown, as the step will store it
    sketch::Constraint c;
    c.id = edit_->id;
    c.type = edit_->pick.type;
    c.refs = edit_->pick.refs;
    c.value = edit_->pick.value;
    trial.constraints.push_back(c);
    edit_->checking = true;
    const std::uint64_t serial = edit_->serial;
    const bool wasBad = tool_->status() == QLatin1String("over") || tool_->status() == QLatin1String("conflict");
    JobHandle<sketch::SolveResult> job =
        jobs->submit<sketch::SolveResult>(JobInfo{"Dimension check", JobPriority::Interactive, false},
                                          [trial = std::move(trial)](JobContext& ctx) -> Result<sketch::SolveResult> {
                                              auto solver = api::sketch_edit::makeSolver();
                                              if (!solver)
                                                  return tl::unexpected(Error{ErrorCode::Internal, "No solver", {}});
                                              return solver->solve(trial, ctx);
                                          });
    QPointer<SketchConstraintsModel> self(this);
    const Uuid id = c.id;
    job.then([self, serial, id, wasBad](Result<sketch::SolveResult> r) {
        if (!self || !self->edit_ || self->edit_->serial != serial)
            return;
        self->edit_->checking = false;
        if (r && (r->status == sketch::SolveStatus::OverConstrained || r->status == sketch::SolveStatus::Conflicting)) {
            const auto names = [&](const std::vector<Uuid>& ids) {
                return std::find(ids.begin(), ids.end(), id) != ids.end();
            };
            self->edit_->overConstrains =
                names(r->conflicting) || names(r->redundant) || names(r->partiallyRedundant) || !wasBad;
        }
        emit self->editChanged();
    });
    emit editChanged();
}

SketchConstraintsModel::Parsed SketchConstraintsModel::parse(const QString& text, bool angle) const
{
    Parsed out;
    const QString t = text.trimmed();
    if (t.isEmpty()) {
        out.error = trs("Type a value");
        return out;
    }
    if (angle) {
        QString n = t;
        n.remove(QStringLiteral("°"));
        if (n.endsWith(QLatin1String("deg")))
            n.chop(3);
        n.replace(QLatin1Char(','), QLatin1Char('.'));
        bool ok = false;
        const double deg = n.trimmed().toDouble(&ok);
        if (ok && std::isfinite(deg)) {
            out.value = std::abs(deg) * su::kPi / 180;
            return out;
        }
    } else {
        const double mm = units_ ? units_->parseLength(t) : std::numeric_limits<double>::quiet_NaN();
        if (std::isfinite(mm)) {
            if (!(mm > 0))
                out.error = trs("A length must be more than 0");
            out.value = mm;
            return out;
        }
    }
    // An expression over the parameters (or a parameter's name).
    api::IApp* a = app();
    api::OpenDocument* od = a != nullptr ? a->active() : nullptr;
    if (od == nullptr || !od->document) {
        out.error = trs("There is no document");
        return out;
    }
    const QByteArray utf8 = t.toUtf8();
    auto v = api::sketch_edit::evaluateExpression(
        *od->document->state(), std::string_view(utf8.constData(), static_cast<std::size_t>(utf8.size())), angle);
    if (!v) {
        out.error = shortError(v.error().message);
        return out;
    }
    if (!angle && !(*v > 0)) {
        out.error = trs("A length must be more than 0 (this is %1)").arg(formatValue(ConstraintType::Distance, *v));
        return out;
    }
    out.value = std::abs(*v);
    out.expression = t.toStdString();
    return out;
}

QString SketchConstraintsModel::check(const QString& text) const
{
    if (!edit_)
        return {};
    const bool angle = edit_->isNew ? edit_->pick.type == ConstraintType::Angle : [&] {
        const sketch::Constraint* c = constraintById(edit_->id);
        return c != nullptr && c->type == ConstraintType::Angle;
    }();
    return parse(text, angle).error;
}

bool SketchConstraintsModel::commitEdit(const QString& text)
{
    const auto feature = tool_ ? tool_->featureId() : std::nullopt;
    if (!edit_ || !feature)
        return false;
    const sketch::Constraint* existing = edit_->isNew ? nullptr : constraintById(edit_->id);
    if (!edit_->isNew && existing == nullptr) {
        closeEdit();
        return true;
    }
    const ConstraintType type = edit_->isNew ? edit_->pick.type : existing->type;
    const Parsed p = parse(text, type == ConstraintType::Angle);
    if (!p.error.isEmpty()) {
        edit_->error = p.error;
        emit editChanged();
        return false;
    }
    if (edit_->isNew) {
        if (edit_->overConstrains) {
            edit_->error = trs("This dimension would over-constrain the sketch: add it as driven instead");
            emit editChanged();
            return false;
        }
        sketch::Constraint c;
        c.id = edit_->id;
        c.type = type;
        c.refs = edit_->pick.refs;
        c.value = su::signedValue(type, edit_->pick.value, p.value);
        if (p.expression)
            c.expression = signedType(type) && c.value < 0 ? "-(" + *p.expression + ")" : *p.expression;
        const std::map<Uuid, Vec2> labels{{c.id, edit_->offset}};
        const auto shown = tool_->shownSolved();
        runStep(QStringLiteral("sketch.constraint.add"),
                api::sketch_edit::constraintAddArgs(*feature, {c}, labels, shown ? &*shown : nullptr),
                trs("Dimension"));
        closeEdit();
        return true;
    }
    if (text.trimmed() == edit_->text.trimmed()) {
        closeEdit();
        return true;
    }
    double current = existing->value;
    for (const sketch::Constraint& e : evaluated_.constraints) {
        if (e.id == existing->id)
            current = e.value;
    }
    const double value = su::signedValue(type, current, p.value);
    QJsonObject args;
    args.insert(QStringLiteral("id"), qs(feature->value.toString()));
    args.insert(QStringLiteral("constraint"), qs(existing->id.toString()));
    args.insert(QStringLiteral("value"), value);
    if (p.expression)
        args.insert(QStringLiteral("expression"),
                    qs(signedType(type) && value < 0 ? "-(" + *p.expression + ")" : *p.expression));
    addSolved(args, tool_->shownSolved());
    runStep(QStringLiteral("sketch.constraint.set"), json(args), trs("Edit dimension"));
    closeEdit();
    return true;
}

void SketchConstraintsModel::addAsDriven()
{
    const auto feature = tool_ ? tool_->featureId() : std::nullopt;
    if (!edit_ || !edit_->isNew || !feature)
        return;
    sketch::Constraint c;
    c.id = edit_->id;
    c.type = edit_->pick.type;
    c.refs = edit_->pick.refs;
    c.value = edit_->pick.value;
    c.driven = true;
    const std::map<Uuid, Vec2> labels{{c.id, edit_->offset}};
    const auto shown = tool_->shownSolved();
    runStep(QStringLiteral("sketch.constraint.add"),
            api::sketch_edit::constraintAddArgs(*feature, {c}, labels, shown ? &*shown : nullptr),
            trs("Driven dimension"));
    closeEdit();
}

bool SketchConstraintsModel::commitByClick()
{
    if (!edit_)
        return false;
    // A click away keeps what the field opened with (a new dimension: at its measured value), unless that would
    // over-constrain: then the new dimension is dropped.
    if (edit_->isNew && !edit_->overConstrains)
        return commitEdit(edit_->text);
    closeEdit();
    return true;
}

// ---------- commands ----------
QString SketchConstraintsModel::commandReason(const QString& id) const
{
    if (!tool_ || !tool_->active())
        return trs("Open a sketch first (New Sketch)");
    const QString name = id.mid(static_cast<int>(qstrlen(kPrefix)));
    if (name == QLatin1String("dimension"))
        return {};
    if (name == QLatin1String("driven"))
        return selectedIsDimension() ? QString() : trs("Select a dimension first");
    if (name == QLatin1String("delete"))
        return selected_ ? QString() : trs("Select a constraint or dimension first");
    return reason(name);
}

bool SketchConstraintsModel::runCommand(const QString& id)
{
    const QString name = id.mid(static_cast<int>(qstrlen(kPrefix)));
    if (name == QLatin1String("dimension")) {
        setDimensionTool(true);
        return true;
    }
    if (name == QLatin1String("driven")) {
        toggleDriven();
        return true;
    }
    if (name == QLatin1String("delete")) {
        removeSelected();
        return true;
    }
    return apply(name);
}

} // namespace ray::ui
