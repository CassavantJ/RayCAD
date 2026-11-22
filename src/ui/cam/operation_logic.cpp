#include "operation_logic.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QLocale>
#include <QtCore/QRegularExpression>

#include <algorithm>
#include <cmath>

namespace ray::ui::camui {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("ray::ui::OperationToolModel", text);
}

QString cText(double v, char format, int precision)
{
    return QLocale::c().toString(v, format, precision);
}

// Fixed decimals without trailing zeros: 12.50 -> "12.5", 300.0 -> "300".
QString trimmed(double v, int decimals)
{
    QString s = cText(v, 'f', decimals);
    if (s.contains(QLatin1Char('.'))) {
        while (s.endsWith(QLatin1Char('0')))
            s.chop(1);
        if (s.endsWith(QLatin1Char('.')))
            s.chop(1);
    }
    return s;
}

} // namespace

GeometrySpec geometrySpec(const QJsonObject& schema)
{
    GeometrySpec g;
    const QJsonObject o = schema.value(QStringLiteral("x-geometry")).toObject();
    const QString kind = o.value(QStringLiteral("kind")).toString();
    if (kind == QLatin1String("hole") || kind == QLatin1String("face") || kind == QLatin1String("edge"))
        g.kind = kind;
    const QString count = o.value(QStringLiteral("count")).toString();
    if (count == QLatin1String("one") || count == QLatin1String("oneOrMore") || count == QLatin1String("any"))
        g.count = count;
    g.description = o.value(QStringLiteral("description")).toString();
    return g;
}

const QStringList& heightKeys()
{
    static const QStringList keys = {QStringLiteral("clearance"), QStringLiteral("retract"),
                                     QStringLiteral("feedPlane"), QStringLiteral("top"), QStringLiteral("bottom")};
    return keys;
}

const QStringList& cuttingKeys()
{
    static const QStringList keys = {QStringLiteral("rpm"),         QStringLiteral("clockwise"),
                                     QStringLiteral("feedCut"),     QStringLiteral("feedPlunge"),
                                     QStringLiteral("feedRamp"),    QStringLiteral("feedLeadIn"),
                                     QStringLiteral("feedLeadOut"), QStringLiteral("coolant")};
    return keys;
}

namespace {
QStringList usedOf(const QJsonObject& schema, const char* key, const QStringList& all)
{
    const QJsonValue v = schema.value(QLatin1String(key));
    if (!v.isArray())
        return all;
    QStringList named;
    for (const QJsonValue& x : v.toArray())
        named << x.toString();
    QStringList out;
    for (const QString& k : all) {
        if (named.contains(k))
            out << k;
    }
    return out;
}
} // namespace

QStringList heightsUsed(const QJsonObject& schema)
{
    return usedOf(schema, "x-heightsUsed", heightKeys());
}

QStringList cuttingUsed(const QJsonObject& schema)
{
    QStringList used = usedOf(schema, "x-cuttingUsed", cuttingKeys());
    for (const char* always : {"rpm", "feedCut"}) {
        if (!used.contains(QLatin1String(always)))
            used << QLatin1String(always);
    }
    QStringList ordered;
    for (const QString& k : cuttingKeys()) {
        if (used.contains(k))
            ordered << k;
    }
    return ordered;
}

QStringList toolTypesFor(const QJsonObject& schema, const QJsonObject& params)
{
    const QJsonValue v = schema.value(QStringLiteral("x-toolTypes"));
    const auto list = [](const QJsonValue& a) {
        QStringList out;
        for (const QJsonValue& x : a.toArray())
            out << x.toString();
        return out;
    };
    if (v.isArray())
        return list(v);
    if (!v.isObject())
        return {};
    const QJsonObject o = v.toObject();
    const QString property = o.value(QStringLiteral("property")).toString();
    const QJsonObject values = o.value(QStringLiteral("values")).toObject();
    QJsonValue current = params.value(property);
    if (current.isUndefined()) {
        // The property's default.
        current = schema.value(QStringLiteral("properties"))
                      .toObject()
                      .value(property)
                      .toObject()
                      .value(QStringLiteral("default"));
    }
    const QString key =
        current.isBool() ? (current.toBool() ? QStringLiteral("true") : QStringLiteral("false")) : current.toString();
    if (values.contains(key))
        return list(values.value(key));
    return list(o.value(QStringLiteral("default")));
}

std::map<QString, double> completeHeights(const std::map<QString, double>& used)
{
    std::map<QString, double> out = used;
    const QStringList& keys = heightKeys();
    for (qsizetype i = 0; i < keys.size(); ++i) {
        if (used.count(keys[i]) != 0)
            continue;
        std::optional<double> v;
        for (qsizetype j = i - 1; j >= 0 && !v; --j) {
            if (const auto it = used.find(keys[j]); it != used.end())
                v = it->second;
        }
        for (qsizetype j = i + 1; j < keys.size() && !v; ++j) {
            if (const auto it = used.find(keys[j]); it != used.end())
                v = it->second;
        }
        out[keys[i]] = v.value_or(0.0);
    }
    return out;
}

