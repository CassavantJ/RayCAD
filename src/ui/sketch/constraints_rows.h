// A list model of plain rows for the constraint glyphs and dimension labels over the view (task T-093).
//
// setRows() keeps the delegates: when the rows have the same keys ("cid" and "slot") in the same order, only the
// changed values are signalled (a camera move changes positions, not rows), so a pan does not rebuild the items; other
// changes reset the model. Roles are the keys of the rows (every row has the same keys).
#pragma once

#include <QtCore/QAbstractListModel>
#include <QtCore/QByteArray>
#include <QtCore/QHash>
#include <QtCore/QStringList>
#include <QtCore/QVariantMap>
#include <QtCore/QVector>

namespace ray::ui {

class ConstraintRows : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    explicit ConstraintRows(QStringList keys, QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return static_cast<int>(rows_.size()); }
    const QVector<QVariantMap>& rows() const { return rows_; }
    // The row whose "cid" is `cid` (the first one), or an empty map.
    Q_INVOKABLE QVariantMap find(const QString& cid) const;
    Q_INVOKABLE QVariantMap at(int row) const;
    void setRows(QVector<QVariantMap> rows);

signals:
    void countChanged();

private:
    static QString identity(const QVariantMap& row);
    QStringList keys_;
    QVector<QVariantMap> rows_;
};

} // namespace ray::ui
