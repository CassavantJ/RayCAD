#include "params.h"

#include "../app_context.h"
#include "../session.h"
#include "../units.h"
#include "model_util.h"

#include <ray_api/feature_edit.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonObject>
#include <QtCore/QLocale>

#include <algorithm>

namespace ray::ui {

using namespace modelui;

namespace {

constexpr const char* kTrContext = "ray::ui::ParamTableModel";
constexpr double kPi = 3.14159265358979323846;

QString trp(const char* text)
{
    return QCoreApplication::translate(kTrContext, text);
}

QString kindName(doc::Parameter::Kind k)
{
    switch (k) {
    case doc::Parameter::Kind::Length:
        return QStringLiteral("length");
    case doc::Parameter::Kind::Angle:
        return QStringLiteral("angle");
    case doc::Parameter::Kind::Count:
        return QStringLiteral("count");
    case doc::Parameter::Kind::Unitless:
        return QStringLiteral("unitless");
    }
    return {};
}

} // namespace

ParamTableModel::ParamTableModel(Session* session, AppContext* context, UnitsObject* units, QObject* parent)
    : QAbstractListModel(parent), session_(session), context_(context), units_(units)
{
    if (session_) {
        connect(session_, &Session::documentChanged, this, [this] {
            if (session_ && session_->documentId().toStdString() != docId_) {
                subscribe();
                sync();
            }
        });
    }
    if (units_)
        connect(units_, &UnitsObject::changed, this, [this] { sync(); });
}

ParamTableModel::~ParamTableModel()
{
    unsubscribe();
}

ParamTableModel* ParamTableModel::create(QQmlEngine* qml, QJSEngine*)
{
    Session* session = nullptr;
    AppContext* context = nullptr;
    UnitsObject* units = nullptr;
    if (qml != nullptr) {
        const QString uri = QStringLiteral("RayCad.Ui");
        session = qml->singletonInstance<Session*>(uri, QStringLiteral("Session"));
        context = qml->singletonInstance<AppContext*>(uri, QStringLiteral("AppContext"));
        units = qml->singletonInstance<UnitsObject*>(uri, QStringLiteral("Units"));
    }
    auto* model = new ParamTableModel(session, context, units);
    model->subscribe();
    model->sync();
    return model;
}

api::IApp* ParamTableModel::app() const
{
    return context_ ? context_->app() : nullptr;
}

doc::Document* ParamTableModel::liveDoc() const
{
    api::IApp* a = app();
    if (a == nullptr || docId_.empty() || doc_ == nullptr)
        return nullptr;
    api::OpenDocument* od = a->findDocument(docId_);
    return od != nullptr && od->document.get() == doc_ ? doc_ : nullptr;
}

void ParamTableModel::subscribe()
{
    unsubscribe();
    api::IApp* a = app();
    api::OpenDocument* od = a != nullptr ? a->active() : nullptr;
    if (od == nullptr || !od->document)
        return;
    docId_ = od->id;
    doc_ = od->document.get();
    const std::uint64_t generation = ++generation_;
    QPointer<ParamTableModel> self(this);
    listener_ = doc_->subscribe([self, generation](doc::DocStatePtr, doc::BuildResultPtr) {
        if (self && self->generation_ == generation)
            self->sync();
    });
}

void ParamTableModel::unsubscribe()
{
    if (doc::Document* d = liveDoc(); d != nullptr && listener_ != 0)
        d->unsubscribe(listener_);
    doc_ = nullptr;
    listener_ = 0;
    docId_.clear();
    ++generation_;
}

void ParamTableModel::sync()
{
    if (doc_ == nullptr && app() != nullptr && app()->active() != nullptr)
        subscribe();
    doc::Document* d = liveDoc();
    std::vector<Row> rows;
    if (d != nullptr) {
        const doc::DocStatePtr state = d->state();
        for (const doc::Parameter& p : state->parameters) {
            Row r;
            r.id = p.id;
            r.name = qs(p.name);
            r.expression = qs(p.expression);
            r.kind = kindName(p.kind);
            const bool angle = p.kind == doc::Parameter::Kind::Angle;
            auto v = api::feature_edit::evaluate(*state, p.name, angle);
            if (!v) {
                r.error = qs(v.error().message);
            } else if (p.kind == doc::Parameter::Kind::Length) {
                r.value = units_ ? units_->formatLength(*v) : QLocale::c().toString(*v) + QStringLiteral(" mm");
            } else if (angle) {
                r.value = QLocale::c().toString(*v * 180 / kPi, 'f', 2) + QStringLiteral(" deg");
            } else {
                r.value = QLocale::c().toString(*v, 'g', 10);
            }
            rows.push_back(std::move(r));
        }
    }
    beginResetModel();
    rows_ = std::move(rows);
    endResetModel();
    ++revision_;
    emit changed();
}

int ParamTableModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : count();
}

