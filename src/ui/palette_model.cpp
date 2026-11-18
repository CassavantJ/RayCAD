#include "palette_model.h"

#include "shortcuts.h"

#include <QtGui/QKeySequence>

#include <algorithm>
#include <set>

namespace ray::ui {

PaletteModel::PaletteModel(QObject* parent) : QAbstractListModel(parent) {}

int PaletteModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : count();
}

QVariant PaletteModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= count()) {
        return {};
    }
    const Row& r = rows_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case Qt::DisplayRole:
    case TitleRole:
        return r.command.title;
    case IdRole:
        return r.command.id;
    case CategoryRole:
        return r.command.category;
    case DescriptionRole:
        return r.command.description;
    case ShortcutRole:
        return r.shortcut;
    case EnabledRole:
        return r.reason.isEmpty();
    case ReasonRole:
        return r.reason;
    case NeedsFormRole:
        return r.needsForm;
    case SectionRole:
        return r.section;
    default:
        return {};
    }
}

QHash<int, QByteArray> PaletteModel::roleNames() const
{
    return {{IdRole, "commandId"},      {TitleRole, "title"},
            {CategoryRole, "category"}, {DescriptionRole, "description"},
            {ShortcutRole, "shortcut"}, {EnabledRole, "available"},
            {ReasonRole, "reason"},     {NeedsFormRole, "needsForm"},
            {SectionRole, "section"}};
}

void PaletteModel::setCommands(CommandCatalog* commands)
{
    if (commands == commands_) {
        return;
    }
    if (commands_) {
        disconnect(commands_, nullptr, this, nullptr);
    }
    commands_ = commands;
    if (commands_) {
        connect(commands_, &CommandCatalog::commandsChanged, this, &PaletteModel::refresh);
    }
    emit commandsChanged();
    refresh();
}

void PaletteModel::setQuery(const QString& query)
{
    if (query == query_) {
        return;
    }
    query_ = query;
    emit queryChanged();
    refresh();
}

void PaletteModel::refresh()
{
    std::vector<Row> rows;
    if (commands_) {
        auto add = [&](const Command& c, const QString& section) {
            Row r;
            r.command = c;
            r.section = section;
            r.reason = commands_->disabledReason(c);
            r.needsForm = commands_->needsForm(c);
            r.shortcut = commands_->shortcuts()
                             ? commands_->shortcuts()->shortcutText(c.id)
                             : QKeySequence::fromString(c.defaultShortcut, QKeySequence::PortableText)
                                   .toString(QKeySequence::NativeText);
            rows.push_back(std::move(r));
        };
        if (query_.trimmed().isEmpty()) {
            std::set<QString> recent;
            for (const Command& c : commands_->mostUsed(kRecentCount)) {
                add(c, QStringLiteral("recent"));
                recent.insert(c.id);
            }
            std::vector<Command> all = commands_->commands();
            std::stable_sort(all.begin(), all.end(), [](const Command& a, const Command& b) {
                if (a.category != b.category) {
                    return a.category < b.category;
                }
                return a.title < b.title;
            });
            for (const Command& c : all) {
                if (!recent.count(c.id)) {
                    add(c, QStringLiteral("all"));
                }
            }
        } else {
            for (const Command& c : commands_->search(query_)) {
                add(c, QStringLiteral("results"));
            }
        }
    }
    const int before = count();
    beginResetModel();
    rows_ = std::move(rows);
    endResetModel();
    if (before != count()) {
        emit countChanged();
    }
}

QVariantMap PaletteModel::get(int row) const
{
    if (row < 0 || row >= count()) {
        return {};
    }
    QVariantMap m;
    const QHash<int, QByteArray> names = roleNames();
    for (auto it = names.begin(); it != names.end(); ++it) {
        m.insert(QString::fromLatin1(it.value()), data(index(row), it.key()));
    }
    return m;
}

int PaletteModel::indexOf(const QString& id) const
{
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i].command.id == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int PaletteModel::indexOfTitle(const QString& title) const
{
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i].command.title == title) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

} // namespace ray::ui
