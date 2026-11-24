// The `CamGcode` QML singleton (task T-047): the G-code viewer of the CAM workspace (qml/cam/GcodeView.qml).
//
//   Program    the last program posted (PostToolModel hands it over with its index, cam/post_logic.h, built on a
//              worker). Read-only. One row per line; the view (a ListView) makes delegates for the lines in sight only,
//              so a program of any length scrolls at the same cost.
//   Colours    each line's words as cam/post_logic.h tokenize() splits them, in the theme's G-code tokens (Theme
//              gcodeGWord, gcodeMWord, gcodeAxis, gcodeFeedSpeed, gcodeComment, gcodeLineNumber, gcodeText), so light
//              and dark follow the theme; the line numbers in the gutter in gcodeLineNumber, the current line's band
//              in gcodeCurrentLine.
//   Linking    through PostOutput::lineMap. Selecting a line highlights its move in the view: the move's backplot
//              segment, as ToolpathDisplay shows the operation, through the selection's path highlight
//              (SelectionModel::setPathHighlight; render.h setHighlight). A line with no move (the program start, a
//              tool change, the end) highlights nothing and says so in lineInfo. The other way: while the viewer is
//              open, clicks in the view pick toolpath moves too (SelectionModel::setPathPicking); a picked move selects
//              and scrolls to its first line (and marks all of its lines). Only a toolpath whose input hash is the one
//              posted is linked: after an edit the program and the view no longer match, and lineInfo says so.
//   Find       findText: case-insensitive; matchCount, the current match, next and previous (Enter, F3, Shift+F3),
//              each match marked in its line.
//   Copy       the selected lines (a click, Shift+click and the arrow keys with Shift extend), or the whole program,
//              to the clipboard exactly as written (line endings included).
#pragma once

#include "post_logic.h"

#include <ray/api.h>
#include <ray/render.h>

#include <QtCore/QAbstractListModel>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtCore/QVariantList>
#include <QtQml/QQmlEngine>

#include <memory>
#include <optional>
#include <vector>

namespace ray::ui {

class AppContext;
class SelectionModel;
class SetupTreeModel;
class ThemeObject;
class ToolpathDisplay;

// What the viewer shows of one post run (PostToolModel makes it).
struct PostedProgram {
    std::shared_ptr<const camui::GcodeIndex> index;
    SetupId setup;
    std::vector<OperationId> sectionOps;
    std::vector<QString> sectionNames;
    std::vector<Hash256> sectionInputs; // the toolpaths' input hashes, to know a shown toolpath is the one posted
    QString fileName, path, postTitle, setupName;
};

class GcodeModel : public QAbstractListModel {
    Q_OBJECT
    QML_NAMED_ELEMENT(CamGcode)
    QML_SINGLETON

    Q_PROPERTY(bool open READ isOpen NOTIFY openChanged)
    Q_PROPERTY(bool hasProgram READ hasProgram NOTIFY programChanged)
    Q_PROPERTY(QString title READ title NOTIFY programChanged)
    Q_PROPERTY(QString path READ path NOTIFY programChanged)
    Q_PROPERTY(int lineCount READ lineCount NOTIFY programChanged)
    Q_PROPERTY(int digits READ digits NOTIFY programChanged)   // of the largest line number
    Q_PROPERTY(int columns READ columns NOTIFY programChanged) // characters of the longest line
    // The current line (0-based, -1 none) and the selected range (inclusive).
    Q_PROPERTY(int currentLine READ currentLine NOTIFY currentChanged)
    Q_PROPERTY(int selectionFirst READ selectionFirst NOTIFY currentChanged)
    Q_PROPERTY(int selectionLast READ selectionLast NOTIFY currentChanged)
    // The lines of the move picked in the view (marked), -1 when none.
    Q_PROPERTY(int pickedFirst READ pickedFirst NOTIFY currentChanged)
    Q_PROPERTY(int pickedLast READ pickedLast NOTIFY currentChanged)
    // About the current line: its operation and move, or that it has none.
    Q_PROPERTY(QString lineInfo READ lineInfo NOTIFY currentChanged)
    // The line's move is highlighted in the view now.
    Q_PROPERTY(bool highlighted READ highlighted NOTIFY currentChanged)
    Q_PROPERTY(QString findText READ findText WRITE setFindText NOTIFY findChanged)
    Q_PROPERTY(int matchCount READ matchCount NOTIFY findChanged)
    Q_PROPERTY(int currentMatch READ currentMatch NOTIFY findChanged) // 0-based, -1 none
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)

public:
    enum Role { NumberRole = Qt::UserRole + 1, TextRole, StyledRole, HasMoveRole, MatchesRole };

