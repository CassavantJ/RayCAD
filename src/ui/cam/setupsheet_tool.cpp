#include "setupsheet_tool.h"

#include "../app_context.h"
#include "../model/model_util.h"
#include "../model/preview.h"
#include "../palette_commands.h"
#include "../scene_controller.h"
#include "../session.h"
#include "../ui_commands.h"
#include "../viewport_item.h"
#include "post_tool.h" // PostToolModel::homeRelative
#include "setup_logic.h"
#include "setup_tool.h"
#include "setup_tree.h"

#include <ray_api/cam_setup.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QDate>
#include <QtCore/QDir>
#include <QtCore/QUrl>
#include <QtQuick/QQuickImageProvider>

#include <algorithm>
#include <cmath>

namespace ray::ui {

using namespace modelui;
namespace cs = api::cam_setupsheet;
namespace cp = api::cam_post;

namespace {
// T-076: the setup sheet. Its key works in the CAM workspace only (CommandContext::Cam).
// clang-format off
constexpr UiCommand kCamSheetCommands[] = {
    {"cam.setupSheet.open", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Setup Sheet…"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "CAM"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Makes the setup sheet of the setup selected in the CAM tree for the operator: preview it, then save it as a PDF or an HTML file"), "Ctrl+Shift+P", CommandContext::Cam, ""},
};
// clang-format on

constexpr int kPictureWidth = 960; // the picture's pixels: 3:2, twice the size it prints at (360 x 240 CSS px)
constexpr int kPictureHeight = 640;
constexpr double kPreviewPpi = 144; // the preview's pages: 1.5 times the layout's 96 per inch
constexpr int kPictureTimeoutMs = 10000;

QString trs(const char* text)
{
    return QCoreApplication::translate("ray::ui::SetupSheetModel", text);
}

const cam::Setup* findSetup(const doc::DocState& state, const SetupId& id)
{
    if (!state.cam)
        return nullptr;
    const auto it = std::find_if(state.cam->setups.begin(), state.cam->setups.end(),
                                 [&](const cam::Setup& s) { return s.id == id; });
    return it == state.cam->setups.end() ? nullptr : &*it;
}

QString reasonText(cp::BlockReason r)
{
    switch (r) {
    case cp::BlockReason::NotBuilt:
        return trs("Not built");
    case cp::BlockReason::NoSetup:
        return trs("No setup");
    case cp::BlockReason::SetupBroken:
        return trs("Setup does not resolve");
    case cp::BlockReason::NoMachine:
        return trs("No machine");
    case cp::BlockReason::NoOperations:
        return trs("Nothing to describe");
    case cp::BlockReason::Stale:
        return trs("Stale");
    case cp::BlockReason::Generating:
        return trs("Still generating");
    case cp::BlockReason::Failed:
        return trs("Failed");
    case cp::BlockReason::ReferenceBroken:
        return trs("Broken reference");
    case cp::BlockReason::NoToolpath:
        return trs("No toolpath");
    case cp::BlockReason::NoTool:
        return trs("No tool");
    }
    return {};
}

// The camera that looks at the setup's stock as the machinist sees the part on the machine: from the front right and
// above (WCS +X, -Y, +Z), Z up, orthographic, the stock box filling about three quarters of the picture.
render::Camera machineView(const cs::View& v, const render::Camera& current, double aspect)
{
    const Vec3 toEye = unit(add(add(v.x, mul(v.y, -1.0)), v.z));
    const Vec3 forward = mul(toEye, -1.0);
    const Vec3 right = unit(cross(forward, v.z));
    const Vec3 up = cross(right, forward);
    Vec3 centre{};
    for (const Vec3& c : v.corners)
        centre = add(centre, mul(c, 1.0 / static_cast<double>(v.corners.size())));
    double halfRight = 0, halfUp = 0, radius = 1;
    for (const Vec3& c : v.corners) {
        const Vec3 d = sub(c, centre);
        halfRight = std::max(halfRight, std::fabs(dot(d, right)));
        halfUp = std::max(halfUp, std::fabs(dot(d, up)));
        radius = std::max(radius, length(d));
    }
    render::Camera cam = current;
    cam.perspective = false;
    cam.target = centre;
    cam.eye = add(centre, mul(toEye, 4.0 * radius));
    cam.up = up;
    cam.orthoHeight = std::max(1.0, 1.3 * std::max(2.0 * halfUp, 2.0 * halfRight / aspect));
    return cam;
}

// The preview's pages for QML: image://setupsheet/<revision>/<page>.
class SheetImageProvider final : public QQuickImageProvider {
public:
    explicit SheetImageProvider(std::shared_ptr<SheetPages> pages)
        : QQuickImageProvider(QQuickImageProvider::Image), pages_(std::move(pages))
    {
    }
    QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override
    {
        const int page = id.section(QLatin1Char('/'), 1, 1).toInt();
        QImage image;
        {
            std::lock_guard<std::mutex> lock(pages_->mutex);
            if (page >= 0 && static_cast<std::size_t>(page) < pages_->images.size())
                image = pages_->images[static_cast<std::size_t>(page)];
        }
        if (image.isNull()) {
            image = QImage(1, 1, QImage::Format_RGB32);
            image.fill(Qt::white); // paper
        }
        if (requestedSize.width() > 0 && requestedSize.width() < image.width())
            image = image.scaledToWidth(requestedSize.width(), Qt::SmoothTransformation);
        if (size != nullptr)
            *size = image.size();
        return image;
    }

private:
    std::shared_ptr<SheetPages> pages_;
};

} // namespace

