#include "tools_model.h"

#include "../app_context.h"
#include "../model/model_util.h"
#include "../palette_commands.h"
#include "../session.h"
#include "../ui_commands.h"
#include "../units.h"

#include <ray_api/cam_tools.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QLocale>
#include <QtCore/QPointF>
#include <QtCore/QTimer>

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

namespace ray::ui {

using namespace modelui;

namespace {
// T-046: the tool library manager. Its key works in the CAM workspace only (CommandContext::Cam); the command needs a
// session, not a document.
// clang-format off
constexpr UiCommand kToolLibraryCommands[] = {
    {"cam.toolLibrary.open", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Tool Library…"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "CAM"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Browse, filter and edit tools, holders and cutting data"), "Shift+T", CommandContext::Cam, ""},
};
// clang-format on

QString trs(const char* text)
{
    return QCoreApplication::translate(toolsui::kTrContext, text);
}

struct TypeInfo {
    const char* name;
    const char* title;
};
// clang-format off
constexpr TypeInfo kTypes[] = {
    {"flatEndMill", QT_TRANSLATE_NOOP("ray::ui::ToolLibraryModel", "Flat end mill")},
    {"ballEndMill", QT_TRANSLATE_NOOP("ray::ui::ToolLibraryModel", "Ball end mill")},
    {"bullEndMill", QT_TRANSLATE_NOOP("ray::ui::ToolLibraryModel", "Bull nose end mill")},
    {"chamferMill", QT_TRANSLATE_NOOP("ray::ui::ToolLibraryModel", "Chamfer mill")},
    {"faceMill", QT_TRANSLATE_NOOP("ray::ui::ToolLibraryModel", "Face mill")},
    {"spotDrill", QT_TRANSLATE_NOOP("ray::ui::ToolLibraryModel", "Spot drill")},
    {"drill", QT_TRANSLATE_NOOP("ray::ui::ToolLibraryModel", "Drill")},
    {"tap", QT_TRANSLATE_NOOP("ray::ui::ToolLibraryModel", "Tap")},
    {"reamer", QT_TRANSLATE_NOOP("ray::ui::ToolLibraryModel", "Reamer")},
    {"boringBar", QT_TRANSLATE_NOOP("ray::ui::ToolLibraryModel", "Boring bar")},
    {"threadMill", QT_TRANSLATE_NOOP("ray::ui::ToolLibraryModel", "Thread mill")},
    {"engraveCutter", QT_TRANSLATE_NOOP("ray::ui::ToolLibraryModel", "Engraving cutter")},
};
constexpr TypeInfo kOpClasses[] = {
    {"rough", QT_TRANSLATE_NOOP("ray::ui::ToolLibraryModel", "Roughing")},
    {"finish", QT_TRANSLATE_NOOP("ray::ui::ToolLibraryModel", "Finishing")},
    {"slot", QT_TRANSLATE_NOOP("ray::ui::ToolLibraryModel", "Slotting")},
    {"drill", QT_TRANSLATE_NOOP("ray::ui::ToolLibraryModel", "Drilling")},
    {"spot", QT_TRANSLATE_NOOP("ray::ui::ToolLibraryModel", "Spotting")},
    {"tap", QT_TRANSLATE_NOOP("ray::ui::ToolLibraryModel", "Tapping")},
    {"ream", QT_TRANSLATE_NOOP("ray::ui::ToolLibraryModel", "Reaming")},
    {"chamfer", QT_TRANSLATE_NOOP("ray::ui::ToolLibraryModel", "Chamfering")},
    {"face", QT_TRANSLATE_NOOP("ray::ui::ToolLibraryModel", "Facing")},
    {"thread", QT_TRANSLATE_NOOP("ray::ui::ToolLibraryModel", "Thread milling")},
    {"bore", QT_TRANSLATE_NOOP("ray::ui::ToolLibraryModel", "Boring")},
    {"engrave", QT_TRANSLATE_NOOP("ray::ui::ToolLibraryModel", "Engraving")},
};
// clang-format on

QString compact(const QJsonObject& o)
{
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

QJsonObject parseObject(const std::string& json)
{
    return QJsonDocument::fromJson(QByteArray::fromStdString(json)).object();
}

QVariantList points(const std::vector<Vec2>& poly)
{
    QVariantList out;
    out.reserve(static_cast<qsizetype>(poly.size()) + 1);
    for (const Vec2& v : poly)
        out.push_back(QPointF(v.x, v.y));
    if (!poly.empty())
        out.push_back(QPointF(poly.front().x, poly.front().y)); // closed
    return out;
}

QVariantMap rowMap(const toolsui::Row& row)
{
    QVariantMap m;
    for (const auto& [k, v] : row.fields)
        m.insert(k, v);
    return m;
}

// A new tool of the type, with plausible proportions in the display unit's usual sizes.
lib::Tool newTool(lib::ToolType type, bool inch, const QString& material)
{
    lib::Tool t;
    t.id = ToolId{Uuid::generate()};
    t.type = type;
    const double d = inch ? 6.35 : 6.0;
    t.diameter = d;
    t.shankDiameter = d;
    t.flutes = 3;
    t.fluteLength = 3 * d;
    t.overallLength = 3 * d + 6 * d;
    t.bodyMaterial = "carbide";
    switch (type) {
    case lib::ToolType::BallEndMill:
        t.cornerRadius = d / 2;
        break;
    case lib::ToolType::BullEndMill:
        t.cornerRadius = inch ? 0.762 : 0.75;
        break;
    case lib::ToolType::ChamferMill:
        t.tipAngleDeg = 90;
        t.flutes = 4;
        t.fluteLength = d / 2;
        break;
    case lib::ToolType::FaceMill:
        t.diameter = inch ? 50.8 : 50;
        t.shankDiameter = inch ? 25.4 : 22;
        t.flutes = 5;
        t.fluteLength = 6;
        t.overallLength = 50;
        t.cornerRadius = 0.8;
        break;
    case lib::ToolType::SpotDrill:
        t.tipAngleDeg = 90;
        t.flutes = 2;
        t.fluteLength = d;
        break;
    case lib::ToolType::Drill:
        t.tipAngleDeg = 135;
        t.flutes = 2;
        t.fluteLength = 8 * d;
        t.overallLength = 12 * d;
        t.bodyMaterial = "HSS-Co";
        break;
    case lib::ToolType::Tap:
        t.flutes = 4;
        t.threadPitch = inch ? 1.27 : 1.0;
        t.threadSpec = inch ? "1/4-20 UNC" : "M6x1";
        t.bodyMaterial = "HSS-Co";
        break;
    case lib::ToolType::Reamer:
        t.flutes = 6;
        break;
    case lib::ToolType::BoringBar:
        t.flutes = 1;
        break;
    case lib::ToolType::ThreadMill:
        t.tipAngleDeg = 60;
        t.fluteLength = d / 4;
        // A single profile on a neck that clears the thread (after M3): 0.7 of the cut, three diameters long.
        t.neckDiameter = 0.7 * d;
        t.neckLength = 3 * d;
        break;
    case lib::ToolType::EngraveCutter:
        t.tipAngleDeg = 60;
        t.flutes = 1;
        t.fluteLength = d;
        break;
    case lib::ToolType::FlatEndMill:
        break;
    }
    t.shoulderLength = std::max(t.fluteLength, t.neckLength);
    if (!material.isEmpty()) {
        toolsui::Units none;
        const toolsui::Row row = toolsui::newPresetRow(type, material, none);
        lib::ToolPreset p;
        p.material = MaterialId{Uuid::fromString(material.toStdString())};
        p.opClass = row.base.value(QStringLiteral("opClass")).toString().toStdString();
        p.surfaceSpeedMMin = row.base.value(QStringLiteral("surfaceSpeed")).toDouble();
        p.chipLoadMm = row.base.value(QStringLiteral("chipLoad")).toDouble();
        p.maxRadialFraction = row.base.value(QStringLiteral("maxRadialFraction")).toDouble();
        p.maxAxialFraction = row.base.value(QStringLiteral("maxAxialFraction")).toDouble();
        t.presets.push_back(p);
    }
    return t;
}

lib::Holder newHolder()
{
    lib::Holder h;
    h.id = HolderId{Uuid::generate()};
    h.gaugeLength = 60;
    h.segments = {{15, 63, 63}, {45, 32, 42}};
    return h;
}

} // namespace

std::span<const UiCommand> camToolsUiCommands()
{
    return kToolLibraryCommands;
}

// ---------- construction ----------
ToolLibraryModel::ToolLibraryModel(Session* session, AppContext* context, QObject* parent)
    : QObject(parent), session_(session), context_(context)
{
    if (context_)
        connect(context_, &AppContext::sessionChanged, this, [this] { subscribeSession(); });
    if (session_)
        connect(session_, &Session::documentChanged, this, [this] { subscribeDocument(); });
    subscribeSession();
}

ToolLibraryModel::~ToolLibraryModel()
{
    *alive_ = false;
    if (commands_)
        commands_->setUiHandler(QStringLiteral("cam.toolLibrary."), {});
    if (lib::LibraryStack* s = stack(); s != nullptr && s == stackSubscribed_ && stackListener_ != 0)
        s->unsubscribe(stackListener_);
    unsubscribeDocument();
}

ToolLibraryModel* ToolLibraryModel::create(QQmlEngine* qml, QJSEngine*)
{
    Session* session = nullptr;
    AppContext* context = nullptr;
    CommandCatalog* commands = nullptr;
    UnitsObject* units = nullptr;
    if (qml != nullptr) {
        const QString uri = QStringLiteral("RayCad.Ui");
        session = qml->singletonInstance<Session*>(uri, QStringLiteral("Session"));
        context = qml->singletonInstance<AppContext*>(uri, QStringLiteral("AppContext"));
        commands = qml->singletonInstance<CommandCatalog*>(uri, QStringLiteral("Commands"));
        units = qml->singletonInstance<UnitsObject*>(uri, QStringLiteral("Units"));
    }
    auto* model = new ToolLibraryModel(session, context);
    model->setCommands(commands);
    model->setUnits(units);
    return model; // owned by the engine
}

void ToolLibraryModel::setCommands(CommandCatalog* commands)
{
    commands_ = commands;
    if (!commands_)
        return;
    QPointer<ToolLibraryModel> self(this);
    commands_->setUiHandler(
        QStringLiteral("cam.toolLibrary."),
        CommandCatalog::UiHandler{[self](const QString& id) {
                                      return self ? self->commandReason(id) : trs("The tool library is not available");
                                  },
                                  [self](const QString& id) { return self && self->runCommand(id); }});
}

void ToolLibraryModel::setUnits(UnitsObject* unitsObject)
{
    units_ = unitsObject;
    if (!units_)
        return;
    connect(units_, &UnitsObject::changed, this, [this] {
        catalogStale();
        // What is shown goes to the new unit unless it holds edits (a text with its unit still reads the same).
        if (draft_ && !dirty()) {
            Draft d = *draft_;
            d.values =
                d.holder
                    ? toolsui::holderValues(*api::cam_tools::holderFromJson(compact(d.baseJson).toStdString()), units())
                    : toolsui::toolValues(*api::cam_tools::toolFromJson(compact(d.baseJson).toStdString()), units());
            draft_ = std::move(d);
            recheck();
            emit valuesChanged();
        }
    });
}

// ---------- the session, the stack and the document ----------
api::IApp* ToolLibraryModel::app() const
{
    return context_ ? context_->app() : nullptr;
}

lib::LibraryStack* ToolLibraryModel::stack() const
{
    api::IApp* a = app();
    return a != nullptr ? a->libraries() : nullptr;
}

doc::DocStatePtr ToolLibraryModel::docState() const
{
    api::IApp* a = app();
    api::OpenDocument* od = a != nullptr ? a->active() : nullptr;
    return od != nullptr && od->document ? od->document->state() : nullptr;
}

bool ToolLibraryModel::hasDocument() const
{
    return docState() != nullptr;
}

void ToolLibraryModel::subscribeSession()
{
    lib::LibraryStack* s = stack();
    if (s != stackSubscribed_) {
        // The old stack went with its app (a listener left in it is never called again); a new one is listened to.
        stackSubscribed_ = s;
        stackListener_ = 0;
        if (s != nullptr) {
            std::weak_ptr<bool> alive = alive_;
            stackListener_ = s->subscribe([this, alive] {
                if (auto a = alive.lock(); a && *a)
                    catalogStale();
            });
        }
    }
    subscribeDocument();
    catalogStale();
}

void ToolLibraryModel::subscribeDocument()
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
        QPointer<ToolLibraryModel> self(this);
        docListener_ = doc_->subscribe([self, generation](doc::DocStatePtr, doc::BuildResultPtr) {
            if (self && self->docGeneration_ == generation)
                self->catalogStale();
        });
    }
    // A document copy in the editor belongs to the document that was active.
    if (draft_ && draft_->origin == QLatin1String("document"))
        clearEditor();
    catalogStale();
}

