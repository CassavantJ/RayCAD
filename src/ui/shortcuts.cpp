#include "shortcuts.h"

#include "palette_commands.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>
#include <QtCore/QVariant>
#include <QtGui/QKeyEvent>
#include <QtGui/QKeySequence>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>

#include <algorithm>
#include <vector>

namespace ray::ui {
namespace {

constexpr const char* kEngineKey = "_ray_ui_shortcuts";
constexpr const char* kFileName = "shortcuts.json";
constexpr const char* kFormat = "raycad.shortcuts";

// "" for an empty or unreadable sequence; otherwise its canonical PortableText.
QString normalize(const QString& text)
{
    const QKeySequence seq = QKeySequence::fromString(text.trimmed(), QKeySequence::PortableText);
    return seq.isEmpty() || seq[0].key() == Qt::Key_unknown ? QString() : seq.toString(QKeySequence::PortableText);
}

QString native(const QString& portable)
{
    return QKeySequence::fromString(portable, QKeySequence::PortableText).toString(QKeySequence::NativeText);
}

} // namespace

ShortcutManager::ShortcutManager(CommandCatalog* commands, QObject* parent) : QObject(parent), commands_(commands)
{
    if (commands_) {
        connect(commands_, &CommandCatalog::commandsChanged, this, &ShortcutManager::reload);
    }
}

ShortcutManager::~ShortcutManager()
{
    if (window_) {
        window_->removeEventFilter(this);
    }
}

ShortcutManager* ShortcutManager::create(QQmlEngine* qml, QJSEngine*)
{
    if (qml != nullptr) {
        if (auto* shortcuts = qobject_cast<ShortcutManager*>(qml->property(kEngineKey).value<QObject*>())) {
            QJSEngine::setObjectOwnership(shortcuts, QJSEngine::CppOwnership);
            return shortcuts;
        }
    }
    return new ShortcutManager(nullptr); // owned by the engine
}

void ShortcutManager::provideFor(QQmlEngine* engine, ShortcutManager* shortcuts)
{
    engine->setProperty(kEngineKey, QVariant::fromValue<QObject*>(shortcuts));
}

void ShortcutManager::attach(QQuickWindow* window)
{
    if (window_) {
        window_->removeEventFilter(this);
    }
    window_ = window;
    if (window_) {
        window_->installEventFilter(this);
    }
}

void ShortcutManager::setRecording(bool on)
{
    if (on == recording_) {
        return;
    }
    recording_ = on;
    emit recordingChanged();
}

void ShortcutManager::reload()
{
    loaded_ = false;
    dirty_ = true;
    ++revision_;
    emit changed();
}

QString ShortcutManager::filePath() const
{
    ensure();
    return dir_.isEmpty() ? QString() : QDir(dir_).filePath(QLatin1String(kFileName));
}

// ---------- the map ----------
void ShortcutManager::ensure() const
{
    const QString dir = commands_ ? commands_->userDataDir() : QString();
    if (!loaded_ || dir != dir_) {
        loaded_ = true;
        dirty_ = true;
        dir_ = dir;
        overrides_.clear();
        if (!dir_.isEmpty()) {
            QFile file(QDir(dir_).filePath(QLatin1String(kFileName)));
            if (file.open(QIODevice::ReadOnly)) {
                const QJsonObject map =
                    QJsonDocument::fromJson(file.readAll()).object().value(QLatin1String("shortcuts")).toObject();
                for (auto it = map.begin(); it != map.end(); ++it) {
                    const QString text = it.value().toString();
                    const QString seq = normalize(text);
                    if (it.value().isString() && (text.trimmed().isEmpty() || !seq.isEmpty())) {
                        overrides_[it.key()] = seq;
                    }
                }
            }
        }
    }
    const std::size_t count = commands_ ? commands_->commands().size() : 0;
    if (dirty_ || count != knownCommands_) {
        rebuild();
    }
}

void ShortcutManager::rebuild() const
{
    commandsById_.clear();
    effective_.clear();
    byKey_.clear();
    conflicts_.clear();
    const std::vector<Command> commands = commands_ ? commands_->commands() : std::vector<Command>();
    for (const Command& c : commands) {
        Binding b{c.title, c.category, normalize(c.defaultShortcut)};
        auto o = overrides_.find(c.id);
        const QString seq = o != overrides_.end() ? o->second : b.defaultSequence;
        commandsById_[c.id] = std::move(b);
        if (!seq.isEmpty()) {
            effective_[c.id] = seq;
        }
    }
    for (const auto& [id, seq] : effective_) {
        auto taken = byKey_.find(seq);
        if (taken == byKey_.end()) {
            byKey_[seq] = id;
        } else {
            conflicts_ << tr("%1 is the shortcut of both %2 and %3")
                              .arg(native(seq), commandsById_[taken->second].title, commandsById_[id].title);
        }
    }
    knownCommands_ = commands.size();
    dirty_ = false;
}

bool ShortcutManager::save() const
{
    if (dir_.isEmpty()) {
        return true;
    }
    if (!QDir().mkpath(dir_)) {
        return false;
    }
    QJsonObject map;
    for (const auto& [id, seq] : overrides_) {
        map.insert(id, seq);
    }
    const QJsonObject root{{QStringLiteral("format"), QLatin1String(kFormat)},
                           {QStringLiteral("version"), 1},
                           {QStringLiteral("shortcuts"), map}};
    QSaveFile file(QDir(dir_).filePath(QLatin1String(kFileName)));
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return file.commit();
}

// ---------- queries ----------
QString ShortcutManager::sequence(const QString& id) const
{
    ensure();
    auto it = effective_.find(id);
    return it == effective_.end() ? QString() : it->second;
}

QString ShortcutManager::shortcutText(const QString& id) const
{
    return native(sequence(id));
}

QString ShortcutManager::defaultShortcutText(const QString& id) const
{
    ensure();
    auto it = commandsById_.find(id);
    return it == commandsById_.end() ? QString() : native(it->second.defaultSequence);
}

QString ShortcutManager::commandFor(const QString& seq) const
{
    ensure();
    auto it = byKey_.find(normalize(seq));
    return it == byKey_.end() ? QString() : it->second;
}

QStringList ShortcutManager::conflicts() const
{
    ensure();
    return conflicts_;
}

QVariantList ShortcutManager::rows(const QString& filter) const
{
    ensure();
    struct Row {
        QString id;
        const Binding* b;
        QString seq;
    };
    std::vector<Row> rows;
    const QString f = filter.trimmed();
    for (const auto& [id, b] : commandsById_) {
        auto e = effective_.find(id);
        const QString seq = e == effective_.end() ? QString() : e->second;
        if (!f.isEmpty() && !b.title.contains(f, Qt::CaseInsensitive) && !b.category.contains(f, Qt::CaseInsensitive) &&
            !id.contains(f, Qt::CaseInsensitive) && !native(seq).contains(f, Qt::CaseInsensitive)) {
            continue;
        }
        rows.push_back(Row{id, &b, seq});
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        if (a.b->category != b.b->category) {
            return a.b->category < b.b->category;
        }
        return a.b->title != b.b->title ? a.b->title < b.b->title : a.id < b.id;
    });
    QVariantList out;
    for (const Row& r : rows) {
        out << QVariantMap{{QStringLiteral("id"), r.id},
                           {QStringLiteral("title"), r.b->title},
                           {QStringLiteral("category"), r.b->category},
                           {QStringLiteral("shortcut"), native(r.seq)},
                           {QStringLiteral("sequence"), r.seq},
                           {QStringLiteral("isDefault"), r.seq == r.b->defaultSequence},
                           {QStringLiteral("defaultShortcut"), native(r.b->defaultSequence)}};
    }
    return out;
}