std::span<const UiCommand> camSheetUiCommands()
{
    return kCamSheetCommands;
}

// ---------- construction ----------
SetupSheetModel::SetupSheetModel(SceneController* scene, Session* session, AppContext* context, QObject* parent)
    : QObject(parent), scene_(scene), session_(session), context_(context),
      overlay_(std::make_unique<LineOverlay>(scene)), pages_(std::make_shared<SheetPages>())
{
    paper_ = camui::defaultPaper(QLocale::system());
    refreshTimer_.setSingleShot(true);
    refreshTimer_.setInterval(0);
    connect(&refreshTimer_, &QTimer::timeout, this, [this] { refresh(); });
    pictureTimeout_.setSingleShot(true);
    connect(&pictureTimeout_, &QTimer::timeout, this, [this] {
        // The picture did not come (no frame was drawn): the sheet goes without it.
        if (!pendingViews_.empty())
            pendingViews_.pop_front();
        nextPicture(serial_);
    });
    if (session_) {
        connect(session_, &Session::documentChanged, this, [this] {
            if (session_ && session_->documentId().toStdString() != docId_ && active())
                close(); // the document closed or another became active
        });
    }
}

SetupSheetModel::~SetupSheetModel()
{
    if (commands_)
        commands_->setUiHandler(QStringLiteral("cam.setupSheet."), {});
    statusJob_.cancel();
    setupJob_.cancel();
    viewsJob_.cancel();
    makeJob_.cancel();
    layoutJob_.cancel();
    writeJob_.cancel();
    endPictures();
    unsubscribe();
}

SetupSheetModel* SetupSheetModel::create(QQmlEngine* qml, QJSEngine*)
{
    SceneController* scene = nullptr;
    Session* session = nullptr;
    AppContext* context = nullptr;
    CommandCatalog* commands = nullptr;
    SetupTreeModel* tree = nullptr;
    SetupToolModel* setupTool = nullptr;
    if (qml != nullptr) {
        const QString uri = QStringLiteral("RayCad.Ui");
        scene = qml->singletonInstance<SceneController*>(uri, QStringLiteral("Scene"));
        session = qml->singletonInstance<Session*>(uri, QStringLiteral("Session"));
        context = qml->singletonInstance<AppContext*>(uri, QStringLiteral("AppContext"));
        commands = qml->singletonInstance<CommandCatalog*>(uri, QStringLiteral("Commands"));
        tree = qml->singletonInstance<SetupTreeModel*>(uri, QStringLiteral("CamTreeModel"));
        setupTool = qml->singletonInstance<SetupToolModel*>(uri, QStringLiteral("CamSetup"));
    }
    auto* model = new SetupSheetModel(scene, session, context);
    model->setTree(tree);
    model->setSetupTool(setupTool);
    model->setCommands(commands);
    if (qml != nullptr)
        qml->addImageProvider(QStringLiteral("setupsheet"), new SheetImageProvider(model->pages_));
    return model; // owned by the engine
}

void SetupSheetModel::setCommands(CommandCatalog* commands)
{
    commands_ = commands;
    if (!commands_)
        return;
    QPointer<SetupSheetModel> self(this);
    commands_->setUiHandler(
        QStringLiteral("cam.setupSheet."),
        CommandCatalog::UiHandler{[self](const QString& id) {
                                      return self ? self->commandReason(id) : trs("Setup sheets are not available");
                                  },
                                  [self](const QString& id) { return self && self->runCommand(id); }});
}

void SetupSheetModel::setTree(SetupTreeModel* tree)
{
    tree_ = tree;
}

void SetupSheetModel::setSetupTool(SetupToolModel* tool)
{
    setupTool_ = tool;
}

QObject* SetupSheetModel::view() const
{
    return view_;
}

void SetupSheetModel::setView(QObject* view)
{
    auto* item = qobject_cast<ViewportItem*>(view);
    if (item == view_)
        return;
    view_ = item;
    emit viewChanged();
}

api::IApp* SetupSheetModel::app() const
{
    return context_ ? context_->app() : nullptr;
}

doc::Document* SetupSheetModel::liveDoc() const
{
    api::IApp* a = app();
    if (a == nullptr || docId_.empty() || doc_ == nullptr)
        return nullptr;
    api::OpenDocument* od = a->findDocument(docId_);
    return od != nullptr && od->document.get() == doc_ ? doc_ : nullptr;
}

void SetupSheetModel::subscribe()
{
    unsubscribe();
    api::IApp* a = app();
    api::OpenDocument* od = a != nullptr ? a->active() : nullptr;
    if (od == nullptr || !od->document)
        return;
    docId_ = od->id;
    doc_ = od->document.get();
    const std::uint64_t generation = ++docGeneration_;
    QPointer<SetupSheetModel> self(this);
    docListener_ = doc_->subscribe([self, generation](doc::DocStatePtr, doc::BuildResultPtr) {
        if (self && self->docGeneration_ == generation && self->active()) {
            self->loadChoices();
            self->scheduleRefresh(false);
        }
    });
    engine_ = a->cam();
    if (engine_ != nullptr) {
        engineListener_ = engine_->subscribe([self](const OperationId&) {
            // A generation started, progressed or ended: the gate and the sheet again, coalesced (progress comes at
            // 30 Hz).
            if (self && self->active() && !self->refreshTimer_.isActive()) {
                self->refreshTimer_.setInterval(150);
                self->refreshTimer_.start();
            }
        });
    }
}

