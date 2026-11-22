// The operation template panel's model (task T-057; templates_model.h).
#include "templates_model.h"

#include "../model/model_util.h"
#include "app_context.h"
#include "palette_commands.h"
#include "selection_model.h"
#include "session.h"
#include "setup_tree.h"
#include "ui_commands.h"

#include <ray_api/cam_tools.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>

#include <algorithm>
#include <set>

namespace ray::ui {

using namespace modelui;

namespace {

// T-057: operation templates. No default keys (the CAM workspace's are taken); the palette and the CAM tree's rows
// reach them.
// clang-format off
constexpr UiCommand kTemplateCommands[] = {
    {"cam.templates.open", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Operation Templates…"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "CAM"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Browses operation templates, previews what they would make on the setup's holes, and applies them"), "", CommandContext::Cam, "cam.template.apply cam.template.preview cam.template.rename cam.template.remove"},
    {"cam.templates.saveFrom", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Save Operation as Template…"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "CAM"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Saves the operation selected in the CAM tree as a template in your library"), "", CommandContext::Cam, "cam.template.saveFrom"},
    {"cam.templates.reapply", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Re-apply Template"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "CAM"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Applies the template the selected operation came from to it again"), "", CommandContext::Cam, "cam.template.reapply"},
};
// clang-format on

QString trt(const char* text)
{
    return QCoreApplication::translate("ray::ui::TemplatesModel", text);
}
QString trt(const char* text, const char* disambiguation, int n)
{
    return QCoreApplication::translate("ray::ui::TemplatesModel", text, disambiguation, n);
}

struct KindText {
    const char* value;
    const char* text;
};
constexpr KindText kKinds[] = {
    {"", QT_TRANSLATE_NOOP("ray::ui::TemplatesModel", "Every kind")},
    {"hole", QT_TRANSLATE_NOOP("ray::ui::TemplatesModel", "Any hole")},
    {"hole.tapped", QT_TRANSLATE_NOOP("ray::ui::TemplatesModel", "Tapped holes")},
    {"hole.plain", QT_TRANSLATE_NOOP("ray::ui::TemplatesModel", "Plain holes")},
    {"hole.through", QT_TRANSLATE_NOOP("ray::ui::TemplatesModel", "Through holes")},
    {"hole.blind", QT_TRANSLATE_NOOP("ray::ui::TemplatesModel", "Blind holes")},
    {"hole.counterbored", QT_TRANSLATE_NOOP("ray::ui::TemplatesModel", "Counterbored holes")},
    {"hole.countersunk", QT_TRANSLATE_NOOP("ray::ui::TemplatesModel", "Countersunk holes")},
};

QString kindText(const QString& kind)
{
    for (const KindText& k : kKinds) {
        if (kind == QLatin1String(k.value))
            return kind.isEmpty() ? trt("Any feature") : trt(k.text);
    }
    return kind;
}

bool kindShown(const QString& templateKind, const QString& filter)
{
    if (filter.isEmpty() || filter == QLatin1String("hole"))
        return filter.isEmpty() || templateKind.isEmpty() || templateKind.startsWith(QLatin1String("hole"));
    return templateKind == filter || templateKind.isEmpty() || templateKind == QLatin1String("hole");
}

QString compact(const QJsonValue& v)
{
    if (v.isString())
        return v.toString();
    if (v.isDouble())
        return QString::number(v.toDouble(), 'g', 10);
    if (v.isBool())
        return v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    if (v.isObject())
        return QString::fromUtf8(QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact));
    if (v.isArray())
        return QString::fromUtf8(QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact));
    return {};
}

// "key: value, key: value" of a JSON object, for people.
QString pairs(const QJsonObject& o)
{
    QStringList out;
    for (auto it = o.begin(); it != o.end(); ++it)
        out << QStringLiteral("%1 %2").arg(it.key(), compact(it.value()));
    return out.join(QStringLiteral(", "));
}

// "1 operation", "3 operations": English plurals without a translation file (none is loaded).
QString counted(int n, const char* one, const char* many)
{
    return (n == 1 ? trt(one) : trt(many)).arg(n);
}

// A template's "when" in words: "depth/diameter > 3 and <= 5".
QString whenText(const QJsonObject& when)
{
    QStringList parts;
    for (auto it = when.begin(); it != when.end(); ++it) {
        QString name = it.key();
        if (name.startsWith(QLatin1Char('$')))
            name = name.mid(1);
        if (name.startsWith(QLatin1String("feature.")))
            name = name.mid(8);
        if (name == QLatin1String("depthRatio"))
            name = trt("depth/diameter");
        const QJsonValue v = it.value();
        if (v.isObject()) {
            QStringList bounds;
            const QJsonObject o = v.toObject();
            const std::pair<const char*, const char*> ops[] = {
                {"above", ">"}, {"min", "\u2265"}, {"below", "<"}, {"max", "\u2264"}};
            for (const auto& [key, op] : ops) {
                if (o.contains(QLatin1String(key)))
                    bounds << QStringLiteral("%1 %2").arg(QString::fromUtf8(op), compact(o.value(QLatin1String(key))));
            }
            parts << name + QLatin1Char(' ') + bounds.join(trt(" and "));
        } else {
            parts << QStringLiteral("%1 = %2").arg(name, compact(v));
        }
    }
    return parts.join(QStringLiteral(", "));
}

