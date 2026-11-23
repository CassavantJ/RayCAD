#include "operation_form.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonDocument>
#include <QtCore/QLocale>
#include <QtCore/QRegularExpression>

#include <algorithm>
#include <cmath>
#include <set>

namespace ray::ui::camui {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("ray::ui::SchemaFormModel", text);
}

// The keywords a property may have (docs/operation_types.md). x- annotations other than the form's are ignored.
const std::set<QString>& propertyKeywords()
{
    static const std::set<QString> keys = {QStringLiteral("type"),
                                           QStringLiteral("enum"),
                                           QStringLiteral("title"),
                                           QStringLiteral("description"),
                                           QStringLiteral("default"),
                                           QStringLiteral("minimum"),
                                           QStringLiteral("maximum"),
                                           QStringLiteral("exclusiveMinimum"),
                                           QStringLiteral("exclusiveMaximum"),
                                           QStringLiteral("minLength"),
                                           QStringLiteral("maxLength"),
                                           QStringLiteral("pattern"),
                                           QStringLiteral("$comment"),
                                           QStringLiteral("examples")};
    return keys;
}

const std::set<QString>& topKeywords()
{
    static const std::set<QString> keys = {QStringLiteral("$schema"),     QStringLiteral("$id"),
                                           QStringLiteral("$comment"),    QStringLiteral("title"),
                                           QStringLiteral("description"), QStringLiteral("type"),
                                           QStringLiteral("properties"),  QStringLiteral("additionalProperties"),
                                           QStringLiteral("required")};
    return keys;
}

std::optional<double> numberOf(const QJsonObject& o, const char* key)
{
    const QJsonValue v = o.value(QLatin1String(key));
    return v.isDouble() ? std::optional<double>(v.toDouble()) : std::nullopt;
}

QString cText(double v, int significant = 10)
{
    return QLocale::c().toString(v, 'g', significant);
}

FormField parseProperty(const QString& key, const QJsonValue& value)
{
    FormField f;
    f.key = key;
    f.title = key;
    if (!value.isObject()) {
        f.reason = tr("its schema is not an object");
        return f;
    }
    const QJsonObject p = value.toObject();
    f.title = p.value(QStringLiteral("title")).toString(key);
    if (f.title.isEmpty())
        f.title = key;
    f.description = p.value(QStringLiteral("description")).toString();
    f.defaultValue =
        p.contains(QStringLiteral("default")) ? p.value(QStringLiteral("default")) : QJsonValue(QJsonValue::Undefined);
    for (auto it = p.begin(); it != p.end(); ++it) {
        if (it.key().startsWith(QLatin1String("x-")) || propertyKeywords().count(it.key()) != 0)
            continue;
        f.reason = tr("it uses '%1'").arg(it.key());
        return f;
    }
    // x-unit
    const QJsonValue unit = p.value(QStringLiteral("x-unit"));
    if (!unit.isUndefined()) {
        const QString u = unit.toString();
        if (u == QLatin1String("length"))
            f.unit = Unit::Length;
        else if (u == QLatin1String("angle"))
            f.unit = Unit::Angle;
        else if (u == QLatin1String("time"))
            f.unit = Unit::Time;
        else if (u == QLatin1String("count"))
            f.unit = Unit::Count;
        else if (u == QLatin1String("percent"))
            f.unit = Unit::Percent;
        else {
            f.reason = tr("its unit '%1' is not one the form knows").arg(u);
            return f;
        }
    }
    // x-relevantWhen (checked against the other properties once all are read)
    const QJsonValue when = p.value(QStringLiteral("x-relevantWhen"));
    if (when.isObject()) {
        const QJsonObject w = when.toObject();
        for (auto it = w.begin(); it != w.end(); ++it)
            f.relevantWhen.emplace_back(it.key(), it.value().toArray());
    }
    const QJsonValue type = p.value(QStringLiteral("type"));
    const QJsonValue options = p.value(QStringLiteral("enum"));
    if (!options.isUndefined()) {
        if (!options.isArray() || options.toArray().isEmpty()) {
            f.reason = tr("its 'enum' is not a list of choices");
            return f;
        }
        for (const QJsonValue& o : options.toArray()) {
            if (!o.isString()) {
                f.reason = tr("its choices are not all text");
                return f;
            }
            f.enumValues << o.toString();
        }
        if (!type.isUndefined() && type.toString() != QLatin1String("string")) {
            f.reason = tr("its choices are of type '%1'").arg(type.toVariant().toString());
            return f;
        }
        f.kind = FormField::Kind::Enum;
        const QJsonArray titles = p.value(QStringLiteral("x-enumTitles")).toArray();
        f.enumTitles = f.enumValues;
        if (titles.size() == f.enumValues.size()) {
            for (qsizetype i = 0; i < titles.size(); ++i) {
                if (titles[i].isString() && !titles[i].toString().isEmpty())
                    f.enumTitles[i] = titles[i].toString();
            }
        }
        // x-enumDescriptions: a detail line per value (a list of another length is ignored, as titles are).
        const QJsonArray descriptions = p.value(QStringLiteral("x-enumDescriptions")).toArray();
        if (descriptions.size() == f.enumValues.size()) {
            for (const QJsonValue& d : descriptions)
                f.enumDescriptions << (d.isString() ? d.toString() : QString());
        }
        return f;
    }
    if (!type.isString()) {
        f.reason = type.isUndefined() ? tr("it has no type") : tr("it has a list of types");
        return f;
    }
    const QString t = type.toString();
    if (t == QLatin1String("number") || t == QLatin1String("integer")) {
        f.kind = t == QLatin1String("number") ? FormField::Kind::Number : FormField::Kind::Integer;
        f.minimum = numberOf(p, "minimum");
        f.maximum = numberOf(p, "maximum");
        f.exclusiveMinimum = numberOf(p, "exclusiveMinimum");
        f.exclusiveMaximum = numberOf(p, "exclusiveMaximum");
    } else if (t == QLatin1String("boolean")) {
        f.kind = FormField::Kind::Boolean;
    } else if (t == QLatin1String("string")) {
        f.kind = FormField::Kind::String;
        if (const auto n = numberOf(p, "minLength"))
            f.minLength = static_cast<int>(*n);
        if (const auto n = numberOf(p, "maxLength"))
            f.maxLength = static_cast<int>(*n);
        f.pattern = p.value(QStringLiteral("pattern")).toString();
        // x-format: what the text holds; a format the form does not know is edited as plain text.
        if (p.value(QStringLiteral("x-format")).toString() == QLatin1String("pointsXY"))
            f.format = QStringLiteral("pointsXY");
        // x-multiline: several lines (not for a list of points, which has its own control).
        f.multiline = p.value(QStringLiteral("x-multiline")).toBool() && f.format.isEmpty();
    } else {
        f.reason = tr("it is of type '%1'").arg(t);
        return f;
    }
    if (f.kind != FormField::Kind::Number && f.kind != FormField::Kind::Integer && f.unit != Unit::None) {
        f.reason = tr("it has a unit but is not a number");
        f.kind = FormField::Kind::Unsupported;
    }
    return f;
}

} // namespace