void SetupSheetModel::unsubscribe()
{
    if (doc::Document* d = liveDoc(); d != nullptr && docListener_ != 0)
        d->unsubscribe(docListener_);
    if (engine_ != nullptr && engineListener_ != 0 && app() != nullptr && app()->cam() == engine_)
        engine_->unsubscribe(engineListener_);
    engine_ = nullptr;
    engineListener_ = 0;
    doc_ = nullptr;
    docListener_ = 0;
    docId_.clear();
    ++docGeneration_;
}

std::vector<std::string> SetupSheetModel::libraryRoots(bool shippedOnly) const
{
    std::vector<std::string> roots;
    api::IApp* a = app();
    if (lib::LibraryStack* stack = a ? a->libraries() : nullptr) {
        for (const lib::LibraryStack::RootInfo& r : stack->roots()) {
            // The library shipped with RayCAD is the read-only root (LibraryStack::setRoots): open, not writable.
            if (!shippedOnly || (r.open && !r.writable))
                roots.push_back(r.dir);
        }
    }
    return roots;
}

QString SetupSheetModel::documentName() const
{
    api::IApp* a = app();
    api::OpenDocument* od = a ? a->findDocument(docId_) : nullptr;
    QString title = od ? qs(od->title) : QString();
    if (title.endsWith(QLatin1String(".raycad"), Qt::CaseInsensitive))
        title.chop(7);
    return title;
}

// ---------- open / close ----------
QString SetupSheetModel::title() const
{
    if (!active())
        return trs("Setup sheet");
    if (choice_ == QLatin1String("all"))
        return trs("Setup sheets: every setup");
    for (const QVariant& c : choices_) {
        const QVariantMap m = c.toMap();
        if (m.value(QStringLiteral("value")).toString() == choice_)
            return trs("Setup sheet: %1").arg(m.value(QStringLiteral("text")).toString());
    }
    return trs("Setup sheet");
}

QString SetupSheetModel::sheetTitle() const
{
    const QString doc = documentName();
    if (choice_ == QLatin1String("all"))
        return doc.isEmpty() ? trs("Setup sheets") : trs("%1: setup sheets").arg(doc);
    doc::Document* d = liveDoc();
    const doc::DocStatePtr state = d ? d->state() : nullptr;
    const std::vector<SetupId> setups = chosenSetups();
    const cam::Setup* s = state && !setups.empty() ? findSetup(*state, setups.front()) : nullptr;
    if (s == nullptr)
        return doc;
    const QString what = trs("%1, program %2").arg(qs(s->name)).arg(s->programNumber);
    return doc.isEmpty() ? what : doc + QStringLiteral(": ") + what;
}

bool SetupSheetModel::open(const QString& setupId)
{
    if (busy() || app() == nullptr)
        return false;
    if (active())
        close();
    subscribe();
    doc::Document* d = liveDoc();
    const doc::DocStatePtr state = d ? d->state() : nullptr;
    const SetupId id{Uuid::fromString(setupId.toStdString())};
    if (state == nullptr || findSetup(*state, id) == nullptr) {
        unsubscribe();
        return false;
    }
    setup_ = id;
    choice_ = idText(id.value);
    loadChoices();
    values_.clear();
    QString folder = lastFolder_;
    if (folder.isEmpty())
        folder = qs(cs::rememberedFolder(app()->userDataDir(), id));
    values_[QStringLiteral("directory")] = folder;
    stemEdited_ = false;
    pictures_.clear();
    html_.clear();
    {
        std::lock_guard<std::mutex> lock(pages_->mutex);
        pages_->images.clear();
    }
    pageCount_ = 0;
    ++revision_;
    blockers_.clear();
    messages_.clear();
    resultPath_.clear();
    existingName_.clear();
    state_ = QStringLiteral("idle");
    statusText_.clear();
    updateName(true);
    emit activeChanged();
    emit choiceChanged();
    emit paperChanged();
    emit pagesChanged();
    emit stateChanged();
    scheduleRefresh(true);
    return true;
}

void SetupSheetModel::close()
{
    if (!active())
        return;
    ++serial_;
    ++saveSerial_;
    statusJob_.cancel();
    setupJob_.cancel();
    viewsJob_.cancel();
    makeJob_.cancel();
    layoutJob_.cancel();
    writeJob_.cancel();
    refreshTimer_.stop();
    endPictures();
    setup_.reset();
    blockers_.clear();
    state_ = QStringLiteral("idle");
    unsubscribe();
    emit activeChanged();
    emit stateChanged();
}