QString setKey(const QString& set, const QString& material)
{
    return QStringLiteral("set:%1|%2").arg(set, material);
}

} // namespace

std::span<const UiCommand> camTemplatesUiCommands()
{
    return kTemplateCommands;
}

// ---------- construction ----------
TemplatesModel::TemplatesModel(Session* session, AppContext* context, QObject* parent)
    : QObject(parent), session_(session), context_(context)
{
    previewTimer_.setSingleShot(true);
    previewTimer_.setInterval(120);
    connect(&previewTimer_, &QTimer::timeout, this, &TemplatesModel::runPreview);
    if (context_)
        connect(context_, &AppContext::sessionChanged, this, [this] { subscribeSession(); });
    if (session_)
        connect(session_, &Session::documentChanged, this, [this] { subscribeDocument(); });
    subscribeSession();
}

TemplatesModel::~TemplatesModel()
{
    *alive_ = false;
    if (commands_)
        commands_->setUiHandler(QStringLiteral("cam.templates."), {});
    if (lib::LibraryStack* s = stack(); s != nullptr && s == stackSubscribed_ && stackListener_ != 0)
        s->unsubscribe(stackListener_);
    unsubscribeDocument();
}

TemplatesModel* TemplatesModel::create(QQmlEngine* qml, QJSEngine*)
{
    Session* session = nullptr;
    AppContext* context = nullptr;
    CommandCatalog* commands = nullptr;
    SetupTreeModel* tree = nullptr;
    SelectionModel* selection = nullptr;
    if (qml != nullptr) {
        const QString uri = QStringLiteral("RayCad.Ui");
        session = qml->singletonInstance<Session*>(uri, QStringLiteral("Session"));
        context = qml->singletonInstance<AppContext*>(uri, QStringLiteral("AppContext"));
        commands = qml->singletonInstance<CommandCatalog*>(uri, QStringLiteral("Commands"));
        tree = qml->singletonInstance<SetupTreeModel*>(uri, QStringLiteral("CamTreeModel"));
        selection = qml->singletonInstance<SelectionModel*>(uri, QStringLiteral("Selection"));
    }
    auto* model = new TemplatesModel(session, context);
    model->setTree(tree);
    model->setSelection(selection);
    model->setCommands(commands);
    return model; // owned by the engine
}

void TemplatesModel::setCommands(CommandCatalog* commands)
{
    commands_ = commands;
    if (!commands_)
        return;
    QPointer<TemplatesModel> self(this);
    commands_->setUiHandler(
        QStringLiteral("cam.templates."),
        CommandCatalog::UiHandler{
            [self](const QString& id) { return self ? self->commandReason(id) : trt("Templates are not available"); },
            [self](const QString& id) { return self && self->runCommand(id); }});
}

void TemplatesModel::setSelection(SelectionModel* selection)
{
    selection_ = selection;
    if (!selection_)
        return;
    connect(selection_, &SelectionModel::changed, this, [this] {
        emit previewChanged(); // the count of selected faces
        if (target_ == QLatin1String("selection"))
            schedulePreview();
    });
}

// ---------- the session, the stack and the document ----------
api::IApp* TemplatesModel::app() const
{
    return context_ ? context_->app() : nullptr;
}

lib::LibraryStack* TemplatesModel::stack() const
{
    api::IApp* a = app();
    return a != nullptr ? a->libraries() : nullptr;
}

doc::DocStatePtr TemplatesModel::docState() const
{
    api::IApp* a = app();
    api::OpenDocument* od = a != nullptr ? a->active() : nullptr;
    return od != nullptr && od->document ? od->document->state() : nullptr;
}

void TemplatesModel::subscribeSession()
{
    lib::LibraryStack* s = stack();
    if (s != stackSubscribed_) {
        stackSubscribed_ = s;
        stackListener_ = 0;
        if (s != nullptr) {
            std::weak_ptr<bool> alive = alive_;
            stackListener_ = s->subscribe([this, alive] {
                if (auto a = alive.lock(); a && *a) {
                    rebuildCatalog();
                    schedulePreview();
                }
            });
        }
    }
    subscribeDocument();
    rebuildCatalog();
}

void TemplatesModel::subscribeDocument()
{
    api::IApp* a = app();
    api::OpenDocument* od = a != nullptr ? a->active() : nullptr;
    const std::string id = od != nullptr ? od->id : std::string();
    if (id == docId_ && (od == nullptr || od->document.get() == doc_))
        return;
    unsubscribeDocument();
    if (od != nullptr && od->document) {
        docId_ = od->id;
        doc_ = od->document.get();
        const std::uint64_t generation = ++docGeneration_;
        QPointer<TemplatesModel> self(this);
        docListener_ = doc_->subscribe([self, generation](doc::DocStatePtr, doc::BuildResultPtr) {
            if (self && self->docGeneration_ == generation) {
                self->rebuildSetups();
                self->schedulePreview();
            }
        });
    }
    rebuildSetups();
}

void TemplatesModel::unsubscribeDocument()
{
    api::IApp* a = app();
    api::OpenDocument* od = a != nullptr && !docId_.empty() ? a->findDocument(docId_) : nullptr;
    if (od != nullptr && od->document.get() == doc_ && docListener_ != 0)
        doc_->unsubscribe(docListener_);
    doc_ = nullptr;
    docListener_ = 0;
    docId_.clear();
    ++docGeneration_;
}