void ToolLibraryModel::unsubscribeDocument()
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
void ToolLibraryModel::catalogStale()
{
    catalogValid_ = false;
    if (!shown_ && !catalogWanted_)
        return;
    // Coalesced: the notifications of one change make one catalog.
    QPointer<ToolLibraryModel> self(this);
    QTimer::singleShot(0, this, [self] {
        if (self && !self->catalogValid_)
            self->buildCatalog();
    });
}

void ToolLibraryModel::useCatalog()
{
    catalogWanted_ = true;
}

const std::vector<toolsui::Entry>& ToolLibraryModel::entries(bool holders)
{
    catalogWanted_ = true;
    if (!catalogValid_)
        buildCatalog();
    return holders ? holders_ : tools_;
}

int ToolLibraryModel::myRoot() const
{
    for (int i = static_cast<int>(roots_.size()) - 1; i >= 0; --i) {
        if (roots_[i].toMap().value(QStringLiteral("writable")).toBool())
            return i;
    }
    return -1;
}

QString ToolLibraryModel::rootLabel(int root) const
{
    if (root < 0 || root >= roots_.size())
        return trs("In this document");
    const QVariantMap r = roots_[root].toMap();
    if (r.value(QStringLiteral("kind")).toString() == QLatin1String("user"))
        return trs("My library");
    return r.value(QStringLiteral("name")).toString();
}

QString ToolLibraryModel::myLibraryName() const
{
    return myRoot() >= 0 ? trs("My library") : QString();
}

QString ToolLibraryModel::typeTitle(const QString& type) const
{
    for (const TypeInfo& t : kTypes) {
        if (type == QLatin1String(t.name))
            return trs(t.title);
    }
    return type;
}

QVariantList ToolLibraryModel::toolTypes() const
{
    QVariantList out;
    for (const TypeInfo& t : kTypes)
        out.push_back(
            QVariantMap{{QStringLiteral("value"), QLatin1String(t.name)}, {QStringLiteral("text"), trs(t.title)}});
    return out;
}

QVariantList ToolLibraryModel::opClasses() const
{
    QVariantList out;
    for (const TypeInfo& t : kOpClasses)
        out.push_back(
            QVariantMap{{QStringLiteral("value"), QLatin1String(t.name)}, {QStringLiteral("text"), trs(t.title)}});
    return out;
}

QString ToolLibraryModel::formatLength(double mm) const
{
    return units_ ? units_->formatLength(mm) : QString::number(mm, 'f', 3) + QStringLiteral(" mm");
}

double ToolLibraryModel::parseLength(const QString& text) const
{
    return units_ ? units_->parseLength(text) : QLocale::c().toDouble(text);
}

toolsui::Units ToolLibraryModel::units() const
{
    toolsui::Units u;
    u.inch = units_ && units_->system() == UnitSystem::Inch;
    QPointer<UnitsObject> units = units_;
    u.parseLength = [units](const QString& text) {
        return units ? units->parseLength(text) : std::nan("");
    };
    return u;
}