void SetupSheetModel::loadChoices()
{
    QVariantList list;
    doc::Document* d = liveDoc();
    const doc::DocStatePtr state = d ? d->state() : nullptr;
    if (state && state->cam) {
        for (const cam::Setup& s : state->cam->setups)
            list.push_back(QVariantMap{{QStringLiteral("value"), idText(s.id.value)},
                                       {QStringLiteral("text"), s.name.empty() ? trs("Setup") : qs(s.name)}});
        if (state->cam->setups.size() > 1)
            list.push_back(QVariantMap{
                {QStringLiteral("value"), QStringLiteral("all")},
                {QStringLiteral("text"), trs("Every setup, in one document (%1)").arg(state->cam->setups.size())}});
    }
    if (list == choices_)
        return;
    choices_ = list;
    // The chosen setup was removed: back to the one the dialog was opened for, else the first.
    bool found = false;
    for (const QVariant& c : choices_)
        found = found || c.toMap().value(QStringLiteral("value")).toString() == choice_;
    if (!found && !choices_.isEmpty())
        choice_ = choices_.front().toMap().value(QStringLiteral("value")).toString();
    emit choiceChanged();
}

std::vector<SetupId> SetupSheetModel::chosenSetups() const
{
    std::vector<SetupId> out;
    doc::Document* d = liveDoc();
    const doc::DocStatePtr state = d ? d->state() : nullptr;
    if (!state || !state->cam)
        return out;
    for (const cam::Setup& s : state->cam->setups) {
        if (choice_ == QLatin1String("all") || idText(s.id.value) == choice_)
            out.push_back(s.id);
    }
    return out;
}

void SetupSheetModel::setChoice(const QString& value)
{
    if (!active() || busy() || value == choice_)
        return;
    choice_ = value;
    stemEdited_ = false;
    updateName(true);
    emit choiceChanged();
    scheduleRefresh(false); // pictures are taken for the setups that have none yet
}

void SetupSheetModel::setPaper(const QString& key)
{
    const camui::Paper paper = camui::paperFromKey(key);
    if (paper == paper_)
        return;
    paper_ = paper;
    emit paperChanged();
    emit pagesChanged(); // the aspect
    if (active() && !html_.empty() && !busy())
        layOut(serial_);
}

// ---------- values ----------
void SetupSheetModel::setValue(const QString& key, const QVariant& value)
{
    if (!active() || busy() || values_.value(key) == value)
        return;
    values_[key] = value;
    if (key == QLatin1String("stem"))
        stemEdited_ = true;
    updateName(false);
    emit valuesChanged();
    emit stateChanged(); // canSave
}

void SetupSheetModel::setFolder(const QVariant& urlOrPath)
{
    QString path;
    if (urlOrPath.canConvert<QUrl>() && urlOrPath.toUrl().isLocalFile())
        path = urlOrPath.toUrl().toLocalFile();
    else
        path = urlOrPath.toString();
    if (path.startsWith(QLatin1String("file:")))
        path = QUrl(path).toLocalFile();
    setValue(QStringLiteral("directory"), QDir::toNativeSeparators(path));
}

QString SetupSheetModel::directoryShown() const
{
    return PostToolModel::homeRelative(values_.value(QStringLiteral("directory")).toString(), QDir::homePath());
}

void SetupSheetModel::updateName(bool resetStem)
{
    QString note;
    doc::Document* d = liveDoc();
    const doc::DocStatePtr state = d ? d->state() : nullptr;
    if (resetStem && state && app() != nullptr) {
        auto f = cs::fileNameFor(*state, chosenSetups(), ss(documentName()), ".pdf", app()->userDataDir());
        values_[QStringLiteral("stem")] = f ? qs(f->stem) : QString();
        if (!f)
            note = qs(f.error().message);
    } else {
        const cp::FileName f =
            cs::fileNameFromStem(ss(values_.value(QStringLiteral("stem")).toString().trimmed()), ".pdf");
        QStringList notes;
        for (const std::string& n : f.notes)
            notes.push_back(qs(n));
        note = notes.join(QLatin1Char(' '));
    }
    nameNote_ = note;
    emit valuesChanged();
}

QString SetupSheetModel::pdfName() const
{
    const QString stem = values_.value(QStringLiteral("stem")).toString().trimmed();
    return stem.isEmpty() ? QString() : qs(cs::fileNameFromStem(ss(stem), ".pdf").name);
}

QString SetupSheetModel::htmlName() const
{
    const QString stem = values_.value(QStringLiteral("stem")).toString().trimmed();
    return stem.isEmpty() ? QString() : qs(cs::fileNameFromStem(ss(stem), ".html").name);
}

double SetupSheetModel::pageAspect() const
{
    return paper_ == camui::Paper::Letter ? 11.0 / 8.5 : 297.0 / 210.0;
}

// ---------- state ----------
bool SetupSheetModel::busy() const
{
    return state_ == QLatin1String("saving");
}

void SetupSheetModel::setState(const QString& state, const QString& status)
{
    state_ = state;
    statusText_ = status;
    emit stateChanged();
}

QVariantList SetupSheetModel::blockers() const
{
    QVariantList out;
    for (const cp::Blocker& b : blockers_) {
        out.push_back(QVariantMap{{QStringLiteral("opId"), (b.op.value == Uuid{}) ? QString() : idText(b.op.value)},
                                  {QStringLiteral("name"), b.name.empty() ? trs("Setup") : qs(b.name)},
                                  {QStringLiteral("reasonText"), reasonText(b.reason)},
                                  {QStringLiteral("message"), qs(b.message)},
                                  {QStringLiteral("canGenerate"), b.generateFixes}});
    }
    return out;
}

