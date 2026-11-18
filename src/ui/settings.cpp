#include "settings.h"

#include "app_context.h"
#include "queued_call.h"
#include "scene_controller.h"
#include "theme_object.h"
#include "units.h"
#include "viewport_item.h"
#include "viewport_navigation.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QThread>

#include <algorithm>
#include <cstdint>

namespace ray::ui {
namespace {

constexpr const char* kEngineKey = "_ray_ui_settings";
constexpr int kPendingDelayMs = 400;

// chooseTier's thresholds (render/tier.cpp, PLAN.md 6.1), with the same allowance for memory the firmware and an
// integrated GPU keep back.
constexpr std::uint64_t kGiB = std::uint64_t{1} << 30;
constexpr std::uint64_t kRamSlack = kGiB;
constexpr std::uint64_t kVramSlack = kGiB / 2;

bool atLeast(std::uint64_t value, std::uint64_t threshold, std::uint64_t slack)
{
    return value + slack >= threshold;
}

bool softwareName(const std::string& name)
{
    QString n = QString::fromStdString(name).toLower();
    for (const char* s : {"warp", "basic render", "llvmpipe", "softpipe", "swiftshader"}) {
        if (n.contains(QLatin1String(s))) {
            return true;
        }
    }
    return false;
}

QString gigabytes(std::uint64_t bytes)
{
    const double gb = double(bytes) / double(kGiB);
    return QCoreApplication::translate("ray::ui::SettingsObject", "%1 GB")
        .arg(QString::number(gb, 'f', gb >= 10 || gb == double(std::uint64_t(gb)) ? 0 : 1));
}

QString tr(const char* text)
{
    return QCoreApplication::translate("ray::ui::SettingsObject", text);
}

QString tierKey(render::Tier t)
{
    switch (t) {
    case render::Tier::Low:
        return QStringLiteral("low");
    case render::Tier::Medium:
        return QStringLiteral("medium");
    case render::Tier::High:
        return QStringLiteral("high");
    }
    return QStringLiteral("low");
}

QString tierTitle(render::Tier t)
{
    switch (t) {
    case render::Tier::Low:
        return tr("Low");
    case render::Tier::Medium:
        return tr("Medium");
    case render::Tier::High:
        return tr("High");
    }
    return tr("Low");
}

std::optional<render::Tier> tierOf(api::TierChoice c)
{
    switch (c) {
    case api::TierChoice::Low:
        return render::Tier::Low;
    case api::TierChoice::Medium:
        return render::Tier::Medium;
    case api::TierChoice::High:
        return render::Tier::High;
    case api::TierChoice::Auto:
        break;
    }
    return std::nullopt;
}

api::TierChoice choiceOf(render::Tier t)
{
    switch (t) {
    case render::Tier::Low:
        return api::TierChoice::Low;
    case render::Tier::Medium:
        return api::TierChoice::Medium;
    case render::Tier::High:
        return api::TierChoice::High;
    }
    return api::TierChoice::Low;
}

// The theme mode as the settings store it: "system", "dark" or "light" (a theme id maps to its kind).
QString settingFromMode(const ThemeObject& theme)
{
    const QString mode = theme.mode();
    if (mode == QLatin1String("system") || mode == QLatin1String("dark") || mode == QLatin1String("light")) {
        return mode;
    }
    return theme.dark() ? QStringLiteral("dark") : QStringLiteral("light");
}

QString jsonText(const QVariant& value)
{
    if (!value.isValid() || value.isNull()) {
        return QStringLiteral("null");
    }
    const QByteArray array = QJsonDocument(QJsonArray{QJsonValue::fromVariant(value)}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(array.mid(1, array.size() - 2));
}

QVariantMap presetMap(const render::TierPreset& p)
{
    return QVariantMap{
        {QStringLiteral("graphics.msaaSamples"), p.msaaSamples},
        {QStringLiteral("graphics.smoothEdges"), p.smoothEdges},
        {QStringLiteral("graphics.transparency"), p.transparency},
        {QStringLiteral("graphics.sectionCaps"), p.sectionCaps},
        {QStringLiteral("graphics.ambientOcclusion"), p.ambientOcclusion},
        {QStringLiteral("graphics.shadows"), p.shadows},
        {QStringLiteral("graphics.meshChordTolMm"), p.meshChordTolMm},
        {QStringLiteral("graphics.meshAngleTolDeg"), p.meshAngleTolDeg},
        {QStringLiteral("graphics.simCellMm"), p.simCellMm},
        {QStringLiteral("graphics.maxBackplotSegments"), p.maxBackplotSegments},
        {QStringLiteral("appearance.glass"), p.liquidGlass},
    };
}

bool isUserGraphicsKey(const std::string& key)
{
    return key.rfind("graphics.", 0) == 0 && key != "graphics.lastDetectedTier" && key != "graphics.lastDriverVersion";
}

} // namespace

// ---------- Tier explanation and preset composition ----------
TierExplanation explainTier(const render::HardwareInfo& hw)
{
    TierExplanation out;
    out.tier = render::chooseTier(hw);
    const QString gpu = hw.gpuName.empty() ? tr("the graphics adapter") : QString::fromStdString(hw.gpuName);
    render::Tier mine = render::Tier::Medium;
    QString reason;
    if (hw.gpuType == render::GpuType::Software || softwareName(hw.gpuName)) {
        mine = render::Tier::Low;
        reason = tr("Low, because the 3D view is drawn in software (%1): there is no graphics hardware in use, so "
                    "anti-aliasing and glass are off.")
                     .arg(gpu);
    } else if (hw.gpuType == render::GpuType::Unknown) {
        mine = render::Tier::Low;
        reason = tr("Low, because the type of graphics adapter could not be detected, and RayCAD does not assume a "
                    "fast one it cannot see.");
    } else if (hw.gpuType == render::GpuType::Integrated) {
        mine = render::Tier::Low;
        reason = tr("Low, because %1 is integrated graphics, which shares the computer's memory.").arg(gpu);
    } else if (hw.vramBytes > 0 && !atLeast(hw.vramBytes, 3 * kGiB, kVramSlack)) {
        mine = render::Tier::Low;
        reason = tr("Low, because the graphics card has %1 of video memory; Medium needs 3 GB or more.")
                     .arg(gigabytes(hw.vramBytes));
    } else if (hw.ramBytes > 0 && !atLeast(hw.ramBytes, 12 * kGiB, kRamSlack)) {
        mine = render::Tier::Low;
        reason =
            tr("Low, because the computer has %1 of memory; Medium needs 12 GB or more.").arg(gigabytes(hw.ramBytes));
    } else {
        QStringList missing;
        if (hw.vramBytes == 0) {
            missing << tr("its video memory could not be read");
        } else if (!atLeast(hw.vramBytes, 8 * kGiB, kVramSlack)) {
            missing << tr("it has %1 of video memory (High needs 8 GB)").arg(gigabytes(hw.vramBytes));
        }
        if (hw.ramBytes == 0) {
            missing << tr("the computer's memory could not be read");
        } else if (!atLeast(hw.ramBytes, 24 * kGiB, kRamSlack)) {
            missing << tr("the computer has %1 of memory (High needs 24 GB)").arg(gigabytes(hw.ramBytes));
        }
        if (!hw.computeShaders) {
            missing << tr("compute shaders are not available");
        }
        if (missing.isEmpty()) {
            mine = render::Tier::High;
            reason = tr("High, because %1 is a dedicated graphics card with %2 of video memory, the computer has %3 "
                        "of memory, and compute shaders are available.")
                         .arg(gpu, gigabytes(hw.vramBytes), gigabytes(hw.ramBytes));
        } else {
            mine = render::Tier::Medium;
            reason = tr("Medium: %1 is a dedicated graphics card, but %2.").arg(gpu, missing.join(tr(", and ")));
        }
    }
    // chooseTier decides; should its rules ever move away from these, say only what it chose.
    out.reason = mine == out.tier ? reason
                                  : tr("%1, chosen from the detected graphics adapter, video memory, memory and "
                                       "graphics features.")
                                        .arg(tierTitle(out.tier));
    return out;
}

render::TierPreset composePreset(render::Tier tier, const api::PresetOverrides& o, std::optional<bool> glass)
{
    render::TierPreset p = render::presetFor(tier);
    if (o.msaaSamples)
        p.msaaSamples = *o.msaaSamples;
    if (o.smoothEdges)
        p.smoothEdges = *o.smoothEdges;
    if (o.transparency)
        p.transparency = *o.transparency;
    if (o.sectionCaps)
        p.sectionCaps = *o.sectionCaps;
    if (o.ambientOcclusion)
        p.ambientOcclusion = *o.ambientOcclusion;
    if (o.shadows)
        p.shadows = *o.shadows;
    if (o.meshChordTolMm)
        p.meshChordTolMm = *o.meshChordTolMm;
    if (o.meshAngleTolDeg)
        p.meshAngleTolDeg = *o.meshAngleTolDeg;
    if (o.simCellMm)
        p.simCellMm = *o.simCellMm;
    if (o.maxBackplotSegments)
        p.maxBackplotSegments = *o.maxBackplotSegments;
    if (glass)
        p.liquidGlass = *glass;
    return p;
}

// ---------- SettingsObject ----------
SettingsObject::SettingsObject(ThemeObject* theme, AppContext* context, SceneController* scene, UnitsObject* units,
                               JobsProvider jobs, QObject* parent)
    : QObject(parent), theme_(theme), context_(context), scene_(scene), units_(units), jobs_(std::move(jobs)),
      preset_(render::presetFor(render::Tier::Low))
{
    pendingTimer_.setSingleShot(true);
    pendingTimer_.setInterval(kPendingDelayMs);
    connect(&pendingTimer_, &QTimer::timeout, this, &SettingsObject::flushPending);
    // Until load() or a session: a store in memory (the panel works; nothing is written).
    bind(api::SettingsStore::open(std::string()));
    // Changes made outside the panel (the top bar's theme and glass buttons, the view menu) are saved too.
    if (theme_ != nullptr) {
        connections_ << connect(theme_, &ThemeObject::modeChanged, this, [this] {
            if (applying_ == 0 && store_)
                writeJson("appearance.theme", jsonText(settingFromMode(*theme_)).toStdString());
        });
    }
    if (context_ != nullptr) {
        connections_ << connect(context_, &AppContext::glassChanged, this, [this] {
            if (applying_ == 0 && store_)
                writeJson("appearance.glass", context_->glass() ? "true" : "false");
        });
    }
    if (units_ != nullptr) {
        connections_ << connect(units_, &UnitsObject::newDocumentSystemRequested, this, [this](UnitSystem s) {
            writeJson("units.newDocument", s == UnitSystem::Inch ? "\"inch\"" : "\"metric\"");
        });
    }
}

SettingsObject::~SettingsObject()
{
    flushPending();
    for (const auto& c : connections_)
        disconnect(c);
    if (store_ && listener_ != 0)
        store_->unsubscribe(listener_);
}

SettingsObject* SettingsObject::create(QQmlEngine* qml, QJSEngine*)
{
    if (qml != nullptr) {
        if (auto* s = qobject_cast<SettingsObject*>(qml->property(kEngineKey).value<QObject*>())) {
            QJSEngine::setObjectOwnership(s, QJSEngine::CppOwnership);
            return s;
        }
    }
    return new SettingsObject(nullptr, nullptr, nullptr, nullptr, JobsProvider{}, nullptr);
}

void SettingsObject::provideFor(QQmlEngine* engine, SettingsObject* settings)
{
    engine->setProperty(kEngineKey, QVariant::fromValue<QObject*>(settings));
}

void SettingsObject::bind(std::shared_ptr<api::SettingsStore> store)
{
    if (store == store_ || !store)
        return;
    flushPending();
    if (store_ && listener_ != 0)
        store_->unsubscribe(listener_);
    store_ = std::move(store);
    QPointer<SettingsObject> self(this);
    listener_ = store_->subscribe([self](const api::UserSettings& s, const std::vector<std::string>& keys) {
        if (!self)
            return;
        if (QThread::currentThread() == self->thread()) {
            self->onStoreChanged(s, keys);
        } else {
            // The store changed on another thread: continue on this object's thread, through its event queue.
            invokeQueued(self.data(), [self, s, keys] {
                if (self)
                    self->onStoreChanged(s, keys);
            });
        }
    });
    refreshValues();
    const api::UserSettings s = current();
    applyUnits(s);
    preset_ = composePreset(baseTier(s), s.overrides, s.glass);
    emit storeChanged();
    emit presetChanged();
}

void SettingsObject::load(const std::optional<std::string>& userDataDir, bool themeForced, bool glassForced)
{
    themeForced_ = themeForced;
    glassForced_ = glassForced;
    bind(api::SettingsStore::open(userDataDir));
    applyAll_ = true;
    graphicsActive_ = true;
    const api::UserSettings s = current();
    applyTheme(s);
    applyGraphics(s);
    applyNavigation(s);
    if (auto problem = store_->loadProblem(); problem && context_ != nullptr)
        context_->showMessage(QString::fromStdString(*problem), true);
}

void SettingsObject::setApp(api::IApp* app)
{
    if (app == nullptr)
        return;
    if (auto s = api::settingsOf(*app); s && s != store_) {
        bind(std::move(s));
        if (applyAll_) {
            const api::UserSettings now = current();
            applyTheme(now);
            applyGraphics(now);
            applyNavigation(now);
        }
    }
}

api::UserSettings SettingsObject::current() const
{
    return store_ ? store_->get() : api::UserSettings{};
}

void SettingsObject::attachView(ViewportItem* view)
{
    if (view == view_)
        return;
    if (viewConnection_)
        disconnect(viewConnection_);
    view_ = view;
    if (view_ == nullptr)
        return;
    viewConnection_ = connect(view_, &ViewportItem::navigationPresetChanged, this, [this] {
        if (applying_ > 0 || !view_)
            return;
        const auto& presets = nav::presets();
        const int i = view_->navigationPreset();
        if (i >= 0 && i < int(presets.size()))
            writeJson("navigation.preset", jsonText(QString::fromLatin1(presets[std::size_t(i)].id)).toStdString());
    });
    if (applyAll_)
        applyNavigation(current());
}

void SettingsObject::onStoreChanged(const api::UserSettings& s, const std::vector<std::string>& keys)
{
    refreshValues();
    bool theme = false, glass = false, graphics = false, units = false, navigation = false;
    for (const std::string& k : keys) {
        theme = theme || k == "appearance.theme";
        glass = glass || k == "appearance.glass";
        graphics = graphics || isUserGraphicsKey(k);
        units = units || k.rfind("units.", 0) == 0;
        navigation = navigation || k == "navigation.preset";
    }
    // A theme or glass the command line set holds until the user changes it.
    if (theme)
        themeForced_ = false;
    if (glass)
        glassForced_ = false;
    if (theme)
        applyTheme(s);
    if (graphics)
        graphicsActive_ = true; // the user changed a graphics setting: from now on the preset is ours
    if (graphics || glass)
        applyGraphics(s, glass);
    if (units)
        applyUnits(s);
    if (navigation)
        applyNavigation(s);
    emit changed();
}

void SettingsObject::refreshValues()
{
    values_.clear();
    setKeys_.clear();
    if (store_) {
        const QJsonObject all = QJsonDocument::fromJson(QByteArray::fromStdString(store_->valuesJson())).object();
        for (auto it = all.begin(); it != all.end(); ++it)
            values_.insert(it.key(), it.value().toVariant());
        for (const std::string& k : store_->setKeys())
            setKeys_ << QString::fromStdString(k);
    }
    ++revision_;
}

void SettingsObject::applyTheme(const api::UserSettings& s)
{
    if (themeForced_ || theme_ == nullptr)
        return;
    const QString wanted = QString::fromLatin1(api::themeModeName(s.theme).data());
    if (settingFromMode(*theme_) == wanted && theme_->mode() == wanted)
        return;
    if (settingFromMode(*theme_) == wanted && theme_->mode() != QLatin1String("system") &&
        wanted != QLatin1String("system"))
        return; // a theme id of the wanted kind ("graphite-dark" for "dark") stays
    ++applying_;
    theme_->setMode(wanted);
    --applying_;
}

render::Tier SettingsObject::baseTier(const api::UserSettings& s) const
{
    if (auto forced = tierOf(s.tier))
        return *forced;
    if (hw_)
        return render::chooseTier(*hw_);
    if (s.lastDetectedTier)
        if (auto cached = tierOf(*s.lastDetectedTier))
            return *cached;
    return render::Tier::Low;
}

void SettingsObject::applyGraphics(const api::UserSettings& s, bool glassChanged)
{
    preset_ = composePreset(baseTier(s), s.overrides, s.glass);
    if (graphicsActive_ && scene_ != nullptr) {
        scene_->setPreset(preset_);
        presetApplied_ = true;
    }
    if ((graphicsActive_ || glassChanged) && !glassForced_ && context_ != nullptr) {
        ++applying_;
        context_->setGlass(preset_.liquidGlass);
        --applying_;
    }
    emit presetChanged();
}

void SettingsObject::applyUnits(const api::UserSettings& s)
{
    if (units_ == nullptr)
        return;
    units_->setDecimals(s.decimalsInch, s.decimalsMetric);
    units_->setNewDocumentSystem(s.newDocumentUnits);
}

void SettingsObject::applyNavigation(const api::UserSettings& s)
{
    if (!view_)
        return;
    const int index = std::max(0, int(navigationPresetIds().indexOf(QString::fromStdString(s.navigationPreset))));
    if (view_->navigationPreset() == index)
        return;
    ++applying_;
    view_->setNavigationPreset(index);
    --applying_;
}

void SettingsObject::requestHardware()
{
    if (hw_ || detecting_)
        return;
    IJobSystem* jobs = jobs_ ? jobs_() : nullptr;
    if (jobs == nullptr)
        return;
    detecting_ = true;
    const std::uint64_t serial = ++detectSerial_;
    emit hardwareChanged();
    // detectHardware creates a throw-away device on Windows (about 0.2 s): never on the UI thread.
    JobHandle<render::HardwareInfo> job = jobs->submit<render::HardwareInfo>(
        JobInfo{"Detecting graphics hardware", JobPriority::Normal, false},
        [](JobContext&) -> Result<render::HardwareInfo> { return render::detectHardware(); });
    QPointer<SettingsObject> self(this);
    job.then([self, serial](Result<render::HardwareInfo> hw) {
        if (!self || serial != self->detectSerial_)
            return;
        self->setDetectedHardware(hw ? *hw : render::HardwareInfo{});
    });
}

void SettingsObject::setDetectedHardware(const render::HardwareInfo& hw)
{
    ++detectSerial_; // a detection still running no longer applies
    hw_ = hw;
    detecting_ = false;
    // Remember it, so the next start uses this tier before detection has run again.
    const api::UserSettings before = current();
    const api::TierChoice detected = choiceOf(render::chooseTier(hw));
    if (store_ && (before.lastDetectedTier != detected || before.lastDriverVersion != hw.driverVersion)) {
        QJsonObject values{
            {QStringLiteral("graphics.lastDetectedTier"), QString::fromLatin1(api::tierChoiceName(detected).data())}};
        // The schema takes printable ASCII only; a driver string with anything else is not remembered.
        const QString driver = QString::fromStdString(hw.driverVersion);
        bool ascii = driver.size() <= 128;
        for (const QChar c : driver)
            ascii = ascii && c.unicode() >= 0x20 && c.unicode() < 0x7f;
        values.insert(QStringLiteral("graphics.lastDriverVersion"), ascii ? QJsonValue(driver) : QJsonValue());
        (void)store_->setMany(QJsonDocument(values).toJson(QJsonDocument::Compact).toStdString());
    }
    applyGraphics(current());
    emit hardwareChanged();
}

void SettingsObject::saveWindow(int x, int y, int width, int height, bool maximized, bool now)
{
    pending_["window.maximized"] = maximized ? "true" : "false";
    if (!maximized) {
        // Maximized: keep the normal geometry saved before, so un-maximizing at the next start has a size.
        pending_["window.x"] = std::to_string(x);
        pending_["window.y"] = std::to_string(y);
        pending_["window.width"] = std::to_string(std::max(width, 200));
        pending_["window.height"] = std::to_string(std::max(height, 200));
    }
    if (now)
        flushPending();
    else
        pendingTimer_.start();
}

void SettingsObject::flushPending()
{
    pendingTimer_.stop();
    if (pending_.empty() || !store_)
        return;
    std::string object = "{";
    for (const auto& [key, value] : pending_) {
        if (object.size() > 1)
            object += ',';
        object += jsonText(QString::fromStdString(key)).toStdString() + ":" + value;
    }
    object += "}";
    pending_.clear();
    if (auto st = store_->setMany(object); !st)
        lastError_ = QString::fromStdString(st.error().message);
}

bool SettingsObject::writeJson(const std::string& key, const std::string& valueJson)
{
    if (!store_)
        return false;
    pending_.erase(key);
    auto st = store_->set(key, valueJson);
    lastError_ = st ? QString() : QString::fromStdString(st.error().message);
    if (!st)
        emit changed();
    return st.has_value();
}

// ---------- QML ----------
QString SettingsObject::filePath() const
{
    return store_ ? QString::fromStdString(store_->filePath()) : QString();
}

QString SettingsObject::loadProblem() const
{
    auto p = store_ ? store_->loadProblem() : std::nullopt;
    return p ? QString::fromStdString(*p) : QString();
}

QString SettingsObject::hardwareState() const
{
    if (detecting_)
        return QStringLiteral("detecting");
    return hw_ ? QStringLiteral("ready") : QStringLiteral("idle");
}

QVariantMap SettingsObject::hardware() const
{
    if (!hw_)
        return {};
    const render::HardwareInfo& hw = *hw_;
    const auto orUnknown = [](const std::string& s) {
        return s.empty() ? tr("unknown") : QString::fromStdString(s);
    };
    QString type;
    switch (hw.gpuType) {
    case render::GpuType::Discrete:
        type = tr("Dedicated graphics card");
        break;
    case render::GpuType::Integrated:
        type = tr("Integrated graphics");
        break;
    case render::GpuType::Software:
        type = tr("Software renderer");
        break;
    case render::GpuType::Unknown:
        type = tr("unknown");
        break;
    }
    return QVariantMap{
        {QStringLiteral("gpuName"), orUnknown(hw.gpuName)},
        {QStringLiteral("gpuVendor"), orUnknown(hw.gpuVendor)},
        {QStringLiteral("gpuType"), type},
        {QStringLiteral("vram"), hw.vramBytes > 0 ? gigabytes(hw.vramBytes) : tr("unknown")},
        {QStringLiteral("ram"), hw.ramBytes > 0 ? gigabytes(hw.ramBytes) : tr("unknown")},
        {QStringLiteral("cores"), hw.physicalCores > 0 ? QString::number(hw.physicalCores) : tr("unknown")},
        {QStringLiteral("graphicsApi"), orUnknown(hw.graphicsApi)},
        {QStringLiteral("driver"), orUnknown(hw.driverVersion)},
        {QStringLiteral("computeShaders"), hw.computeShaders ? tr("available") : tr("not available")},
    };
}

QString SettingsObject::detectedTier() const
{
    return hw_ ? tierKey(render::chooseTier(*hw_)) : QString();
}

QString SettingsObject::tierReason() const
{
    return hw_ ? explainTier(*hw_).reason : QString();
}

QString SettingsObject::effectiveTier() const
{
    return tierKey(preset_.tier);
}

bool SettingsObject::tierForced() const
{
    return tierOf(current().tier).has_value();
}

QVariantMap SettingsObject::tierDefaults() const
{
    return presetMap(render::presetFor(preset_.tier));
}

QVariantMap SettingsObject::effectivePreset() const
{
    return presetMap(preset_);
}

QStringList SettingsObject::navigationPresetIds() const
{
    QStringList out;
    for (const nav::Preset& p : nav::presets())
        out << QString::fromLatin1(p.id);
    return out;
}

QStringList SettingsObject::navigationPresetNames() const
{
    QStringList out;
    for (const nav::Preset& p : nav::presets())
        out << QCoreApplication::translate("ray::ui::ViewportItem", p.title);
    return out;
}

QStringList SettingsObject::navigationPresetDescriptions() const
{
    QStringList out;
    for (const nav::Preset& p : nav::presets())
        out << QCoreApplication::translate("ray::ui::ViewportItem", p.description);
    return out;
}

int SettingsObject::navigationPresetIndex() const
{
    return std::max(0,
                    int(navigationPresetIds().indexOf(values_.value(QStringLiteral("navigation.preset")).toString())));
}

bool SettingsObject::set(const QString& key, const QVariant& value)
{
    return writeJson(key.toStdString(), jsonText(value).toStdString());
}

bool SettingsObject::reset(const QString& keyOrPrefix)
{
    if (!store_)
        return false;
    flushPending();
    auto st = store_->reset(keyOrPrefix.toStdString());
    lastError_ = st ? QString() : QString::fromStdString(st.error().message);
    return st.has_value();
}

QString SettingsObject::title(const QString& key) const
{
    const api::SettingInfo* info = api::SettingsStore::find(key.toStdString());
    return info ? tr(info->title.c_str()) : key;
}

QString SettingsObject::description(const QString& key) const
{
    const api::SettingInfo* info = api::SettingsStore::find(key.toStdString());
    return info ? tr(info->description.c_str()) : QString();
}

bool SettingsObject::panelOpen(const QString& panel) const
{
    const QVariant v = values_.value(QStringLiteral("panels.%1.open").arg(panel));
    return v.isValid() && !v.isNull() ? v.toBool() : true;
}

void SettingsObject::setPanelOpen(const QString& panel, bool open)
{
    // Also called when Shell.qml first binds the value: nothing to write then (no file write at start).
    if (panelOpen(panel) == open)
        return;
    writeJson(QStringLiteral("panels.%1.open").arg(panel).toStdString(), open ? "true" : "false");
}

qreal SettingsObject::panelWidth(const QString& panel, qreal fallback) const
{
    const QVariant v = values_.value(QStringLiteral("panels.%1.width").arg(panel));
    return v.isValid() && !v.isNull() ? v.toReal() : fallback;
}

void SettingsObject::setPanelWidth(const QString& panel, qreal width)
{
    const std::string key = QStringLiteral("panels.%1.width").arg(panel).toStdString();
    pending_[key] = width < 0 ? std::string("null") : std::to_string(std::clamp(int(width + 0.5), 100, 4000));
    pendingTimer_.start();
}

void SettingsObject::open()
{
    emit openRequested();
}

} // namespace ray::ui