    GcodeModel(AppContext* context, SelectionModel* selection, ThemeObject* theme, QObject* parent = nullptr);
    ~GcodeModel() override;
    static GcodeModel* create(QQmlEngine* qml, QJSEngine* js);
    void setDisplay(ToolpathDisplay* display);
    void setTree(SetupTreeModel* tree);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    bool isOpen() const { return open_; }
    bool hasProgram() const { return program_ != nullptr; }
    QString title() const;
    QString path() const { return program_ ? program_->path : QString(); }
    int lineCount() const { return program_ ? static_cast<int>(program_->index->lineCount()) : 0; }
    int digits() const;
    int columns() const { return program_ ? static_cast<int>(program_->index->longestLine) : 0; }
    int currentLine() const { return current_; }
    int selectionFirst() const { return std::min(anchor_, current_); }
    int selectionLast() const { return std::max(anchor_, current_); }
    int pickedFirst() const { return pickedFirst_; }
    int pickedLast() const { return pickedLast_; }
    QString lineInfo() const { return lineInfo_; }
    bool highlighted() const { return highlighted_; }
    QString findText() const { return findText_; }
    void setFindText(const QString& text);
    int matchCount() const { return static_cast<int>(matches_.size()); }
    int currentMatch() const { return currentMatch_; }
    int revision() const { return revision_; }

    // ---- QML ----
    Q_INVOKABLE void show();
    Q_INVOKABLE void close();
    // Makes `line` current (and its move highlighted); `extend` keeps the selection's other end.
    Q_INVOKABLE void selectLine(int line, bool extend = false);
    Q_INVOKABLE void moveCurrent(int delta, bool extend = false);
    Q_INVOKABLE void selectAll();
    Q_INVOKABLE bool copySelection();
    Q_INVOKABLE bool copyAll();
    Q_INVOKABLE void findNext();
    Q_INVOKABLE void findPrevious();

    // ---- C++ ----
    // T-074 (the Simulate workspace): opens the viewer on move `move` of section `section`, as a move picked in the
    // view does (its first line current and scrolled to, all of its lines marked), without highlighting anything in the
    // view. False (and lineInfo says why) when there is no program or no line writes that move.
    bool showMove(std::uint32_t section, std::uint32_t move);
    void setProgram(std::shared_ptr<const PostedProgram> program);
    const PostedProgram* program() const { return program_.get(); }
    // The move the view highlights for the current line (null when none).
    std::optional<render::PickHit> highlightedHit() const;
    // Text that copySelection would put on the clipboard.
    QString selectionText() const;

signals:
    void openChanged();
    void programChanged();
    void currentChanged();
    void findChanged();
    void revisionChanged();
    // The view should bring `line` into sight.
    void scrollRequested(int line);
    // The user made `line` current (selectLine without extending: a click, the arrow keys, a find), not showMove. The
    // Simulate workspace seeks there (sim_tool.h, "G-code"); the CAM workspace does nothing with it. Added after T-074.
    void lineChosen(int line);

private:
    struct Shown {
        render::ItemId item = 0;
        bool linked = false; // the shown toolpath is the posted one
    };
    std::optional<Shown> shownFor(std::uint32_t section) const;
    std::optional<std::uint32_t> sectionOfItem(render::ItemId item, bool* linked) const;
    void applyHighlight();
    void onPathPicked(const render::PickHit& hit);
    void updatePathPicking();
    void setCurrent(int line, int anchor);
    QString styled(std::string_view line, const QString& needle) const;
    void refindMatches();

    QPointer<AppContext> context_;
    QPointer<SelectionModel> selection_;
    QPointer<ThemeObject> theme_;
    QPointer<ToolpathDisplay> display_;
    QPointer<SetupTreeModel> tree_;
    std::shared_ptr<const PostedProgram> program_;
    bool open_ = false;
    int current_ = -1, anchor_ = -1;
    int pickedFirst_ = -1, pickedLast_ = -1;
    QString lineInfo_;
    bool highlighted_ = false;
    QString findText_;
    std::vector<camui::FindHit> matches_;
    int currentMatch_ = -1;
    int revision_ = 0;
};

} // namespace ray::ui