QString SetupSheetModel::saveBlockedReason() const
{
    if (!active())
        return trs("Choose a setup");
    if (busy())
        return trs("Saving…");
    if (state_ == QLatin1String("asking"))
        return trs("Answer the question about the existing file first");
    if (!blockers_.empty())
        return trs("The sheet is blocked: generate the toolpaths it lists first");
    if (state_ == QLatin1String("checking") || state_ == QLatin1String("picturing") ||
        state_ == QLatin1String("making") || state_ == QLatin1String("idle"))
        return trs("Making the sheet…");
    if (html_.empty())
        return trs("The sheet could not be made");
    if (values_.value(QStringLiteral("directory")).toString().trimmed().isEmpty())
        return trs("Choose the folder to save the setup sheet in");
    if (pdfName().isEmpty())
        return trs("Type a file name");
    return {};
}

void SetupSheetModel::fail(const Error& error, const QString& status)
{
    std::size_t from = 0;
    const std::string& text = error.message;
    while (from <= text.size()) {
        const std::size_t nl = text.find('\n', from);
        const std::string line = text.substr(from, nl == std::string::npos ? std::string::npos : nl - from);
        if (!line.empty())
            messages_.push_back(
                QVariantMap{{QStringLiteral("kind"), QStringLiteral("error")}, {QStringLiteral("text"), qs(line)}});
        if (nl == std::string::npos)
            break;
        from = nl + 1;
    }
    setState(QStringLiteral("failed"), status);
}

// ---------- the refresh: gate, pictures, HTML, pages ----------
void SetupSheetModel::scheduleRefresh(bool pictures)
{
    wantPictures_ = wantPictures_ || pictures;
    refreshTimer_.setInterval(0);
    refreshTimer_.start();
    // What is shown is out of date from now on (a save in progress keeps its own state).
    if (active() && !busy() && state_ != QLatin1String("asking") && state_ != QLatin1String("checking"))
        setState(QStringLiteral("checking"), trs("Checking the operations…"));
}

void SetupSheetModel::retakePictures()
{
    if (!active() || busy())
        return;
    scheduleRefresh(true);
}

void SetupSheetModel::refresh()
{
    doc::Document* d = liveDoc();
    IJobSystem* jobs = scene_ ? scene_->jobs() : nullptr;
    cam::ICamEngine* engine = app() ? app()->cam() : nullptr;
    if (!active() || d == nullptr || jobs == nullptr || engine == nullptr)
        return;
    if (busy() || state_ == QLatin1String("asking")) {
        // Not under a save: again when it is over.
        refreshTimer_.setInterval(500);
        refreshTimer_.start();
        return;
    }
    const std::uint64_t serial = ++serial_;
    statusJob_.cancel();
    setupJob_.cancel();
    viewsJob_.cancel();
    makeJob_.cancel();
    layoutJob_.cancel();
    endPictures();
    messages_.clear();
    setState(QStringLiteral("checking"), trs("Checking the operations…"));
    const doc::DocStatePtr state = d->state();
    const doc::BuildResultPtr build = d->build();
    statusJob_ = engine->statusAsync(state, build);
    setupJob_ = jobs->submit<std::shared_ptr<const std::map<SetupId, Status>>>(
        JobInfo{"Setup sheet check", JobPriority::Interactive, false},
        [state, build](JobContext& ctx) -> Result<std::shared_ptr<const std::map<SetupId, Status>>> {
            return std::make_shared<const std::map<SetupId, Status>>(api::cam_setup::setupStates(state, build, ctx));
        });
    auto statuses = std::make_shared<std::shared_ptr<const std::map<OperationId, cam::OpState>>>();
    auto setups = std::make_shared<std::shared_ptr<const std::map<SetupId, Status>>>();
    QPointer<SetupSheetModel> self(this);
    const auto both = [self, serial, state, build, statuses, setups] {
        if (!self || serial != self->serial_ || !*statuses || !*setups || !self->active())
            return;
        self->blockers_ = cs::blockers(*state, build != nullptr, self->chosenSetups(), **statuses, **setups);
        if (!self->blockers_.empty()) {
            self->html_.clear();
            {
                std::lock_guard<std::mutex> lock(self->pages_->mutex);
                self->pages_->images.clear();
            }
            self->pageCount_ = 0;
            ++self->revision_;
            emit self->pagesChanged();
            self->setState(QStringLiteral("blocked"),
                           trs("The sheet describes the program as it would be posted: generate the toolpaths first"));
            return;
        }
        self->afterGate(state, build, *statuses);
    };
    statusJob_.then([self, serial, statuses, both](Result<std::map<OperationId, cam::OpState>> r) {
        if (!self || serial != self->serial_ || !r)
            return;
        *statuses = std::make_shared<const std::map<OperationId, cam::OpState>>(std::move(*r));
        both();
    });
    setupJob_.then([self, serial, setups, both](Result<std::shared_ptr<const std::map<SetupId, Status>>> r) {
        if (!self || serial != self->serial_ || !r)
            return;
        *setups = *r;
        both();
    });
}