// ---------- the catalog ----------
QVariantList TemplatesModel::kinds() const
{
    QVariantList out;
    for (const KindText& k : kKinds)
        out.push_back(
            QVariantMap{{QStringLiteral("value"), QLatin1String(k.value)}, {QStringLiteral("text"), trt(k.text)}});
    return out;
}

QString TemplatesModel::materialName(const QString& id) const
{
    if (id.isEmpty())
        return trt("Any material");
    if (lib::LibraryStack* s = stack()) {
        if (const lib::Material* m = s->material(MaterialId{Uuid::fromString(id.toStdString())}))
            return qs(m->name);
    }
    if (doc::DocStatePtr state = docState(); state && state->cam) {
        const auto it = state->cam->materials.find(MaterialId{Uuid::fromString(id.toStdString())});
        if (it != state->cam->materials.end())
            return qs(it->second.item.name);
    }
    return trt("Unknown material");
}

QString TemplatesModel::materialOfSetup() const
{
    doc::DocStatePtr state = docState();
    if (!state || !state->cam || !setup_)
        return {};
    for (const cam::Setup& s : state->cam->setups) {
        if (s.id == *setup_)
            return s.material.value == Uuid{} ? QString() : idText(s.material.value);
    }
    return {};
}

void TemplatesModel::rebuildCatalog()
{
    templates_.clear();
    lib::LibraryStack* s = stack();
    if (s != nullptr) {
        const auto roots = s->roots();
        std::size_t lastWritable = roots.size();
        for (std::size_t i = 0; i < roots.size(); ++i) {
            if (roots[i].writable)
                lastWritable = i;
        }
        for (const lib::OpTemplate& t : s->templates()) {
            Template e;
            e.item = t;
            const QJsonObject extra = jsonObject(t.extraJson);
            e.set = extra.value(QStringLiteral("set")).toString();
            e.sequence = extra.value(QStringLiteral("sequence")).toInt();
            if (const auto root = s->rootOf(lib::ItemKind::Template, t.id); root && *root < roots.size()) {
                e.library = *root == lastWritable ? trt("My library") : qs(roots[*root].name);
                e.readOnly = !roots[*root].writable;
            }
            e.hash = qs(api::cam_tools::contentHash(t).hex());
            templates_.push_back(std::move(e));
        }
    }
    // The material choices.
    materials_.clear();
    const QString setupMaterial = materialOfSetup();
    materials_.push_back(
        QVariantMap{{QStringLiteral("value"), QStringLiteral("setup")},
                    {QStringLiteral("text"), setupMaterial.isEmpty()
                                                 ? trt("The setup's material (none: every material)")
                                                 : trt("The setup's material (%1)").arg(materialName(setupMaterial))}});
    materials_.push_back(
        QVariantMap{{QStringLiteral("value"), QString()}, {QStringLiteral("text"), trt("Any material")}});
    if (s != nullptr) {
        std::vector<lib::Material> list = s->materials();
        std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
        for (const lib::Material& m : list)
            materials_.push_back(
                QVariantMap{{QStringLiteral("value"), idText(m.id.value)}, {QStringLiteral("text"), qs(m.name)}});
    }

    // The rows: the filters, then grouped by set and material.
    const QString material = materialFilter_ == QLatin1String("setup") ? setupMaterial : materialFilter_;
    const QStringList words = search_.toLower().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    struct Group {
        QString set, material, materialName;
        std::vector<const Template*> members;
    };
    std::map<std::pair<QString, QString>, Group> groups; // (set lower, material name) -> group
    std::vector<const Template*> loose;
    int count = 0;
    for (const Template& t : templates_) {
        const QString tMaterial = t.item.material ? idText(t.item.material->value) : QString();
        if (!material.isEmpty() && !tMaterial.isEmpty() && tMaterial != material)
            continue;
        if (!kindShown(qs(t.item.featureKind), kindFilter_))
            continue;
        const QString haystack = (qs(t.item.name) + QLatin1Char(' ') + t.set + QLatin1Char(' ') +
                                  kindText(qs(t.item.featureKind)) + QLatin1Char(' ') + materialName(tMaterial) +
                                  QLatin1Char(' ') + t.library + QLatin1Char(' ') + qs(t.item.opType))
                                     .toLower();
        if (!std::all_of(words.begin(), words.end(), [&](const QString& w) { return haystack.contains(w); }))
            continue;
        ++count;
        if (t.set.isEmpty()) {
            loose.push_back(&t);
            continue;
        }
        Group& g = groups[{t.set.toLower(), materialName(tMaterial)}];
        g.set = t.set;
        g.material = tMaterial;
        g.materialName = materialName(tMaterial);
        g.members.push_back(&t);
    }
    const auto templateRow = [this](const Template& t, int depth) {
        const QString tMaterial = t.item.material ? idText(t.item.material->value) : QString();
        QStringList detail;
        if (depth == 0) {
            detail << kindText(qs(t.item.featureKind)) << materialName(tMaterial) << t.library;
        } else {
            // Under its set: its step, and when it is the one taken (the peck rules).
            detail << trt("Step %1").arg(t.sequence);
            const QString when = whenText(jsonObject(t.item.extraJson).value(QStringLiteral("when")).toObject());
            detail << (when.isEmpty() ? kindText(qs(t.item.featureKind)) : trt("when %1").arg(when));
        }
        return QVariantMap{{QStringLiteral("rowKind"), QStringLiteral("template")},
                           {QStringLiteral("key"), idText(t.item.id)},
                           {QStringLiteral("title"), qs(t.item.name)},
                           {QStringLiteral("detail"), detail.join(QStringLiteral(" · "))},
                           {QStringLiteral("readOnly"), t.readOnly},
                           {QStringLiteral("library"), t.library},
                           {QStringLiteral("depth"), depth}};
    };
    rows_.clear();
    for (auto& [key, g] : groups) {
        std::stable_sort(g.members.begin(), g.members.end(), [](const Template* a, const Template* b) {
            return a->sequence != b->sequence ? a->sequence < b->sequence : a->item.name < b->item.name;
        });
        const bool readOnly =
            std::all_of(g.members.begin(), g.members.end(), [](const Template* t) { return t->readOnly; });
        rows_.push_back(QVariantMap{
            {QStringLiteral("rowKind"), QStringLiteral("set")},
            {QStringLiteral("key"), setKey(g.set, g.material)},
            {QStringLiteral("title"), g.set},
            {QStringLiteral("detail"), g.materialName + QStringLiteral(" · ") +
                                           counted(static_cast<int>(g.members.size()), "%1 template", "%1 templates")},
            {QStringLiteral("readOnly"), readOnly},
            {QStringLiteral("library"), g.members.front()->library},
            {QStringLiteral("depth"), 0}});
        for (const Template* t : g.members)
            rows_.push_back(templateRow(*t, 1));
    }
    std::sort(loose.begin(), loose.end(),
              [](const Template* a, const Template* b) { return a->item.name < b->item.name; });
    for (const Template* t : loose)
        rows_.push_back(templateRow(*t, 0));
    templateCount_ = count;
    emit catalogChanged();
    // The selection stays while its row is there.
    if (!selected_.isEmpty() && selectedTemplates().empty()) {
        selected_.clear();
        emit selectionChanged();
        schedulePreview();
    } else {
        emit selectionChanged(); // its details may have changed
    }
}

