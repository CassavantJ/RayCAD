#include "constraints_rows.h"

namespace ray::ui {

ConstraintRows::ConstraintRows(QStringList keys, QObject* parent) : QAbstractListModel(parent), keys_(std::move(keys))
{
}

int ConstraintRows::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant ConstraintRows::data(const QModelIndex& index, int role) const
{
    const int row = index.row();
    const int key = role - Qt::UserRole - 1;
    if (!index.isValid() || row < 0 || row >= rows_.size() || key < 0 || key >= keys_.size())
        return {};
    return rows_[row].value(keys_[key]);
}

QHash<int, QByteArray> ConstraintRows::roleNames() const
{
    QHash<int, QByteArray> out;
    for (int i = 0; i < keys_.size(); ++i)
        out.insert(Qt::UserRole + 1 + i, keys_[i].toUtf8());
    return out;
}

QVariantMap ConstraintRows::find(const QString& cid) const
{
    for (const QVariantMap& row : rows_) {
        if (row.value(QStringLiteral("cid")).toString() == cid)
            return row;
    }
    return {};
}

QVariantMap ConstraintRows::at(int row) const
{
    return row >= 0 && row < rows_.size() ? rows_[row] : QVariantMap();
}

QString ConstraintRows::identity(const QVariantMap& row)
{
    return row.value(QStringLiteral("cid")).toString() + QLatin1Char('/') +
           row.value(QStringLiteral("slot")).toString();
}

void ConstraintRows::setRows(QVector<QVariantMap> rows)
{
    bool same = rows.size() == rows_.size();
    for (int i = 0; same && i < rows.size(); ++i)
        same = identity(rows[i]) == identity(rows_[i]);
    if (!same) {
        const bool countChanges = rows.size() != rows_.size();
        beginResetModel();
        rows_ = std::move(rows);
        endResetModel();
        if (countChanges)
            emit countChanged();
        return;
    }
    for (int i = 0; i < rows.size(); ++i) {
        if (rows[i] == rows_[i])
            continue;
        QList<int> roles;
        for (int k = 0; k < keys_.size(); ++k) {
            if (rows[i].value(keys_[k]) != rows_[i].value(keys_[k]))
                roles << Qt::UserRole + 1 + k;
        }
        rows_[i] = std::move(rows[i]);
        emit dataChanged(index(i), index(i), roles);
    }
}

} // namespace ray::ui