void ToolLibraryModel::buildCatalog()
{
    QElapsedTimer timer;
    timer.start();
    tools_.clear();
    holders_.clear();
    roots_.clear();
    materials_.clear();
    holderChoices_.clear();
    issueCount_ = 0;
    lib::LibraryStack* s = stack();
    const doc::DocStatePtr state = docState();
    const cam::CamState* cam = state && state->cam ? state->cam.get() : nullptr;

    if (s != nullptr) {
        const auto infos = s->roots();
        int last = -1;
        for (int i = 0; i < static_cast<int>(infos.size()); ++i) {
            if (infos[static_cast<std::size_t>(i)].writable)
                last = i;
        }
        for (int i = 0; i < static_cast<int>(infos.size()); ++i) {
            const auto& r = infos[static_cast<std::size_t>(i)];
            const QString kind = i == last               ? QStringLiteral("user")
                                 : r.open && !r.writable ? QStringLiteral("builtin")
                                                         : QStringLiteral("shared");
            roots_.push_back(QVariantMap{{QStringLiteral("index"), i},
                                         {QStringLiteral("name"), qs(r.name)},
                                         {QStringLiteral("dir"), qs(r.dir)},
                                         {QStringLiteral("kind"), kind},
                                         {QStringLiteral("writable"), r.writable},
                                         {QStringLiteral("open"), r.open},
                                         {QStringLiteral("loading"), r.loading}});
        }
        issueCount_ = static_cast<int>(s->issues().size());
    }
    const auto writable = [this](int root) {
        return root >= 0 && root < roots_.size() && roots_[root].toMap().value(QStringLiteral("writable")).toBool();
    };
    const auto materialsOf = [](const lib::Tool& t) {
        std::vector<QString> m;
        for (const lib::ToolPreset& p : t.presets)
            m.push_back(idText(p.material.value));
        std::sort(m.begin(), m.end());
        m.erase(std::unique(m.begin(), m.end()), m.end());
        return m;
    };
    const auto toolEntry = [&](const lib::Tool& t, bool document, int root) {
        toolsui::Entry e;
        e.document = document;
        e.id = idText(t.id.value);
        e.name = qs(t.name);
        e.type = QString::fromLatin1(api::cam_tools::toolTypeName(t.type));
        e.diameter = t.diameter;
        e.flutes = t.flutes;
        e.root = root;
        e.readOnly = document || !writable(root);
        e.search = toolsui::searchText(t, typeTitle(e.type));
        e.materials = materialsOf(t);
        return e;
    };
    // The document's copies first, each compared with the library's original.
    std::set<QString> materialIds;
    if (cam != nullptr) {
        for (const auto& [id, embedded] : cam->tools) {
            toolsui::Entry e = toolEntry(embedded.item, true, -1);
            if (const lib::Tool* original = s != nullptr ? s->tool(id) : nullptr) {
                e.hasLibraryCopy = true;
                e.libraryChanged = original->geometryHash() != embedded.item.geometryHash();
                e.libraryDiffers = original->contentHash() != embedded.item.contentHash();
            }
            tools_.push_back(std::move(e));
        }
        for (const auto& [id, embedded] : cam->holders) {
            toolsui::Entry e;
            e.document = true;
            e.holder = true;
            e.id = idText(id.value);
            e.name = qs(embedded.item.name);
            for (const lib::HolderSegment& seg : embedded.item.segments)
                e.diameter = std::max({e.diameter, seg.lowerDiameter, seg.upperDiameter});
            e.readOnly = true;
            e.search = toolsui::searchText(embedded.item);
            if (const lib::Holder* original = s != nullptr ? s->holder(id) : nullptr) {
                e.hasLibraryCopy = true;
                e.libraryChanged = original->geometryHash() != embedded.item.geometryHash();
                e.libraryDiffers = api::cam_tools::contentHash(*original) != api::cam_tools::contentHash(embedded.item);
            }
            holders_.push_back(std::move(e));
            holderChoices_.push_back(QVariantMap{{QStringLiteral("id"), idText(id.value)},
                                                 {QStringLiteral("name"), qs(embedded.item.name)}});
        }
        for (const auto& [id, embedded] : cam->materials) {
            materialIds.insert(idText(id.value));
            materials_.push_back(QVariantMap{{QStringLiteral("id"), idText(id.value)},
                                             {QStringLiteral("name"), qs(embedded.item.name)}});
        }
    }
    if (s != nullptr) {
        const std::vector<lib::Tool> tools = s->tools();
        tools_.reserve(tools_.size() + tools.size());
        for (const lib::Tool& t : tools) {
            const auto root = s->rootOf(lib::ItemKind::Tool, t.id.value);
            tools_.push_back(toolEntry(t, false, root ? static_cast<int>(*root) : -1));
        }
        std::set<QString> holderIds;
        for (const QVariant& h : holderChoices_)
            holderIds.insert(h.toMap().value(QStringLiteral("id")).toString());
        for (const lib::Holder& h : s->holders()) {
            const auto root = s->rootOf(lib::ItemKind::Holder, h.id.value);
            toolsui::Entry e;
            e.holder = true;
            e.id = idText(h.id.value);
            e.name = qs(h.name);
            for (const lib::HolderSegment& seg : h.segments)
                e.diameter = std::max({e.diameter, seg.lowerDiameter, seg.upperDiameter});
            e.root = root ? static_cast<int>(*root) : -1;
            e.readOnly = !writable(e.root);
            e.search = toolsui::searchText(h);
            if (holderIds.insert(e.id).second)
                holderChoices_.push_back(QVariantMap{{QStringLiteral("id"), e.id}, {QStringLiteral("name"), e.name}});
            holders_.push_back(std::move(e));
        }
        for (const lib::Material& m : s->materials()) {
            if (materialIds.insert(idText(m.id.value)).second)
                materials_.push_back(
                    QVariantMap{{QStringLiteral("id"), idText(m.id.value)}, {QStringLiteral("name"), qs(m.name)}});
        }
    }
    const auto byName = [](const QVariant& a, const QVariant& b) {
        const QString na = a.toMap().value(QStringLiteral("name")).toString();
        const QString nb = b.toMap().value(QStringLiteral("name")).toString();
        const int c = na.compare(nb, Qt::CaseInsensitive);
        return c != 0 ? c < 0
                      : a.toMap().value(QStringLiteral("id")).toString() <
                            b.toMap().value(QStringLiteral("id")).toString();
    };
    std::stable_sort(materials_.begin(), materials_.end(), byName);
    std::stable_sort(holderChoices_.begin(), holderChoices_.end(), byName);
    catalogValid_ = true;
    ++catalogBuilds_;
    ++revision_;
    lastCatalogMs_ = static_cast<double>(timer.nsecsElapsed()) / 1e6;
    emit catalogChanged(); // the lists filter again first

    // The item in the editor follows the library while it holds no edits; with edits the save checks for a conflict.
    if (draft_ && draft_->origin == QLatin1String("library") && !dirty() && s != nullptr) {
        const QString id = draft_->id;
        const bool holder = draft_->holder;
        const bool gone = holder ? libraryHolder(id) == nullptr : libraryTool(id) == nullptr;
        std::optional<Hash256> now;
        if (!gone)
            now = holder ? api::cam_tools::contentHash(*libraryHolder(id)) : libraryTool(id)->contentHash();
        if (gone) {
            clearEditor();
        } else if (now != draft_->base) {
            const QString status = status_;
            const bool error = statusIsError_;
            draft_.reset();
            select(QStringLiteral("library"), holder ? QStringLiteral("holder") : QStringLiteral("tool"), id);
            setStatus(status, error);
        }
    } else if (draft_ && draft_->origin == QLatin1String("document")) {
        const bool gone = draft_->holder ? documentHolder(draft_->id) == nullptr : documentTool(draft_->id) == nullptr;
        if (gone) {
            clearEditor(); // an undo took it out of the document
        } else {
            const QJsonObject now = draft_->holder
                                        ? parseObject(api::cam_tools::holderJson(*documentHolder(draft_->id)))
                                        : parseObject(api::cam_tools::toolJson(*documentTool(draft_->id)));
            if (now != draft_->baseJson) { // the document's copy changed (an update, an undo): shown as it is now
                const QString id = draft_->id;
                const bool holder = draft_->holder;
                draft_.reset();
                select(QStringLiteral("document"), holder ? QStringLiteral("holder") : QStringLiteral("tool"), id);
            } else {
                emit editorChanged(); // libraryChanged may differ now
            }
        }
    }
}

// ---------- showing ----------
void ToolLibraryModel::setShown(bool shown)
{
    if (shown)
        show();
    else
        hide();
}

void ToolLibraryModel::show()
{
    if (shown_)
        return;
    shown_ = true;
    if (!catalogValid_)
        buildCatalog();
    emit shownChanged();
    refresh(); // the folders may have changed since the watch last looked
}

void ToolLibraryModel::hide()
{
    if (!shown_)
        return;
    if (hold())
        return;
    shown_ = false;
    deleteAsked_ = false;
    emit shownChanged();
    if (choosing_) {
        choosing_ = false;
        emit choosingChanged();
        emit choiceEnded();
    }
}

void ToolLibraryModel::showToChoose()
{
    if (!choosing_) {
        choosing_ = true;
        emit choosingChanged();
    }
    show();
}

bool ToolLibraryModel::useSelected()
{
    if (!choosing_ || editorKind() != QLatin1String("tool") || dirty())
        return false;
    const QString origin = selectedOrigin();
    if (origin != QLatin1String("library") && origin != QLatin1String("document"))
        return false; // a new item or a copy not saved yet
    const QString id = selectedId();
    emit toolChosen(id, origin);
    hide();
    return true;
}

void ToolLibraryModel::refresh()
{
    api::IApp* a = app();
    lib::LibraryStack* s = stack();
    if (a == nullptr || s == nullptr || refreshing_)
        return;
    refreshing_ = true;
    ++jobsRunning_;
    emit refreshingChanged();
    QPointer<ToolLibraryModel> self(this);
    s->refreshInBackground(a->jobs(), [self](bool) {
        if (!self)
            return;
        self->refreshing_ = false;
        --self->jobsRunning_;
        emit self->refreshingChanged();
    });
}

bool ToolLibraryModel::busy() const
{
    return jobsRunning_ > 0 || saving_ || ((catalogWanted_ || shown_) && !catalogValid_);
}

