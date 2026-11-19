// The `Parameters` QML singleton (task T-098): the document's named parameters for the parameter table
// (qml/model/ParameterTable.qml). Every change is one action and one undo step:
//   add          param.set with a new id (name, expression, kind)
//   rename       param.rename: every expression that uses the old name (other parameters, feature fields, sketch
//                dimensions) is rewritten in the same step
//   expression   param.set (a plain number gets the display unit for a length, degrees for an angle)
//   kind         param.set
//   delete       param.remove, which the document refuses while something uses the parameter; the refusal names
//                the users and shows in message.
// Values are evaluated from the current state (in the display unit); a parameter that does not evaluate shows why.
#pragma once

#include <ray/api.h>

#include <QtCore/QAbstractListModel>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtQml/QQmlEngine>

#include <cstdint>
#include <string>
#include <vector>

namespace ray::ui {

class AppContext;
class Session;
class UnitsObject;

class ParamTableModel : public QAbstractListModel {
    Q_OBJECT
    QML_NAMED_ELEMENT(Parameters)
    QML_SINGLETON

    Q_PROPERTY(int count READ count NOTIFY changed)
    Q_PROPERTY(int revision READ revision NOTIFY changed)
    // Why the last change was refused ("" when it went through).
    Q_PROPERTY(QString message READ message NOTIFY messageChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY messageChanged)

public:
    enum Role { IdRole = Qt::UserRole + 1, NameRole, ExpressionRole, KindRole, ValueRole, ErrorRole };

    ParamTableModel(Session* session, AppContext* context, UnitsObject* units, QObject* parent = nullptr);
    ~ParamTableModel() override;
    static ParamTableModel* create(QQmlEngine* qml, QJSEngine* js);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return static_cast<int>(rows_.size()); }
    int revision() const { return revision_; }
    QString message() const { return message_; }
    bool busy() const { return inFlight_ > 0; }

    // kind: "length", "angle", "count", "unitless".
    Q_INVOKABLE bool add(const QString& name, const QString& expression, const QString& kind);
    Q_INVOKABLE bool rename(int row, const QString& name);
    Q_INVOKABLE bool setExpression(int row, const QString& expression);
    Q_INVOKABLE bool setKind(int row, const QString& kind);
    Q_INVOKABLE bool remove(int row);
    Q_INVOKABLE int rowOf(const QString& name) const;
    // A free name: "p1", "p2", ...
    Q_INVOKABLE QString nextName() const;
    Q_INVOKABLE void dismissMessage();

signals:
    void changed();
    void messageChanged();

private:
    struct Row {
        ParamId id;
        QString name, expression, kind, value, error;
    };
    api::IApp* app() const;
    doc::Document* liveDoc() const;
    void subscribe();
    void unsubscribe();
    void sync();
    bool run(const char* actionId, const std::string& argsJson);
    QString withUnit(const QString& expression, const QString& kind) const;

    QPointer<Session> session_;
    QPointer<AppContext> context_;
    QPointer<UnitsObject> units_;
    std::string docId_;
    doc::Document* doc_ = nullptr;
    int listener_ = 0;
    std::uint64_t generation_ = 0;
    std::vector<Row> rows_;
    int revision_ = 0;
    QString message_;
    int inFlight_ = 0;
};

} // namespace ray::ui
