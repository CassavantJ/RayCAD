#include "palette_schema.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>

#include <cmath>

namespace ray::ui::palette {
namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("ray::ui::palette", text);
}

QJsonObject parseObject(const QString& json)
{
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    return doc.isObject() ? doc.object() : QJsonObject();
}

// "chordTolerance" -> "Chord tolerance", "discard_changes" -> "Discard changes".
QString humanize(const QString& name)
{
    QString out;
    for (qsizetype i = 0; i < name.size(); ++i) {
        const QChar c = name.at(i);
        if (c == QLatin1Char('_') || c == QLatin1Char('-')) {
            out += QLatin1Char(' ');
        } else if (c.isUpper() && i > 0 && name.at(i - 1).isLower()) {
            out += QLatin1Char(' ');
            out += c.toLower();
        } else {
            out += c;
        }
    }
    if (!out.isEmpty()) {
        out[0] = out.at(0).toUpper();
    }
    return out;
}

// The first type that is not "null" ("type": ["string", "null"] is a string).
QString typeOf(const QJsonObject& prop)
{
    const QJsonValue type = prop.value(QLatin1String("type"));
    if (type.isString()) {
        return type.toString();
    }
    for (const QJsonValue& t : type.toArray()) {
        if (t.toString() != QLatin1String("null")) {
            return t.toString();
        }
    }
    return {};
}

QString kindOf(const QJsonObject& prop)
{
    if (prop.value(QLatin1String("enum")).isArray()) {
        return QStringLiteral("enum");
    }
    const QString type = typeOf(prop);
    if (type == QLatin1String("string") || type == QLatin1String("number") || type == QLatin1String("integer") ||
        type == QLatin1String("boolean")) {
        return type;
    }
    return QStringLiteral("json");
}

bool isPathName(const QString& name)
{
    const QString lower = name.toLower();
    return lower == QLatin1String("path") || lower == QLatin1String("file") || name.endsWith(QLatin1String("Path")) ||
           name.endsWith(QLatin1String("File"));
}

QStringList filtersFor(const QString& actionId)
{
    const QString all = tr("All files (*)");
    const QString id = actionId.toLower();
    if (id.contains(QLatin1String("step"))) {
        return {tr("STEP files (*.step *.stp)"), all};
    }
    if (id.contains(QLatin1String("iges"))) {
        return {tr("IGES files (*.iges *.igs)"), all};
    }
    if (id.contains(QLatin1String("stl"))) {
        return {tr("STL files (*.stl)"), all};
    }
    if (id.contains(QLatin1String("save"))) {
        return {tr("RayCAD documents (*.raycad)"), all};
    }
    if (id.contains(QLatin1String("import"))) {
        return {tr("STEP and IGES files (*.step *.stp *.p21 *.iges *.igs)"), all};
    }
    if (id.contains(QLatin1String("open"))) {
        return {tr("RayCAD, STEP and IGES files (*.raycad *.step *.stp *.p21 *.iges *.igs)"), all};
    }
    return {all};
}

QString valueText(const QJsonValue& v)
{
    if (v.isString()) {
        return v.toString();
    }
    if (v.isDouble()) {
        return QString::number(v.toDouble(), 'g', 15);
    }
    if (v.isBool()) {
        return v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    }
    if (v.isArray()) {
        return QString::fromUtf8(QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact));
    }
    if (v.isObject()) {
        return QString::fromUtf8(QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact));
    }
    return {};
}

QStringList requiredOf(const QJsonObject& schema)
{
    QStringList out;
    for (const QJsonValue& r : schema.value(QLatin1String("required")).toArray()) {
        if (r.isString() && r.toString() != QLatin1String("document") && !out.contains(r.toString())) {
            out << r.toString();
        }
    }
    return out;
}

// Field order: required first (schema order), then the rest by name (QJsonObject keeps keys sorted).
QStringList fieldOrder(const QJsonObject& schema)
{
    const QJsonObject props = schema.value(QLatin1String("properties")).toObject();
    QStringList order;
    for (const QString& r : requiredOf(schema)) {
        if (props.contains(r)) {
            order << r;
        }
    }
    for (auto it = props.begin(); it != props.end(); ++it) {
        if (it.key() != QLatin1String("document") && !order.contains(it.key())) {
            order << it.key();
        }
    }
    return order;
}

} // namespace