QString ToolLibraryModel::commandReason(const QString&) const
{
    if (app() == nullptr)
        return trs("There is no session");
    return {};
}

bool ToolLibraryModel::runCommand(const QString& id)
{
    if (id != QLatin1String("cam.toolLibrary.open") || app() == nullptr)
        return false;
    show();
    return true;
}

// ---------- the editor ----------
const lib::Tool* ToolLibraryModel::libraryTool(const QString& id) const
{
    lib::LibraryStack* s = stack();
    return s != nullptr ? s->tool(ToolId{Uuid::fromString(id.toStdString())}) : nullptr;
}

const lib::Holder* ToolLibraryModel::libraryHolder(const QString& id) const
{
    lib::LibraryStack* s = stack();
    return s != nullptr ? s->holder(HolderId{Uuid::fromString(id.toStdString())}) : nullptr;
}

const lib::Tool* ToolLibraryModel::documentTool(const QString& id) const
{
    const doc::DocStatePtr state = docState();
    if (!state || !state->cam)
        return nullptr;
    const auto it = state->cam->tools.find(ToolId{Uuid::fromString(id.toStdString())});
    return it == state->cam->tools.end() ? nullptr : &it->second.item;
}

const lib::Holder* ToolLibraryModel::documentHolder(const QString& id) const
{
    const doc::DocStatePtr state = docState();
    if (!state || !state->cam)
        return nullptr;
    const auto it = state->cam->holders.find(HolderId{Uuid::fromString(id.toStdString())});
    return it == state->cam->holders.end() ? nullptr : &it->second.item;
}

QString ToolLibraryModel::editorKind() const
{
    return !draft_ ? QString() : draft_->holder ? QStringLiteral("holder") : QStringLiteral("tool");
}

QString ToolLibraryModel::selectedId() const
{
    return draft_ ? draft_->id : QString();
}

QString ToolLibraryModel::selectedOrigin() const
{
    return draft_ ? draft_->origin : QString();
}

QString ToolLibraryModel::title() const
{
    if (!draft_)
        return {};
    const QString name = draft_->values.fields.count(QStringLiteral("name"))
                             ? draft_->values.fields.at(QStringLiteral("name")).trimmed()
                             : QString();
    if (!name.isEmpty())
        return name;
    return draft_->holder ? trs("Holder without a name") : trs("Tool without a name");
}

QString ToolLibraryModel::whereText() const
{
    if (!draft_)
        return {};
    if (draft_->origin == QLatin1String("document"))
        return trs("In this document (read-only: change it through the library)");
    if (draft_->origin == QLatin1String("new"))
        return draft_->root >= 0 ? trs("New in %1, not saved yet").arg(rootLabel(draft_->root))
                                 : trs("New, with no library to save it in");
    const QString root = rootLabel(draft_->sourceRoot);
    return draft_->readOnly ? trs("%1 (read-only)").arg(root) : root;
}

bool ToolLibraryModel::readOnly() const
{
    return !draft_ || draft_->readOnly;
}

bool ToolLibraryModel::dirty() const
{
    return draft_ && !draft_->readOnly && (draft_->origin == QLatin1String("new") || toolsui::edited(draft_->values));
}

bool ToolLibraryModel::canSave() const
{
    return dirty() && !saving_ && draft_->root >= 0 && checked_.ok() && conflict_.isEmpty();
}

QVariantMap ToolLibraryModel::values() const
{
    QVariantMap out;
    if (draft_) {
        for (const auto& [k, v] : draft_->values.fields)
            out.insert(k, v);
    }
    return out;
}

QVariantList ToolLibraryModel::rows() const
{
    QVariantList out;
    if (draft_) {
        for (const toolsui::Row& r : draft_->values.rows)
            out.push_back(rowMap(r));
    }
    return out;
}

QVariantList ToolLibraryModel::centreCuttingChoices() const
{
    lib::ToolType type = lib::ToolType::FlatEndMill;
    double tip = 0;
    if (draft_ && !draft_->holder) {
        const auto& f = draft_->values.fields;
        const auto at = [&f](const char* key) {
            const auto it = f.find(QLatin1String(key));
            return it == f.end() ? QString() : it->second;
        };
        type = api::cam_tools::toolTypeFromName(at("type").toStdString()).value_or(type);
        const auto d = toolsui::parseValue("length", at("tipDiameter"), units(), nullptr);
        tip = d && *d > 0 ? *d : 0;
    }
    QVariantList out;
    for (const toolsui::Choice& c : toolsui::centreCuttingChoices(type, tip))
        out.push_back(QVariantMap{{QStringLiteral("value"), c.value}, {QStringLiteral("text"), c.text}});
    return out;
}

QVariantMap ToolLibraryModel::problems() const
{
    QVariantMap out;
    if (draft_ && !draft_->readOnly) {
        for (const auto& [k, v] : checked_.problems)
            out.insert(k, v);
    }
    return out;
}

void ToolLibraryModel::updateFields()
{
    QVariantList out;
    QVariantList rows;
    if (draft_) {
        for (const toolsui::Field& f : draft_->holder ? toolsui::segmentFields() : toolsui::presetFields())
            rows.push_back(QVariantMap{{QStringLiteral("key"), QLatin1String(f.key)},
                                       {QStringLiteral("label"), trs(f.label)},
                                       {QStringLiteral("kind"), QLatin1String(f.kind)}});
    }
    if (!draft_) {
        if (!fields_.isEmpty() || !rowFields_.isEmpty()) {
            fields_.clear();
            rowFields_.clear();
            emit fieldsChanged();
        }
        return;
    }
    std::vector<toolsui::Field> list;
    if (draft_->holder) {
        list = toolsui::holderFields();
    } else {
        const auto type =
            api::cam_tools::toolTypeFromName(draft_->values.fields.count(QStringLiteral("type"))
                                                 ? draft_->values.fields.at(QStringLiteral("type")).toStdString()
                                                 : std::string());
        list = toolsui::toolFields(type.value_or(lib::ToolType::FlatEndMill));
    }
    for (const toolsui::Field& f : list)
        out.push_back(QVariantMap{{QStringLiteral("key"), QLatin1String(f.key)},
                                  {QStringLiteral("label"), trs(f.label)},
                                  {QStringLiteral("kind"), QLatin1String(f.kind)}});
    if (out != fields_ || rows != rowFields_) {
        fields_ = out;
        rowFields_ = rows;
        emit fieldsChanged();
    }
}

bool ToolLibraryModel::libraryChanged() const
{
    if (!draft_ || draft_->origin != QLatin1String("document"))
        return false;
    if (draft_->holder) {
        const lib::Holder* lh = libraryHolder(draft_->id);
        const lib::Holder* dh = documentHolder(draft_->id);
        return lh && dh && lh->geometryHash() != dh->geometryHash();
    }
    const lib::Tool* lt = libraryTool(draft_->id);
    const lib::Tool* dt = documentTool(draft_->id);
    return lt && dt && lt->geometryHash() != dt->geometryHash();
}

bool ToolLibraryModel::canUpdateFromLibrary() const
{
    if (!draft_ || draft_->origin != QLatin1String("document"))
        return false;
    api::IApp* a = app();
    if (a == nullptr || a->active() == nullptr || a->active()->readOnly)
        return false;
    if (draft_->holder) {
        const lib::Holder* lh = libraryHolder(draft_->id);
        const lib::Holder* dh = documentHolder(draft_->id);
        return lh && dh && api::cam_tools::contentHash(*lh) != api::cam_tools::contentHash(*dh);
    }
    const lib::Tool* lt = libraryTool(draft_->id);
    const lib::Tool* dt = documentTool(draft_->id);
    return lt && dt && lt->contentHash() != dt->contentHash();
}

bool ToolLibraryModel::hold()
{
    if (!dirty() && !saving_)
        return false;
    setStatus(saving_ ? trs("Wait until %1 is saved").arg(title())
                      : trs("Save or revert the changes to %1 first").arg(title()),
              true);
    return true;
}