FormSchema parseFormSchema(const QString& schemaJson)
{
    FormSchema out;
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(schemaJson.toUtf8(), &error);
    if (!doc.isObject()) {
        out.problems << tr("The parameter schema is not a JSON object, so there is nothing to edit here");
        return out;
    }
    out.top = doc.object();
    out.title = out.top.value(QStringLiteral("title")).toString();
    out.description = out.top.value(QStringLiteral("description")).toString();
    const QJsonValue type = out.top.value(QStringLiteral("type"));
    if (!type.isUndefined() && type.toString() != QLatin1String("object"))
        out.problems << tr("The parameters are not an object (type '%1'): the form cannot edit them")
                            .arg(type.toVariant().toString());
    for (auto it = out.top.begin(); it != out.top.end(); ++it) {
        if (!it.key().startsWith(QLatin1String("x-")) && topKeywords().count(it.key()) == 0)
            out.problems << tr("The schema's '%1' cannot be edited here").arg(it.key());
    }
    if (out.top.contains(QStringLiteral("properties")) && !out.top.value(QStringLiteral("properties")).isObject())
        out.problems << tr("The schema's 'properties' is not an object: there is nothing to edit here");
    const QJsonObject props = out.top.value(QStringLiteral("properties")).toObject();
    // Order: x-propertyOrder, then the rest by key.
    QStringList order;
    for (const QJsonValue& v : out.top.value(QStringLiteral("x-propertyOrder")).toArray()) {
        const QString k = v.toString();
        if (props.contains(k) && !order.contains(k))
            order << k;
    }
    QStringList rest = props.keys();
    std::sort(rest.begin(), rest.end());
    for (const QString& k : rest) {
        if (!order.contains(k))
            order << k;
    }
    for (const QString& k : order)
        out.fields.push_back(parseProperty(k, props.value(k)));
    // x-relevantWhen names choices or booleans of this schema, with their values.
    for (FormField& f : out.fields) {
        for (const auto& [other, values] : f.relevantWhen) {
            const auto it =
                std::find_if(out.fields.begin(), out.fields.end(), [&](const FormField& g) { return g.key == other; });
            if (it == out.fields.end() || (it->kind != FormField::Kind::Enum && it->kind != FormField::Kind::Boolean)) {
                out.problems << tr("'%1' depends on '%2', which is not a choice or a switch of this operation")
                                    .arg(f.title, other);
                continue;
            }
            for (const QJsonValue& v : values) {
                const bool ok = it->kind == FormField::Kind::Enum
                                    ? it->enumValues.contains(v.toString()) && v.isString()
                                    : v.isBool();
                if (!ok)
                    out.problems << tr("'%1' depends on a value '%2' that '%3' cannot have")
                                        .arg(f.title, v.toVariant().toString(), it->title);
            }
        }
    }
    return out;
}

