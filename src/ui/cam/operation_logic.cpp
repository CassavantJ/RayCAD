#include "operation_logic.h"

#include <ray_api/cam_operation.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonDocument>
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
    // One name, or a list of names.
    const QJsonValue kind = o.value(QStringLiteral("kind"));
    QStringList named;
    if (kind.isArray()) {
        for (const QJsonValue& k : kind.toArray())
            named << k.toString();
    } else if (!kind.isUndefined()) {
        named << kind.toString();
    }
    QStringList unknown;
    for (const QString& k : named) {
        if (k == QLatin1String("hole") || k == QLatin1String("face") || k == QLatin1String("edge")) {
            if (!g.kinds.contains(k))
                g.kinds << k;
        } else if (k != QLatin1String("none")) {
            unknown << k;
        }
    }
    if (!unknown.isEmpty())
        g.problem =
            tr("The geometry kind '%1' is not one the panel can pick").arg(unknown.join(QStringLiteral("', '")));
    if (g.kinds.contains(QStringLiteral("hole")) && g.kinds.size() > 1) {
        // A click on a face of a hole would pick the hole or the face: the hole goes, the others stay.
        g.kinds.removeAll(QStringLiteral("hole"));
        g.problem = tr("Holes cannot be picked together with other geometry: the panel picks %1 only")
                        .arg(g.kinds.join(QStringLiteral(" and ")));
    }
    if (!g.kinds.isEmpty())
        g.kind = g.kinds.front();
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
    const QJsonValue v = schema.value(QStringLiteral("x-heightsUsed"));
    if (!v.isObject())
        return usedOf(schema, "x-heightsUsed", heightKeys());
    const QJsonObject o = v.toObject();
    QStringList out;
    for (const QString& k : heightKeys()) {
        if (o.contains(k))
            out << k;
    }
    return out;
}

QString heightFrom(const QJsonObject& schema, const QString& key)
{
    return schema.value(QStringLiteral("x-heightsUsed")).toObject().value(key).toString();
}

QString heightFromText(const QString& from, const QString& kind)
{
    if (from == QLatin1String("stockTop"))
        return tr("from stock top");
    if (from == QLatin1String("partTop"))
        return tr("from part top");
    if (from == QLatin1String("workZero"))
        return tr("from work zero");
    if (from == QLatin1String("eachPick")) {
        if (kind == QLatin1String("hole"))
            return tr("from each hole's top");
        if (kind == QLatin1String("edge"))
            return tr("from the picked edges");
        if (kind == QLatin1String("face"))
            return tr("from the picked faces");
        if (kind == QLatin1String("edge,face") || kind == QLatin1String("face,edge"))
            return tr("from the picked edges and faces");
        return tr("from each pick");
    }
    if (from == QLatin1String("highestPick"))
        return kind == QLatin1String("hole") ? tr("from the highest hole top") : tr("from the highest pick");
    return {};
}

QStringList orderedTypeNames(const QStringList& names)
{
    static const QStringList fixed = {QStringLiteral("drill"),      QStringLiteral("face"),
                                      QStringLiteral("contour2d"),  QStringLiteral("pocket2d"),
                                      QStringLiteral("adaptive2d"), // T-061
                                      QStringLiteral("slot"),       QStringLiteral("chamfer2d"),
                                      QStringLiteral("engrave"),    QStringLiteral("threadmill")};
    QStringList out;
    for (const QString& n : fixed) {
        if (names.contains(n))
            out << n;
    }
    QStringList rest;
    for (const QString& n : names) {
        if (!fixed.contains(n))
            rest << n;
    }
    std::sort(rest.begin(), rest.end());
    return out + rest;
}

QString defaultTypeName(const QStringList& names, const QString& lastUsed)
{
    if (!lastUsed.isEmpty() && names.contains(lastUsed))
        return lastUsed;
    const QStringList ordered = orderedTypeNames(names);
    return ordered.isEmpty() ? QString() : ordered.front();
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

std::optional<std::pair<QString, QString>> heightsOutOfOrder(const QJsonObject& schema,
                                                             const std::map<QString, double>& h)
{
    const auto value = [&h](const char* key) {
        const auto it = h.find(QLatin1String(key));
        return it == h.end() ? 0.0 : it->second;
    };
    cam::Heights z;
    z.clearance = value("clearance");
    z.retract = value("retract");
    z.feedPlane = value("feedPlane");
    z.top = value("top");
    z.bottom = value("bottom");
    const QByteArray text = QJsonDocument(schema).toJson(QJsonDocument::Compact);
    const auto bad = api::cam_operation::heightsOutOfOrder(
        std::string_view(text.constData(), static_cast<std::size_t>(text.size())), z);
    if (!bad)
        return std::nullopt;
    return std::make_pair(QString::fromStdString(bad->higher), QString::fromStdString(bad->lower));
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

QString feedPerRevText(double mm, bool inch)
{
    if (inch)
        return cText(mm / 25.4, 'f', 4) + QStringLiteral(" in/rev");
    return trimmed(mm, 4) + QStringLiteral(" mm/rev");
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

int newToolNumber(const cam::CamState* cam, const ToolId& tool, int toolDefault)
{
    std::map<ToolId, int> numbers; // each tool's number: its first operation in program order
    int highest = 0;               // and every number any operation uses
    bool defaultTaken = false;
    if (cam != nullptr) {
        for (const cam::Setup& s : cam->setups) {
            for (const OperationId& id : s.operations) {
                const auto it = cam->operations.find(id);
                if (it == cam->operations.end())
                    continue;
                const int n = it->second.tool.toolNumber;
                numbers.emplace(it->second.tool.tool, n);
                highest = std::max(highest, n);
                defaultTaken = defaultTaken || n == toolDefault;
            }
        }
    }
    if (const auto it = numbers.find(tool); it != numbers.end())
        return it->second;
    if (toolDefault > 0 && !defaultTaken)
        return toolDefault;
    return highest + 1;
}

} // namespace ray::ui::camui