void TemplatesModel::rebuildSetups()
{
    setups_.clear();
    doc::DocStatePtr state = docState();
    if (state && state->cam) {
        for (const cam::Setup& s : state->cam->setups)
            setups_.push_back(
                QVariantMap{{QStringLiteral("value"), idText(s.id.value)}, {QStringLiteral("text"), qs(s.name)}});
    }
    if (setup_ && std::none_of(setups_.begin(), setups_.end(), [&](const QVariant& v) {
            return v.toMap().value(QStringLiteral("value")).toString() == idText(setup_->value);
        }))
        setup_.reset();
    emit setupsChanged();
}

void TemplatesModel::setSearch(const QString& text)
{
    if (text == search_)
        return;
    search_ = text;
    emit filterChanged();
    rebuildCatalog();
}

void TemplatesModel::setKindFilter(const QString& kind)
{
    if (kind == kindFilter_)
        return;
    kindFilter_ = kind;
    emit filterChanged();
    rebuildCatalog();
}

void TemplatesModel::setMaterialFilter(const QString& material)
{
    if (material == materialFilter_)
        return;
    materialFilter_ = material;
    emit filterChanged();
    rebuildCatalog();
}

std::vector<const TemplatesModel::Template*> TemplatesModel::selectedTemplates() const
{
    std::vector<const Template*> out;
    if (selected_.startsWith(QLatin1String("set:"))) {
        const QString rest = selected_.mid(4);
        const qsizetype bar = rest.lastIndexOf(QLatin1Char('|'));
        const QString set = rest.left(bar), material = rest.mid(bar + 1);
        for (const Template& t : templates_) {
            const QString tMaterial = t.item.material ? idText(t.item.material->value) : QString();
            if (t.set == set && tMaterial == material)
                out.push_back(&t);
        }
        std::stable_sort(out.begin(), out.end(), [](const Template* a, const Template* b) {
            return a->sequence != b->sequence ? a->sequence < b->sequence : a->item.name < b->item.name;
        });
        return out;
    }
    for (const Template& t : templates_) {
        if (idText(t.item.id) == selected_)
            out.push_back(&t);
    }
    return out;
}

void TemplatesModel::select(const QString& key)
{
    if (key == selected_)
        return;
    selected_ = key;
    deleteAsked_.clear();
    emit selectionChanged();
    emit statusChanged();
    schedulePreview();
}

