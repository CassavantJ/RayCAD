// The command palette's result list (task T-021), created by CommandPalette.qml when the palette first opens.
//
// With a query: CommandCatalog::search (the registry's deterministic ranking with the view commands merged in).
// Without one: the most used commands first (section "recent"), then every command by category and title (section
// "all"). Each row says whether the command can run now and, if not, why.
#pragma once

#include "palette_commands.h"

#include <QtCore/QAbstractListModel>
#include <QtCore/QPointer>
#include <QtQml/QQmlEngine>

#include <vector>

namespace ray::ui {

class PaletteModel : public QAbstractListModel {
    Q_OBJECT
    QML_NAMED_ELEMENT(PaletteModel)

    Q_PROPERTY(ray::ui::CommandCatalog* commands READ commands WRITE setCommands NOTIFY commandsChanged)
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        TitleRole,
        CategoryRole,
        DescriptionRole,
        ShortcutRole,
        EnabledRole,
        ReasonRole,
        NeedsFormRole,
        SectionRole, // "recent" or "all" (empty query), "results" otherwise
    };
    static constexpr int kRecentCount = 5;

    explicit PaletteModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    CommandCatalog* commands() const { return commands_; }
    void setCommands(CommandCatalog* commands);
    QString query() const { return query_; }
    void setQuery(const QString& query);
    int count() const { return static_cast<int>(rows_.size()); }

    // Recomputes the rows (enabled state changes with the document and the selection).
    Q_INVOKABLE void refresh();
    Q_INVOKABLE QVariantMap get(int row) const;
    Q_INVOKABLE int indexOf(const QString& id) const;
    Q_INVOKABLE int indexOfTitle(const QString& title) const;

signals:
    void commandsChanged();
    void queryChanged();
    void countChanged();

private:
    struct Row {
        Command command;
        QString shortcut, reason, section;
        bool needsForm = false;
    };
    QPointer<CommandCatalog> commands_;
    QString query_;
    std::vector<Row> rows_;
};

} // namespace ray::ui