void SetupSheetModel::afterGate(doc::DocStatePtr state, doc::BuildResultPtr build,
                                std::shared_ptr<const std::map<OperationId, cam::OpState>> statuses)
{
    madeState_ = state;
    madeBuild_ = build;
    madeStatuses_ = std::move(statuses);
    const std::vector<SetupId> setups = chosenSetups();
    bool missing = false;
    for (const SetupId& id : setups)
        missing = missing || pictures_.find(id) == pictures_.end();
    IJobSystem* jobs = scene_ ? scene_->jobs() : nullptr;
    const bool canPicture = view_ && view_->viewport() != nullptr && jobs != nullptr;
    if (!canPicture || (!wantPictures_ && !missing)) {
        make(serial_);
        return;
    }
    wantPictures_ = false;
    setState(QStringLiteral("picturing"), trs("Taking the pictures…"));
    const std::uint64_t serial = serial_;
    viewsJob_ =
        jobs->submit<std::vector<cs::View>>(JobInfo{"Setup sheet pictures", JobPriority::Interactive, false},
                                            [state, build, setups](JobContext& ctx) -> Result<std::vector<cs::View>> {
                                                return cs::views(state, build, setups, ctx);
                                            });
    QPointer<SetupSheetModel> self(this);
    viewsJob_.then([self, serial](Result<std::vector<cs::View>> r) {
        if (!self || serial != self->serial_)
            return;
        if (!r) {
            self->make(serial);
            return;
        }
        self->takePictures(std::move(*r), serial);
    });
}

void SetupSheetModel::takePictures(std::vector<cs::View> views, std::uint64_t serial)
{
    if (!view_ || view_->viewport() == nullptr) {
        make(serial);
        return;
    }
    pendingViews_.assign(views.begin(), views.end());
    for (const cs::View& v : views)
        pictures_.erase(v.setup);
    savedCamera_ = view_->camera();
    // The CAM tree's current setup shows its work offset's axes in the view: hidden while the pictures are taken, as
    // each picture draws its own setup's.
    if (setupTool_) {
        const LineOverlay& wcs = setupTool_->wcsLines();
        for (const QString& key : wcs.keys()) {
            if (const render::ItemId item = wcs.itemOf(key); item != 0) {
                view_->viewport()->setVisible(item, false);
                hiddenItems_.push_back(item);
            }
        }
    }
    nextPicture(serial);
}

void SetupSheetModel::nextPicture(std::uint64_t serial)
{
    if (serial != serial_ || !active())
        return;
    overlay_->clear();
    if (pendingViews_.empty() || !view_ || view_->viewport() == nullptr) {
        endPictures();
        make(serial);
        return;
    }
    const cs::View v = pendingViews_.front();
    // The setup's stock outlined and its work offset's axes, for the picture only.
    const std::array<std::pair<int, int>, 12> edges{
        {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}}};
    std::vector<Vec3> box;
    for (const auto& [a, b] : edges) {
        box.push_back(v.corners[static_cast<std::size_t>(a)]);
        box.push_back(v.corners[static_cast<std::size_t>(b)]);
    }
    overlay_->set(QStringLiteral("stock"), box, render::Token::AccentThinLine, 1.5f);
    Box3 extent{v.corners[0], v.corners[0]};
    for (const Vec3& c : v.corners) {
        extent.min = Vec3{std::min(extent.min.x, c.x), std::min(extent.min.y, c.y), std::min(extent.min.z, c.z)};
        extent.max = Vec3{std::max(extent.max.x, c.x), std::max(extent.max.y, c.y), std::max(extent.max.z, c.z)};
    }
    const camui::Triad triad = camui::triadGeometry(v.origin, v.x, v.y, v.z, camui::triadLength(extent));
    const render::Token tokens[] = {render::Token::AxisX, render::Token::AxisY, render::Token::AxisZ};
    const char* keys[] = {"triad.x", "triad.y", "triad.z"};
    for (std::size_t i = 0; i < 3; ++i)
        overlay_->set(QLatin1String(keys[i]), triad.segments[i], tokens[i], 2.5f);

    view_->setCamera(machineView(v, *savedCamera_, static_cast<double>(kPictureWidth) / kPictureHeight));
    render::IRenderViewport* viewport = view_->viewport();
    viewport->requestFrame();
    pictureTimeout_.start(kPictureTimeoutMs);
    QPointer<SetupSheetModel> self(this);
    const SetupId id = v.setup;
    viewport->captureImage(kPictureWidth, kPictureHeight, [self, serial, id](std::vector<std::byte> png) {
        if (!self || serial != self->serial_ || self->pendingViews_.empty() ||
            !(self->pendingViews_.front().setup == id))
            return;
        self->pictureTimeout_.stop();
        if (!png.empty())
            self->pictures_[id] = std::move(png);
        self->pendingViews_.pop_front();
        self->nextPicture(serial);
    });
}

void SetupSheetModel::endPictures()
{
    pictureTimeout_.stop();
    pendingViews_.clear();
    if (overlay_)
        overlay_->clear();
    if (view_ && view_->viewport() != nullptr) {
        for (render::ItemId item : hiddenItems_)
            view_->viewport()->setVisible(item, true);
        if (savedCamera_)
            view_->setCamera(*savedCamera_);
    }
    hiddenItems_.clear();
    savedCamera_.reset();
}