QVariantMap TemplatesModel::selectedInfo() const
{
    const auto list = selectedTemplates();
    if (list.empty())
        return {};
    const auto line = [](const QString& label, const QString& value) {
        return QVariantMap{{QStringLiteral("label"), label}, {QStringLiteral("value"), value}};
    };
    QVariantMap out;
    if (selected_.startsWith(QLatin1String("set:"))) {
        const Template& first = *list.front();
        QStringList members;
        for (const Template* t : list) {
            const QString when = whenText(jsonObject(t->item.extraJson).value(QStringLiteral("when")).toObject());
            members << (when.isEmpty() ? QStringLiteral("%1. %2").arg(t->sequence).arg(qs(t->item.name))
                                       : trt("%1. %2, when %3").arg(t->sequence).arg(qs(t->item.name), when));
        }
        out.insert(QStringLiteral("kind"), QStringLiteral("set"));
        out.insert(QStringLiteral("title"), first.set);
        out.insert(QStringLiteral("subtitle"),
                   trt("Template set for %1, in %2")
                       .arg(materialName(first.item.material ? idText(first.item.material->value) : QString()),
                            first.library));
        out.insert(QStringLiteral("members"), members);
        out.insert(QStringLiteral("readOnly"), true);
        out.insert(QStringLiteral("canRename"), false);
        return out;
    }
    const Template& t = *list.front();
    const QJsonObject extra = jsonObject(t.item.extraJson);
    QVariantList lines;
    lines << line(trt("Operation"), qs(t.item.opType));
    lines << line(trt("For"), kindText(qs(t.item.featureKind)));
    lines << line(trt("Material"), materialName(t.item.material ? idText(t.item.material->value) : QString()));
    if (!t.set.isEmpty())
        lines << line(trt("Set"), trt("%1, step %2").arg(t.set).arg(t.sequence));
    if (extra.contains(QStringLiteral("when")))
        lines << line(trt("When"), whenText(extra.value(QStringLiteral("when")).toObject()));
    lines << line(trt("Tool"), pairs(jsonObject(t.item.toolQueryJson)));
    lines << line(trt("Parameters"), pairs(jsonObject(t.item.paramsJson)));
    const QJsonObject feeds = extra.value(QStringLiteral("feeds")).toObject();
    lines << line(trt("Feeds"), feeds.isEmpty() ? trt("calculated") : pairs(feeds));
    if (extra.contains(QStringLiteral("heights")))
        lines << line(trt("Heights"), pairs(extra.value(QStringLiteral("heights")).toObject()) + trt(" (mm)"));
    lines << line(trt("Library"), t.library + (t.readOnly ? trt(" (read-only)") : QString()));
    if (!t.item.comment.empty())
        lines << line(trt("Comment"), qs(t.item.comment));
    out.insert(QStringLiteral("kind"), QStringLiteral("template"));
    out.insert(QStringLiteral("id"), idText(t.item.id));
    out.insert(QStringLiteral("title"), qs(t.item.name));
    out.insert(QStringLiteral("subtitle"), kindText(qs(t.item.featureKind)));
    out.insert(QStringLiteral("lines"), lines);
    out.insert(QStringLiteral("readOnly"), t.readOnly);
    out.insert(QStringLiteral("canRename"), !t.readOnly);
    out.insert(QStringLiteral("library"), t.library);
    return out;
}

QString TemplatesModel::templateName(const QString& templateId) const
{
    if (templateId.isEmpty())
        return {};
    if (lib::LibraryStack* s = stack()) {
        if (const lib::OpTemplate* t = s->opTemplate(Uuid::fromString(templateId.toStdString())))
            return qs(t->name);
    }
    return trt("a template not in the libraries");
}

// ---------- the target ----------
QString TemplatesModel::setupIdText() const
{
    return setup_ ? idText(setup_->value) : QString();
}

void TemplatesModel::setSetup(const QString& id)
{
    const std::optional<SetupId> next =
        id.isEmpty() ? std::nullopt : std::optional<SetupId>(SetupId{Uuid::fromString(id.toStdString())});
    if (next == setup_)
        return;
    setup_ = next;
    groups_.clear();
    target_ = QStringLiteral("all");
    emit setupsChanged();
    emit previewChanged();
    rebuildCatalog(); // "the setup's material" follows it
    schedulePreview();
}

QVariantList TemplatesModel::targets() const
{
    QVariantList out;
    int holes = 0;
    for (const QVariant& g : groups_)
        holes += g.toMap().value(QStringLiteral("count")).toInt();
    out.push_back(QVariantMap{
        {QStringLiteral("value"), QStringLiteral("all")},
        {QStringLiteral("text"), groups_.isEmpty() ? trt("Every recognised hole of the setup")
                                                   : trt("Every recognised hole of the setup (%1)").arg(holes)}});
    for (const QVariant& g : groups_) {
        const QVariantMap m = g.toMap();
        out.push_back(QVariantMap{{QStringLiteral("value"), m.value(QStringLiteral("key"))},
                                  {QStringLiteral("text"), m.value(QStringLiteral("label"))}});
    }
    out.push_back(QVariantMap{
        {QStringLiteral("value"), QStringLiteral("selection")},
        {QStringLiteral("text"), trt("The holes of the faces selected in the view (%1)").arg(selectedFaces())}});
    return out;
}

void TemplatesModel::setTarget(const QString& target)
{
    if (target == target_)
        return;
    target_ = target;
    emit previewChanged();
    schedulePreview();
}

int TemplatesModel::selectedFaces() const
{
    if (!selection_)
        return 0;
    int n = 0;
    for (const SelectionEntry& e : selection_->entries())
        n += e.sub && e.sub->kind == kernel::SubKind::Face && e.ref ? 1 : 0;
    return n;
}

// ---------- the preview and applying ----------
std::string TemplatesModel::applyArgs() const
{
    return lastArgs_;
}