bool ToolLibraryModel::select(const QString& origin, const QString& kind, const QString& id)
{
    if (draft_ && draft_->id == id && draft_->origin == origin)
        return true;
    if (hold())
        return false;
    Draft d;
    d.holder = kind == QLatin1String("holder");
    d.origin = origin;
    d.id = id;
    const toolsui::Units u = units();
    lib::LibraryStack* s = stack();
    if (origin == QLatin1String("document")) {
        d.readOnly = true;
        if (d.holder) {
            const lib::Holder* h = documentHolder(id);
            if (h == nullptr)
                return false;
            d.baseJson = parseObject(api::cam_tools::holderJson(*h));
            d.values = toolsui::holderValues(*h, u);
        } else {
            const lib::Tool* t = documentTool(id);
            if (t == nullptr)
                return false;
            d.baseJson = parseObject(api::cam_tools::toolJson(*t));
            d.values = toolsui::toolValues(*t, u);
            // The preview shows it in the holder an operation of the document uses it with.
            const doc::DocStatePtr state = docState();
            for (const auto& [opId, op] : state->cam->operations) {
                if (op.tool.tool.value == t->id.value && op.tool.holder.value != Uuid{}) {
                    previewHolder_ = idText(op.tool.holder.value);
                    break;
                }
            }
        }
    } else {
        if (s == nullptr)
            return false;
        const auto root =
            s->rootOf(d.holder ? lib::ItemKind::Holder : lib::ItemKind::Tool, Uuid::fromString(id.toStdString()));
        if (!root)
            return false;
        d.sourceRoot = static_cast<int>(*root);
        const bool writable =
            d.sourceRoot < roots_.size() && roots_[d.sourceRoot].toMap().value(QStringLiteral("writable")).toBool();
        d.readOnly = !writable;
        d.root = writable ? d.sourceRoot : -1;
        if (d.holder) {
            const lib::Holder* h = libraryHolder(id);
            if (h == nullptr)
                return false;
            d.baseJson = parseObject(api::cam_tools::holderJson(*h));
            d.values = toolsui::holderValues(*h, u);
            d.base = api::cam_tools::contentHash(*h);
        } else {
            const lib::Tool* t = libraryTool(id);
            if (t == nullptr)
                return false;
            d.baseJson = parseObject(api::cam_tools::toolJson(*t));
            d.values = toolsui::toolValues(*t, u);
            d.base = t->contentHash();
        }
    }
    open(std::move(d));
    return true;
}

void ToolLibraryModel::open(Draft draft)
{
    draft_ = std::move(draft);
    deleteAsked_ = false;
    conflict_.clear();
    status_.clear();
    statusIsError_ = false;
    recheck();
    updateFields();
    updateProfile();
    emit editorChanged();
    emit valuesChanged();
    emit statusChanged();
}

void ToolLibraryModel::clearEditor()
{
    draft_.reset();
    checked_ = {};
    updateFields();
    deleteAsked_ = false;
    conflict_.clear();
    if (!profile_.isEmpty()) {
        profile_.clear();
        profileKey_.clear();
        ++profileRevision_;
        emit profileChanged();
    }
    emit editorChanged();
    emit valuesChanged();
    emit statusChanged();
}

bool ToolLibraryModel::newItem(const QString& type)
{
    if (hold())
        return false;
    Draft d;
    d.origin = QStringLiteral("new");
    d.root = myRoot();
    const toolsui::Units u = units();
    if (type == QLatin1String("holder")) {
        lib::Holder h = newHolder();
        h.name = trs("New holder").toStdString();
        d.holder = true;
        d.id = idText(h.id.value);
        d.baseJson = parseObject(api::cam_tools::holderJson(h));
        d.values = toolsui::holderValues(h, u);
    } else {
        const auto t = api::cam_tools::toolTypeFromName(type.toStdString());
        const QString material =
            materials_.isEmpty() ? QString() : materials_.front().toMap().value(QStringLiteral("id")).toString();
        lib::Tool tool = newTool(t.value_or(lib::ToolType::FlatEndMill), u.inch, material);
        tool.name = trs("New %1").arg(typeTitle(type).toLower()).toStdString();
        d.id = idText(tool.id.value);
        d.baseJson = parseObject(api::cam_tools::toolJson(tool));
        d.values = toolsui::toolValues(tool, u);
    }
    open(std::move(d));
    if (draft_->root < 0)
        setStatus(trs("There is no library to save into: set a user library folder in Settings"), true);
    return true;
}

bool ToolLibraryModel::duplicate()
{
    if (!draft_ || saving_)
        return false;
    if (draft_->origin != QLatin1String("library") && draft_->origin != QLatin1String("document") && dirty()) {
        // A new item: save it first.
        return !hold();
    }
    if (dirty() && hold())
        return false;
    Draft d;
    d.holder = draft_->holder;
    d.origin = QStringLiteral("new");
    d.root = myRoot();
    const toolsui::Units u = units();
    if (d.holder) {
        auto h = api::cam_tools::holderFromJson(compact(draft_->baseJson).toStdString());
        if (!h)
            return false;
        h->id = HolderId{Uuid::generate()};
        h->name = trs("%1 (copy)").arg(qs(h->name)).toStdString();
        d.id = idText(h->id.value);
        d.baseJson = parseObject(api::cam_tools::holderJson(*h));
        d.values = toolsui::holderValues(*h, u);
    } else {
        auto t = api::cam_tools::toolFromJson(compact(draft_->baseJson).toStdString());
        if (!t)
            return false;
        t->id = ToolId{Uuid::generate()};
        t->name = trs("%1 (copy)").arg(qs(t->name)).toStdString();
        d.id = idText(t->id.value);
        d.baseJson = parseObject(api::cam_tools::toolJson(*t));
        d.values = toolsui::toolValues(*t, u);
    }
    open(std::move(d));
    if (draft_->root < 0)
        setStatus(trs("There is no library to save into: set a user library folder in Settings"), true);
    return true;
}

void ToolLibraryModel::setValue(const QString& key, const QString& text)
{
    if (!draft_ || draft_->readOnly)
        return;
    auto& fields = draft_->values.fields;
    if (fields.count(key) && fields.at(key) == text)
        return;
    fields[key] = text;
    deleteAsked_ = false;
    recheck();
    if (key == QLatin1String("type"))
        updateFields(); // other fields apply
    emit valuesChanged();
    updateProfile();
}

void ToolLibraryModel::setRowValue(int row, const QString& key, const QString& text)
{
    if (!draft_ || draft_->readOnly || row < 0 || row >= static_cast<int>(draft_->values.rows.size()))
        return;
    auto& fields = draft_->values.rows[static_cast<std::size_t>(row)].fields;
    if (fields.count(key) && fields.at(key) == text)
        return;
    fields[key] = text;
    recheck();
    emit valuesChanged();
    updateProfile();
}

void ToolLibraryModel::addRow()
{
    if (!draft_ || draft_->readOnly)
        return;
    const toolsui::Units u = units();
    if (draft_->holder) {
        draft_->values.rows.push_back(toolsui::newSegmentRow(u));
    } else {
        const auto type = api::cam_tools::toolTypeFromName(draft_->values.fields[QStringLiteral("type")].toStdString());
        // A material and kind of cut the tool has no preset for yet (two presets for one pair are not allowed):
        // the type's usual cut for the first material without it, else the first free pair.
        std::set<std::pair<QString, QString>> used;
        for (const toolsui::Row& r : draft_->values.rows)
            used.insert(
                {r.fields.count(QStringLiteral("material")) ? r.fields.at(QStringLiteral("material")) : QString(),
                 r.fields.count(QStringLiteral("opClass")) ? r.fields.at(QStringLiteral("opClass")) : QString()});
        toolsui::Row row = toolsui::newPresetRow(type.value_or(lib::ToolType::FlatEndMill), QString(), u);
        const QString usual = row.fields[QStringLiteral("opClass")];
        QString material, opClass = usual;
        bool found = false;
        for (const QVariant& m : materials_) {
            const QString id = m.toMap().value(QStringLiteral("id")).toString();
            if (!used.count({id, usual})) {
                material = id;
                found = true;
                break;
            }
        }
        for (std::size_t c = 0; !found && c < std::size(kOpClasses); ++c) {
            for (const QVariant& m : materials_) {
                const QString id = m.toMap().value(QStringLiteral("id")).toString();
                if (!used.count({id, QLatin1String(kOpClasses[c].name)})) {
                    material = id;
                    opClass = QLatin1String(kOpClasses[c].name);
                    found = true;
                    break;
                }
            }
        }
        row.fields[QStringLiteral("material")] = material;
        row.fields[QStringLiteral("opClass")] = opClass;
        draft_->values.rows.push_back(std::move(row));
    }
    recheck();
    emit valuesChanged();
    updateProfile();
}

void ToolLibraryModel::duplicateRow(int row)
{
    if (!draft_ || draft_->readOnly || row < 0 || row >= static_cast<int>(draft_->values.rows.size()))
        return;
    toolsui::Row copy = draft_->values.rows[static_cast<std::size_t>(row)];
    copy.shown.clear(); // a new row: the editor's
    draft_->values.rows.insert(draft_->values.rows.begin() + row + 1, std::move(copy));
    recheck();
    emit valuesChanged();
}

void ToolLibraryModel::removeRow(int row)
{
    if (!draft_ || draft_->readOnly || row < 0 || row >= static_cast<int>(draft_->values.rows.size()))
        return;
    draft_->values.rows.erase(draft_->values.rows.begin() + row);
    draft_->values.rowsEdited = true; // an edit even when the rest is as shown
    recheck();
    emit valuesChanged();
    updateProfile();
}

