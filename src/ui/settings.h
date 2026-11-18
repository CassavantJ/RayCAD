// The `AppSettings` QML singleton (task T-024): the user settings (ray_api/settings.h) as the desktop app sees and
// applies them. (The settings panel is panels/Settings.qml, so the QML type is named AppSettings.)
//
// What it applies, and when:
//   - theme mode (Theme.mode) and glass (AppContext.glass): at start, unless the command line set them (--theme,
//     --glass: those hold for the session until the user changes the setting); a change made in the UI is saved.
//   - graphics: the tier (auto: from hardware detection; or forced) with the per-option overrides on top, as the
//     scene's preset (SceneController::setPreset, which passes it to the viewport at once: no restart). Glass with no
//     explicit setting follows the preset's liquidGlass.
//   - units: the decimals and the default unit system go to the Units singleton (units.h).
//   - navigation preset: the viewport item's mouse mapping (attachView); a change there is saved.
//   - panels: Shell.qml reads panelOpen / panelWidth and reports changes; the window geometry is AppHost's.
// "At start" means AppHost::loadSettings (runApp calls it before the window is created). Without it (tests that
// build their own windows) nothing is applied until a setting is changed, so their own theme, glass and tier stand.
//
// Hardware detection (render::detectHardware, up to a few hundred ms) runs on a worker of the job system, never
// before the first frame: AppHost::startSession asks for it, and so does the panel's Graphics section. Until it
// ends, hardwareState is "detecting" and the tier in effect is the one the last detection chose (stored in the
// settings), else Low.
#pragma once

#include "scene_jobs.h"

#include <ray/render.h>
#include <ray_api/settings.h>

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtCore/QVariantMap>
#include <QtQml/QQmlEngine>

#include <map>
#include <memory>
#include <optional>
#include <string>

namespace ray::api {
class IApp;
}

namespace ray::ui {

class AppContext;
class SceneController;
class ThemeObject;
class UnitsObject;
class ViewportItem;

// Why chooseTier picked its tier, in plain words (PLAN.md 6.1). render.h has no such function; this one mirrors
// chooseTier's rules and thresholds (render/tier.cpp), and tier is what chooseTier itself returns.
struct TierExplanation {
    render::Tier tier = render::Tier::Low;
    QString reason;
};
TierExplanation explainTier(const render::HardwareInfo& hw);

// The preset of `tier` with the overrides applied; glass is the explicit setting, else the tier's liquidGlass.
render::TierPreset composePreset(render::Tier tier, const api::PresetOverrides& overrides, std::optional<bool> glass);

class SettingsObject : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(AppSettings)
    QML_SINGLETON

    // Every setting's value in effect by key ("units.decimalsInch" -> 4); an override that is not set is undefined.
    Q_PROPERTY(QVariantMap values READ values NOTIFY changed)
    // The keys the user set (the rest have their defaults).
    Q_PROPERTY(QStringList setKeys READ setKeys NOTIFY changed)
    Q_PROPERTY(int revision READ revision NOTIFY changed)
    Q_PROPERTY(QString filePath READ filePath NOTIFY storeChanged)
    // The settings file could not be used at start (shown once in the banner too).
    Q_PROPERTY(QString loadProblem READ loadProblem NOTIFY storeChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY changed)

    // ---- graphics ----
    // "idle" (not asked for yet), "detecting" or "ready".
    Q_PROPERTY(QString hardwareState READ hardwareState NOTIFY hardwareChanged)
    // {gpuName, gpuVendor, gpuType, vram, ram, cores, graphicsApi, driver, computeShaders} as text.
    Q_PROPERTY(QVariantMap hardware READ hardware NOTIFY hardwareChanged)
    Q_PROPERTY(QString detectedTier READ detectedTier NOTIFY hardwareChanged) // "low", "medium", "high"; "" until known
    Q_PROPERTY(QString tierReason READ tierReason NOTIFY hardwareChanged)
    // The tier in effect ("low", "medium", "high") and whether it was forced.
    Q_PROPERTY(QString effectiveTier READ effectiveTier NOTIFY presetChanged)
    Q_PROPERTY(bool tierForced READ tierForced NOTIFY presetChanged)
    // The base tier's preset ("tier defaults") and the preset in effect, by override key ("graphics.msaaSamples",
    // ..., and "appearance.glass").
    Q_PROPERTY(QVariantMap tierDefaults READ tierDefaults NOTIFY presetChanged)
    Q_PROPERTY(QVariantMap effectivePreset READ effectivePreset NOTIFY presetChanged)
    // ---- navigation ----
    Q_PROPERTY(QStringList navigationPresetIds READ navigationPresetIds CONSTANT)
    Q_PROPERTY(QStringList navigationPresetNames READ navigationPresetNames CONSTANT)
    Q_PROPERTY(QStringList navigationPresetDescriptions READ navigationPresetDescriptions CONSTANT)
    Q_PROPERTY(int navigationPresetIndex READ navigationPresetIndex NOTIFY changed)

public:
    SettingsObject(ThemeObject* theme, AppContext* context, SceneController* scene, UnitsObject* units,
                   JobsProvider jobs, QObject* parent = nullptr);
    ~SettingsObject() override;

