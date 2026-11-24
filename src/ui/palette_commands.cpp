#include "palette_commands.h"

#include "app_context.h"
#include "palette_schema.h"
#include "scene_controller.h"
#include "section_model.h"
#include "selection_model.h"
#include "shortcuts.h"
#include "viewport_item.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>
#include <QtCore/QVariant>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeySequence>
#include <QtGui/QMouseEvent>
#include <QtGui/QStyleHints>
#include <QtQuick/QQuickItem>

#include <algorithm>
#include <set>
#include <string>
#include <string_view>

namespace ray::ui {
namespace {

constexpr const char* kEngineKey = "_ray_ui_commands";
constexpr const char* kTrContext = "ray::ui::CommandCatalog";

QString trc(const char* text)
{
    return QCoreApplication::translate(kTrContext, text);
}

// ---------- View commands: what the UI can do that no registry action does (ui_commands.h) ----------
// The general ones, run here (runUi). The measure, section, sketch, constraint and model commands are declared in the
// files that run them; allUiCommands() gathers every table.
// clang-format off
constexpr UiCommand kGeneralCommands[] = {
    {"ui.commandPalette", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Command Palette"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "General"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Searches every command and runs it"), "Ctrl+K", CommandContext::Global, ""},
    {"ui.keyboardShortcuts", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Keyboard Shortcuts"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "General"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Shows and changes the keyboard shortcuts"), "", CommandContext::Global, ""},
    {"ui.settings", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Settings"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "General"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Shows and changes units, appearance, graphics and navigation"), "Ctrl+,", CommandContext::Global, ""},
    {"view.fitAll", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Fit All"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "View"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Frames everything shown"), "", CommandContext::Global, ""},
    {"view.fitSelection", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Fit Selection"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "View"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Frames the selected elements"), "", CommandContext::Global, ""},
    {"view.front", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Front View"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "View"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Looks at the model from the front"), "", CommandContext::Global, ""},
    {"view.back", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Back View"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "View"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Looks at the model from the back"), "", CommandContext::Global, ""},
    {"view.left", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Left View"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "View"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Looks at the model from the left"), "", CommandContext::Global, ""},
    {"view.right", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Right View"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "View"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Looks at the model from the right"), "", CommandContext::Global, ""},
    {"view.top", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Top View"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "View"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Looks down at the model"), "", CommandContext::Global, ""},
    {"view.bottom", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Bottom View"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "View"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Looks up at the model"), "", CommandContext::Global, ""},
    {"view.iso", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Isometric View"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "View"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Looks at the model from a corner"), "", CommandContext::Global, ""},
    {"view.toggleProjection", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Perspective Projection"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "View"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Switches between perspective and orthographic projection"), "", CommandContext::Global, ""},
    {"view.hideSelectedBodies", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Hide Body"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "View"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Hides the bodies of the selected elements"), "", CommandContext::Global, ""},
    {"view.isolateSelectedBodies", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Show Only This Body"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "View"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Hides every body except those of the selected elements"), "", CommandContext::Global, ""},
    {"view.showAllBodies", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Show All Bodies"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "View"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Shows every hidden body"), "", CommandContext::Global, ""},
    {"selection.clear", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Clear Selection"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Selection"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Deselects everything"), "", CommandContext::Global, ""},
    {"selection.selectBody", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Select Body"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Selection"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Selects the bodies of the selected faces, edges and vertices"), "", CommandContext::Global, ""},
    {"selection.copyProperties", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Copy Properties"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Selection"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Copies the selected element's measured values as text"), "", CommandContext::Global, ""},
    {"selection.copyCoordinates", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Copy Coordinates"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Selection"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Copies the selected vertex's X, Y and Z in mm"), "", CommandContext::Global, ""},
};
// clang-format on

// ---------- What an action needs before it can run ----------
// Auto: a "document" argument means it needs an open document, and changesDocument a writable one.
enum class Needs { Auto, Nothing, WritableDocument };
struct Requirement {
    const char* id;
    Needs needs;
};
constexpr Requirement kRequirements[] = {
    {"file.new", Needs::Nothing},
    {"file.open", Needs::Nothing},
    {"file.import", Needs::Nothing}, // makes a new document when none is open
    {"file.save", Needs::WritableDocument},
};

// ---------- Context menus: which commands apply to which selection ----------
// The selection kinds an entry applies to. Until the api has a field for it this table is the declaration; a
// proper field would be ActionInfo::appliesTo, a list of selection contexts such as "selection.none",
// "selection.face", "selection.edge", "selection.vertex", "selection.body" (later "selection.feature",
// "selection.operation"), plus a menu group and order, so scripts and plug-ins can add context-menu entries too.
enum Scope : unsigned {
    kNothing = 1u << 0, // nothing selected
    kFace = 1u << 1,
    kEdge = 1u << 2,
    kVertex = 1u << 3,
    kBody = 1u << 4,
    kElement = kFace | kEdge | kVertex,
    kAnySelection = kElement | kBody,
    kAlways = kNothing | kAnySelection,
};
struct ContextEntry {
    const char* id;
    unsigned scope;
    int group;             // a separator goes between groups
    const char* submenu;   // entries with the same submenu title are gathered into one submenu
    bool hideWhenDisabled; // leave it out instead of showing it greyed
};
constexpr const char* kStandardViews = QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Standard View");
constexpr ContextEntry kContextMenu[] = {
    {"view.fitAll", kNothing, 0, nullptr, false},
    {"view.front", kNothing, 0, kStandardViews, false},
    {"view.back", kNothing, 0, kStandardViews, false},
    {"view.left", kNothing, 0, kStandardViews, false},
    {"view.right", kNothing, 0, kStandardViews, false},
    {"view.top", kNothing, 0, kStandardViews, false},
    {"view.bottom", kNothing, 0, kStandardViews, false},
    {"view.iso", kNothing, 0, kStandardViews, false},
    {"view.toggleProjection", kNothing, 0, nullptr, false},
    {"view.section.toggle", kNothing, 0, nullptr, false}, // T-023
    {"view.draft.toggle", kNothing, 0, nullptr, false},
    {"view.showAllBodies", kNothing, 0, nullptr, true},
    {"file.open", kNothing, 1, nullptr, false},
    {"file.import", kNothing, 1, nullptr, false},
    {"view.fitSelection", kAnySelection, 2, nullptr, false},
    {"selection.selectBody", kElement, 2, nullptr, false},
    {"view.section.fromFace", kFace, 2, nullptr, false}, // T-023
    {"view.draft.fromFace", kFace, 2, nullptr, false},
    {"view.hideSelectedBodies", kAnySelection, 3, nullptr, false},
    {"view.isolateSelectedBodies", kAnySelection, 3, nullptr, false},
    {"view.showAllBodies", kAnySelection, 3, nullptr, true},
    {"selection.copyProperties", kFace | kEdge | kBody, 4, nullptr, false},
    {"selection.copyCoordinates", kVertex, 4, nullptr, false},
    {"selection.clear", kAnySelection, 4, nullptr, false},
    // T-022
    {"measure.tool", kAnySelection, 6, nullptr, false},
    {"measure.radius", kFace | kEdge, 6, nullptr, true},
    {"measure.massProperties", kBody, 6, nullptr, false},
    {"measure.boundingBox", kBody, 6, nullptr, false},
    {"edit.undo", kAlways, 5, nullptr, false},
    {"edit.redo", kAlways, 5, nullptr, false},
};

unsigned scopeOf(kernel::SubKind kind)
{
    switch (kind) {
    case kernel::SubKind::Face:
        return kFace;
    case kernel::SubKind::Edge:
        return kEdge;
    case kernel::SubKind::Vertex:
        return kVertex;
    case kernel::SubKind::Solid:
        return kBody;
    }
    return 0;
}

QString qs(const std::string& s)
{
    return QString::fromStdString(s);
}

QByteArray utf8(const QString& s)
{
    return s.toUtf8();
}

std::string_view view(const QByteArray& bytes)
{
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

Command fromAction(const api::ActionInfo& a)
{
    Command c;
    c.id = qs(a.id);
    c.title = qs(a.title);
    c.category = qs(a.category);
    c.description = qs(a.description);
    c.schema = qs(a.argsSchemaJson);
    c.defaultShortcut = qs(a.defaultShortcut);
    c.fromRegistry = true;
    c.longRunning = a.longRunning;
    c.changesDocument = a.changesDocument;
    return c;
}

Command fromUi(const UiCommand& u)
{
    Command c;
    c.id = QString::fromLatin1(u.id);
    c.title = trc(u.title);
    c.category = trc(u.category);
    c.description = trc(u.description);
    c.defaultShortcut = QString::fromLatin1(u.shortcut);
    c.context = u.context;
    return c;
}

// Registry actions a UI command stands for (UiCommand::fronts): the palette lists the UI command only.
const std::set<QString>& frontedActions()
{
    static const std::set<QString> fronted = [] {
        std::set<QString> out;
        for (const UiCommand& u : allUiCommands()) {
            for (const QString& id : QString::fromLatin1(u.fronts).split(QLatin1Char(' '), Qt::SkipEmptyParts))
                out.insert(id);
        }
        return out;
    }();
    return fronted;
}

// How well `query` names a command: 6 exact, 5 prefix, 4 a word starts with it, 3 substring, 2 subsequence of the
// title, 1 found in the category or description, 0 no match. Used to merge view commands into the registry's
// ranking.
QStringList words(const QString& text)
{
    QStringList out;
    QString word;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        const bool split =
            c == QLatin1Char(' ') || c == QLatin1Char('.') || c == QLatin1Char('-') || c == QLatin1Char('_');
        if (split || (c.isUpper() && i > 0 && text.at(i - 1).isLower())) {
            if (!word.isEmpty()) {
                out << word.toLower();
            }
            word = split ? QString() : QString(c);
        } else {
            word += c;
        }
    }
    if (!word.isEmpty()) {
        out << word.toLower();
    }
    return out;
}

bool subsequence(const QString& query, const QString& text)
{
    qsizetype at = 0;
    for (const QChar c : query) {
        at = text.indexOf(c, at);
        if (at < 0) {
            return false;
        }
        ++at;
    }
    return true;
}

int matchTier(const QString& rawQuery, const Command& c)
{
    const QString q = rawQuery.trimmed().toLower();
    if (q.isEmpty()) {
        return 1;
    }
    const QString title = c.title.toLower();
    const QString id = c.id.toLower();
    const QString last = id.section(QLatin1Char('.'), -1);
    if (title == q || last == q || id == q) {
        return 6;
    }
    if (title.startsWith(q) || last.startsWith(q) || id.startsWith(q)) {
        return 5;
    }
    const QStringList ws = words(c.title) + words(c.id);
    if (std::any_of(ws.begin(), ws.end(), [&](const QString& w) { return w.startsWith(q); })) {
        return 4;
    }
    if (title.contains(q) || id.contains(q)) {
        return 3;
    }
    if (subsequence(q, title)) {
        return 2;
    }
    if (c.category.toLower().contains(q) || c.description.toLower().contains(q)) {
        return 1;
    }
    return 0;
}

QString nativeText(const QString& portable)
{
    return QKeySequence::fromString(portable, QKeySequence::PortableText).toString(QKeySequence::NativeText);
}

// The banner text for a finished action: its title and a few plain fields of the result.
QString summarize(const QString& title, const api::ActionResult& result)
{
    static const QStringList kSkip = {QStringLiteral("document"), QStringLiteral("stateHash"), QStringLiteral("commit"),
                                      QStringLiteral("dirty"),    QStringLiteral("readOnly"),  QStringLiteral("lock")};
    const QJsonObject o = QJsonDocument::fromJson(QByteArray::fromStdString(result.resultJson)).object();
    QStringList parts;
    for (auto it = o.begin(); it != o.end() && parts.size() < 3; ++it) {
        if (kSkip.contains(it.key())) {
            continue;
        }
        const QJsonValue v = it.value();
        QString text;
        if (v.isString()) {
            text = v.toString();
        } else if (v.isDouble()) {
            text = QString::number(v.toDouble(), 'g', 10);
        } else if (v.isBool()) {
            text = v.toBool() ? trc("yes") : trc("no");
        }
        if (!text.isEmpty()) {
            parts << QStringLiteral("%1 %2").arg(it.key(), text);
        }
    }
    QString message =
        parts.isEmpty() ? trc("%1: done").arg(title) : trc("%1: %2").arg(title, parts.join(QStringLiteral(", ")));
    for (const std::string& w : result.warnings) {
        message += QLatin1Char('\n') + qs(w);
    }
    return message;
}

} // namespace

std::span<const UiCommand> generalUiCommands()
{
    return kGeneralCommands;
}

const std::vector<UiCommand>& allUiCommands()
{
    static const std::vector<UiCommand> all = [] {
        std::vector<UiCommand> out;
        for (const std::span<const UiCommand> table :
             {generalUiCommands(), measureUiCommands(), sectionUiCommands(), sketchUiCommands(), constraintUiCommands(),
              modelUiCommands(), camSetupUiCommands(), camTreeUiCommands(), camOperationUiCommands(),
              camPostUiCommands()})
            out.insert(out.end(), table.begin(), table.end());
        const std::span<const UiCommand> tools = camToolsUiCommands(); // T-046
        out.insert(out.end(), tools.begin(), tools.end());
        const std::span<const UiCommand> templates = camTemplatesUiCommands(); // T-057
        out.insert(out.end(), templates.begin(), templates.end());
        const std::span<const UiCommand> sheets = camSheetUiCommands(); // T-076
        out.insert(out.end(), sheets.begin(), sheets.end());
        const std::span<const UiCommand> fixtures = camFixturesUiCommands(); // T-077
        out.insert(out.end(), fixtures.begin(), fixtures.end());
        return out;
    }();
    return all;
}

CommandCatalog::CommandCatalog(AppContext* context, SceneController* scene, SelectionModel* selection,
                               ViewportProvider viewport, QObject* parent)
    : QObject(parent), context_(context), scene_(scene), selection_(selection), viewport_(std::move(viewport))
{
    if (context_) {
        connect(context_, &AppContext::sessionChanged, this, &CommandCatalog::sessionChanged);
        // T-044: the palette lists the commands of the workspace shown (inPalette).
        connect(context_, &AppContext::workspaceChanged, this, [this] {
            ++revision_;
            emit commandsChanged();
        });
    }
}

CommandContext CommandCatalog::activeContext() const
{
    if (context_ && context_->workspace() == kCamWorkspace)
        return CommandContext::Cam;
    return contextProbe_ ? contextProbe_() : CommandContext::Model;
}

bool CommandCatalog::inPalette(const Command& command) const
{
    const bool cam = context_ && context_->workspace() == kCamWorkspace;
    switch (command.context) {
    case CommandContext::Global:
        return true;
    case CommandContext::Cam:
        return cam;
    case CommandContext::Model:
    case CommandContext::Sketch:
        return !cam;
    }
    return true;
}

CommandCatalog::~CommandCatalog()
{
    if (window_) {
        window_->removeEventFilter(this);
    }
}

CommandCatalog* CommandCatalog::create(QQmlEngine* qml, QJSEngine*)
{
    if (qml != nullptr) {
        if (auto* commands = qobject_cast<CommandCatalog*>(qml->property(kEngineKey).value<QObject*>())) {
            QJSEngine::setObjectOwnership(commands, QJSEngine::CppOwnership);
            return commands;
        }
    }
    // Without AppHost (a bare engine): a catalog with nothing behind it, owned by the engine.
    return new CommandCatalog(new AppContext(nullptr), nullptr, nullptr, {});
}

void CommandCatalog::provideFor(QQmlEngine* engine, CommandCatalog* commands)
{
    engine->setProperty(kEngineKey, QVariant::fromValue<QObject*>(commands));
}

void CommandCatalog::sessionChanged()
{
    usageLoaded_ = false;
    usage_.clear();
    ++revision_;
    emit commandsChanged();
}

QString CommandCatalog::userDataDir() const
{
    api::IApp* app = context_ ? context_->app() : nullptr;
    return app != nullptr ? qs(app->userDataDir()) : QString();
}

std::vector<Command> CommandCatalog::commands() const
{
    std::vector<Command> out;
    std::set<QString> ids;
    if (api::IApp* app = context_ ? context_->app() : nullptr) {
        for (const api::ActionInfo& a : app->actions().all()) {
            ids.insert(qs(a.id));
            if (!frontedActions().count(qs(a.id))) { // the UI command that stands for it is listed instead
                out.push_back(fromAction(a));
            }
        }
    }
    for (const UiCommand& u : allUiCommands()) {
        if (!ids.count(QString::fromLatin1(u.id))) {
            out.push_back(fromUi(u));
        }
    }
    std::sort(out.begin(), out.end(), [](const Command& a, const Command& b) { return a.id < b.id; });
    return out;
}

std::optional<Command> CommandCatalog::find(const QString& id) const
{
    if (api::IApp* app = context_ ? context_->app() : nullptr) {
        const QByteArray bytes = utf8(id);
        if (auto a = app->actions().find(view(bytes))) {
            return fromAction(*a);
        }
    }
    for (const UiCommand& u : allUiCommands()) {
        if (id == QLatin1String(u.id)) {
            return fromUi(u);
        }
    }
    return std::nullopt;
}

std::vector<Command> CommandCatalog::search(const QString& query) const
{
    std::vector<Command> registry;
    std::vector<std::pair<int, Command>> missed; // registry actions the registry's search did not return
    std::set<QString> ids;
    if (api::IApp* app = context_ ? context_->app() : nullptr) {
        const QByteArray q = utf8(query.trimmed());
        std::set<QString> found;
        for (const api::ActionInfo& a : app->actions().search(view(q), 0)) {
            found.insert(qs(a.id));
            if (!frontedActions().count(qs(a.id))) {
                registry.push_back(fromAction(a));
            }
        }
        // Titles with punctuation ("Open...") typed as shown: the registry's matcher may not take them, the
        // title match below does. They come after the registry's own results.
        for (const api::ActionInfo& a : app->actions().all()) {
            ids.insert(qs(a.id));
            if (!found.count(qs(a.id)) && !frontedActions().count(qs(a.id))) {
                Command c = fromAction(a);
                if (const int t = matchTier(query, c); t >= 3) {
                    missed.emplace_back(t, std::move(c));
                }
            }
        }
    }
    std::stable_sort(missed.begin(), missed.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (auto& [t, c] : missed) {
        registry.push_back(std::move(c));
    }
    std::vector<std::pair<int, Command>> ui;
    for (const UiCommand& u : allUiCommands()) {
        if (ids.count(QString::fromLatin1(u.id))) {
            continue;
        }
        Command c = fromUi(u);
        if (const int t = matchTier(query, c); t > 0) {
            ui.emplace_back(t, std::move(c));
        }
    }
    std::stable_sort(ui.begin(), ui.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) {
            return a.first > b.first;
        }
        return a.second.title != b.second.title ? a.second.title < b.second.title : a.second.id < b.second.id;
    });
    // The registry's order is kept; a view command goes before the first registry action it matches better.
    std::vector<Command> out;
    std::size_t u = 0;
    for (Command& r : registry) {
        const int t = matchTier(query, r);
        while (u < ui.size() && ui[u].first > t) {
            out.push_back(std::move(ui[u++].second));
        }
        out.push_back(std::move(r));
    }
    while (u < ui.size()) {
        out.push_back(std::move(ui[u++].second));
    }
    return out;
}

std::vector<Command> CommandCatalog::mostUsed(int limit) const
{
    loadUsage();
    std::vector<std::pair<Usage, Command>> used;
    for (const Command& c : commands()) {
        auto it = usage_.find(c.id);
        if (it != usage_.end() && it->second.count > 0) {
            used.emplace_back(it->second, c);
        }
    }
    std::sort(used.begin(), used.end(), [](const auto& a, const auto& b) {
        if (a.first.count != b.first.count) {
            return a.first.count > b.first.count;
        }
        if (a.first.lastMs != b.first.lastMs) {
            return a.first.lastMs > b.first.lastMs;
        }
        return a.second.id < b.second.id;
    });
    std::vector<Command> out;
    for (auto& [u, c] : used) {
        if (limit > 0 && static_cast<int>(out.size()) >= limit) {
            break;
        }
        out.push_back(std::move(c));
    }
    return out;
}

bool CommandCatalog::needsForm(const Command& command) const
{
    return command.fromRegistry && handlerFor(command.id) == nullptr && palette::needsForm(command.schema);
}

void CommandCatalog::setUiHandler(const QString& prefix, UiHandler handler)
{
    if (handler.reason && handler.run) {
        handlers_[prefix] = std::move(handler);
    } else {
        handlers_.erase(prefix);
    }
}

const CommandCatalog::UiHandler* CommandCatalog::handlerFor(const QString& id) const
{
    for (const auto& [prefix, handler] : handlers_) {
        if (id.startsWith(prefix)) {
            return &handler;
        }
    }
    return nullptr;
}

QString CommandCatalog::disabledReason(const Command& command) const
{
    if (const UiHandler* handler = handlerFor(command.id)) {
        return handler->reason(command.id);
    }
    if (!command.fromRegistry) {
        return uiReason(command.id);
    }
    api::IApp* app = context_ ? context_->app() : nullptr;
    if (app == nullptr) {
        return trc("There is no session");
    }
    Needs needs = Needs::Auto;
    for (const Requirement& r : kRequirements) {
        if (command.id == QLatin1String(r.id)) {
            needs = r.needs;
        }
    }
    const bool documentArg = palette::hasProperty(command.schema, QStringLiteral("document"));
    const bool needsDocument =
        needs == Needs::WritableDocument || (needs == Needs::Auto && (documentArg || command.changesDocument));
    const bool needsWritable = needs == Needs::WritableDocument || (needs == Needs::Auto && command.changesDocument);
    api::OpenDocument* active = app->active();
    if (needsDocument && active == nullptr) {
        return trc("Open or create a document first");
    }
    if (active != nullptr && active->readOnly && (needsWritable || command.changesDocument)) {
        return command.id == QLatin1String("file.save") ? trc("The document is read-only; use Save As")
                                                        : trc("The document is read-only");
    }
    if (active != nullptr && active->document) {
        if (command.id == QLatin1String("edit.undo") && !active->document->canUndo()) {
            return trc("There is nothing to undo");
        }
        if (command.id == QLatin1String("edit.redo") && !active->document->canRedo()) {
            return trc("There is nothing to redo");
        }
    }
    return {};
}

QString CommandCatalog::uiReason(const QString& id) const
{
    if (id.startsWith(QLatin1String("ui."))) {
        return {};
    }
    const int count = selection_ ? selection_->count() : 0;
    if (SectionModel::handles(id)) { // T-023
        return section_ ? section_->commandReason(id) : trc("There is no 3D view");
    }
    if (id.startsWith(QLatin1String("view."))) {
        ViewportItem* v = viewport_ ? viewport_() : nullptr;
        if (v == nullptr) {
            return trc("There is no 3D view");
        }
        if (id == QLatin1String("view.fitAll") && !v->hasContent()) {
            return trc("Nothing is shown");
        }
        if (id == QLatin1String("view.showAllBodies")) {
            bool hidden = false;
            for (int i = 0; scene_ && i < scene_->bodyCount(); ++i) {
                hidden = hidden || !scene_->bodyVisible(i);
            }
            return hidden ? QString() : trc("Every body is shown");
        }
        if ((id == QLatin1String("view.fitSelection") || id == QLatin1String("view.hideSelectedBodies") ||
             id == QLatin1String("view.isolateSelectedBodies")) &&
            count == 0) {
            return trc("Nothing is selected");
        }
        return {};
    }
    if (count == 0) {
        return trc("Nothing is selected");
    }
    const std::vector<SelectionEntry>& entries = selection_->entries();
    if (id == QLatin1String("selection.selectBody")) {
        const bool element = std::any_of(entries.begin(), entries.end(),
                                         [](const SelectionEntry& e) { return e.hit.kind != kernel::SubKind::Solid; });
        return element ? QString() : trc("Select a face, edge or vertex first");
    }
    if (id == QLatin1String("selection.copyProperties")) {
        if (count != 1) {
            return trc("Select one element");
        }
        if (selection_->infoBusy() || selection_->info().isEmpty()) {
            return trc("The properties are still being worked out");
        }
    }
    if (id == QLatin1String("selection.copyCoordinates") &&
        (count != 1 || entries.front().hit.kind != kernel::SubKind::Vertex)) {
        return trc("Select one vertex");
    }
    return {};
}

QString CommandCatalog::shortcutText(const QString& id) const
{
    if (shortcuts_ != nullptr) {
        return shortcuts_->shortcutText(id);
    }
    const auto c = find(id);
    return c ? nativeText(c->defaultShortcut) : QString();
}

QVariantMap CommandCatalog::entry(const Command& c) const
{
    const QString reason = disabledReason(c);
    QVariantMap m;
    m.insert(QStringLiteral("kind"), QStringLiteral("item"));
    m.insert(QStringLiteral("id"), c.id);
    m.insert(QStringLiteral("title"), c.title);
    m.insert(QStringLiteral("category"), c.category);
    m.insert(QStringLiteral("description"), c.description);
    m.insert(QStringLiteral("shortcut"), shortcutText(c.id));
    m.insert(QStringLiteral("enabled"), reason.isEmpty());
    m.insert(QStringLiteral("reason"), reason);
    m.insert(QStringLiteral("needsForm"), needsForm(c));
    m.insert(QStringLiteral("longRunning"), c.longRunning);
    bool marked = false;
    if (c.id == QLatin1String("view.toggleProjection")) {
        if (ViewportItem* v = viewport_ ? viewport_() : nullptr) {
            marked = v->perspective();
        }
    } else if (section_ && SectionModel::handles(c.id)) {
        marked = section_->commandMarked(c.id); // T-023
    }
    m.insert(QStringLiteral("marked"), marked);
    return m;
}

QVariantMap CommandCatalog::info(const QString& id) const
{
    const auto c = find(id);
    return c ? entry(*c) : QVariantMap();
}

QVariantList CommandCatalog::formFields(const QString& id) const
{
    const auto c = find(id);
    return c ? palette::formFields(c->id, c->schema) : QVariantList();
}

QVariantMap CommandCatalog::buildArgs(const QString& id, const QVariantMap& values) const
{
    QVariantMap out;
    const auto c = find(id);
    if (!c) {
        out.insert(QStringLiteral("ok"), false);
        out.insert(QStringLiteral("error"), trc("Unknown command %1").arg(id));
        return out;
    }
    const palette::Args args = palette::buildArgs(c->schema, values);
    out.insert(QStringLiteral("ok"), args.ok);
    out.insert(QStringLiteral("json"), args.json);
    out.insert(QStringLiteral("error"), args.error);
    out.insert(QStringLiteral("field"), args.field);
    return out;
}

bool CommandCatalog::trigger(const QString& id)
{
    const auto c = find(id);
    if (!c) {
        const QString message = trc("There is no command %1").arg(id);
        context_->showMessage(message, true);
        emit finished(id, false, message);
        return false;
    }
    if (const QString reason = disabledReason(*c); !reason.isEmpty()) {
        const QString message = trc("%1 is not available: %2").arg(c->title, reason);
        context_->showMessage(message, true);
        emit finished(id, false, message);
        return false;
    }
    if (handlerFor(id) != nullptr) {
        return run(id);
    }
    // T-019: file actions that take a path (open, import, save as, export) use the native file dialogs, and close,
    // new and open go through the unsaved-changes prompt (Files::handleAction); the generated form is for the rest.
    if (c->fromRegistry && context_->handleAction(id, QStringLiteral("{}"))) {
        recordUse(id);
        finishLater(id);
        return true;
    }
    if (needsForm(*c)) {
        emit formRequested(id);
        return true;
    }
    return run(id);
}

bool CommandCatalog::run(const QString& id, const QString& argsJson)
{
    const auto c = find(id);
    if (!c) {
        const QString message = trc("There is no command %1").arg(id);
        context_->showMessage(message, true);
        emit finished(id, false, message);
        return false;
    }
    if (const QString reason = disabledReason(*c); !reason.isEmpty()) {
        const QString message = trc("%1 is not available: %2").arg(c->title, reason);
        context_->showMessage(message, true);
        emit finished(id, false, message);
        return false;
    }
    recordUse(id);
    // T-022: a UI handler's command, or its registry action without arguments (it acts on the selection).
    if (const UiHandler* handler = handlerFor(id);
        handler != nullptr && (!c->fromRegistry || argsJson.isEmpty() || argsJson == QLatin1String("{}"))) {
        const bool ok = handler->run(id);
        emit finished(id, ok, QString());
        return ok;
    }
    if (!c->fromRegistry) {
        const bool ok = runUi(id);
        emit finished(id, ok, QString());
        return ok;
    }
    // A file action with its arguments (a path from the form): Files runs it (prompt, replace, status, errors).
    if (context_->handleAction(id, argsJson.isEmpty() ? QStringLiteral("{}") : argsJson)) {
        finishLater(id);
        return true;
    }
    api::IApp* app = context_->app();
    const QByteArray idBytes = utf8(id);
    const QByteArray argBytes = utf8(argsJson.isEmpty() ? QStringLiteral("{}") : argsJson);
    JobHandle<api::ActionResult> job = app->actions().invoke(view(idBytes), view(argBytes), api::Caller::User);
    if (!job.valid()) {
        const QString message = trc("%1 could not be started").arg(c->title);
        context_->showMessage(message, true);
        emit finished(id, false, message);
        return false;
    }
    // Long-running actions are IApp jobs: the jobs panel and the status bar's slot show them, and Esc or Cancel
    // there cancels them (Session, T-019), so nothing here tracks them.
    const QString title = c->title;
    QPointer<CommandCatalog> self(this);
    job.then([self, id, title](Result<api::ActionResult> result) {
        if (!self || !self->context_) {
            return;
        }
        QString message;
        if (result) {
            message = summarize(title, *result);
            self->context_->showMessage(message, false);
        } else if (result.error().code == ErrorCode::Cancelled) {
            message = trc("%1 was cancelled").arg(title);
            self->context_->showMessage(message, false);
        } else {
            message = trc("%1 failed: %2").arg(title, qs(result.error().message));
            self->context_->showMessage(message, true);
        }
        emit self->finished(id, result.has_value(), message);
    });
    return true;
}

void CommandCatalog::finishLater(const QString& id)
{
    // Like a registry action's result: after the call that started it returned.
    QMetaObject::invokeMethod(this, [this, id] { emit finished(id, true, QString()); }, Qt::QueuedConnection);
}

bool CommandCatalog::runUi(const QString& id)
{
    if (id == QLatin1String("ui.commandPalette")) {
        emit paletteRequested(QString());
        return true;
    }
    if (id == QLatin1String("ui.keyboardShortcuts")) {
        emit shortcutSettingsRequested();
        return true;
    }
    if (id == QLatin1String("ui.settings")) {
        emit settingsRequested();
        return true;
    }
    std::vector<int> bodies;
    if (selection_) {
        for (const SelectionEntry& e : selection_->entries()) {
            if (e.body >= 0 && std::find(bodies.begin(), bodies.end(), e.body) == bodies.end()) {
                bodies.push_back(e.body);
            }
        }
        std::sort(bodies.begin(), bodies.end());
    }
    if (SectionModel::handles(id)) { // T-023
        return section_ && section_->runCommand(id);
    }
    ViewportItem* v = viewport_ ? viewport_() : nullptr;
    if (id.startsWith(QLatin1String("view.")) && v == nullptr) {
        return false;
    }
    if (id == QLatin1String("view.fitAll")) {
        v->fitAll();
    } else if (id == QLatin1String("view.fitSelection")) {
        v->fitSelection();
    } else if (id == QLatin1String("view.toggleProjection")) {
        v->toggleProjection();
    } else if (id == QLatin1String("view.hideSelectedBodies")) {
        for (const int b : bodies) {
            scene_->setBodyVisible(b, false);
        }
        selection_->clear();
    } else if (id == QLatin1String("view.isolateSelectedBodies")) {
        for (int i = 0; i < scene_->bodyCount(); ++i) {
            scene_->setBodyVisible(i, std::binary_search(bodies.begin(), bodies.end(), i));
        }
    } else if (id == QLatin1String("view.showAllBodies")) {
        for (int i = 0; i < scene_->bodyCount(); ++i) {
            scene_->setBodyVisible(i, true);
        }
    } else if (id.startsWith(QLatin1String("view."))) {
        return v->setStandardView(id.mid(5));
    } else if (id == QLatin1String("selection.clear")) {
        selection_->clear();
    } else if (id == QLatin1String("selection.selectBody")) {
        for (std::size_t i = 0; i < bodies.size(); ++i) {
            selection_->selectBody(bodies[i], i == 0 ? int(Qt::NoModifier) : int(Qt::ShiftModifier));
        }
    } else if (id == QLatin1String("selection.copyProperties") || id == QLatin1String("selection.copyCoordinates")) {
        const bool coordinates = id == QLatin1String("selection.copyCoordinates");
        const QString position = QCoreApplication::translate("ray::ui::SelectionModel", "Position");
        QStringList lines;
        if (!coordinates) {
            lines << selection_->infoTitle();
        }
        for (const QVariant& row : selection_->info()) {
            const QVariantMap m = row.toMap();
            const QString label = m.value(QStringLiteral("label")).toString();
            const QString value = m.value(QStringLiteral("value")).toString();
            if (!coordinates) {
                lines << QStringLiteral("%1: %2").arg(label, value);
            } else if (label == position) {
                lines << value;
            }
        }
        if (coordinates && lines.isEmpty()) {
            const Vec3 p = selection_->entries().front().hit.point;
            lines << QStringLiteral("%1, %2, %3")
                         .arg(QString::number(p.x, 'f', 3), QString::number(p.y, 'f', 3), QString::number(p.z, 'f', 3));
        }
        if (QClipboard* clipboard = QGuiApplication::clipboard()) {
            clipboard->setText(lines.join(QLatin1Char('\n')));
        }
        context_->showMessage(coordinates ? trc("Copied the coordinates %1").arg(lines.join(QString()))
                                          : trc("Copied the properties of the selected element"));
    } else {
        return false;
    }
    return true;
}

QVariantList CommandCatalog::contextEntries() const
{
    unsigned kinds = 0;
    if (selection_ && selection_->count() > 0) {
        for (const SelectionEntry& e : selection_->entries()) {
            kinds |= scopeOf(e.hit.kind);
        }
    } else {
        kinds = kNothing;
    }
    QVariantList out;
    int lastGroup = -1;
    std::set<QString> seen;
    for (std::size_t i = 0; i < std::size(kContextMenu); ++i) {
        const ContextEntry& e = kContextMenu[i];
        const QString id = QString::fromLatin1(e.id);
        if ((e.scope & kinds) == 0 || seen.count(id)) {
            continue;
        }
        const auto c = find(id);
        if (!c) {
            continue;
        }
        QVariantMap item = entry(*c);
        if (e.hideWhenDisabled && !item.value(QStringLiteral("enabled")).toBool()) {
            continue;
        }
        seen.insert(id);
        if (lastGroup >= 0 && e.group != lastGroup) {
            out << QVariantMap{{QStringLiteral("kind"), QStringLiteral("separator")}};
        }
        lastGroup = e.group;
        if (e.submenu == nullptr) {
            out << item;
            continue;
        }
        // Gather the following entries of the same submenu.
        QVariantList items{item};
        while (i + 1 < std::size(kContextMenu) && kContextMenu[i + 1].submenu == e.submenu) {
            const ContextEntry& next = kContextMenu[++i];
            if (const auto nc = find(QString::fromLatin1(next.id))) {
                items << entry(*nc);
                seen.insert(nc->id);
            }
        }
        out << QVariantMap{{QStringLiteral("kind"), QStringLiteral("menu")},
                           {QStringLiteral("title"), trc(e.submenu)},
                           {QStringLiteral("items"), items}};
    }
    return out;
}

// ---------- right-click watching ----------
void CommandCatalog::attachWindow(QQuickWindow* window)
{
    if (window_) {
        window_->removeEventFilter(this);
    }
    window_ = window;
    if (window_) {
        window_->installEventFilter(this);
    }
}

bool CommandCatalog::eventFilter(QObject* watched, QEvent* event)
{
    if (watched != window_) {
        return QObject::eventFilter(watched, event);
    }
    // Only looks; every event goes on to the items unchanged.
    switch (event->type()) {
    case QEvent::MouseButtonPress: {
        auto* e = static_cast<QMouseEvent*>(event);
        rightDown_ = false;
        if (e->button() != Qt::RightButton) {
            break;
        }
        // Not on an open popup (a menu, the palette): those handle their own clicks.
        const QPointF at = e->scenePosition();
        bool onPopup = false;
        for (QQuickItem* child : window_->contentItem()->childItems()) {
            if (!child->inherits("QQuickOverlay")) {
                continue;
            }
            for (QQuickItem* popup : child->childItems()) {
                onPopup = onPopup || (popup->isVisible() && popup->contains(popup->mapFromScene(at)));
            }
        }
        rightDown_ = !onPopup;
        rightAt_ = at;
        break;
    }
    case QEvent::MouseMove: {
        auto* e = static_cast<QMouseEvent*>(event);
        if (rightDown_ &&
            (e->scenePosition() - rightAt_).manhattanLength() >= QGuiApplication::styleHints()->startDragDistance()) {
            rightDown_ = false; // a drag (pan, orbit): no menu
        }
        break;
    }
    case QEvent::MouseButtonRelease: {
        auto* e = static_cast<QMouseEvent*>(event);
        if (e->button() == Qt::RightButton && rightDown_) {
            rightDown_ = false;
            const QPointF at = rightAt_;
            // After the release has been delivered.
            QMetaObject::invokeMethod(this, [this, at] { emit contextMenuRequested(at); }, Qt::QueuedConnection);
        }
        break;
    }
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}

void CommandCatalog::setSectionView(SectionModel* section)
{
    section_ = section;
}

void CommandCatalog::openPalette(const QString& text)
{
    emit paletteRequested(text);
}

void CommandCatalog::openShortcutSettings()
{
    emit shortcutSettingsRequested();
}

void CommandCatalog::openSettings()
{
    emit settingsRequested();
}

// ---------- use statistics ----------
void CommandCatalog::loadUsage() const
{
    if (usageLoaded_) {
        return;
    }
    usageLoaded_ = true;
    usage_.clear();
    const QString dir = userDataDir();
    if (dir.isEmpty()) {
        return;
    }
    QFile file(QDir(dir).filePath(QStringLiteral("palette-usage.json")));
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }
    const QJsonObject commands =
        QJsonDocument::fromJson(file.readAll()).object().value(QLatin1String("commands")).toObject();
    for (auto it = commands.begin(); it != commands.end(); ++it) {
        const QJsonObject o = it.value().toObject();
        usage_[it.key()] =
            Usage{o.value(QLatin1String("count")).toInt(), qint64(o.value(QLatin1String("lastMs")).toDouble())};
    }
}

void CommandCatalog::saveUsage() const
{
    const QString dir = userDataDir();
    if (dir.isEmpty() || !QDir().mkpath(dir)) {
        return;
    }
    QJsonObject commands;
    for (const auto& [id, u] : usage_) {
        commands.insert(id,
                        QJsonObject{{QStringLiteral("count"), u.count}, {QStringLiteral("lastMs"), double(u.lastMs)}});
    }
    const QJsonObject root{{QStringLiteral("format"), QStringLiteral("raycad.palette-usage")},
                           {QStringLiteral("version"), 1},
                           {QStringLiteral("commands"), commands}};
    QSaveFile file(QDir(dir).filePath(QStringLiteral("palette-usage.json")));
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        file.commit();
    }
}

void CommandCatalog::recordUse(const QString& id)
{
    // Opening the palette is how commands are found, not a command one looks for.
    if (id == QLatin1String("ui.commandPalette")) {
        return;
    }
    loadUsage();
    Usage& u = usage_[id];
    ++u.count;
    u.lastMs = QDateTime::currentMSecsSinceEpoch();
    saveUsage();
}

int CommandCatalog::useCount(const QString& id) const
{
    loadUsage();
    auto it = usage_.find(id);
    return it == usage_.end() ? 0 : it->second.count;
}

} // namespace ray::ui
