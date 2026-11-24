#include "post_view.h"

#include "../app_context.h"
#include "../selection_model.h"
#include "../theme_object.h"
#include "../ui_commands.h"
#include "operation_view.h"
#include "setup_tool.h" // setup_tree.h's inline setSetupTool needs the whole type
#include "setup_tree.h"

#include <QtCore/QCoreApplication>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>

#include <algorithm>

namespace ray::ui {

namespace {

QString trv(const char* text)
{
    return QCoreApplication::translate("ray::ui::GcodeModel", text);
}

QString hexOf(const QColor& c)
{
    return c.name(QColor::HexRgb);
}

void escapeInto(QString& out, std::string_view text)
{
    for (const char c : text) {
        switch (c) {
        case '<':
            out += QLatin1String("&lt;");
            break;
        case '>':
            out += QLatin1String("&gt;");
            break;
        case '&':
            out += QLatin1String("&amp;");
            break;
        case ' ':
            out += QLatin1String("&nbsp;"); // columns stay put: the view is monospaced and find marks by column
            break;
        case '"':
            out += QLatin1String("&quot;");
            break;
        default:
            out += QChar::fromLatin1(c);
        }
    }
}

} // namespace

GcodeModel::GcodeModel(AppContext* context, SelectionModel* selection, ThemeObject* theme, QObject* parent)
    : QAbstractListModel(parent), context_(context), selection_(selection), theme_(theme)
{
    if (selection_)
        connect(selection_, &SelectionModel::pathPicked, this,
                [this](const render::PickHit& hit) { onPathPicked(hit); });
    if (theme_) {
        connect(theme_, &ThemeObject::themeChanged, this, [this] {
            ++revision_;
            emit revisionChanged();
            if (rowCount() > 0)
                emit dataChanged(index(0), index(rowCount() - 1), {StyledRole});
        });
    }
    if (context_)
        connect(context_, &AppContext::workspaceChanged, this, [this] { updatePathPicking(); });
}

GcodeModel::~GcodeModel()
{
    if (selection_) {
        selection_->setPathPicking(false);
        selection_->setPathHighlight(std::nullopt);
    }
}

GcodeModel* GcodeModel::create(QQmlEngine* qml, QJSEngine*)
{
    AppContext* context = nullptr;
    SelectionModel* selection = nullptr;
    ThemeObject* theme = nullptr;
    ToolpathDisplay* display = nullptr;
    SetupTreeModel* tree = nullptr;
    if (qml != nullptr) {
        const QString uri = QStringLiteral("RayCad.Ui");
        context = qml->singletonInstance<AppContext*>(uri, QStringLiteral("AppContext"));
        selection = qml->singletonInstance<SelectionModel*>(uri, QStringLiteral("Selection"));
        theme = qml->singletonInstance<ThemeObject*>(uri, QStringLiteral("Theme"));
        display = qml->singletonInstance<ToolpathDisplay*>(uri, QStringLiteral("CamToolpaths"));
        tree = qml->singletonInstance<SetupTreeModel*>(uri, QStringLiteral("CamTreeModel"));
    }
    auto* model = new GcodeModel(context, selection, theme);
    model->setDisplay(display);
    model->setTree(tree);
    return model; // owned by the engine
}

void GcodeModel::setDisplay(ToolpathDisplay* display)
{
    if (display_)
        disconnect(display_, nullptr, this, nullptr);
    display_ = display;
    if (display_)
        connect(display_, &ToolpathDisplay::changed, this, [this] {
            if (open_ && current_ >= 0 && !highlighted_)
                applyHighlight(); // the operation's backplot arrived
        });
}

void GcodeModel::setTree(SetupTreeModel* tree)
{
    tree_ = tree;
}

// ---------- rows ----------
int GcodeModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : lineCount();
}

QHash<int, QByteArray> GcodeModel::roleNames() const
{
    return {{NumberRole, "lineNumber"},
            {TextRole, "lineText"},
            {StyledRole, "styled"},
            {HasMoveRole, "hasMove"},
            {MatchesRole, "matches"}};
}

