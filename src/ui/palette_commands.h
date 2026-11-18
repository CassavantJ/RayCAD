// The `Commands` QML singleton (task T-021): every command the keyboard, the command palette and the context menus
// can run, and the one place that runs them.
//
// A command is either an action of the session's api::IActionRegistry (file.save, edit.undo, ...), or a view
// command of the UI itself (view.fitAll, selection.clear, ui.commandPalette, ...: kUiCommands in the .cpp) for which
// no action exists. When the registry has an action with a view command's id, the action wins.
//
// Running (run / trigger): file actions go to Files first (AppContext::handleAction, T-019: native dialogs for a
// path, the unsaved-changes prompt); other registry actions go through IActionRegistry::invoke as Caller::User, with
// results and errors in the shell's message banner (AppContext::showMessage). Long-running ones are IApp jobs, shown
// and cancelled through the jobs panel and the status bar's slot (Session). A command that cannot run now says why
// (disabledReason): no session, no document open, the document is read-only, nothing selected, nothing to undo.
// trigger() asks for the argument form (formRequested) when the action has required arguments.
//
// Context menus (contextEntries): which commands apply to the current selection comes from kContextMenu in the .cpp,
// a table keyed by command id with the selection kinds each entry applies to (nothing selected, face, edge, vertex,
// body). A proper api field for it is described there.
//
// Use counts for "recently used first" are kept per user in <IApp::userDataDir()>/palette-usage.json.
#pragma once

#include "app_context.h"

#include <ray/api.h>

#include <QtCore/QObject>
#include <QtCore/QPointF>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtCore/QUrl>
#include <QtCore/QVariantList>
#include <QtCore/QVariantMap>
#include <QtQml/QQmlEngine>
#include <QtQuick/QQuickWindow>

#include <functional>
#include <map>
#include <optional>
#include <vector>

namespace ray::ui {

class SceneController;
class SelectionModel;
class ShortcutManager;
class ViewportItem;

struct Command {
    QString id, title, category, description;
    QString schema;          // JSON Schema of the arguments ("" for view commands)
    QString defaultShortcut; // QKeySequence::PortableText, "" for none
    bool fromRegistry = false;
    bool longRunning = false;
    bool changesDocument = false;
};

class CommandCatalog : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(Commands)
    QML_SINGLETON

    // Changes when the session changes (the set of commands may differ); QML re-reads what it shows.
    Q_PROPERTY(int revision READ revision NOTIFY commandsChanged)

public:
    using ViewportProvider = std::function<ViewportItem*()>;

    // All may be null except `context`; they must outlive this object.
    CommandCatalog(AppContext* context, SceneController* scene, SelectionModel* selection, ViewportProvider viewport,
                   QObject* parent = nullptr);
    ~CommandCatalog() override;

    static CommandCatalog* create(QQmlEngine* qml, QJSEngine* js);
    static void provideFor(QQmlEngine* engine, CommandCatalog* commands);

    // Watches the window's right button (without taking any event from the items): a press and release within the
    // drag distance, not on a popup, emits contextMenuRequested. Right drags (pan, orbit) are left alone.
    void attachWindow(QQuickWindow* window);
    void setShortcuts(ShortcutManager* shortcuts) { shortcuts_ = shortcuts; }
    ShortcutManager* shortcuts() const { return shortcuts_; }
    AppContext* context() const { return context_; }

    // ---- the commands ----
    std::vector<Command> commands() const; // registry actions and view commands, by id
    std::optional<Command> find(const QString& id) const;
    // Best match first: the registry's ranking (IActionRegistry::search) with view commands merged in by how well
    // their title matches. Deterministic.
    std::vector<Command> search(const QString& query) const;
    // Most used first (then most recent, then id), at most `limit`.
    std::vector<Command> mostUsed(int limit) const;
    // Why the command cannot run now; "" when it can.
    QString disabledReason(const Command& command) const;
    bool needsForm(const Command& command) const;
    // The user data folder of the session ("" without one: nothing is persisted).
    QString userDataDir() const;

    // ---- QML ----
    // { id, title, category, description, shortcut (native text), enabled, reason, needsForm }; empty if unknown.
    Q_INVOKABLE QVariantMap info(const QString& id) const;
    Q_INVOKABLE QVariantList formFields(const QString& id) const;
    // { ok, json, error, field } (see palette::buildArgs).
    Q_INVOKABLE QVariantMap buildArgs(const QString& id, const QVariantMap& values) const;
    // Keyboard shortcut, palette Enter, menu: runs the command, or asks for its argument form. False if it cannot run
    // now (the reason is shown in the banner).
    Q_INVOKABLE bool trigger(const QString& id);
    // Runs with these arguments (a JSON object). False if it did not start.
    Q_INVOKABLE bool run(const QString& id, const QString& argsJson = QStringLiteral("{}"));
    // The context menu for the current selection: a list of { kind: "item" | "separator" | "menu", id, title,
    // shortcut, enabled, reason, marked, items (kind "menu") }.
    Q_INVOKABLE QVariantList contextEntries() const;
    Q_INVOKABLE void openPalette(const QString& text = QString());
    Q_INVOKABLE void openShortcutSettings();
    Q_INVOKABLE void openSettings(); // T-024: panels/Settings.qml (ui.settings, Ctrl+,)
    Q_INVOKABLE QString localPath(const QUrl& url) const { return url.toLocalFile(); }
    Q_INVOKABLE QUrl fileUrl(const QString& path) const { return QUrl::fromLocalFile(path); }

    int revision() const { return revision_; }

    // Use statistics (persisted). recordUse is called by run().
    void recordUse(const QString& id);
    int useCount(const QString& id) const;

signals:
    void commandsChanged();
    void paletteRequested(const QString& text);
    void formRequested(const QString& id);
    void shortcutSettingsRequested();
    void settingsRequested();
    // A command finished (or failed to start): what the banner showed.
    void finished(const QString& id, bool ok, const QString& message);
    // A right-click (press and release without dragging) at this window point; ContextMenu.qml opens there.
    void contextMenuRequested(const QPointF& scenePos);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Usage {
        int count = 0;
        qint64 lastMs = 0;
    };
    bool runUi(const QString& id);
    void finishLater(const QString& id); // `finished` for a command Files ran, posted
    QString uiReason(const QString& id) const;
    void loadUsage() const;
    void saveUsage() const;
    void sessionChanged();
    QString shortcutText(const QString& id) const;
    QVariantMap entry(const Command& command) const;

    QPointer<AppContext> context_;
    QPointer<SceneController> scene_;
    QPointer<SelectionModel> selection_;
    ViewportProvider viewport_;
    ShortcutManager* shortcuts_ = nullptr;
    QPointer<QQuickWindow> window_;
    bool rightDown_ = false;
    QPointF rightAt_;
    int revision_ = 0;
    mutable bool usageLoaded_ = false;
    mutable std::map<QString, Usage> usage_;
};

} // namespace ray::ui