QVariantList formFields(const QString& actionId, const QString& schemaJson)
{
    const QJsonObject schema = parseObject(schemaJson);
    const QJsonObject props = schema.value(QLatin1String("properties")).toObject();
    const QStringList required = requiredOf(schema);
    const bool saving = actionId.contains(QLatin1String("save"), Qt::CaseInsensitive) ||
                        actionId.contains(QLatin1String("export"), Qt::CaseInsensitive);
    QVariantList out;
    for (const QString& name : fieldOrder(schema)) {
        const QJsonObject prop = props.value(name).toObject();
        const QString kind = kindOf(prop);
        QVariantMap f;
        f.insert(QStringLiteral("name"), name);
        f.insert(QStringLiteral("label"), prop.contains(QLatin1String("title"))
                                              ? prop.value(QLatin1String("title")).toString()
                                              : humanize(name));
        f.insert(QStringLiteral("description"), prop.value(QLatin1String("description")).toString());
        f.insert(QStringLiteral("kind"), kind);
        f.insert(QStringLiteral("required"), required.contains(name));
        const bool hasDefault = prop.contains(QLatin1String("default"));
        f.insert(QStringLiteral("hasDefault"), hasDefault);
        f.insert(QStringLiteral("defaultText"),
                 hasDefault ? valueText(prop.value(QLatin1String("default"))) : QString());
        f.insert(QStringLiteral("defaultBool"), prop.value(QLatin1String("default")).toBool(false));
        QStringList choices;
        for (const QJsonValue& v : prop.value(QLatin1String("enum")).toArray()) {
            choices << valueText(v);
        }
        f.insert(QStringLiteral("choices"), choices);
        const bool path = kind == QLatin1String("string") && isPathName(name);
        f.insert(QStringLiteral("path"), path);
        f.insert(QStringLiteral("save"), path && saving);
        f.insert(QStringLiteral("filters"), path ? filtersFor(actionId) : QStringList());
        out << f;
    }
    return out;
}

bool needsForm(const QString& schemaJson)
{
    const QJsonObject schema = parseObject(schemaJson);
    return !requiredOf(schema).isEmpty() || schema.contains(QLatin1String("anyOf")) ||
           schema.contains(QLatin1String("oneOf"));
}

bool hasProperty(const QString& schemaJson, const QString& name)
{
    return parseObject(schemaJson).value(QLatin1String("properties")).toObject().contains(name);
}

Args buildArgs(const QString& schemaJson, const QVariantMap& values)
{
    const QJsonObject schema = parseObject(schemaJson);
    const QJsonObject props = schema.value(QLatin1String("properties")).toObject();
    const QStringList required = requiredOf(schema);
    QJsonObject out;
    Args args;
    auto fail = [&](const QString& field, const QString& message) {
        args.ok = false;
        args.field = field;
        args.error = message;
        return args;
    };
    for (const QString& name : fieldOrder(schema)) {
        const QJsonObject prop = props.value(name).toObject();
        const QString kind = kindOf(prop);
        const QString label =
            prop.contains(QLatin1String("title")) ? prop.value(QLatin1String("title")).toString() : humanize(name);
        const bool isRequired = required.contains(name);
        const QVariant value = values.value(name);
        if (kind == QLatin1String("boolean")) {
            const bool on = value.toBool();
            const bool def = prop.value(QLatin1String("default")).toBool(false);
            if (isRequired || on != def) {
                out.insert(name, on);
            }
            continue;
        }
        const QString text = kind == QLatin1String("string") ? value.toString() : value.toString().trimmed();
        if (text.isEmpty()) {
            if (isRequired) {
                return fail(name, tr("%1 is required").arg(label));
            }
            continue;
        }
        if (kind == QLatin1String("string")) {
            out.insert(name, text);
        } else if (kind == QLatin1String("number") || kind == QLatin1String("integer")) {
            bool ok = false;
            const double v = text.toDouble(&ok);
            if (!ok || !std::isfinite(v)) {
                return fail(name, tr("%1 must be a number").arg(label));
            }
            if (kind == QLatin1String("integer")) {
                if (std::floor(v) != v || std::abs(v) > 9007199254740992.0) {
                    return fail(name, tr("%1 must be a whole number").arg(label));
                }
                out.insert(name, static_cast<qint64>(v));
            } else {
                out.insert(name, v);
            }
        } else if (kind == QLatin1String("enum")) {
            bool found = false;
            for (const QJsonValue& choice : prop.value(QLatin1String("enum")).toArray()) {
                if (valueText(choice) == text) {
                    out.insert(name, choice);
                    found = true;
                    break;
                }
            }
            if (!found) {
                return fail(name, tr("%1 must be one of the listed values").arg(label));
            }
        } else {
            QJsonParseError error{};
            const QJsonDocument doc = QJsonDocument::fromJson(QStringLiteral("{\"v\":%1}").arg(text).toUtf8(), &error);
            if (error.error != QJsonParseError::NoError || !doc.isObject()) {
                return fail(name, tr("%1 is not valid JSON").arg(label));
            }
            out.insert(name, doc.object().value(QLatin1String("v")));
        }
    }
    args.ok = true;
    args.json = QString::fromUtf8(QJsonDocument(out).toJson(QJsonDocument::Compact));
    return args;
}

} // namespace ray::ui::palette