    static SettingsObject* create(QQmlEngine* qml, QJSEngine* js);
    static void provideFor(QQmlEngine* engine, SettingsObject* settings);

    // ---- C++ (AppHost) ----
    // Opens the store of a user data folder (nullopt: $RAYCAD_USER_DATA_DIR or the per-user folder) and applies
    // what it holds (see above). `themeForced` / `glassForced`: the command line set them; they are left alone.
    void load(const std::optional<std::string>& userDataDir, bool themeForced, bool glassForced);
    bool loaded() const { return applyAll_; }
    // The session's store (settingsOf(app)); same folder as load() in the real app. Tests that did not load() get
    // the session's store for the panel.
    void setApp(api::IApp* app);
    std::shared_ptr<api::SettingsStore> store() const { return store_; }
    api::UserSettings current() const;
    // The viewport item whose navigation preset follows the setting (null detaches).
    void attachView(ViewportItem* view);
    // Starts hardware detection on a worker unless it ran or runs (deferred: call after the first frame).
    Q_INVOKABLE void requestHardware();
    std::optional<render::HardwareInfo> detectedHardware() const { return hw_; }
    // Sets the detection result directly (tests); applies it like a finished detection.
    void setDetectedHardware(const render::HardwareInfo& hw);
    render::TierPreset preset() const { return preset_; }
    // Window geometry: AppHost restores and saves it (`now` false: after a short pause, as it moves).
    void saveWindow(int x, int y, int width, int height, bool maximized, bool now = true);
    // Writes what waits for the pause now.
    void flush() { flushPending(); }

    // ---- QML ----
    QVariantMap values() const { return values_; }
    QStringList setKeys() const { return setKeys_; }
    int revision() const { return revision_; }
    QString filePath() const;
    QString loadProblem() const;
    QString lastError() const { return lastError_; }
    QString hardwareState() const;
    QVariantMap hardware() const;
    QString detectedTier() const;
    QString tierReason() const;
    QString effectiveTier() const;
    bool tierForced() const;
    QVariantMap tierDefaults() const;
    QVariantMap effectivePreset() const;
    QStringList navigationPresetIds() const;
    QStringList navigationPresetNames() const;
    QStringList navigationPresetDescriptions() const;
    int navigationPresetIndex() const;

    // Sets a key (value as QML has it: string, number, bool; undefined or null resets it). False and lastError
    // when the value is refused.
    Q_INVOKABLE bool set(const QString& key, const QVariant& value);
    Q_INVOKABLE bool reset(const QString& keyOrPrefix);
    Q_INVOKABLE bool isSet(const QString& key) const { return setKeys_.contains(key); }
    Q_INVOKABLE QVariant value(const QString& key) const { return values_.value(key); }
    Q_INVOKABLE QString title(const QString& key) const;
    Q_INVOKABLE QString description(const QString& key) const;
    // Panels of the shell ("model", "properties").
    Q_INVOKABLE bool panelOpen(const QString& panel) const;
    Q_INVOKABLE void setPanelOpen(const QString& panel, bool open);
    // fallback when no width was saved; width < 0 forgets the saved width. Saved after a short pause.
    Q_INVOKABLE qreal panelWidth(const QString& panel, qreal fallback) const;
    Q_INVOKABLE void setPanelWidth(const QString& panel, qreal width);
    // Opens the settings panel (ui.settings).
    Q_INVOKABLE void open();

signals:
    void changed();
    void storeChanged();
    void hardwareChanged();
    void presetChanged();
    void openRequested();

private:
    void bind(std::shared_ptr<api::SettingsStore> store);
    void onStoreChanged(const api::UserSettings& s, const std::vector<std::string>& keys);
    void refreshValues();
    void applyTheme(const api::UserSettings& s);
    void applyGraphics(const api::UserSettings& s, bool glassChanged = false);
    void applyUnits(const api::UserSettings& s);
    void applyNavigation(const api::UserSettings& s);
    render::Tier baseTier(const api::UserSettings& s) const;
    void flushPending();
    bool writeJson(const std::string& key, const std::string& valueJson);

    QPointer<ThemeObject> theme_;
    QPointer<AppContext> context_;
    QPointer<SceneController> scene_;
    QPointer<UnitsObject> units_;
    QPointer<ViewportItem> view_;
    JobsProvider jobs_;
    std::shared_ptr<api::SettingsStore> store_;
    int listener_ = 0;
    bool applyAll_ = false;       // load() ran: apply everything at start
    bool graphicsActive_ = false; // the preset is ours to set (load() ran, or a graphics setting changed)
    bool themeForced_ = false, glassForced_ = false;
    int applying_ = 0; // > 0 while this object changes the theme, glass or view itself
    QVariantMap values_;
    QStringList setKeys_;
    int revision_ = 0;
    QString lastError_;
    // Hardware detection.
    std::optional<render::HardwareInfo> hw_;
    bool detecting_ = false;
    std::uint64_t detectSerial_ = 0;
    render::TierPreset preset_;
    bool presetApplied_ = false;
    // Values saved after a pause (panel widths dragged with the splitter, window geometry).
    std::map<std::string, std::string> pending_;
    QTimer pendingTimer_;
    QMetaObject::Connection viewConnection_;
    QList<QMetaObject::Connection> connections_;
};

} // namespace ray::ui