void SetupSheetModel::make(std::uint64_t serial)
{
    IJobSystem* jobs = scene_ ? scene_->jobs() : nullptr;
    api::IApp* a = app();
    if (serial != serial_ || !active() || jobs == nullptr || a == nullptr || !madeStatuses_)
        return;
    setState(QStringLiteral("making"), trs("Making the setup sheet…"));
    cs::Inputs in;
    in.setups = chosenSetups();
    in.state = madeState_;
    in.build = madeBuild_;
    in.statuses = *madeStatuses_;
    in.libraryRoots = libraryRoots(false);
    in.shippedRoots = libraryRoots(true);
    in.documentName = ss(documentName());
    in.userDataDir = a->userDataDir();
    for (const SetupId& id : in.setups) {
        if (const auto it = pictures_.find(id); it != pictures_.end())
            in.pictures[id] = it->second;
    }
    in.date = ss(QDate::currentDate().toString(Qt::ISODate));
    makeJob_ = jobs->submit<cs::Made>(JobInfo{"Setup sheet", JobPriority::Interactive, false},
                                      [in](JobContext& ctx) -> Result<cs::Made> { return cs::make(in, ctx); });
    QPointer<SetupSheetModel> self(this);
    makeJob_.then([self, serial](Result<cs::Made> r) {
        if (!self || serial != self->serial_)
            return;
        if (!r) {
            if (r.error().code == ErrorCode::PostFailed) {
                self->scheduleRefresh(false); // the gate refused after all (something changed): it lists why
                return;
            }
            self->fail(r.error(), trs("The setup sheet could not be made"));
            return;
        }
        self->html_ = std::move(r->html);
        for (const std::string& n : r->notes)
            self->messages_.push_back(
                QVariantMap{{QStringLiteral("kind"), QStringLiteral("note")}, {QStringLiteral("text"), qs(n)}});
        self->layOut(serial);
    });
}

void SetupSheetModel::layOut(std::uint64_t serial)
{
    IJobSystem* jobs = scene_ ? scene_->jobs() : nullptr;
    if (serial != serial_ || jobs == nullptr || html_.empty())
        return;
    const QString html = qs(html_);
    const camui::Paper paper = paper_;
    const QString title = sheetTitle();
    if (state_ != QLatin1String("making"))
        setState(QStringLiteral("making"), trs("Laying out the pages…"));
    layoutJob_ = jobs->submit<std::shared_ptr<const Laid>>(
        JobInfo{"Setup sheet pages", JobPriority::Interactive, false},
        [html, paper, title](JobContext&) -> Result<std::shared_ptr<const Laid>> {
            auto laid = std::make_shared<Laid>();
            laid->images = camui::toImages(html, paper, title, kPreviewPpi);
            laid->pages = static_cast<int>(laid->images.size());
            return std::shared_ptr<const Laid>(std::move(laid));
        });
    QPointer<SetupSheetModel> self(this);
    layoutJob_.then([self, serial](Result<std::shared_ptr<const Laid>> r) {
        if (!self || serial != self->serial_ || !r)
            return;
        {
            std::lock_guard<std::mutex> lock(self->pages_->mutex);
            self->pages_->images = (*r)->images;
        }
        self->pageCount_ = (*r)->pages;
        ++self->revision_;
        emit self->pagesChanged();
        self->setState(QStringLiteral("ready"),
                       self->pageCount_ == 1 ? trs("Ready: 1 page") : trs("Ready: %1 pages").arg(self->pageCount_));
    });
}

// ---------- saving ----------
bool SetupSheetModel::savePdf()
{
    return save(true);
}

bool SetupSheetModel::saveHtml()
{
    return save(false);
}

bool SetupSheetModel::save(bool pdf)
{
    if (!canSave()) {
        statusText_ = saveBlockedReason();
        emit stateChanged();
        return false;
    }
    IJobSystem* jobs = scene_ ? scene_->jobs() : nullptr;
    if (jobs == nullptr)
        return false;
    savingPdf_ = pdf;
    savingName_ = pdf ? pdfName() : htmlName();
    const QString directory = values_.value(QStringLiteral("directory")).toString().trimmed();
    lastFolder_ = directory;
    messages_.clear();
    resultPath_.clear();
    existingName_.clear();
    bytes_.reset();
    const std::uint64_t serial = ++saveSerial_;
    setState(QStringLiteral("saving"), trs("Saving %1…").arg(savingName_));
    const QString html = qs(html_);
    const camui::Paper paper = paper_;
    const QString title = sheetTitle();
    const std::string dir = ss(directory);
    const std::string name = ss(savingName_);
    post::IOutputFs* fs = fs_;
    struct Prepared {
        std::shared_ptr<const QByteArray> bytes;
        bool exists = false;
    };
    // The PDF (or the HTML's bytes), then whether the file is there (the overwrite question), on a worker: the folder
    // may be a slow share.
    JobHandle<Prepared> job = jobs->submit<Prepared>(
        JobInfo{"Setup sheet file", JobPriority::Interactive, false},
        [html, paper, title, pdf, dir, name, fs](JobContext&) -> Result<Prepared> {
            Prepared out;
            if (pdf) {
                QString error;
                QByteArray bytes = camui::toPdf(html, paper, title, &error);
                if (bytes.isEmpty())
                    return tl::unexpected(Error{ErrorCode::Internal, "The PDF could not be made: " + ss(error), {}});
                out.bytes = std::make_shared<const QByteArray>(std::move(bytes));
            } else {
                out.bytes = std::make_shared<const QByteArray>(html.toUtf8());
            }
            auto there = cs::exists(dir, name, fs);
            if (!there)
                return tl::unexpected(there.error());
            out.exists = *there;
            return out;
        });
    QPointer<SetupSheetModel> self(this);
    job.then([self, serial](Result<Prepared> r) {
        if (!self || serial != self->saveSerial_)
            return;
        if (!r) {
            const std::string problem = cp::problemKey(r.error());
            self->fail(r.error(), problem.empty() ? trs("Nothing was written: the setup sheet could not be made")
                                                  : trs("Nothing was written: the setup sheet could not be put in "
                                                        "the folder"));
            return;
        }
        self->bytes_ = r->bytes;
        if (r->exists) {
            self->existingName_ = self->savingName_;
            self->setState(QStringLiteral("asking"), trs("%1 already exists in the folder").arg(self->savingName_));
            return;
        }
        self->write(cp::Overwrite::Refuse);
    });
    return true;
}