QString unitLabel(Unit unit, const QString& lengthUnit)
{
    switch (unit) {
    case Unit::Length:
        return lengthUnit;
    case Unit::Angle:
        return QStringLiteral("deg");
    case Unit::Time:
        return QStringLiteral("s");
    case Unit::Percent:
        return QStringLiteral("%");
    case Unit::Count:
    case Unit::None:
        break;
    }
    return {};
}

QString numberText(double value, Unit unit, const QString& lengthUnit)
{
    if (unit == Unit::Length) {
        if (lengthUnit == QLatin1String("in"))
            return cText(value / 25.4) + QStringLiteral(" in");
        return cText(value) + QStringLiteral(" mm");
    }
    const QString label = unitLabel(unit, lengthUnit);
    return label.isEmpty() ? cText(value) : cText(value) + QLatin1Char(' ') + label;
}

ExprFn plainExprFn()
{
    return [](const QString& expression, int kind, QString* error) -> std::optional<double> {
        static const QRegularExpression re(
            QStringLiteral(R"(^\s*([-+]?[0-9]*\.?[0-9]+(?:[eE][-+]?[0-9]+)?)\s*([a-z]*)\s*$)"));
        const QRegularExpressionMatch m = re.match(expression);
        if (!m.hasMatch()) {
            *error = tr("'%1' is not a number").arg(expression.trimmed());
            return std::nullopt;
        }
        const double v = m.captured(1).toDouble();
        const QString u = m.captured(2);
        if (kind == 0 && (u == QLatin1String("mm") || u.isEmpty()))
            return v;
        if (kind == 0 && u == QLatin1String("in"))
            return v * 25.4;
        if (kind == 1 && (u == QLatin1String("deg") || u.isEmpty()))
            return v;
        if (kind == 1 && u == QLatin1String("rad"))
            return v * 57.29577951308232;
        if (kind == 2 && u.isEmpty())
            return v;
        *error = tr("'%1' has a unit that does not fit here").arg(expression.trimmed());
        return std::nullopt;
    };
}

std::optional<double> readNumber(const QString& raw, Unit unit, const QString& lengthUnit, const ExprFn& expr,
                                 QString* error)
{
    const QString text = raw.trimmed();
    if (text.isEmpty()) {
        *error = tr("Enter a value");
        return std::nullopt;
    }
    bool bare = false;
    QLocale::c().toDouble(text, &bare);
    switch (unit) {
    case Unit::Length:
        return expr(bare ? text + QLatin1Char(' ') + lengthUnit : text, 0, error);
    case Unit::Angle:
        return expr(bare ? text + QStringLiteral(" deg") : text, 1, error);
    case Unit::Time: {
        static const QRegularExpression suffix(QStringLiteral(R"(^(.*?)\s*(ms|s|sec|min)$)"));
        const QRegularExpressionMatch m = suffix.match(text);
        if (m.hasMatch() && !m.captured(1).trimmed().isEmpty()) {
            const QString u = m.captured(2);
            const double scale = u == QLatin1String("ms") ? 0.001 : u == QLatin1String("min") ? 60.0 : 1.0;
            const auto v = expr(m.captured(1), 2, error);
            return v ? std::optional<double>(*v * scale) : std::nullopt;
        }
        return expr(text, 2, error);
    }
    case Unit::Percent: {
        // A trailing '%' is the unit; the rest is a plain number or expression.
        QString rest = text;
        if (rest.endsWith(QLatin1Char('%'))) {
            rest.chop(1);
            rest = rest.trimmed();
            if (rest.isEmpty()) {
                *error = tr("Enter a value");
                return std::nullopt;
            }
        }
        return expr(rest, 2, error);
    }
    case Unit::Count:
    case Unit::None:
        break;
    }
    return expr(text, 2, error);
}