QVariant ParamTableModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= count())
        return {};
    const Row& r = rows_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case IdRole:
        return idText(r.id.value);
    case NameRole:
        return r.name;
    case ExpressionRole:
        return r.expression;
    case KindRole:
        return r.kind;
    case ValueRole:
        return r.value;
    case ErrorRole:
        return r.error;
    default:
        return {};
    }
}

QHash<int, QByteArray> ParamTableModel::roleNames() const
{
    return {{IdRole, "paramId"},     {NameRole, "paramName"},   {ExpressionRole, "expression"},
            {KindRole, "paramKind"}, {ValueRole, "paramValue"}, {ErrorRole, "paramError"}};
}

bool ParamTableModel::run(const char* actionId, const std::string& argsJson)
{
    api::IApp* a = app();
    if (a == nullptr || liveDoc() == nullptr) {
        message_ = trp("Open or create a document first");
        emit messageChanged();
        return false;
    }
    JobHandle<api::ActionResult> job = a->actions().invoke(actionId, argsJson, api::Caller::User);
    if (!job.valid())
        return false;
    ++inFlight_;
    message_.clear();
    emit messageChanged();
    QPointer<ParamTableModel> self(this);
    job.then([self](Result<api::ActionResult> r) {
        if (!self)
            return;
        --self->inFlight_;
        if (!r)
            self->message_ = qs(r.error().message);
        emit self->messageChanged();
    });
    return true;
}

QString ParamTableModel::withUnit(const QString& expression, const QString& kind) const
{
    const QString text = expression.trimmed();
    bool ok = false;
    QLocale::c().toDouble(text, &ok);
    if (!ok)
        return text;
    if (kind == QLatin1String("length"))
        return text + QLatin1Char(' ') + (units_ ? units_->unitLabel() : QStringLiteral("mm"));
    if (kind == QLatin1String("angle"))
        return text + QStringLiteral(" deg");
    return text;
}

bool ParamTableModel::add(const QString& name, const QString& expression, const QString& kind)
{
    const QJsonObject args{{QStringLiteral("id"), idText(Uuid::generate())},
                           {QStringLiteral("name"), name.trimmed()},
                           {QStringLiteral("expression"), withUnit(expression, kind)},
                           {QStringLiteral("kind"), kind}};
    return run("param.set", jsonText(args));
}

bool ParamTableModel::rename(int row, const QString& name)
{
    if (row < 0 || row >= count() || name.trimmed() == rows_[static_cast<std::size_t>(row)].name)
        return false;
    const QJsonObject args{{QStringLiteral("id"), idText(rows_[static_cast<std::size_t>(row)].id.value)},
                           {QStringLiteral("name"), name.trimmed()}};
    return run("param.rename", jsonText(args));
}

bool ParamTableModel::setExpression(int row, const QString& expression)
{
    if (row < 0 || row >= count())
        return false;
    const Row& r = rows_[static_cast<std::size_t>(row)];
    const QString e = withUnit(expression, r.kind);
    if (e == r.expression || e.isEmpty())
        return false;
    const QJsonObject args{{QStringLiteral("id"), idText(r.id.value)}, {QStringLiteral("expression"), e}};
    return run("param.set", jsonText(args));
}

bool ParamTableModel::setKind(int row, const QString& kind)
{
    if (row < 0 || row >= count() || kind == rows_[static_cast<std::size_t>(row)].kind)
        return false;
    const QJsonObject args{{QStringLiteral("id"), idText(rows_[static_cast<std::size_t>(row)].id.value)},
                           {QStringLiteral("kind"), kind}};
    return run("param.set", jsonText(args));
}

bool ParamTableModel::remove(int row)
{
    if (row < 0 || row >= count())
        return false;
    const QJsonObject args{{QStringLiteral("id"), idText(rows_[static_cast<std::size_t>(row)].id.value)}};
    return run("param.remove", jsonText(args));
}

int ParamTableModel::rowOf(const QString& name) const
{
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i].name == name)
            return static_cast<int>(i);
    }
    return -1;
}

QString ParamTableModel::nextName() const
{
    for (int i = 1;; ++i) {
        const QString n = QStringLiteral("p%1").arg(i);
        if (rowOf(n) < 0)
            return n;
    }
}

void ParamTableModel::dismissMessage()
{
    message_.clear();
    emit messageChanged();
}

} // namespace ray::ui