void ToolLibraryModel::recheck()
{
    if (!draft_) {
        checked_ = {};
        return;
    }
    const toolsui::Units u = units();
    checked_ = draft_->holder ? toolsui::checkHolder(draft_->values, draft_->baseJson, u)
                              : toolsui::checkTool(draft_->values, draft_->baseJson, u);
}

std::optional<lib::Tool> ToolLibraryModel::editedTool() const
{
    if (!draft_ || draft_->holder || !checked_.ok())
        return std::nullopt;
    auto t = api::cam_tools::toolFromJson(compact(checked_.json).toStdString());
    return t ? std::optional<lib::Tool>(std::move(*t)) : std::nullopt;
}

std::optional<lib::Holder> ToolLibraryModel::editedHolder() const
{
    if (!draft_ || !draft_->holder || !checked_.ok())
        return std::nullopt;
    auto h = api::cam_tools::holderFromJson(compact(checked_.json).toStdString());
    return h ? std::optional<lib::Holder>(std::move(*h)) : std::nullopt;
}

void ToolLibraryModel::setPreviewHolder(const QString& id)
{
    if (id == previewHolder_)
        return;
    previewHolder_ = id;
    updateProfile();
    emit profileChanged(); // the choice itself
}

void ToolLibraryModel::updateProfile()
{
    if (!draft_ || checked_.valid.isEmpty()) {
        return; // nothing valid to draw: the last drawing stays
    }
    const std::string json = compact(checked_.valid).toStdString();
    toolsui::Profile p;
    QString key;
    double diameter = 0;
    double length = 0;
    if (draft_->holder) {
        const auto h = api::cam_tools::holderFromJson(json);
        if (!h)
            return;
        key = QStringLiteral("holder:") + QString::fromStdString(h->geometryHash().hex());
        p = toolsui::holderProfile(*h);
        diameter = 2 * p.halfWidth;
        length = h->gaugeLength;
    } else {
        const auto t = api::cam_tools::toolFromJson(json);
        if (!t)
            return;
        const lib::Holder* holder = nullptr;
        if (!previewHolder_.isEmpty()) {
            holder = documentHolder(previewHolder_);
            if (holder == nullptr)
                holder = libraryHolder(previewHolder_);
        }
        key = QStringLiteral("tool:") + QString::fromStdString(t->geometryHash().hex()) + QLatin1Char('|') +
              (holder != nullptr ? QString::fromStdString(holder->geometryHash().hex()) : QString());
        p = toolsui::toolProfile(*t, holder);
        diameter = t->diameter;
        length = t->overallLength;
    }
    if (key == profileKey_)
        return;
    profileKey_ = key;
    profile_ = QVariantMap{{QStringLiteral("outline"), points(p.outline)}, {QStringLiteral("cutter"), points(p.cutter)},
                           {QStringLiteral("holder"), points(p.holder)},   {QStringLiteral("halfWidth"), p.halfWidth},
                           {QStringLiteral("height"), p.height},           {QStringLiteral("stickOut"), p.stickOut},
                           {QStringLiteral("diameter"), diameter},         {QStringLiteral("length"), length}};
    ++profileRevision_;
    emit profileChanged();
}

void ToolLibraryModel::setStatus(const QString& text, bool error)
{
    status_ = text;
    statusIsError_ = error;
    emit statusChanged();
}

void ToolLibraryModel::dismissStatus()
{
    if (status_.isEmpty())
        return;
    setStatus(QString());
}

void ToolLibraryModel::revert()
{
    if (!draft_ || saving_)
        return;
    conflict_.clear();
    deleteAsked_ = false;
    if (draft_->origin == QLatin1String("new")) {
        clearEditor();
        setStatus(QString());
        return;
    }
    Draft d = *draft_;
    const toolsui::Units u = units();
    if (d.holder) {
        if (auto h = api::cam_tools::holderFromJson(compact(d.baseJson).toStdString()))
            d.values = toolsui::holderValues(*h, u);
    } else if (auto t = api::cam_tools::toolFromJson(compact(d.baseJson).toStdString())) {
        d.values = toolsui::toolValues(*t, u);
    }
    open(std::move(d));
}

// ---------- saving ----------
bool ToolLibraryModel::save()
{
    return startSave(false, false);
}

bool ToolLibraryModel::startSave(bool overwrite, bool asCopy)
{
    if (!draft_ || draft_->readOnly || saving_)
        return false;
    if (!checked_.ok()) {
        setStatus(trs("Correct the fields marked in red first"), true);
        return false;
    }
    lib::LibraryStack* s = stack();
    api::IApp* a = app();
    const int root = asCopy ? myRoot() : draft_->root;
    std::shared_ptr<lib::ILibrary> library =
        s != nullptr && root >= 0 ? s->sharedLibrary(static_cast<std::size_t>(root)) : nullptr;
    if (library == nullptr || a == nullptr) {
        setConflict(Error{ErrorCode::IoError, trs("The library cannot be written now").toStdString(), {}},
                    QStringLiteral("save"));
        return false;
    }
    const bool holder = draft_->holder;
    std::optional<lib::Tool> tool;
    std::optional<lib::Holder> holderItem;
    QString id = draft_->id;
    std::optional<Hash256> base = asCopy ? std::nullopt : draft_->base;
    if (holder) {
        holderItem = editedHolder();
        if (!holderItem)
            return false;
        if (asCopy) {
            holderItem->id = HolderId{Uuid::generate()};
            holderItem->name = trs("%1 (copy)").arg(qs(holderItem->name)).toStdString();
            id = idText(holderItem->id.value);
        }
    } else {
        tool = editedTool();
        if (!tool)
            return false;
        if (asCopy) {
            tool->id = ToolId{Uuid::generate()};
            tool->name = trs("%1 (copy)").arg(qs(tool->name)).toStdString();
            id = idText(tool->id.value);
        }
    }
    saving_ = true;
    ++jobsRunning_;
    conflict_.clear();
    deleteAsked_ = false;
    emit statusChanged();
    emit valuesChanged();
    struct Saved {
        Status status;
    };
    JobHandle<Saved> job = a->jobs().submit<Saved>(
        JobInfo{"Saving to the tool library", JobPriority::Interactive, false},
        [library, tool, holderItem, base, overwrite](JobContext&) -> Result<Saved> {
            Saved out;
            if (tool)
                out.status = overwrite ? library->save(*tool) : api::cam_tools::saveTool(*library, *tool, base);
            else
                out.status =
                    overwrite ? library->save(*holderItem) : api::cam_tools::saveHolder(*library, *holderItem, base);
            return out;
        });
    QPointer<ToolLibraryModel> self(this);
    job.then([self, tool, holderItem, id, root, holder](Result<Saved> r) {
        if (!self)
            return;
        self->saving_ = false;
        --self->jobsRunning_;
        const Status st = r ? r->status : Status(tl::unexpected(r.error()));
        if (!st) {
            self->setConflict(st.error(), QStringLiteral("save"));
            emit self->valuesChanged();
            return;
        }
        // In the library now: the editor shows the saved item as the library's, the lists follow.
        if (lib::LibraryStack* s = self->stack())
            s->remerge();
        if (!self->catalogValid_)
            self->buildCatalog();
        Draft d;
        d.holder = holder;
        d.origin = QStringLiteral("library");
        d.id = id;
        d.root = root;
        d.sourceRoot = root;
        const toolsui::Units u = self->units();
        if (holder) {
            d.baseJson = parseObject(api::cam_tools::holderJson(*holderItem));
            d.values = toolsui::holderValues(*holderItem, u);
            d.base = api::cam_tools::contentHash(*holderItem);
        } else {
            d.baseJson = parseObject(api::cam_tools::toolJson(*tool));
            d.values = toolsui::toolValues(*tool, u);
            d.base = tool->contentHash();
        }
        // The stack may show another root's copy of the id (a higher root has it): say where it went.
        self->open(std::move(d));
        self->setStatus(trs("Saved to %1").arg(self->rootLabel(root)));
        emit self->saved(holder ? QStringLiteral("holder") : QStringLiteral("tool"), id);
    });
    return true;
}