void SetupSheetModel::write(std::optional<cp::Overwrite> rule)
{
    IJobSystem* jobs = scene_ ? scene_->jobs() : nullptr;
    if (jobs == nullptr || !bytes_)
        return;
    const std::uint64_t serial = ++saveSerial_;
    setState(QStringLiteral("saving"), trs("Saving %1…").arg(savingName_));
    const std::shared_ptr<const QByteArray> bytes = bytes_;
    const std::string dir = ss(values_.value(QStringLiteral("directory")).toString().trimmed());
    const std::string name = ss(savingName_);
    const cp::Overwrite overwrite = rule.value_or(cp::Overwrite::Refuse);
    post::IOutputFs* fs = fs_;
    writeJob_ = jobs->submit<cp::Written>(
        JobInfo{"Save setup sheet", JobPriority::Interactive, false},
        [bytes, dir, name, overwrite, fs](JobContext& ctx) -> Result<cp::Written> {
            return cs::write(std::string_view(bytes->constData(), static_cast<std::size_t>(bytes->size())), dir, name,
                             overwrite, fs, &ctx.cancel);
        });
    QPointer<SetupSheetModel> self(this);
    writeJob_.then([self, serial](Result<cp::Written> w) {
        if (!self || serial != self->saveSerial_)
            return;
        if (!w) {
            if (w.error().code == ErrorCode::Cancelled) {
                self->setState(QStringLiteral("cancelled"), trs("Saving was cancelled; nothing was written"));
                return;
            }
            const std::string problem = cp::problemKey(w.error());
            self->fail(w.error(), problem == "exists"
                                      ? trs("Nothing was written: the file exists")
                                      : trs("Nothing was written: the setup sheet could not be put in the folder"));
            return;
        }
        ++self->saves_;
        self->resultPath_ = qs(w->path);
        const QString name = qs(w->fileName);
        self->setState(QStringLiteral("done"),
                       w->replaced ? trs("Saved %1, replacing the old file").arg(name)
                       : name != self->savingName_
                           ? trs("Saved %1 (%2 exists; it was kept)").arg(name, self->savingName_)
                           : trs("Saved %1").arg(name));
        emit self->saved(self->resultPath_);
    });
}

void SetupSheetModel::answerOverwrite(const QString& answer)
{
    if (state_ != QLatin1String("asking"))
        return;
    if (answer == QLatin1String("replace"))
        write(cp::Overwrite::Replace);
    else if (answer == QLatin1String("newName"))
        write(cp::Overwrite::NewName);
    else
        setState(QStringLiteral("cancelled"), trs("Nothing was written: the existing file was kept"));
}

void SetupSheetModel::cancel()
{
    if (state_ == QLatin1String("asking")) {
        setState(QStringLiteral("cancelled"), trs("Nothing was written: the existing file was kept"));
        return;
    }
    if (!busy()) {
        close();
        return;
    }
    // The PDF being made is dropped (its result is ignored); a write is cancelled unless the file is already being put
    // in place, which then reports what it did.
    if (!writeJob_.done() && !writeJob_.cancel()) {
        statusText_ = trs("Too late to cancel: the file is being put in place");
        emit stateChanged();
        return;
    }
    ++saveSerial_;
    setState(QStringLiteral("cancelled"), trs("Saving was cancelled; nothing was written"));
}

bool SetupSheetModel::generateAll()
{
    doc::Document* d = liveDoc();
    cam::ICamEngine* engine = app() ? app()->cam() : nullptr;
    if (d == nullptr || engine == nullptr || !d->build())
        return false;
    bool any = false;
    QPointer<SetupSheetModel> self(this);
    for (const cp::Blocker& b : blockers_) {
        if (!b.generateFixes || b.op.value == Uuid{})
            continue;
        JobHandle<std::shared_ptr<const cam::Toolpath>> job = engine->generate(d->state(), d->build(), b.op);
        job.then([self](Result<std::shared_ptr<const cam::Toolpath>>) {
            if (self && self->active())
                self->scheduleRefresh(false);
        });
        any = true;
    }
    if (any)
        scheduleRefresh(false);
    return any;
}

// ---------- commands ----------
QString SetupSheetModel::commandReason(const QString&) const
{
    api::IApp* a = app();
    if (a == nullptr)
        return trs("There is no session");
    if (a->active() == nullptr)
        return trs("Open or import a part first");
    if (busy())
        return trs("A setup sheet is being saved");
    if (!tree_ || !tree_->currentSetup())
        return trs("Select a setup in the CAM tree first");
    return {};
}

bool SetupSheetModel::runCommand(const QString& id)
{
    if (!commandReason(id).isEmpty())
        return false;
    return open(idText(tree_->currentSetup()->value));
}

} // namespace ray::ui