QString GcodeModel::styled(std::string_view line, const QString& needle) const
{
    (void)needle;
    QString out;
    out.reserve(static_cast<qsizetype>(line.size() * 3));
    const auto colour = [&](camui::GToken kind) -> QColor {
        if (!theme_)
            return QColor();
        switch (kind) {
        case camui::GToken::Comment:
            return theme_->gcodeComment();
        case camui::GToken::Sequence:
        case camui::GToken::Program:
            return theme_->gcodeLineNumber();
        case camui::GToken::GWord:
            return theme_->gcodeGWord();
        case camui::GToken::MWord:
            return theme_->gcodeMWord();
        case camui::GToken::Axis:
            return theme_->gcodeAxis();
        case camui::GToken::FeedSpeed:
            return theme_->gcodeFeedSpeed();
        case camui::GToken::Word:
        case camui::GToken::Text:
            return theme_->gcodeText();
        }
        return theme_->gcodeText();
    };
    for (const camui::GSpan& s : camui::tokenize(line)) {
        const std::string_view part = line.substr(s.start, s.length);
        const QColor c = colour(s.kind);
        if (c.isValid()) {
            out += QLatin1String("<font color=\"") + hexOf(c) + QLatin1String("\">");
            escapeInto(out, part);
            out += QLatin1String("</font>");
        } else {
            escapeInto(out, part);
        }
    }
    return out;
}

QVariant GcodeModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || !program_ || index.row() < 0 || index.row() >= lineCount())
        return {};
    const std::size_t row = static_cast<std::size_t>(index.row());
    const std::string_view line = program_->index->line(row);
    switch (role) {
    case NumberRole:
        return index.row() + 1;
    case TextRole:
        return QString::fromUtf8(line.data(), static_cast<qsizetype>(line.size()));
    case StyledRole:
        return styled(line, findText_);
    case HasMoveRole:
        return program_->index->moveOf(row).has_value();
    case MatchesRole: {
        QVariantList out;
        if (findText_.isEmpty())
            return out;
        // The matches of this line (matches_ is in line order).
        auto it = std::lower_bound(matches_.begin(), matches_.end(), static_cast<std::uint32_t>(row),
                                   [](const camui::FindHit& h, std::uint32_t l) { return h.line < l; });
        for (; it != matches_.end() && it->line == row; ++it) {
            const bool current = currentMatch_ >= 0 && &*it == &matches_[static_cast<std::size_t>(currentMatch_)];
            out.push_back(QVariantMap{{QStringLiteral("column"), static_cast<int>(it->column)},
                                      {QStringLiteral("length"), static_cast<int>(findText_.toUtf8().size())},
                                      {QStringLiteral("current"), current}});
        }
        return out;
    }
    default:
        return {};
    }
}

QString GcodeModel::title() const
{
    if (!program_)
        return trv("G-code");
    return program_->fileName;
}

int GcodeModel::digits() const
{
    int n = std::max(1, lineCount());
    int d = 0;
    for (; n > 0; n /= 10)
        ++d;
    return std::max(3, d);
}

// ---------- open / close ----------
void GcodeModel::setProgram(std::shared_ptr<const PostedProgram> program)
{
    beginResetModel();
    program_ = std::move(program);
    current_ = anchor_ = -1;
    pickedFirst_ = pickedLast_ = -1;
    highlighted_ = false;
    lineInfo_.clear();
    matches_.clear();
    currentMatch_ = -1;
    endResetModel();
    refindMatches();
    if (selection_)
        selection_->setPathHighlight(std::nullopt);
    emit programChanged();
    emit currentChanged();
    emit findChanged();
}

void GcodeModel::show()
{
    if (!program_)
        return;
    if (!open_) {
        open_ = true;
        emit openChanged();
    }
    // The posted setup's toolpaths in the view, none dimmed: its tree row is the current one.
    if (tree_) {
        const int row = tree_->rowOf(QString::fromStdString(program_->setup.value.toString()));
        if (row >= 0)
            tree_->setCurrentIndex(row);
    }
    updatePathPicking();
    if (current_ < 0 && lineCount() > 0)
        selectLine(0);
}

void GcodeModel::close()
{
    if (!open_)
        return;
    open_ = false;
    updatePathPicking();
    if (selection_)
        selection_->setPathHighlight(std::nullopt);
    highlighted_ = false;
    emit openChanged();
    emit currentChanged();
}

void GcodeModel::updatePathPicking()
{
    if (!selection_)
        return;
    const bool on = open_ && program_ && context_ && context_->workspace() == kCamWorkspace;
    selection_->setPathPicking(on);
    if (!on)
        selection_->setPathHighlight(std::nullopt);
    else if (current_ >= 0)
        applyHighlight();
}

// ---------- the current line and its move ----------
void GcodeModel::setCurrent(int line, int anchor)
{
    current_ = line;
    anchor_ = anchor;
    applyHighlight();
    emit currentChanged();
}

void GcodeModel::selectLine(int line, bool extend)
{
    if (!program_ || lineCount() == 0)
        return;
    line = std::clamp(line, 0, lineCount() - 1);
    pickedFirst_ = pickedLast_ = -1;
    setCurrent(line, extend && anchor_ >= 0 ? anchor_ : line);
}

