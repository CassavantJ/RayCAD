// The `Shortcuts` QML singleton (task T-021): the one keyboard-shortcut manager. It binds every command of the
// catalog (palette_commands.h) to its key: the default each command declares where it is registered
// (ActionInfo::defaultShortcut for registry actions, UiCommand::shortcut for the UI's own, ui_commands.h) unless the
// user's map says otherwise.
//
//   Contexts      Every command's key works in a context (ui_commands.h): Global everywhere, Model while no sketch
//                 is edited, Sketch while one is. One key may serve one Model and one Sketch command; any other
//                 sharing (two in one context, or one of them Global) is a conflict. The context now is
//                 CommandCatalog::activeContext().
//   Keys          A key press in the main window that matches a bound key runs the command (CommandCatalog::trigger),
//                 unless a text field (TextInput, TextEdit) has the focus, the focused item claims the key itself
//                 (it accepts the ShortcutOverride, as Qt's own shortcuts allow), or a key is being recorded.
//                 Sequences are written in QKeySequence::PortableText ("Ctrl+S"); Ctrl is Cmd on macOS. One key
//                 combination per command; multi-chord sequences are not used.
//   Remapping     assign() reports a conflict (the command that already owns the key) instead of taking the key,
//                 unless asked to replace it, which unbinds the other command. reset() and resetAll() go back to
//                 the defaults.
//   Persistence   Only the differences from the defaults are stored, per user, in
//                 <IApp::userDataDir()>/shortcuts.json: {"format":"raycad.shortcuts","version":1,
//                 "shortcuts":{"<command id>":"<PortableText>" or "" for no shortcut}}. Without a session (or with
//                 an empty user data folder) changes last until the app closes.
// Nothing is read before it is needed: the map is loaded on the first key press or query.
#pragma once

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVariantList>
#include <QtCore/QVariantMap>
#include <QtQml/QQmlEngine>

#include "ui_commands.h"

#include <map>
#include <vector>

class QKeyEvent;
class QQuickWindow;

namespace ray::ui {

class CommandCatalog;

class ShortcutManager : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(Shortcuts)
    QML_SINGLETON

    // Changes whenever a binding changes; QML re-reads rows() and shortcut texts.
    Q_PROPERTY(int revision READ revision NOTIFY changed)
    // While true no shortcut fires (the settings panel is recording a key).
    Q_PROPERTY(bool recording READ recording WRITE setRecording NOTIFY recordingChanged)
    // Where the map is stored ("" when nothing is persisted).
    Q_PROPERTY(QString filePath READ filePath NOTIFY changed)

public:
    explicit ShortcutManager(CommandCatalog* commands, QObject* parent = nullptr);
    ~ShortcutManager() override;

    static ShortcutManager* create(QQmlEngine* qml, QJSEngine* js);
    static void provideFor(QQmlEngine* engine, ShortcutManager* shortcuts);

    // Listens to the window's key presses.
    void attach(QQuickWindow* window);

    int revision() const { return revision_; }
    bool recording() const { return recording_; }
    void setRecording(bool on);
    QString filePath() const;
    bool loaded() const { return loaded_; }

    // The command's key now (PortableText; "" for none) and as the platform shows it.
    QString sequence(const QString& id) const;
    Q_INVOKABLE QString shortcutText(const QString& id) const;
    Q_INVOKABLE QString defaultShortcutText(const QString& id) const;
    // The command a key runs now, in the active context ("" for none).
    Q_INVOKABLE QString commandFor(const QString& sequence) const;
    // Keys two commands are bound to at once in one context (from an edited file, or clashing defaults): one line
    // each.
    Q_INVOKABLE QStringList conflicts() const;

    // The settings panel's rows, filtered by title, category, id or key text (case-insensitive), sorted by category
    // then title: { id, title, category, context ("" everywhere, "Model", "Sketch"; translated), shortcut (native),
    // sequence (portable), isDefault, defaultShortcut }.
    Q_INVOKABLE QVariantList rows(const QString& filter = QString()) const;

    // Binds `sequence` (PortableText, "" to unbind) to the command. Result: { ok, conflictId, conflictTitle, message }.
    // A key owned by another command of an overlapping context is a conflict: nothing changes unless `replace`,
    // which unbinds the other one.
    Q_INVOKABLE QVariantMap assign(const QString& id, const QString& sequence, bool replace = false);
    Q_INVOKABLE QVariantMap reset(const QString& id, bool replace = false);
    Q_INVOKABLE void resetAll();

    // The PortableText of a key press (QML Keys event key and modifiers); "" for a lone modifier key.
    Q_INVOKABLE QString sequenceFromKey(int key, int modifiers) const;
    Q_INVOKABLE QString nativeText(const QString& sequence) const;

    // Re-reads the user's map (the session changed).
    void reload();

signals:
    void changed();
    void recordingChanged();
    // A shortcut ran a command.
    void triggered(const QString& id, const QString& sequence);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Binding {
        QString title, category, defaultSequence;
        CommandContext context = CommandContext::Global;
    };
    void ensure() const;
    void rebuild() const;
    bool save() const;
    bool handleKey(QKeyEvent* event);
    QVariantMap conflictResult(const QString& owner, const QString& sequence) const;

    QPointer<CommandCatalog> commands_;
    QPointer<QQuickWindow> window_;
    int revision_ = 0;
    bool recording_ = false;
    mutable bool loaded_ = false;
    mutable bool dirty_ = true;
    mutable std::size_t knownCommands_ = 0;
    mutable QString dir_;
    mutable std::map<QString, QString> overrides_; // id -> PortableText ("" = no shortcut)
    mutable std::map<QString, Binding> commandsById_;
    mutable std::map<QString, QString> effective_;          // id -> PortableText, bound commands only
    mutable std::map<QString, std::vector<QString>> byKey_; // PortableText -> ids bound to it, by id
    mutable QStringList conflicts_;
};

} // namespace ray::ui