QString TemplatesModel::applyBlockedReason() const
{
    if (!docState())
        return trt("Open a document first");
    if (!setup_)
        return trt("Add a setup first: templates make operations in a setup");
    if (selectedTemplates().empty())
        return trt("Choose a template set or a template");
    if (busy_)
        return trt("Busy");
    if (previewState_ == QLatin1String("working") || previewState_.isEmpty())
        return trt("Working out what it would make…");
    if (previewState_ != QLatin1String("ok"))
        return previewMessage_;
    return {};
}

void TemplatesModel::schedulePreview()
{
    if (!shown_ || mode_ != QLatin1String("apply"))
        return;
    ++previewSerial_; // an answer still on its way is out of date
    if (previewState_ != QLatin1String("working")) {
        previewState_ = QStringLiteral("working");
        emit previewChanged();
    }
    previewTimer_.start();
}

void TemplatesModel::runPreview()
{
    api::IApp* a = app();
    const auto list = selectedTemplates();
    if (a == nullptr || !shown_ || !setup_ || list.empty() || !docState()) {
        previewState_.clear();
        previewOps_.clear();
        previewMessage_.clear();
        previewSummary_.clear();
        emit previewChanged();
        return;
    }
    QJsonArray ids;
    for (const Template* t : list)
        ids.append(idText(t->item.id));
    QJsonObject args{{QStringLiteral("setup"), idText(setup_->value)}, {QStringLiteral("templates"), ids}};
    if (target_ == QLatin1String("selection")) {
        QJsonArray refs;
        if (selection_) {
            for (const SelectionEntry& e : selection_->entries()) {
                if (e.sub && e.sub->kind == kernel::SubKind::Face && e.ref)
                    refs.append(QJsonDocument::fromJson(QByteArray::fromStdString(naming::toJson(*e.ref))).object());
            }
        }
        if (refs.isEmpty()) {
            previewState_ = QStringLiteral("refused");
            previewOps_.clear();
            previewMessage_ = trt("Select a face of each hole in the view first");
            previewSummary_.clear();
            emit previewChanged();
            return;
        }
        args.insert(QStringLiteral("geometry"), refs);
    } else if (target_ != QLatin1String("all")) {
        args.insert(QStringLiteral("holes"), QJsonObject{{QStringLiteral("group"), target_}});
    }
    lastArgs_ = jsonText(args);
    const std::uint64_t serial = previewSerial_;
    ++previews_;
    JobHandle<api::ActionResult> job = a->actions().invoke("cam.template.preview", lastArgs_, api::Caller::User);
    if (!job.valid())
        return;
    QPointer<TemplatesModel> self(this);
    const QString setup = idText(setup_->value);
    job.then([self, serial, setup](Result<api::ActionResult> r) {
        if (!self || serial != self->previewSerial_)
            return;
        self->previewOps_.clear();
        self->previewSummary_.clear();
        if (!r) {
            self->previewState_ = QStringLiteral("failed");
            self->previewMessage_ = qs(r.error().message);
            emit self->previewChanged();
            return;
        }
        const QJsonObject o = jsonObject(r->resultJson);
        self->groups_.clear();
        for (const QJsonValue& g : o.value(QStringLiteral("groups")).toArray())
            self->groups_.push_back(g.toObject().toVariantMap());
        self->groupsSetup_ = setup;
        int holes = 0;
        for (const QJsonValue& v : o.value(QStringLiteral("operations")).toArray()) {
            const QJsonObject op = v.toObject();
            holes = std::max(holes, op.value(QStringLiteral("holes")).toInt());
            self->previewOps_.push_back(
                QVariantMap{{QStringLiteral("name"), op.value(QStringLiteral("name")).toString()},
                            {QStringLiteral("tool"), op.value(QStringLiteral("toolName")).toString()},
                            {QStringLiteral("toolNumber"), op.value(QStringLiteral("toolNumber")).toInt()},
                            {QStringLiteral("holes"), op.value(QStringLiteral("holes")).toInt()},
                            {QStringLiteral("template"), op.value(QStringLiteral("templateName")).toString()}});
        }
        const bool ok = o.value(QStringLiteral("ok")).toBool();
        self->previewState_ = ok ? QStringLiteral("ok") : QStringLiteral("refused");
        self->previewMessage_ = o.value(QStringLiteral("message")).toString();
        const int targets = o.value(QStringLiteral("targets")).toInt();
        const int untouched = o.value(QStringLiteral("untouched")).toInt();
        if (ok) {
            self->previewSummary_ =
                counted(static_cast<int>(self->previewOps_.size()), "%1 operation", "%1 operations") +
                QStringLiteral(", ") + counted(targets, "on %1 hole named", "on the %1 holes named");
            if (untouched > 0)
                self->previewSummary_ += QStringLiteral(", ") + counted(untouched, "%1 left alone (another kind)",
                                                                        "%1 left alone (another kind)");
        }
        emit self->previewChanged();
    });
}

bool TemplatesModel::invoke(const char* action, const std::string& args, std::function<void(const QJsonObject&)> done,
                            const QString& what)
{
    api::IApp* a = app();
    if (a == nullptr || busy_)
        return false;
    JobHandle<api::ActionResult> job = a->actions().invoke(action, args, api::Caller::User);
    if (!job.valid())
        return false;
    busy_ = true;
    status_.clear();
    emit statusChanged();
    emit previewChanged();
    QPointer<TemplatesModel> self(this);
    job.then([self, done = std::move(done), what](Result<api::ActionResult> r) {
        if (!self)
            return;
        self->busy_ = false;
        if (!r) {
            self->setStatus(what + QStringLiteral(": ") + qs(r.error().message), true);
            emit self->previewChanged();
            return;
        }
        done(jsonObject(r->resultJson));
        emit self->previewChanged();
    });
    return true;
}