void GcodeModel::moveCurrent(int delta, bool extend)
{
    if (lineCount() == 0)
        return;
    const int from = current_ < 0 ? 0 : current_;
    selectLine(from + delta, extend);
    emit scrollRequested(current_);
}

void GcodeModel::selectAll()
{
    if (lineCount() == 0)
        return;
    pickedFirst_ = pickedLast_ = -1;
    current_ = lineCount() - 1;
    anchor_ = 0;
    applyHighlight();
    emit currentChanged();
}

std::optional<GcodeModel::Shown> GcodeModel::shownFor(std::uint32_t section) const
{
    if (!display_ || !program_ || section >= program_->sectionOps.size())
        return std::nullopt;
    for (const ToolpathDisplay::Shown& s : display_->shown()) {
        if (s.op != program_->sectionOps[section] || s.preview || s.item == 0)
            continue;
        Shown out;
        out.item = s.item;
        out.linked = s.toolpath && s.toolpath->inputHash == program_->sectionInputs[section];
        return out;
    }
    return std::nullopt;
}

std::optional<std::uint32_t> GcodeModel::sectionOfItem(render::ItemId item, bool* linked) const
{
    if (!display_ || !program_)
        return std::nullopt;
    for (const ToolpathDisplay::Shown& s : display_->shown()) {
        if (s.item != item)
            continue;
        for (std::size_t i = 0; i < program_->sectionOps.size(); ++i) {
            if (program_->sectionOps[i] == s.op) {
                *linked = !s.preview && s.toolpath && s.toolpath->inputHash == program_->sectionInputs[i];
                return static_cast<std::uint32_t>(i);
            }
        }
    }
    return std::nullopt;
}

void GcodeModel::applyHighlight()
{
    const bool wasHighlighted = highlighted_;
    highlighted_ = false;
    std::optional<render::PickHit> hit;
    if (!program_ || current_ < 0) {
        lineInfo_.clear();
    } else {
        const int number = current_ + 1;
        const auto move = program_->index->moveOf(static_cast<std::size_t>(current_));
        if (!move) {
            // Quietly: the program's start and end, tool changes and the like belong to no move.
            lineInfo_ = trv("Line %1: no toolpath move on this line").arg(number);
        } else {
            const QString opName =
                move->section < program_->sectionNames.size() ? program_->sectionNames[move->section] : QString();
            const auto shown = shownFor(move->section);
            if (!shown) {
                lineInfo_ =
                    trv("Line %1: %2, move %3 (its toolpath is not shown)").arg(number).arg(opName).arg(move->move + 1);
            } else if (!shown->linked) {
                lineInfo_ = trv("Line %1: %2, move %3. The toolpath changed since this program was posted: post "
                                "again to see the move")
                                .arg(number)
                                .arg(opName)
                                .arg(move->move + 1);
            } else {
                lineInfo_ = trv("Line %1: %2, move %3").arg(number).arg(opName).arg(move->move + 1);
                hit = render::PickHit{shown->item, kernel::SubKind::Edge, move->move, Vec3{}, move->move};
                highlighted_ = true;
            }
        }
    }
    if (selection_ && open_)
        selection_->setPathHighlight(hit);
    if (wasHighlighted != highlighted_)
        emit currentChanged();
}

std::optional<render::PickHit> GcodeModel::highlightedHit() const
{
    return selection_ && highlighted_ ? selection_->pathHighlight() : std::nullopt;
}

void GcodeModel::onPathPicked(const render::PickHit& hit)
{
    if (!open_ || !program_)
        return;
    bool linked = false;
    const auto section = sectionOfItem(hit.item, &linked);
    if (!section) {
        lineInfo_ = trv("That toolpath is not in this program");
        emit currentChanged();
        return;
    }
    const QString opName = program_->sectionNames[*section];
    if (!linked) {
        lineInfo_ = trv("%1 changed since this program was posted: post again to find its lines").arg(opName);
        emit currentChanged();
        return;
    }
    const auto lines = program_->index->linesOf(camui::MoveRef{*section, hit.moveIndex});
    if (!lines) {
        lineInfo_ = trv("%1, move %2: no line of the program writes this move").arg(opName).arg(hit.moveIndex + 1);
        emit currentChanged();
        return;
    }
    current_ = anchor_ = static_cast<int>(lines->first);
    pickedFirst_ = static_cast<int>(lines->first);
    pickedLast_ = static_cast<int>(lines->second);
    highlighted_ = true; // the selection holds the picked move already
    lineInfo_ = lines->first == lines->second
                    ? trv("%1, move %2: line %3").arg(opName).arg(hit.moveIndex + 1).arg(lines->first + 1)
                    : trv("%1, move %2: lines %3 to %4")
                          .arg(opName)
                          .arg(hit.moveIndex + 1)
                          .arg(lines->first + 1)
                          .arg(lines->second + 1);
    emit currentChanged();
    emit scrollRequested(current_);
}