void ToolLibraryModel::setConflict(const Error& error, const QString& action)
{
    const QString message = qs(error.message);
    const bool locked = error.code == ErrorCode::Locked;
    const bool changed = locked && message.contains(QStringLiteral("by someone else"));
    const QString kind = changed  ? QStringLiteral("changed")
                         : locked ? QStringLiteral("locked")
                                  : QStringLiteral("failed");
    const QString what = draft_ && draft_->holder ? trs("holder") : trs("tool");
    const int root = draft_ ? (draft_->root >= 0 ? draft_->root : draft_->sourceRoot) : -1;
    const QString library = rootLabel(root);
    QString file;
    if (!error.context.empty()) {
        file = qs(error.context.front());
    } else if (draft_ && root >= 0 && root < roots_.size()) {
        const QString dir = roots_[root].toMap().value(QStringLiteral("dir")).toString();
        file =
            dir + (draft_->holder ? QStringLiteral("/holders/%1.holder.json") : QStringLiteral("/tools/%1.tool.json"))
                      .arg(draft_->id);
    }
    QString heading;
    QString text;
    const QString name = draft_ ? title() : QString();
    if (kind == QLatin1String("changed")) {
        heading = trs("Someone else changed this %1").arg(what);
        text = action == QLatin1String("delete")
                   ? trs("%1 was changed in %2 after you opened it, so it was not deleted.").arg(name, library)
                   : trs("%1 was changed in %2 after you opened it, so your changes were not saved. They are still "
                         "here.")
                         .arg(name, library);
    } else if (kind == QLatin1String("locked")) {
        heading = trs("The library file is in use");
        text = trs("Another program has the file of %1 open in %2, so it could not be written. Your changes are still "
                   "here.")
                   .arg(name, library);
    } else {
        heading = action == QLatin1String("delete") ? trs("The %1 could not be deleted").arg(what)
                                                    : trs("The %1 could not be saved").arg(what);
        text = trs("%1 could not be written to %2. Your changes are still here.").arg(name, library);
    }
    conflict_ = QVariantMap{{QStringLiteral("kind"), kind},
                            {QStringLiteral("action"), action},
                            {QStringLiteral("title"), heading},
                            {QStringLiteral("message"), text},
                            {QStringLiteral("detail"), message},
                            {QStringLiteral("library"), library},
                            {QStringLiteral("file"), QDir::toNativeSeparators(file)},
                            {QStringLiteral("canCopy"), action == QLatin1String("save") && myRoot() >= 0}};
    status_.clear();
    emit statusChanged();
}

bool ToolLibraryModel::resolveConflict(const QString& choice)
{
    if (conflict_.isEmpty() || !draft_)
        return false;
    const QString action = conflict_.value(QStringLiteral("action")).toString();
    const bool changed = conflict_.value(QStringLiteral("kind")).toString() == QLatin1String("changed");
    if (choice == QLatin1String("cancel")) {
        conflict_.clear();
        emit statusChanged();
        emit valuesChanged();
        return true;
    }
    if (choice == QLatin1String("retry")) {
        conflict_.clear();
        if (action == QLatin1String("delete")) {
            lib::LibraryStack* s = stack();
            if (changed && s != nullptr) {
                // The library now holds the other version: that one is deleted.
                s->remerge();
                if (draft_->holder) {
                    if (const lib::Holder* h = libraryHolder(draft_->id))
                        draft_->base = api::cam_tools::contentHash(*h);
                } else if (const lib::Tool* t = libraryTool(draft_->id)) {
                    draft_->base = t->contentHash();
                }
            }
            return confirmDelete();
        }
        return startSave(changed, false);
    }
    if (choice == QLatin1String("copy") && action == QLatin1String("save")) {
        conflict_.clear();
        return startSave(false, true);
    }
    if (choice == QLatin1String("discard")) {
        conflict_.clear();
        const QString id = draft_->id;
        const bool holder = draft_->holder;
        // The edits go; the library's version (as it is on disk now) is shown.
        draft_.reset();
        if (lib::LibraryStack* s = stack())
            s->remerge();
        if (!catalogValid_)
            buildCatalog();
        if (!select(QStringLiteral("library"), holder ? QStringLiteral("holder") : QStringLiteral("tool"), id)) {
            clearEditor();
            setStatus(holder ? trs("The holder is no longer in the library")
                             : trs("The tool is no longer in the library"));
        } else {
            setStatus(trs("Your changes were discarded; this is the library's version"));
        }
        return true;
    }
    return false;
}

void ToolLibraryModel::askDelete()
{
    if (!draft_ || draft_->readOnly || saving_)
        return;
    if (draft_->origin == QLatin1String("new")) {
        revert(); // never saved: it just goes
        return;
    }
    deleteAsked_ = true;
    emit statusChanged();
}

void ToolLibraryModel::cancelDelete()
{
    if (!deleteAsked_)
        return;
    deleteAsked_ = false;
    emit statusChanged();
}

bool ToolLibraryModel::confirmDelete()
{
    if (!draft_ || draft_->readOnly || draft_->origin != QLatin1String("library") || !draft_->base || saving_)
        return false;
    lib::LibraryStack* s = stack();
    api::IApp* a = app();
    std::shared_ptr<lib::ILibrary> library =
        s != nullptr && draft_->root >= 0 ? s->sharedLibrary(static_cast<std::size_t>(draft_->root)) : nullptr;
    if (library == nullptr || a == nullptr)
        return false;
    deleteAsked_ = false;
    saving_ = true;
    ++jobsRunning_;
    emit statusChanged();
    const bool holder = draft_->holder;
    const QString id = draft_->id;
    const QString name = title();
    const int root = draft_->root;
    const Hash256 base = *draft_->base;
    JobHandle<bool> job =
        a->jobs().submit<bool>(JobInfo{"Deleting from the tool library", JobPriority::Interactive, false},
                               [library, holder, id, base](JobContext&) -> Result<bool> {
                                   const Uuid uuid = Uuid::fromString(id.toStdString());
                                   const Status st = holder
                                                         ? api::cam_tools::removeHolder(*library, HolderId{uuid}, base)
                                                         : api::cam_tools::removeTool(*library, ToolId{uuid}, base);
                                   if (!st)
                                       return tl::unexpected(st.error());
                                   return true;
                               });
    QPointer<ToolLibraryModel> self(this);
    job.then([self, name, root](Result<bool> r) {
        if (!self)
            return;
        self->saving_ = false;
        --self->jobsRunning_;
        if (!r) {
            self->setConflict(r.error(), QStringLiteral("delete"));
            return;
        }
        self->draft_.reset();
        if (lib::LibraryStack* s = self->stack())
            s->remerge();
        if (!self->catalogValid_)
            self->buildCatalog();
        self->clearEditor();
        self->setStatus(trs("Deleted %1 from %2").arg(name, self->rootLabel(root)));
    });
    return true;
}

// ---------- the document ----------
bool ToolLibraryModel::updateFromLibrary()
{
    if (!canUpdateFromLibrary())
        return false;
    api::IApp* a = app();
    QJsonObject args;
    if (draft_->holder) {
        args[QStringLiteral("holders")] =
            QJsonArray{jsonObject(api::cam_tools::holderJson(*libraryHolder(draft_->id)))};
    } else {
        args[QStringLiteral("tools")] = QJsonArray{jsonObject(api::cam_tools::toolJson(*libraryTool(draft_->id)))};
    }
    JobHandle<api::ActionResult> job = a->actions().invoke("cam.library.embed", jsonText(args), api::Caller::User);
    if (!job.valid())
        return false;
    ++jobsRunning_;
    QPointer<ToolLibraryModel> self(this);
    const QString id = draft_->id;
    const bool holder = draft_->holder;
    job.then([self, id, holder](Result<api::ActionResult> r) {
        if (!self)
            return;
        --self->jobsRunning_;
        if (!r) {
            self->setStatus(qs(r.error().message), true);
            return;
        }
        if (!self->catalogValid_)
            self->buildCatalog();
        // Show the document's copy as it is now.
        self->draft_.reset();
        self->select(QStringLiteral("document"), holder ? QStringLiteral("holder") : QStringLiteral("tool"), id);
        self->setStatus(trs("The document's copy now matches the library; its operations will be generated again"));
    });
    return true;
}

QVariantMap ToolLibraryModel::libraryArgsFor(const QString& toolId, const QString& holderId) const
{
    QVariantMap out;
    const doc::DocStatePtr state = docState();
    const cam::CamState* cam = state && state->cam ? state->cam.get() : nullptr;
    if (!toolId.isEmpty() &&
        (cam == nullptr || cam->tools.count(ToolId{Uuid::fromString(toolId.toStdString())}) == 0)) {
        if (const lib::Tool* t = libraryTool(toolId))
            out.insert(QStringLiteral("tools"), QVariantList{jsonObject(api::cam_tools::toolJson(*t)).toVariantMap()});
    }
    if (!holderId.isEmpty() &&
        (cam == nullptr || cam->holders.count(HolderId{Uuid::fromString(holderId.toStdString())}) == 0)) {
        if (const lib::Holder* h = libraryHolder(holderId))
            out.insert(QStringLiteral("holders"),
                       QVariantList{jsonObject(api::cam_tools::holderJson(*h)).toVariantMap()});
    }
    return out;
}