// ---------- remapping ----------
QVariantMap ShortcutManager::conflictResult(const QString& owner, const QString& seq) const
{
    const QString title = commandsById_.count(owner) ? commandsById_.at(owner).title : owner;
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("conflictId"), owner},
            {QStringLiteral("conflictTitle"), title},
            {QStringLiteral("sequence"), seq},
            {QStringLiteral("message"), tr("%1 is already the shortcut of %2").arg(native(seq), title)}};
}

QVariantMap ShortcutManager::assign(const QString& id, const QString& seq, bool replace)
{
    ensure();
    auto command = commandsById_.find(id);
    if (command == commandsById_.end()) {
        return {{QStringLiteral("ok"), false}, {QStringLiteral("message"), tr("There is no command %1").arg(id)}};
    }
    const QString norm = normalize(seq);
    if (!seq.trimmed().isEmpty() && norm.isEmpty()) {
        return {{QStringLiteral("ok"), false}, {QStringLiteral("message"), tr("%1 is not a key").arg(seq)}};
    }
    if (!norm.isEmpty()) {
        std::vector<QString> owners;
        for (const auto& [other, s] : effective_) {
            if (s == norm && other != id) {
                owners.push_back(other);
            }
        }
        if (!owners.empty() && !replace) {
            return conflictResult(owners.front(), norm);
        }
        for (const QString& other : owners) {
            overrides_[other] = QString(); // the other command loses the key
        }
    }
    if (norm == command->second.defaultSequence) {
        overrides_.erase(id);
    } else {
        overrides_[id] = norm;
    }
    rebuild();
    const bool saved = save();
    ++revision_;
    emit changed();
    QVariantMap out{{QStringLiteral("ok"), true}, {QStringLiteral("sequence"), norm}};
    if (!saved) {
        out.insert(QStringLiteral("message"), tr("The shortcuts could not be saved to %1").arg(filePath()));
    }
    return out;
}