bool GcodeModel::showMove(std::uint32_t section, std::uint32_t move)
{
    if (!program_ || section >= program_->sectionOps.size())
        return false;
    if (!open_) {
        open_ = true;
        emit openChanged();
        updatePathPicking();
    }
    const QString opName = section < program_->sectionNames.size() ? program_->sectionNames[section] : QString();
    const auto lines = program_->index->linesOf(camui::MoveRef{section, move});
    if (!lines) {
        lineInfo_ = trv("%1, move %2: no line of the program writes this move").arg(opName).arg(move + 1);
        emit currentChanged();
        return false;
    }
    current_ = anchor_ = static_cast<int>(lines->first);
    pickedFirst_ = static_cast<int>(lines->first);
    pickedLast_ = static_cast<int>(lines->second);
    highlighted_ = false; // nothing in the view: the simulation shows the tool there
    lineInfo_ =
        lines->first == lines->second
            ? trv("%1, move %2: line %3").arg(opName).arg(move + 1).arg(lines->first + 1)
            : trv("%1, move %2: lines %3 to %4").arg(opName).arg(move + 1).arg(lines->first + 1).arg(lines->second + 1);
    emit currentChanged();
    emit scrollRequested(current_);
    return true;
}

// ---------- copy ----------
QString GcodeModel::selectionText() const
{
    if (!program_ || current_ < 0)
        return {};
    const std::string_view bytes =
        program_->index->lines(static_cast<std::size_t>(selectionFirst()), static_cast<std::size_t>(selectionLast()));
    return QString::fromUtf8(bytes.data(), static_cast<qsizetype>(bytes.size()));
}

bool GcodeModel::copySelection()
{
    const QString text = selectionText();
    if (text.isEmpty())
        return false;
    QGuiApplication::clipboard()->setText(text);
    return true;
}

bool GcodeModel::copyAll()
{
    if (!program_ || !program_->index->text)
        return false;
    const std::string& t = *program_->index->text;
    QGuiApplication::clipboard()->setText(QString::fromUtf8(t.data(), static_cast<qsizetype>(t.size())));
    return true;
}

// ---------- find ----------
void GcodeModel::refindMatches()
{
    matches_.clear();
    currentMatch_ = -1;
    if (program_ && !findText_.isEmpty()) {
        const QByteArray needle = findText_.toUtf8();
        matches_ = camui::findAll(*program_->index, std::string_view(needle.constData(), needle.size()));
    }
}

void GcodeModel::setFindText(const QString& text)
{
    if (text == findText_)
        return;
    findText_ = text;
    refindMatches();
    // The first match at or after the current line.
    if (!matches_.empty()) {
        const std::uint32_t from = current_ < 0 ? 0 : static_cast<std::uint32_t>(current_);
        const auto it = std::lower_bound(matches_.begin(), matches_.end(), from,
                                         [](const camui::FindHit& h, std::uint32_t l) { return h.line < l; });
        currentMatch_ = static_cast<int>(it == matches_.end() ? 0 : it - matches_.begin());
        const int line = static_cast<int>(matches_[static_cast<std::size_t>(currentMatch_)].line);
        selectLine(line);
        emit scrollRequested(line);
    }
    emit findChanged();
    if (rowCount() > 0)
        emit dataChanged(index(0), index(rowCount() - 1), {MatchesRole});
}

void GcodeModel::findNext()
{
    if (matches_.empty())
        return;
    currentMatch_ = (currentMatch_ + 1) % static_cast<int>(matches_.size());
    const int line = static_cast<int>(matches_[static_cast<std::size_t>(currentMatch_)].line);
    selectLine(line);
    emit findChanged();
    emit scrollRequested(line);
    if (rowCount() > 0)
        emit dataChanged(index(0), index(rowCount() - 1), {MatchesRole});
}

void GcodeModel::findPrevious()
{
    if (matches_.empty())
        return;
    currentMatch_ = (currentMatch_ <= 0 ? static_cast<int>(matches_.size()) : currentMatch_) - 1;
    const int line = static_cast<int>(matches_[static_cast<std::size_t>(currentMatch_)].line);
    selectLine(line);
    emit findChanged();
    emit scrollRequested(line);
    if (rowCount() > 0)
        emit dataChanged(index(0), index(rowCount() - 1), {MatchesRole});
}

} // namespace ray::ui