std::optional<std::pair<QString, QString>> heightsOutOfOrder(const std::map<QString, double>& h)
{
    const QStringList& keys = heightKeys();
    for (qsizetype i = 0; i + 1 < keys.size(); ++i) {
        for (qsizetype j = i + 1; j < keys.size(); ++j) {
            const auto a = h.find(keys[i]), b = h.find(keys[j]);
            if (a != h.end() && b != h.end() && a->second < b->second)
                return std::make_pair(keys[i], keys[j]);
        }
    }
    return std::nullopt;
}

// ---------- Feeds ----------
QString rpmText(double rpm)
{
    return trimmed(rpm, 0) + QStringLiteral(" rpm");
}

QString feedText(double mmMin, bool inch)
{
    if (inch)
        return trimmed(mmMin / 25.4, 2) + QStringLiteral(" in/min");
    return trimmed(mmMin, 1) + QStringLiteral(" mm/min");
}

QString surfaceSpeedText(double mMin, bool inch)
{
    if (inch)
        return trimmed(mMin / 0.3048, 0) + QStringLiteral(" sfm");
    return trimmed(mMin, 1) + QStringLiteral(" m/min");
}

QString chipLoadText(double mm, bool inch)
{
    if (inch)
        return cText(mm / 25.4, 'f', 4) + QStringLiteral(" in/tooth");
    return trimmed(mm, 4) + QStringLiteral(" mm/tooth");
}

QString powerText(double kw)
{
    return trimmed(kw, 2) + QStringLiteral(" kW");
}

std::optional<double> readFeed(const QString& raw, bool inch, const ExprFn& expr, QString* error)
{
    QString text = raw.trimmed();
    if (text.isEmpty()) {
        *error = tr("Enter a feed");
        return std::nullopt;
    }
    double scale = inch ? 25.4 : 1.0;
    static const QRegularExpression suffix(QStringLiteral(R"(^(.*?)\s*(mm/min|mmpm|in/min|ipm)$)"));
    if (const QRegularExpressionMatch m = suffix.match(text); m.hasMatch()) {
        const QString u = m.captured(2);
        scale = u == QLatin1String("in/min") || u == QLatin1String("ipm") ? 25.4 : 1.0;
        text = m.captured(1);
    }
    const auto v = expr(text, 2, error);
    if (!v)
        return std::nullopt;
    if (!(*v > 0) || !std::isfinite(*v)) {
        *error = tr("Must be more than 0");
        return std::nullopt;
    }
    return *v * scale;
}

std::optional<double> readRpm(const QString& raw, const ExprFn& expr, QString* error)
{
    QString text = raw.trimmed();
    if (text.endsWith(QLatin1String("rpm")))
        text.chop(3);
    if (text.trimmed().isEmpty()) {
        *error = tr("Enter a spindle speed");
        return std::nullopt;
    }
    const auto v = expr(text, 2, error);
    if (!v)
        return std::nullopt;
    if (!(*v >= 1) || !std::isfinite(*v)) {
        *error = tr("Must be at least 1 rpm");
        return std::nullopt;
    }
    return *v;
}

QString limitStateText(cam::LimitState state)
{
    switch (state) {
    case cam::LimitState::NotReached:
        return tr("Not reached");
    case cam::LimitState::Applied:
        return tr("Applied");
    case cam::LimitState::Exceeded:
        return tr("Exceeded");
    case cam::LimitState::Unknown:
        break;
    }
    return tr("Not known");
}

bool matchesRecord(const cam::CalculatedFeeds& r, double rpm, double feedCut, double feedPlunge, double feedRamp,
                   const QStringList& used)
{
    const auto same = [](double a, double b) {
        return std::abs(a - b) <= 1e-6 * std::max(1.0, std::abs(b));
    };
    if (!same(rpm, r.spindleRpm) || !same(feedCut, r.feedCut))
        return false;
    if (used.contains(QLatin1String("feedPlunge")) && !same(feedPlunge, r.feedPlunge))
        return false;
    if (used.contains(QLatin1String("feedRamp")) && !same(feedRamp, r.feedRamp))
        return false;
    return true;
}