bool ToolLibraryModel::assignTool(const QString& operationId, const QString& toolId, const QString& holderId)
{
    api::IApp* a = app();
    if (a == nullptr || a->active() == nullptr || toolId.isEmpty())
        return false;
    if (documentTool(toolId) == nullptr && libraryTool(toolId) == nullptr) {
        setStatus(trs("The tool is in neither the document nor the library"), true);
        return false;
    }
    QJsonObject ref{{QStringLiteral("tool"), toolId}};
    ref[QStringLiteral("holder")] = holderId.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(holderId);
    QJsonObject args{{QStringLiteral("id"), operationId}, {QStringLiteral("tool"), ref}};
    const QVariantMap library = libraryArgsFor(toolId, holderId);
    if (!library.isEmpty())
        args[QStringLiteral("library")] = QJsonObject::fromVariantMap(library);
    JobHandle<api::ActionResult> job = a->actions().invoke("cam.operation.edit", jsonText(args), api::Caller::User);
    if (!job.valid())
        return false;
    ++jobsRunning_;
    QPointer<ToolLibraryModel> self(this);
    job.then([self](Result<api::ActionResult> r) {
        if (!self)
            return;
        --self->jobsRunning_;
        if (!r)
            self->setStatus(qs(r.error().message), true);
    });
    return true;
}

// ---------- ToolListModel ----------
ToolListModel::ToolListModel(QObject* parent) : QAbstractListModel(parent) {}

ToolListModel::~ToolListModel() = default;

void ToolListModel::componentComplete()
{
    if (source_)
        return;
    if (QQmlEngine* engine = qmlEngine(this))
        setSource(
            engine->singletonInstance<ToolLibraryModel*>(QStringLiteral("RayCad.Ui"), QStringLiteral("CamTools")));
}

void ToolListModel::setSource(ToolLibraryModel* source)
{
    if (source == source_)
        return;
    if (sourceConnection_)
        disconnect(sourceConnection_);
    source_ = source;
    if (source_) {
        source_->useCatalog();
        sourceConnection_ = connect(source_, &ToolLibraryModel::catalogChanged, this, [this] { refilter(); });
    }
    refilter();
}

void ToolListModel::setKind(const QString& kind)
{
    const bool holders = kind == QLatin1String("holders");
    if (holders == holders_)
        return;
    holders_ = holders;
    emit filterChanged();
    refilter();
}

void ToolListModel::setText(const QString& text)
{
    if (text == text_)
        return;
    text_ = text;
    emit filterChanged();
    refilter();
}

void ToolListModel::setType(const QString& type)
{
    if (type == type_)
        return;
    type_ = type;
    emit filterChanged();
    refilter();
}

void ToolListModel::setTypes(const QStringList& types)
{
    if (types == types_)
        return;
    types_ = types;
    emit filterChanged();
    refilter();
}

void ToolListModel::setMinDiameter(const QString& text)
{
    if (text == min_)
        return;
    min_ = text;
    emit filterChanged();
    refilter();
}

void ToolListModel::setMaxDiameter(const QString& text)
{
    if (text == max_)
        return;
    max_ = text;
    emit filterChanged();
    refilter();
}

void ToolListModel::setMaterial(const QString& id)
{
    if (id == material_)
        return;
    material_ = id;
    emit filterChanged();
    refilter();
}

QString ToolListModel::sort() const
{
    switch (sort_) {
    case toolsui::SortKey::Name:
        return QStringLiteral("name");
    case toolsui::SortKey::Type:
        return QStringLiteral("type");
    case toolsui::SortKey::Diameter:
        break;
    }
    return QStringLiteral("diameter");
}

void ToolListModel::setSort(const QString& key)
{
    const toolsui::SortKey k = key == QLatin1String("name")   ? toolsui::SortKey::Name
                               : key == QLatin1String("type") ? toolsui::SortKey::Type
                                                              : toolsui::SortKey::Diameter;
    if (k == sort_)
        return;
    sort_ = k;
    emit filterChanged();
    refilter();
}

void ToolListModel::setIncludeDocument(bool on)
{
    if (on == includeDocument_)
        return;
    includeDocument_ = on;
    emit filterChanged();
    refilter();
}

void ToolListModel::refilter()
{
    // Making the catalog (entries()) announces it, which asks again: this pass reads the new one anyway.
    if (refiltering_)
        return;
    refiltering_ = true;
    QElapsedTimer timer;
    timer.start();
    toolsui::Filter f;
    f.text = text_;
    f.type = type_;
    f.types = types_;
    f.material = material_;
    f.sort = sort_;
    QStringList bad;
    const auto bound = [&](const QString& text, double* out) {
        if (text.trimmed().isEmpty())
            return;
        const double mm = source_ ? source_->parseLength(text) : std::nan("");
        if (std::isfinite(mm))
            *out = mm;
        else
            bad << text.trimmed();
    };
    bound(min_, &f.minDiameter);
    bound(max_, &f.maxDiameter);
    const QString problem = bad.isEmpty() ? QString()
                                          : QCoreApplication::translate(toolsui::kTrContext, "Not a diameter: %1")
                                                .arg(bad.join(QStringLiteral(", ")));
    beginResetModel();
    rows_.clear();
    total_ = 0;
    if (source_) {
        const std::vector<toolsui::Entry>& all = source_->entries(holders_);
        total_ = static_cast<int>(all.size());
        // Sorted once per catalog, list and key; a key stroke only filters.
        if (orderRevision_ != source_->revision() || orderHolders_ != holders_ || orderSort_ != sort_ ||
            order_.size() != all.size()) {
            order_ = toolsui::sortEntries(all, sort_);
            orderRevision_ = source_->revision();
            orderHolders_ = holders_;
            orderSort_ = sort_;
        }
        rows_ = toolsui::filterEntries(all, f, &order_);
        if (!includeDocument_)
            std::erase_if(rows_, [&all](int i) { return all[static_cast<std::size_t>(i)].document; });
    }
    endResetModel();
    ++filterRuns_;
    lastFilterMs_ = static_cast<double>(timer.nsecsElapsed()) / 1e6;
    refiltering_ = false;
    if (problem != diameterProblem_) {
        diameterProblem_ = problem;
        emit filterChanged();
    }
    emit countChanged();
}

const toolsui::Entry* ToolListModel::entryAt(int row) const
{
    if (!source_ || row < 0 || row >= static_cast<int>(rows_.size()))
        return nullptr;
    // The rows index the catalog they were filtered from; it is made anew only before a refilter (never from a view's
    // read of a row, which would reset the model under the view).
    const std::vector<toolsui::Entry>& all = source_->cachedEntries(holders_);
    const int i = rows_[static_cast<std::size_t>(row)];
    return i < static_cast<int>(all.size()) ? &all[static_cast<std::size_t>(i)] : nullptr;
}

int ToolListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant ToolListModel::data(const QModelIndex& index, int role) const
{
    const toolsui::Entry* e = entryAt(index.row());
    if (e == nullptr)
        return {};
    switch (role) {
    case IdRole:
        return e->id;
    case OriginRole:
        return e->document ? QStringLiteral("document") : QStringLiteral("library");
    case GroupRole:
        return e->document ? QCoreApplication::translate(toolsui::kTrContext, "In this document")
                           : QCoreApplication::translate(toolsui::kTrContext, "Library");
    case Qt::DisplayRole:
    case NameRole:
        return e->name;
    case TypeRole:
        return e->type;
    case TypeTextRole:
        return e->holder ? QCoreApplication::translate(toolsui::kTrContext, "Holder") : source_->typeTitle(e->type);
    case DiameterTextRole:
        return source_->formatLength(e->diameter);
    case FlutesRole:
        return e->flutes;
    case RootNameRole:
        return source_->rootLabel(e->root);
    case ReadOnlyRole:
        return e->readOnly;
    case LibraryChangedRole:
        return e->libraryChanged;
    default:
        return {};
    }
}

QHash<int, QByteArray> ToolListModel::roleNames() const
{
    return {{IdRole, "itemId"},
            {OriginRole, "origin"},
            {GroupRole, "group"},
            {NameRole, "name"},
            {TypeRole, "type"},
            {TypeTextRole, "typeText"},
            {DiameterTextRole, "diameterText"},
            {FlutesRole, "flutes"},
            {RootNameRole, "rootName"},
            {ReadOnlyRole, "readOnly"},
            {LibraryChangedRole, "libraryChanged"}};
}

QVariantMap ToolListModel::get(int row) const
{
    QVariantMap out;
    const QHash<int, QByteArray> names = roleNames();
    for (auto it = names.begin(); it != names.end(); ++it)
        out.insert(QString::fromLatin1(it.value()), data(index(row, 0), it.key()));
    return out;
}

int ToolListModel::rowOf(const QString& origin, const QString& id) const
{
    const bool document = origin == QLatin1String("document");
    for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
        const toolsui::Entry* e = entryAt(i);
        if (e != nullptr && e->id == id && e->document == document)
            return i;
    }
    return -1;
}

} // namespace ray::ui