QVariantMap ShortcutManager::reset(const QString& id, bool replace)
{
    ensure();
    auto command = commandsById_.find(id);
    if (command == commandsById_.end()) {
        return {{QStringLiteral("ok"), false}, {QStringLiteral("message"), tr("There is no command %1").arg(id)}};
    }
    return assign(id, command->second.defaultSequence, replace);
}

void ShortcutManager::resetAll()
{
    ensure();
    overrides_.clear();
    rebuild();
    save();
    ++revision_;
    emit changed();
}

QString ShortcutManager::sequenceFromKey(int key, int modifiers) const
{
    switch (key) {
    case 0:
    case Qt::Key_unknown:
    case Qt::Key_Shift:
    case Qt::Key_Control:
    case Qt::Key_Meta:
    case Qt::Key_Alt:
    case Qt::Key_AltGr:
    case Qt::Key_CapsLock:
    case Qt::Key_NumLock:
    case Qt::Key_ScrollLock:
    case Qt::Key_Super_L:
    case Qt::Key_Super_R:
        return {};
    default:
        break;
    }
    Qt::KeyboardModifiers mods = Qt::KeyboardModifiers(modifiers) &
                                 (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
    if (key == Qt::Key_Backtab) {
        key = Qt::Key_Tab;
        mods |= Qt::ShiftModifier;
    }
    return QKeySequence(QKeyCombination(mods, Qt::Key(key))).toString(QKeySequence::PortableText);
}

QString ShortcutManager::nativeText(const QString& seq) const
{
    return native(seq);
}

// ---------- keys ----------
bool ShortcutManager::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::KeyPress && watched == window_ && handleKey(static_cast<QKeyEvent*>(event))) {
        return true;
    }
    return QObject::eventFilter(watched, event);
}

bool ShortcutManager::handleKey(QKeyEvent* event)
{
    if (recording_) {
        return false;
    }
    const QString seq = sequenceFromKey(event->key(), int(event->modifiers()));
    if (seq.isEmpty()) {
        return false;
    }
    ensure();
    auto bound = byKey_.find(seq);
    if (bound == byKey_.end()) {
        return false;
    }
    // Typing in a text field is never a shortcut.
    QQuickItem* focus = window_ ? window_->activeFocusItem() : nullptr;
    if (focus != nullptr && (focus->inherits("QQuickTextInput") || focus->inherits("QQuickTextEdit"))) {
        return false;
    }
    // An item that uses the key itself says so the way Qt's shortcuts ask: it accepts the ShortcutOverride.
    if (focus != nullptr) {
        QKeyEvent override(QEvent::ShortcutOverride, event->key(), event->modifiers(), event->text(),
                           event->isAutoRepeat(), event->count());
        override.ignore();
        QCoreApplication::sendEvent(focus, &override);
        if (override.isAccepted()) {
            return false;
        }
    }
    if (event->isAutoRepeat()) {
        return true; // a held key runs the command once
    }
    const QString id = bound->second;
    emit triggered(id, seq);
    if (commands_) {
        commands_->trigger(id);
    }
    return true;
}

} // namespace ray::ui