QJsonObject calculatedJson(const cam::CalculatedFeeds& r)
{
    const auto id = [](const Uuid& u) {
        return u == Uuid{} ? QJsonValue() : QJsonValue(QString::fromStdString(u.toString()));
    };
    QJsonObject o;
    o.insert(QStringLiteral("tool"), QString::fromStdString(r.tool.value.toString()));
    o.insert(QStringLiteral("material"), id(r.material.value));
    o.insert(QStringLiteral("machine"), id(r.machine.value));
    o.insert(QStringLiteral("opClass"), QString::fromStdString(r.opClass));
    o.insert(QStringLiteral("surfaceSpeed"), r.surfaceSpeedMMin ? QJsonValue(*r.surfaceSpeedMMin) : QJsonValue());
    o.insert(QStringLiteral("chipLoad"), r.chipLoadMm ? QJsonValue(*r.chipLoadMm) : QJsonValue());
    o.insert(QStringLiteral("rpm"), r.spindleRpm);
    o.insert(QStringLiteral("feedCut"), r.feedCut);
    o.insert(QStringLiteral("feedPlunge"), r.feedPlunge);
    o.insert(QStringLiteral("feedRamp"), r.feedRamp);
    return o;
}

// ---------- Tools ----------
void sortTools(std::vector<ToolRow>& rows)
{
    std::stable_sort(rows.begin(), rows.end(), [](const ToolRow& a, const ToolRow& b) {
        if (a.embedded != b.embedded)
            return a.embedded;
        if (a.suits != b.suits)
            return a.suits;
        if (a.diameter != b.diameter)
            return a.diameter < b.diameter;
        if (a.name != b.name)
            return a.name < b.name;
        return a.id < b.id;
    });
}

QString toolDetail(const ToolRow& row, bool inch)
{
    const QString d = inch ? trimmed(row.diameter / 25.4, 4) + QStringLiteral(" in")
                           : trimmed(row.diameter, 3) + QStringLiteral(" mm");
    const QString flutes = row.flutes == 1 ? tr("1 flute") : tr("%1 flutes").arg(row.flutes);
    return tr("⌀%1, %2, %3").arg(d, flutes, row.type);
}

bool toolMatches(const ToolRow& row, const QString& search, bool inch)
{
    const QString hay = (row.name + QLatin1Char(' ') + toolDetail(row, inch)).toLower();
    const QStringList words = search.toLower().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    return std::all_of(words.begin(), words.end(), [&](const QString& w) { return hay.contains(w); });
}

// ---------- Arguments ----------
QJsonObject operationArgs(const OperationInput& in, bool adding)
{
    QJsonObject args;
    args.insert(QStringLiteral("id"), in.id);
    if (adding) {
        args.insert(QStringLiteral("setup"), in.setup);
        args.insert(QStringLiteral("type"), in.type);
    }
    args.insert(QStringLiteral("name"), in.name);
    QJsonObject tool;
    tool.insert(QStringLiteral("tool"), in.tool);
    tool.insert(QStringLiteral("holder"), in.holder.isEmpty() ? QJsonValue() : QJsonValue(in.holder));
    if (in.toolNumber) {
        tool.insert(QStringLiteral("number"), *in.toolNumber);
        tool.insert(QStringLiteral("lengthOffset"), *in.toolNumber);
        tool.insert(QStringLiteral("diameterOffset"), *in.toolNumber);
    }
    if (in.stickOut)
        tool.insert(QStringLiteral("stickOut"), *in.stickOut);
    args.insert(QStringLiteral("tool"), tool);
    QJsonObject cutting;
    cutting.insert(QStringLiteral("rpm"), in.rpm);
    cutting.insert(QStringLiteral("feedCut"), in.feedCut);
    cutting.insert(QStringLiteral("feedPlunge"), in.feedPlunge);
    cutting.insert(QStringLiteral("feedRamp"), in.feedRamp);
    cutting.insert(QStringLiteral("feedLeadIn"), in.feedLeadIn);
    cutting.insert(QStringLiteral("feedLeadOut"), in.feedLeadOut);
    cutting.insert(QStringLiteral("clockwise"), in.clockwise);
    cutting.insert(QStringLiteral("coolant"), in.coolant);
    args.insert(QStringLiteral("cutting"), cutting);
    QJsonObject heights;
    for (const auto& [key, v] : in.heights)
        heights.insert(key, v);
    args.insert(QStringLiteral("heights"), heights);
    args.insert(QStringLiteral("geometry"), in.geometry);
    args.insert(QStringLiteral("params"), in.params);
    if (!in.calculated.isUndefined())
        args.insert(QStringLiteral("calculatedFeeds"), in.calculated);
    QJsonObject library;
    if (!in.libraryTools.isEmpty())
        library.insert(QStringLiteral("tools"), in.libraryTools);
    if (!in.libraryHolders.isEmpty())
        library.insert(QStringLiteral("holders"), in.libraryHolders);
    if (!library.isEmpty())
        args.insert(QStringLiteral("library"), library);
    return args;
}

} // namespace ray::ui::camui