void TemplatesModel::setStatus(const QString& text, bool error)
{
    status_ = text;
    statusIsError_ = error;
    emit statusChanged();
}

void TemplatesModel::dismissStatus()
{
    setStatus(QString(), false);
}

bool TemplatesModel::apply()
{
    if (!canApply())
        return false;
    const std::string args = lastArgs_;
    QPointer<TemplatesModel> self(this);
    return invoke(
        "cam.template.apply", args,
        [self](const QJsonObject& o) {
            QStringList ids, names;
            for (const QJsonValue& v : o.value(QStringLiteral("operations")).toArray()) {
                ids << v.toObject().value(QStringLiteral("id")).toString();
                names << v.toObject().value(QStringLiteral("name")).toString();
            }
            self->setStatus(counted(static_cast<int>(ids.size()), "Made %1 operation", "Made %1 operations") +
                                QStringLiteral(": ") + names.join(QStringLiteral(", ")),
                            false);
            if (self->tree_ && !ids.isEmpty())
                self->tree_->selectWhenListed(ids.front());
            emit self->applied(ids);
            self->schedulePreview();
        },
        trt("Apply"));
}

// ---------- open, close, save ----------
bool TemplatesModel::open(const QString& setupId)
{
    if (!docState())
        return false;
    rebuildSetups();
    QString id = setupId;
    if (id.isEmpty() && tree_) {
        if (const auto current = tree_->currentSetup())
            id = idText(current->value);
    }
    if (id.isEmpty() && !setups_.isEmpty())
        id = setups_.front().toMap().value(QStringLiteral("value")).toString();
    mode_ = QStringLiteral("apply");
    shown_ = true;
    deleteAsked_.clear();
    setup_.reset();
    setSetup(id);
    rebuildCatalog();
    emit shownChanged();
    schedulePreview();
    return true;
}

bool TemplatesModel::openSave(const QString& operationId)
{
    doc::DocStatePtr state = docState();
    if (!state || !state->cam)
        return false;
    const auto it = state->cam->operations.find(OperationId{Uuid::fromString(operationId.toStdString())});
    if (it == state->cam->operations.end())
        return false;
    const cam::Operation& op = it->second;
    saveOp_ = operationId;
    saveOpName_ = qs(op.name);
    saveValues_ =
        QVariantMap{{QStringLiteral("name"), qs(op.name)},
                    {QStringLiteral("set"), QString()},
                    {QStringLiteral("sequence"), QStringLiteral("0")},
                    {QStringLiteral("featureKind"), QStringLiteral("auto")},
                    {QStringLiteral("material"), QStringLiteral("setup")},
                    {QStringLiteral("tool"), QStringLiteral("rule")},
                    {QStringLiteral("feeds"), op.calculated ? QStringLiteral("calculated") : QStringLiteral("fixed")},
                    {QStringLiteral("heights"), QStringLiteral("operation")}};
    for (const cam::Setup& s : state->cam->setups) {
        if (std::find(s.operations.begin(), s.operations.end(), op.id) != s.operations.end())
            setup_ = s.id;
    }
    mode_ = QStringLiteral("save");
    shown_ = true;
    status_.clear();
    emit saveValuesChanged();
    emit statusChanged();
    emit shownChanged();
    return true;
}

void TemplatesModel::close()
{
    if (!shown_)
        return;
    shown_ = false;
    deleteAsked_.clear();
    previewTimer_.stop();
    ++previewSerial_;
    emit shownChanged();
}

void TemplatesModel::setSaveValue(const QString& key, const QVariant& value)
{
    if (saveValues_.value(key) == value)
        return;
    saveValues_.insert(key, value);
    emit saveValuesChanged();
}

bool TemplatesModel::canSave() const
{
    bool ok = false;
    saveValues_.value(QStringLiteral("sequence")).toString().trimmed().toInt(&ok);
    return !busy_ && !saveOp_.isEmpty() && !saveValues_.value(QStringLiteral("name")).toString().trimmed().isEmpty() &&
           ok;
}