std::optional<std::vector<PointTokens>> parsePoints(const QString& text, QString* error)
{
    // The grammar pocket2d's parsePointList reads (src/cam/ops/pocket2d.h): fixed-point numbers, an optional '+',
    // spaces or tabs, a comma between x and y, ';' between points.
    std::vector<PointTokens> out;
    qsizetype i = 0;
    const auto space = [&] {
        while (i < text.size() && (text[i] == QLatin1Char(' ') || text[i] == QLatin1Char('\t')))
            ++i;
    };
    const auto number = [&](QString& token) {
        space();
        const qsizetype start = i;
        if (i < text.size() && (text[i] == QLatin1Char('+') || text[i] == QLatin1Char('-')))
            ++i;
        bool digits = false;
        while (i < text.size() && text[i].isDigit() && text[i].unicode() < 128) {
            ++i;
            digits = true;
        }
        if (i < text.size() && text[i] == QLatin1Char('.')) {
            ++i;
            while (i < text.size() && text[i].isDigit() && text[i].unicode() < 128) {
                ++i;
                digits = true;
            }
        }
        if (!digits)
            return false;
        token = text.mid(start, i - start);
        if (token.startsWith(QLatin1Char('+')))
            token.remove(0, 1);
        bool ok = false;
        const double v = QLocale::c().toDouble(token, &ok);
        return ok && std::isfinite(v) && std::abs(v) <= 1e6;
    };
    for (int count = 1;; ++count) {
        space();
        if (i >= text.size())
            break;
        PointTokens p;
        if (!number(p.x)) {
            *error = tr("Point %1 does not start with a number").arg(count);
            return std::nullopt;
        }
        space();
        if (i < text.size() && text[i] == QLatin1Char(','))
            ++i;
        if (!number(p.y)) {
            *error = tr("Point %1 has no second number").arg(count);
            return std::nullopt;
        }
        out.push_back(p);
        space();
        if (i < text.size()) {
            if (text[i] != QLatin1Char(';')) {
                *error = tr("After point %1 comes something other than ';'").arg(count);
                return std::nullopt;
            }
            ++i;
        }
    }
    return out;
}

QString pointsText(const std::vector<PointTokens>& points)
{
    QStringList parts;
    for (const PointTokens& p : points)
        parts << p.x + QLatin1Char(' ') + p.y;
    return parts.join(QStringLiteral("; "));
}

QString mmToken(double mm)
{
    QString s = QLocale::c().toString(mm, 'f', 6);
    while (s.contains(QLatin1Char('.')) && s.endsWith(QLatin1Char('0')))
        s.chop(1);
    if (s.endsWith(QLatin1Char('.')))
        s.chop(1);
    if (s == QLatin1String("-0"))
        s = QStringLiteral("0");
    return s;
}

} // namespace ray::ui::camui