bool TemplatesModel::save()
{
    if (!canSave())
        return false;
    const QVariantMap& v = saveValues_;
    QJsonObject args{{QStringLiteral("operation"), saveOp_},
                     {QStringLiteral("name"), v.value(QStringLiteral("name")).toString().trimmed()},
                     {QStringLiteral("sequence"), v.value(QStringLiteral("sequence")).toString().trimmed().toInt()},
                     {QStringLiteral("tool"), v.value(QStringLiteral("tool")).toString()},
                     {QStringLiteral("feeds"), v.value(QStringLiteral("feeds")).toString()},
                     {QStringLiteral("heights"), v.value(QStringLiteral("heights")).toString()}};
    const QString set = v.value(QStringLiteral("set")).toString().trimmed();
    if (!set.isEmpty())
        args.insert(QStringLiteral("set"), set);
    const QString kind = v.value(QStringLiteral("featureKind")).toString();
    if (kind != QLatin1String("auto"))
        args.insert(QStringLiteral("featureKind"), kind);
    args.insert(QStringLiteral("material"), v.value(QStringLiteral("material")).toString() == QLatin1String("any")
                                                ? QJsonValue(QJsonValue::Null)
                                                : QJsonValue(QStringLiteral("setup")));
    QPointer<TemplatesModel> self(this);
    return invoke(
        "cam.template.saveFrom", jsonText(args),
        [self](const QJsonObject& o) {
            const QString id = o.value(QStringLiteral("id")).toString();
            // On to browsing, with the new template selected: the search and kind cleared, the material kept when
            // the template is for it.
            self->mode_ = QStringLiteral("apply");
            const QJsonValue material =
                o.value(QStringLiteral("template")).toObject().value(QStringLiteral("material"));
            if (self->materialFilter_ != QLatin1String("setup") ||
                (!material.isNull() && material.toString() != self->materialOfSetup()))
                self->materialFilter_ = QString();
            self->search_.clear();
            self->kindFilter_.clear();
            emit self->filterChanged();
            self->rebuildCatalog();
            QString library = o.value(QStringLiteral("library")).toString();
            for (const Template& t : self->templates_) {
                if (idText(t.item.id) == id)
                    library = t.library;
            }
            self->setStatus(trt("Saved '%1' to %2").arg(o.value(QStringLiteral("name")).toString(), library), false);
            self->selected_ = id;
            emit self->selectionChanged();
            emit self->shownChanged();
            emit self->saved(id);
            self->schedulePreview();
        },
        trt("Save as template"));
}

// ---------- rename, delete, re-apply ----------
bool TemplatesModel::rename(const QString& templateId, const QString& name)
{
    const QString trimmed = name.trimmed();
    const auto it = std::find_if(templates_.begin(), templates_.end(),
                                 [&](const Template& t) { return idText(t.item.id) == templateId; });
    if (it == templates_.end() || trimmed.isEmpty() || trimmed == qs(it->item.name) || it->readOnly)
        return false;
    const QJsonObject args{
        {QStringLiteral("id"), templateId}, {QStringLiteral("name"), trimmed}, {QStringLiteral("base"), it->hash}};
    QPointer<TemplatesModel> self(this);
    return invoke(
        "cam.template.rename", jsonText(args),
        [self, trimmed](const QJsonObject&) { self->setStatus(trt("Renamed to '%1'").arg(trimmed), false); },
        trt("Rename"));
}

void TemplatesModel::askDelete(const QString& templateId)
{
    deleteAsked_ = templateId;
    emit statusChanged();
}

void TemplatesModel::cancelDelete()
{
    deleteAsked_.clear();
    emit statusChanged();
}

bool TemplatesModel::confirmDelete()
{
    const QString id = deleteAsked_;
    const auto it =
        std::find_if(templates_.begin(), templates_.end(), [&](const Template& t) { return idText(t.item.id) == id; });
    deleteAsked_.clear();
    emit statusChanged();
    if (it == templates_.end() || it->readOnly)
        return false;
    const QString name = qs(it->item.name);
    const QJsonObject args{{QStringLiteral("id"), id}, {QStringLiteral("base"), it->hash}};
    QPointer<TemplatesModel> self(this);
    return invoke(
        "cam.template.remove", jsonText(args),
        [self, name](const QJsonObject&) { self->setStatus(trt("Deleted '%1'").arg(name), false); }, trt("Delete"));
}

bool TemplatesModel::reapply(const QString& operationId)
{
    const QJsonObject args{{QStringLiteral("operation"), operationId}};
    QPointer<TemplatesModel> self(this);
    return invoke(
        "cam.template.reapply", jsonText(args),
        [self](const QJsonObject& o) {
            const QJsonArray ops = o.value(QStringLiteral("operations")).toArray();
            self->setStatus(trt("Re-applied '%1'")
                                .arg(ops.isEmpty()
                                         ? QString()
                                         : ops.first().toObject().value(QStringLiteral("templateName")).toString()),
                            false);
        },
        trt("Re-apply template"));
}

// ---------- commands ----------
QString TemplatesModel::commandReason(const QString& id) const
{
    if (!docState())
        return trt("Open a document first");
    if (id == QLatin1String("cam.templates.open"))
        return setups_.isEmpty() ? trt("Add a setup first: templates make operations in a setup") : QString();
    const std::optional<OperationId> op = tree_ ? tree_->currentOperation() : std::nullopt;
    if (!op)
        return trt("Select an operation in the CAM tree first");
    if (id == QLatin1String("cam.templates.reapply")) {
        doc::DocStatePtr state = docState();
        const cam::Operation* found = nullptr;
        if (state && state->cam) {
            if (const auto it = state->cam->operations.find(*op); it != state->cam->operations.end())
                found = &it->second;
        }
        if (found == nullptr || found->templateId.empty())
            return trt("The selected operation was not made from a template");
    }
    return {};
}

bool TemplatesModel::runCommand(const QString& id)
{
    if (!commandReason(id).isEmpty())
        return false;
    if (id == QLatin1String("cam.templates.open"))
        return open();
    const std::optional<OperationId> op = tree_ ? tree_->currentOperation() : std::nullopt;
    if (!op)
        return false;
    if (id == QLatin1String("cam.templates.saveFrom"))
        return openSave(idText(op->value));
    if (id == QLatin1String("cam.templates.reapply"))
        return reapply(idText(op->value));
    return false;
}

} // namespace ray::ui