namespace ray::ui {

using camui::FormField;

SchemaFormModel::SchemaFormModel(QObject* parent) : QObject(parent), expr_(camui::plainExprFn()) {}

void SchemaFormModel::setSchemaJson(const QString& json)
{
    if (json == schemaJson_ && !schema_.fields.empty())
        return;
    schemaJson_ = json;
    schema_ = camui::parseFormSchema(json);
    entries_.clear();
    kept_ = {};
    external_.clear();
    load({});
    emit schemaChanged();
}

void SchemaFormModel::setLengthUnit(const QString& unit)
{
    const QString u = unit == QLatin1String("in") ? unit : QStringLiteral("mm");
    if (u == lengthUnit_)
        return;
    // The values stay; their text follows the unit.
    const QJsonObject now = params();
    lengthUnit_ = u;
    load(now);
}

void SchemaFormModel::setExprFn(camui::ExprFn fn)
{
    expr_ = fn ? std::move(fn) : camui::plainExprFn();
    bump();
}

const FormField* SchemaFormModel::field(const QString& key) const
{
    for (const FormField& f : schema_.fields) {
        if (f.key == key)
            return &f;
    }
    return nullptr;
}

QVariant SchemaFormModel::fullTextOf(const FormField& f, const QJsonValue& v) const
{
    if ((f.kind != FormField::Kind::Number && f.kind != FormField::Kind::Integer) || !v.isDouble())
        return {};
    return QVariant(camui::numberText(v.toDouble(), f.unit, lengthUnit_));
}

void SchemaFormModel::setLengthFormat(std::function<QString(double mm)> format)
{
    const QJsonObject now = params();
    lengthFormat_ = std::move(format);
    load(now);
}

QVariant SchemaFormModel::textOf(const FormField& f, const QJsonValue& v) const
{
    switch (f.kind) {
    case FormField::Kind::Number:
    case FormField::Kind::Integer:
        if (!v.isDouble())
            return QVariant(QString());
        // A length at rest in the app's display format (Units); the full value while it is edited (fullTextOf).
        if (f.unit == camui::Unit::Length && lengthFormat_)
            return QVariant(lengthFormat_(v.toDouble()));
        return QVariant(camui::numberText(v.toDouble(), f.unit, lengthUnit_));
    case FormField::Kind::Boolean:
        return QVariant(v.toBool());
    case FormField::Kind::Enum:
    case FormField::Kind::String:
        return QVariant(v.toString());
    case FormField::Kind::Unsupported:
        break;
    }
    return {};
}

void SchemaFormModel::load(const QJsonObject& params)
{
    entries_.clear();
    kept_ = {};
    for (const FormField& f : schema_.fields) {
        const QJsonValue stored = params.contains(f.key) ? params.value(f.key) : f.defaultValue;
        if (f.kind == FormField::Kind::Unsupported) {
            if (params.contains(f.key))
                kept_.insert(f.key, params.value(f.key));
            continue;
        }
        Entry e;
        e.text = textOf(f, stored);
        if (!stored.isUndefined()) {
            e.loaded = stored;
            e.loadedText = e.text;
            e.fullText = fullTextOf(f, stored);
        }
        entries_[f.key] = std::move(e);
    }
    // Keys the schema does not have are not the form's: the API refuses them, so they are not kept.
    external_.clear();
    pointEdits_.clear();
    bump();
}

std::optional<QJsonValue> SchemaFormModel::read(const FormField& f, QString* error) const
{
    const auto it = entries_.find(f.key);
    if (it == entries_.end()) {
        *error = tr("Missing");
        return std::nullopt;
    }
    const Entry& e = it->second;
    if (f.kind == FormField::Kind::String && f.format == QLatin1String("pointsXY")) {
        // A coordinate typed that does not read, or a stored text that is not points (an older document's typing).
        if (const QString pending = pointEditError(f); !pending.isEmpty()) {
            *error = pending;
            return std::nullopt;
        }
        if (!camui::parsePoints(e.text.toString(), error))
            return std::nullopt;
    }
    if (e.loaded && (e.text == e.loadedText || (e.fullText.isValid() && e.text == e.fullText)))
        return *e.loaded; // as stored: no round trip through the display text
    switch (f.kind) {
    case FormField::Kind::Boolean:
        return QJsonValue(e.text.toBool());
    case FormField::Kind::Enum: {
        const QString v = e.text.toString();
        if (!f.enumValues.contains(v)) {
            *error = tr("Choose one of the list");
            return std::nullopt;
        }
        return QJsonValue(v);
    }
    case FormField::Kind::String: {
        const QString v = e.text.toString();
        if (f.minLength && v.size() < *f.minLength) {
            *error = *f.minLength == 1 ? tr("Must not be empty") : tr("At least %1 characters").arg(*f.minLength);
            return std::nullopt;
        }
        if (f.maxLength && v.size() > *f.maxLength) {
            *error = tr("At most %1 characters").arg(*f.maxLength);
            return std::nullopt;
        }
        if (!f.pattern.isEmpty() && !QRegularExpression(f.pattern).match(v).hasMatch()) {
            *error = tr("Not in the form this parameter takes");
            return std::nullopt;
        }
        return QJsonValue(v);
    }
    case FormField::Kind::Number:
    case FormField::Kind::Integer: {
        const auto v = camui::readNumber(e.text.toString(), f.unit, lengthUnit_, expr_, error);
        if (!v)
            return std::nullopt;
        if (!std::isfinite(*v)) {
            *error = tr("Must be a finite number");
            return std::nullopt;
        }
        if (f.kind == FormField::Kind::Integer && std::floor(*v) != *v) {
            *error = tr("Must be a whole number");
            return std::nullopt;
        }
        const auto limit = [&](double x) {
            return camui::numberText(x, f.unit, lengthUnit_);
        };
        if (f.minimum && *v < *f.minimum) {
            *error = tr("Must be at least %1").arg(limit(*f.minimum));
            return std::nullopt;
        }
        if (f.exclusiveMinimum && *v <= *f.exclusiveMinimum) {
            *error = tr("Must be more than %1").arg(limit(*f.exclusiveMinimum));
            return std::nullopt;
        }
        if (f.maximum && *v > *f.maximum) {
            *error = tr("Must be at most %1").arg(limit(*f.maximum));
            return std::nullopt;
        }
        if (f.exclusiveMaximum && *v >= *f.exclusiveMaximum) {
            *error = tr("Must be less than %1").arg(limit(*f.exclusiveMaximum));
            return std::nullopt;
        }
        return QJsonValue(*v);
    }
    case FormField::Kind::Unsupported:
        break;
    }
    *error = tr("Cannot be edited here");
    return std::nullopt;
}

bool SchemaFormModel::relevant(const FormField& f, int depth) const
{
    if (depth > 32)
        return true; // a cycle of conditions: shown rather than lost
    for (const auto& [other, values] : f.relevantWhen) {
        const FormField* g = field(other);
        if (g == nullptr || (g->kind != FormField::Kind::Enum && g->kind != FormField::Kind::Boolean))
            continue; // reported as a schema problem
        if (!relevant(*g, depth + 1))
            return false;
        const QJsonValue v = valueOf(other);
        if (!values.contains(v))
            return false;
    }
    return true;
}

bool SchemaFormModel::isRelevant(const QString& key) const
{
    const FormField* f = field(key);
    return f != nullptr && relevant(*f);
}

QJsonValue SchemaFormModel::valueOf(const QString& key) const
{
    const FormField* f = field(key);
    if (f == nullptr)
        return QJsonValue(QJsonValue::Undefined);
    QString error;
    if (auto v = read(*f, &error))
        return *v;
    return f->defaultValue;
}

QString SchemaFormModel::messageOf(const QString& key) const
{
    const FormField* f = field(key);
    if (f == nullptr || f->kind == FormField::Kind::Unsupported || !relevant(*f))
        return {};
    QString error;
    if (!read(*f, &error))
        return error;
    const auto ext = external_.find(key);
    return ext == external_.end() ? QString() : ext->second;
}

std::optional<std::pair<QString, QString>> SchemaFormModel::firstError() const
{
    for (const FormField& f : schema_.fields) {
        const QString m = messageOf(f.key);
        if (!m.isEmpty())
            return std::make_pair(f.key, m);
    }
    return std::nullopt;
}

bool SchemaFormModel::valid() const
{
    return !firstError().has_value();
}

QJsonObject SchemaFormModel::params() const
{
    QJsonObject out = kept_;
    for (const FormField& f : schema_.fields) {
        if (f.kind == FormField::Kind::Unsupported)
            continue;
        QString error;
        auto v = read(f, &error);
        if (v)
            out.insert(f.key, *v);
        else if (!f.defaultValue.isUndefined())
            out.insert(f.key, f.defaultValue);
    }
    return out;
}

QVariantList SchemaFormModel::fields() const
{
    // Each row of SchemaForm.qml reads the list: it is made once per change.
    if (fieldsRevision_ == revision_)
        return fieldsCache_;
    QVariantList& out = fieldsCache_;
    out.clear();
    fieldsRevision_ = revision_;
    for (const FormField& f : schema_.fields) {
        QVariantMap m;
        m.insert(QStringLiteral("key"), f.key);
        m.insert(QStringLiteral("title"), f.title);
        m.insert(QStringLiteral("description"), f.description);
        QString kind;
        switch (f.kind) {
        case FormField::Kind::Number:
            kind = QStringLiteral("number");
            break;
        case FormField::Kind::Integer:
            kind = QStringLiteral("integer");
            break;
        case FormField::Kind::Boolean:
            kind = QStringLiteral("boolean");
            break;
        case FormField::Kind::Enum:
            kind = QStringLiteral("enum");
            break;
        case FormField::Kind::String:
            kind = f.format == QLatin1String("pointsXY") ? QStringLiteral("points") : QStringLiteral("string");
            break;
        case FormField::Kind::Unsupported:
            kind = QStringLiteral("unsupported");
            break;
        }
        m.insert(QStringLiteral("kind"), kind);
        m.insert(QStringLiteral("multiline"), f.multiline);
        if (kind == QLatin1String("points")) {
            // Each coordinate as a length field shows it; what is being typed where it does not read.
            const auto e = entries_.find(f.key);
            QString unused;
            const auto points =
                camui::parsePoints(e == entries_.end() ? QString() : e->second.text.toString(), &unused);
            const auto edits = pointEdits_.find(f.key);
            QVariantList list;
            for (std::size_t i = 0; points && i < points->size(); ++i) {
                QVariantMap point;
                const QString tokens[2] = {(*points)[i].x, (*points)[i].y};
                for (int axis = 0; axis < 2; ++axis) {
                    const QString name = axis == 0 ? QStringLiteral("x") : QStringLiteral("y");
                    const double mm = QLocale::c().toDouble(tokens[axis]);
                    QString shown =
                        lengthFormat_ ? lengthFormat_(mm) : camui::numberText(mm, camui::Unit::Length, lengthUnit_);
                    QString full = camui::numberText(mm, camui::Unit::Length, lengthUnit_);
                    bool bad = false;
                    if (edits != pointEdits_.end()) {
                        if (const auto t = edits->second.find(static_cast<int>(i) * 2 + axis);
                            t != edits->second.end()) {
                            shown = full = t->second; // as typed
                            QString error;
                            const auto v =
                                camui::readNumber(t->second, camui::Unit::Length, lengthUnit_, expr_, &error);
                            bad = !v || !std::isfinite(*v) || std::abs(*v) > 1e6;
                        }
                    }
                    point.insert(name, shown);
                    point.insert(name + QStringLiteral("Full"), full);
                    point.insert(name + QStringLiteral("Bad"), bad);
                }
                list.push_back(point);
            }
            m.insert(QStringLiteral("points"), list);
            m.insert(QStringLiteral("pointsRead"), points.has_value());
        }
        m.insert(QStringLiteral("unit"), camui::unitLabel(f.unit, lengthUnit_));
        const auto e = entries_.find(f.key);
        m.insert(QStringLiteral("value"), e == entries_.end() ? QVariant(QString()) : e->second.text);
        // While the field shows a stored value, the text it edits: that value in full.
        const bool atRest = e != entries_.end() && e->second.loaded && e->second.text == e->second.loadedText;
        m.insert(QStringLiteral("fullValue"), atRest ? e->second.fullText.toString() : QString());
        QVariantList choices;
        for (qsizetype i = 0; i < f.enumValues.size(); ++i)
            choices.push_back(QVariantMap{{QStringLiteral("value"), f.enumValues[i]},
                                          {QStringLiteral("text"), f.enumTitles.value(i, f.enumValues[i])}});
        m.insert(QStringLiteral("choices"), choices);
        m.insert(QStringLiteral("relevant"), f.kind == FormField::Kind::Unsupported || relevant(f));
        m.insert(QStringLiteral("message"), messageOf(f.key));
        const bool hasDefault = !f.defaultValue.isUndefined();
        m.insert(QStringLiteral("hasDefault"), hasDefault);
        // A text that does not read is not the default (reset is what brings it back).
        QString unused;
        const auto now = f.kind == FormField::Kind::Unsupported ? std::optional<QJsonValue>() : read(f, &unused);
        m.insert(QStringLiteral("isDefault"),
                 !hasDefault || f.kind == FormField::Kind::Unsupported || (now && *now == f.defaultValue));
        m.insert(QStringLiteral("defaultText"), hasDefault ? textOf(f, f.defaultValue).toString() : QString());
        m.insert(QStringLiteral("reason"), f.reason);
        out.push_back(m);
    }
    return out;
}

QVariantList SchemaFormModel::choicesOf(const QString& key) const
{
    QVariantList choices;
    if (const FormField* f = field(key)) {
        for (qsizetype i = 0; i < f->enumValues.size(); ++i)
            choices.push_back(QVariantMap{{QStringLiteral("value"), f->enumValues[i]},
                                          {QStringLiteral("text"), f->enumTitles.value(i, f->enumValues[i])},
                                          {QStringLiteral("description"), f->enumDescriptions.value(i)}});
    }
    return choices;
}

void SchemaFormModel::setValue(const QString& key, const QVariant& value)
{
    const FormField* f = field(key);
    if (f == nullptr || f->kind == FormField::Kind::Unsupported)
        return;
    Entry& e = entries_[key];
    const QVariant v = f->kind == FormField::Kind::Boolean ? QVariant(value.toBool()) : QVariant(value.toString());
    if (e.text == v)
        return;
    e.text = v;
    external_.erase(key);
    pointEdits_.erase(key); // a points field typed as text
    bump();
    emit edited(key);
}

void SchemaFormModel::setPoints(const QString& key, const std::vector<camui::PointTokens>& points)
{
    Entry& e = entries_[key];
    e.text = QVariant(camui::pointsText(points));
    external_.erase(key);
    bump();
    emit edited(key);
}

void SchemaFormModel::setPointCoordinate(const QString& key, int index, int axis, const QString& text)
{
    const FormField* f = field(key);
    if (f == nullptr || f->format != QLatin1String("pointsXY") || index < 0 || axis < 0 || axis > 1)
        return;
    QString unused;
    auto points = camui::parsePoints(entries_[key].text.toString(), &unused);
    if (!points || index >= static_cast<int>(points->size()))
        return;
    // The field keeps the text as typed (as a length field does); the stored point follows it when it reads, else
    // keeps its value and the field says why.
    pointEdits_[key][index * 2 + axis] = text;
    QString error;
    const auto mm = camui::readNumber(text, camui::Unit::Length, lengthUnit_, expr_, &error);
    if (!mm || !std::isfinite(*mm) || std::abs(*mm) > 1e6) {
        external_.erase(key);
        bump();
        emit edited(key);
        return;
    }
    // The text as it shows the stored value (at rest or in full) is that value: no round trip through the display.
    const QString& token =
        axis == 0 ? (*points)[static_cast<std::size_t>(index)].x : (*points)[static_cast<std::size_t>(index)].y;
    const double stored = QLocale::c().toDouble(token);
    const bool asShown = text == camui::numberText(stored, camui::Unit::Length, lengthUnit_) ||
                         (lengthFormat_ && text == lengthFormat_(stored));
    if (!asShown)
        (axis == 0 ? (*points)[static_cast<std::size_t>(index)].x : (*points)[static_cast<std::size_t>(index)].y) =
            camui::mmToken(*mm);
    setPoints(key, *points);
}

void SchemaFormModel::addPoint(const QString& key, double xMm, double yMm)
{
    const FormField* f = field(key);
    if (f == nullptr || f->format != QLatin1String("pointsXY") || !std::isfinite(xMm) || !std::isfinite(yMm))
        return;
    QString unused;
    auto points = camui::parsePoints(entries_[key].text.toString(), &unused);
    if (!points)
        return; // the stored text does not read: it is fixed as text first
    points->push_back(camui::PointTokens{camui::mmToken(xMm), camui::mmToken(yMm)});
    setPoints(key, *points);
}

void SchemaFormModel::removePoint(const QString& key, int index)
{
    const FormField* f = field(key);
    if (f == nullptr || f->format != QLatin1String("pointsXY") || index < 0)
        return;
    QString unused;
    auto points = camui::parsePoints(entries_[key].text.toString(), &unused);
    if (!points || index >= static_cast<int>(points->size()))
        return;
    points->erase(points->begin() + index);
    // The coordinates being typed follow their points.
    if (const auto it = pointEdits_.find(key); it != pointEdits_.end()) {
        std::map<int, QString> moved;
        for (const auto& [slot, text] : it->second) {
            if (slot / 2 < index)
                moved[slot] = text;
            else if (slot / 2 > index)
                moved[slot - 2] = text;
        }
        if (moved.empty())
            pointEdits_.erase(it);
        else
            it->second = std::move(moved);
    }
    setPoints(key, *points);
}

QString SchemaFormModel::pointEditError(const FormField& f) const
{
    const auto it = pointEdits_.find(f.key);
    if (it == pointEdits_.end())
        return {};
    for (const auto& [slot, text] : it->second) {
        QString error;
        const auto mm = camui::readNumber(text, camui::Unit::Length, lengthUnit_, expr_, &error);
        if (mm && std::isfinite(*mm) && std::abs(*mm) <= 1e6)
            continue;
        if (mm)
            error = tr("Out of range");
        return tr("Point %1, %2: %3")
            .arg(slot / 2 + 1)
            .arg(slot % 2 == 0 ? QStringLiteral("X") : QStringLiteral("Y"), error);
    }
    return {};
}

QStringList SchemaFormModel::pointKeys() const
{
    QStringList out;
    for (const FormField& f : schema_.fields) {
        if (f.kind == FormField::Kind::String && f.format == QLatin1String("pointsXY"))
            out << f.key;
    }
    return out;
}

std::vector<std::pair<double, double>> SchemaFormModel::pointsOf(const QString& key) const
{
    std::vector<std::pair<double, double>> out;
    const auto e = entries_.find(key);
    if (e == entries_.end())
        return out;
    QString unused;
    if (const auto points = camui::parsePoints(e->second.text.toString(), &unused)) {
        for (const camui::PointTokens& p : *points)
            out.emplace_back(QLocale::c().toDouble(p.x), QLocale::c().toDouble(p.y));
    }
    return out;
}

void SchemaFormModel::resetField(const QString& key)
{
    const FormField* f = field(key);
    if (f == nullptr || f->kind == FormField::Kind::Unsupported || f->defaultValue.isUndefined())
        return;
    Entry& e = entries_[key];
    e.text = textOf(*f, f->defaultValue);
    e.loaded = f->defaultValue; // exactly the default, whatever the display unit
    e.loadedText = e.text;
    e.fullText = fullTextOf(*f, f->defaultValue);
    external_.erase(key);
    pointEdits_.erase(key);
    bump();
    emit edited(key);
}

void SchemaFormModel::resetAll()
{
    load({});
    emit edited(QString());
}

void SchemaFormModel::setExternalMessage(const QString& key, const QString& message)
{
    if (message.isEmpty())
        external_.erase(key);
    else
        external_[key] = message;
    bump();
}

void SchemaFormModel::clearExternal()
{
    if (external_.empty())
        return;
    external_.clear();
    bump();
}

void SchemaFormModel::bump()
{
    ++revision_;
    emit changed();
}

} // namespace ray::ui
