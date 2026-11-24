#include "tools_logic.h"

#include <ray_api/cam_tools.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QLocale>
#include <QtCore/QRegularExpression>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ray::ui::toolsui {

namespace {

constexpr double kPi = std::numbers::pi;
constexpr int kArcSteps = 16;           // points per quarter circle
constexpr double kThreadDepth = 0.6134; // depth of an external 60 degree thread, in pitches (ISO 68-1 basic profile)
constexpr int kMaxTeeth = 200;          // thread teeth drawn at most (finer ones are drawn as a plain cylinder)

double positive(double v, double fallback)
{
    return std::isfinite(v) && v > 0 ? v : fallback;
}

// The right half of a tool's side view, from the axis at the tip up to the axis at the top: (x >= 0, y up).
std::vector<Vec2> halfProfile(const lib::Tool& t)
{
    const double r = positive(t.diameter, 1.0) / 2;
    const double rs = positive(t.shankDiameter, 2 * r) / 2;
    const double lf = positive(t.fluteLength, 2 * r);
    const double ls = std::max(lf, positive(t.shoulderLength, lf));
    const double l = std::max({positive(t.overallLength, ls + 2 * rs), ls, lf});
    std::vector<Vec2> p;
    const auto arc = [&p](double cx, double cy, double radius) { // quarter from the bottom (cx, cy - radius) out to +x
        for (int i = 1; i <= kArcSteps; ++i) {
            const double a = (kPi / 2) * i / kArcSteps;
            p.push_back({cx + radius * std::sin(a), cy - radius * std::cos(a)});
        }
    };
    // Teeth of a thread over the flute length, between the major radius and the minor one; deeper for a thread angle
    // under 60 degrees, shallower over it.
    const auto teeth = [&](double pitch, double halfAngle) {
        const double depth = kThreadDepth * pitch * std::tan(kPi / 6) / std::tan(halfAngle);
        const double minor = std::max(r - depth, r * 0.5);
        p.push_back({0, 0});
        p.push_back({minor, 0});
        for (double y = 0; y + pitch <= lf + 1e-9; y += pitch) {
            p.push_back({r, y + pitch / 2});
            p.push_back({minor, y + pitch});
        }
        p.push_back({p.back().x, lf});
    };
    // A cone from the tip flat (radius `tip`) out to the full radius at half included angle `half` (radians).
    const auto cone = [&](double tip, double half) {
        p.push_back({0, 0});
        const double rt = std::clamp(tip, 0.0, r);
        if (rt > 0)
            p.push_back({rt, 0});
        const double h = std::min((r - rt) / std::tan(half), l);
        p.push_back({r, h});
    };
    const double tipAngle = t.tipAngleDeg > 0 && t.tipAngleDeg < 180 ? t.tipAngleDeg : 0;
    switch (t.type) {
    case lib::ToolType::BallEndMill: {
        // Half the diameter by definition; a corner radius the file gives otherwise is drawn as it is.
        const double rc = std::min({t.cornerRadius > 0 ? t.cornerRadius : r, r, lf});
        p.push_back({0, 0});
        if (rc < r)
            p.push_back({r - rc, 0});
        arc(r - rc, rc, rc);
        break;
    }
    case lib::ToolType::BullEndMill:
    case lib::ToolType::FaceMill: {
        const double rc = std::clamp(t.cornerRadius, 0.0, std::min(r, lf));
        p.push_back({0, 0});
        p.push_back({r - rc, 0});
        if (rc > 0)
            arc(r - rc, rc, rc);
        break;
    }
    case lib::ToolType::ChamferMill:
    case lib::ToolType::SpotDrill:
    case lib::ToolType::EngraveCutter:
    case lib::ToolType::Drill:
        if (tipAngle > 0) {
            cone(t.type == lib::ToolType::Drill ? 0.0 : t.tipDiameter / 2, tipAngle * kPi / 360);
        } else {
            p.push_back({0, 0});
            p.push_back({r, 0});
        }
        break;
    case lib::ToolType::Tap:
        if (t.threadPitch > 0 && lf / t.threadPitch <= kMaxTeeth) {
            teeth(t.threadPitch, kPi / 6);
        } else {
            const double c = std::min(r * 0.2, lf / 3);
            p.push_back({0, 0});
            p.push_back({r - c, 0});
            p.push_back({r, c});
        }
        break;
    case lib::ToolType::ThreadMill:
        if (t.threadPitch > 0 && lf / t.threadPitch <= kMaxTeeth) {
            teeth(t.threadPitch, (tipAngle > 0 ? tipAngle : 60.0) * kPi / 360);
        } else {
            // A single-profile head: a V of the tip angle (60 degrees when none is given), its point at mid-height.
            const double half = (tipAngle > 0 ? tipAngle : 60.0) * kPi / 360;
            const double depth = std::min(r * 0.9, (lf / 2) / std::tan(half));
            p.push_back({0, 0});
            p.push_back({r - depth, 0});
            p.push_back({r, lf / 2});
            p.push_back({r - depth, lf});
        }
        break;
    case lib::ToolType::Reamer: {
        const double c = std::min(r * 0.1, lf / 4);
        p.push_back({0, 0});
        p.push_back({r - c, 0});
        p.push_back({r, c});
        break;
    }
    case lib::ToolType::FlatEndMill:
    case lib::ToolType::BoringBar:
        p.push_back({0, 0});
        p.push_back({r, 0});
        break;
    }
    // Up the flutes (a tip that ends above the flute length ends there), the neck to the shoulder, the step to the
    // shank (at 45 degrees), the shank.
    const double top = std::max(p.back().y, lf);
    if (p.back().y < top)
        p.push_back({p.back().x, top});
    const double cutTop = p.back().x;
    double neck = std::min(cutTop, rs);
    double y0 = std::max(ls, top);
    if (t.neckDiameter > 0) {
        // A stated neck (after M3) from the flutes up to its length (else the shoulder), then up to the shoulder the
        // narrower of the cut and the shank (a widening at 45 degrees), then the step to the shank.
        const double rn = t.neckDiameter / 2;
        const double nt = std::max(top, t.neckLength > 0 ? t.neckLength : ls);
        p.push_back({rn, top});
        p.push_back({rn, nt});
        neck = rn;
        y0 = nt;
        if (ls > nt) {
            const double body = std::min(cutTop, rs);
            const double y1 = std::min(ls, nt + std::max(0.0, body - rn));
            if (body > rn)
                p.push_back({body, y1});
            neck = std::max(body, rn);
            p.push_back({neck, ls});
            y0 = ls;
        }
    } else {
        if (std::abs(neck - p.back().x) > 1e-9)
            p.push_back({neck, top});
        if (ls > top)
            p.push_back({neck, ls});
    }
    if (std::abs(rs - neck) > 1e-9) {
        const double y1 = std::min(l, y0 + std::abs(rs - neck));
        p.push_back({rs, y1});
    }
    const double lt = std::max(l, p.back().y);
    p.push_back({rs, lt});
    p.push_back({0, lt});
    return p;
}

// The half profile up to height `y` (cut there), closed along the axis.
std::vector<Vec2> clipBelow(const std::vector<Vec2>& half, double y)
{
    std::vector<Vec2> out;
    for (std::size_t i = 0; i < half.size(); ++i) {
        const Vec2 a = half[i];
        if (a.y <= y + 1e-12) {
            out.push_back(a);
            continue;
        }
        if (i > 0) {
            const Vec2 b = half[i - 1];
            if (b.y < y) {
                const double t = (y - b.y) / (a.y - b.y);
                out.push_back({b.x + (a.x - b.x) * t, y});
            }
        }
        break;
    }
    if (!out.empty() && out.back().x != 0)
        out.push_back({0, out.back().y});
    return out;
}

// A closed outline from a half profile: up the right side, back down the left.
std::vector<Vec2> mirrored(const std::vector<Vec2>& half)
{
    std::vector<Vec2> out;
    out.reserve(half.size() * 2);
    for (const Vec2& v : half)
        out.push_back(v);
    for (auto it = half.rbegin(); it != half.rend(); ++it) {
        if (it->x != 0)
            out.push_back({-it->x, it->y});
    }
    return out;
}

// The right half of a holder from its face (y = `face`) up to the gauge line, segments listed from the gauge line down.
std::vector<Vec2> holderHalf(const lib::Holder& h, double face)
{
    double total = 0;
    for (const lib::HolderSegment& s : h.segments)
        total += std::max(0.0, s.height);
    const double gauge = std::max(positive(h.gaugeLength, total), total);
    double y = face + gauge; // the gauge line
    std::vector<Vec2> top;   // from the gauge line down
    for (const lib::HolderSegment& s : h.segments) {
        const double height = std::max(0.0, s.height);
        top.push_back({std::max(0.0, s.upperDiameter) / 2, y});
        y -= height;
        top.push_back({std::max(0.0, s.lowerDiameter) / 2, y});
    }
    std::vector<Vec2> half;
    if (top.empty())
        return half;
    half.push_back({0, top.back().y});
    for (auto it = top.rbegin(); it != top.rend(); ++it)
        half.push_back(*it);
    half.push_back({0, face + gauge});
    return half;
}

void bounds(Profile& p)
{
    for (const auto* poly : {&p.outline, &p.cutter, &p.holder}) {
        for (const Vec2& v : *poly) {
            p.halfWidth = std::max(p.halfWidth, std::abs(v.x));
            p.height = std::max(p.height, v.y);
        }
    }
}

QString tr(const char* text)
{
    return QCoreApplication::translate(kTrContext, text);
}

QString num(double v, int digits = 10)
{
    return QLocale::c().toString(v, 'g', digits);
}

// ---------- fields ----------
#define RAY_F(key, label, kind)                                                                                        \
    Field                                                                                                              \
    {                                                                                                                  \
        key, QT_TRANSLATE_NOOP("ray::ui::ToolLibraryModel", label), kind                                               \
    }

const std::vector<Field>& allToolFields()
{
    static const std::vector<Field> fields = {
        RAY_F("name", "Name", "text"),
        RAY_F("type", "Type", "choice"),
        RAY_F("vendor", "Maker", "text"),
        RAY_F("productId", "Order number", "text"),
        RAY_F("bodyMaterial", "Body material", "text"),
        RAY_F("flutes", "Flutes", "int"),
        RAY_F("diameter", "Diameter", "length"),
        RAY_F("cornerRadius", "Corner radius", "length"),
        RAY_F("tipAngle", "Tip angle (°)", "angle"),
        RAY_F("tipDiameter", "Tip diameter", "length"),
        RAY_F("threadPitch", "Thread pitch", "length"),
        RAY_F("threadSpec", "Thread", "text"),
        RAY_F("shankDiameter", "Shank diameter", "length"),
        RAY_F("fluteLength", "Flute length", "length"),
        RAY_F("shoulderLength", "Shoulder length", "length"),
        RAY_F("neckDiameter", "Neck diameter", "length"),
        RAY_F("neckLength", "Neck length", "length"),
        RAY_F("overallLength", "Overall length", "length"),
        RAY_F("centreCutting", "Centre cutting", "choice"),
        RAY_F("defaultToolNumber", "Tool number", "int"),
        RAY_F("comment", "Comment", "multiline"),
    };
    return fields;
}

const Field* findField(const std::vector<Field>& fields, const QString& key)
{
    for (const Field& f : fields) {
        if (key == QLatin1String(f.key))
            return &f;
    }
    return nullptr;
}

bool numeric(const char* kind)
{
    const QLatin1String k(kind);
    return k == QLatin1String("int") || k == QLatin1String("length") || k == QLatin1String("angle") ||
           k == QLatin1String("fraction") || k == QLatin1String("speed");
}

// The number the schema's message ends with gets the file format's unit (the schema speaks in mm and m/min).
QString withUnit(QString message, const char* kind)
{
    if (message.isEmpty() || !message.back().isDigit())
        return message;
    const QLatin1String k(kind ? kind : "");
    if (k == QLatin1String("length"))
        return message + QStringLiteral(" mm");
    if (k == QLatin1String("speed"))
        return message + QStringLiteral(" m/min");
    if (k == QLatin1String("angle"))
        return message + QStringLiteral("°");
    return message;
}

QString text(const QJsonObject& o, const char* key)
{
    return o.value(QLatin1String(key)).toString();
}

QString formatJson(const char* kind, const QJsonValue& v, const Units& units)
{
    if (numeric(kind))
        return formatValue(kind, v.toDouble(), units);
    if (v.isBool()) // centreCutting: "true" / "false"; absent: "" (the type's default)
        return v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    return v.toString();
}

QJsonObject parseObject(const std::string& json)
{
    return QJsonDocument::fromJson(QByteArray::fromStdString(json)).object();
}

// Applies the fields of `values` (with their table) to `json`; problems of text that does not read go to `problems`
// under `prefix + key`.
void apply(const std::map<QString, QString>& fields, const std::map<QString, QString>& shown,
           const std::vector<Field>& table, QJsonObject& json, const Units& units, const QString& prefix,
           std::map<QString, QString>& problems)
{
    for (const auto& [key, value] : fields) {
        const Field* f = findField(table, key);
        if (f == nullptr)
            continue;
        const auto was = shown.find(key);
        if (was != shown.end() && was->second == value)
            continue; // as shown: the stored value (or its absence) stays exactly
        if (key == QLatin1String(kCentreCutting)) {
            // A yes/no the file may leave out: "" is the type's default, so the key goes.
            if (value.isEmpty())
                json.remove(key);
            else
                json[key] = value == QLatin1String("true");
            continue;
        }
        if (!numeric(f->kind)) {
            json[key] = value;
            continue;
        }
        QString error;
        const auto v = parseValue(f->kind, value, units, &error);
        if (!v) {
            problems[prefix + key] = error;
            continue;
        }
        if (QLatin1String(f->kind) == QLatin1String("int"))
            json[key] = static_cast<qint64>(std::llround(*v));
        else
            json[key] = *v;
    }
}

// The value at a schema pointer's field key ("diameter", "presets.0.chipLoad") in `json`.
QJsonValue valueAt(const QJsonObject& json, const QStringList& path)
{
    QJsonValue v = json;
    for (const QString& part : path) {
        if (v.isObject()) {
            v = v.toObject().value(part);
        } else if (v.isArray()) {
            bool ok = false;
            const int i = part.toInt(&ok);
            v = ok ? v.toArray().at(i) : QJsonValue(QJsonValue::Undefined);
        } else {
            return QJsonValue(QJsonValue::Undefined);
        }
    }
    return v;
}

// Sets (or, for an undefined value, removes) the value at `path`; false if the path does not lead there.
bool setAt(QJsonValue& node, const QStringList& path, int depth, const QJsonValue& value)
{
    if (depth == path.size())
        return false;
    const QString& part = path[depth];
    const bool last = depth + 1 == path.size();
    if (node.isObject()) {
        QJsonObject o = node.toObject();
        if (last) {
            if (value.isUndefined())
                o.remove(part);
            else
                o[part] = value;
        } else {
            QJsonValue child = o.value(part);
            if (!setAt(child, path, depth + 1, value))
                return false;
            o[part] = child;
        }
        node = o;
        return true;
    }
    if (node.isArray()) {
        bool ok = false;
        const int i = part.toInt(&ok);
        QJsonArray a = node.toArray();
        if (!ok || i < 0 || i >= a.size() || last)
            return false;
        QJsonValue child = a.at(i);
        if (!setAt(child, path, depth + 1, value))
            return false;
        a[i] = child;
        node = a;
        return true;
    }
    return false;
}

// Runs the schema check until it passes, naming every problem: each one found is set back to `fallback`'s value.
// Returns the JSON that passed (empty when none did).
template <class FromJson>
QJsonObject schemaProblems(QJsonObject json, const QJsonObject& fallback, FromJson fromJson,
                           const std::function<const char*(const QString& key)>& kindOf,
                           std::map<QString, QString>& problems)
{
    for (int round = 0; round < 64; ++round) {
        const auto r = fromJson(QJsonDocument(json).toJson(QJsonDocument::Compact).toStdString());
        if (r)
            return json;
        auto [key, message] = splitProblem(QString::fromStdString(r.error().message));
        if (problems.count(key) != 0)
            return {}; // no progress
        problems[key] = withUnit(message, kindOf(key));
        if (key.isEmpty())
            return {};
        const QStringList path = key.split(QLatin1Char('.'));
        const QJsonValue back = valueAt(fallback, path);
        if (back == valueAt(json, path))
            return {}; // the fallback is the same value: it would fail again
        QJsonValue root = json;
        if (!setAt(root, path, 0, back))
            return {};
        json = root.toObject();
    }
    return {};
}

} // namespace

// ---------- profile ----------
Profile toolProfile(const lib::Tool& tool, const lib::Holder* holder, double stickOut)
{
    Profile out;
    const std::vector<Vec2> half = halfProfile(tool);
    out.outline = mirrored(half);
    out.cutter = mirrored(clipBelow(half, positive(tool.fluteLength, positive(tool.diameter, 1.0))));
    if (holder != nullptr) {
        const double length = half.back().y;
        double s = stickOut > 0 ? stickOut : positive(tool.shoulderLength, positive(tool.fluteLength, length));
        s = std::min(s, length);
        out.stickOut = s;
        out.holder = mirrored(holderHalf(*holder, s));
    }
    bounds(out);
    return out;
}

Profile holderProfile(const lib::Holder& holder)
{
    Profile out;
    out.holder = mirrored(holderHalf(holder, 0));
    bounds(out);
    return out;
}

// ---------- catalog ----------
QString searchText(const lib::Tool& t, const QString& typeTitle)
{
    const QStringList parts = {QString::fromStdString(t.name),
                               QString::fromStdString(t.vendor),
                               QString::fromStdString(t.productId),
                               QString::fromStdString(std::string(api::cam_tools::toolTypeName(t.type))),
                               typeTitle,
                               QString::fromStdString(t.threadSpec),
                               QString::fromStdString(t.bodyMaterial),
                               QString::fromStdString(t.comment)};
    return parts.join(QLatin1Char('\n')).toLower();
}

QString searchText(const lib::Holder& h)
{
    return QStringList{QString::fromStdString(h.name), QString::fromStdString(h.vendor),
                       QString::fromStdString(h.comment)}
        .join(QLatin1Char('\n'))
        .toLower();
}

std::vector<int> filterEntries(const std::vector<Entry>& entries, const Filter& filter, const std::vector<int>* order)
{
    static const QRegularExpression space(QStringLiteral("\\s+"));
    const QStringList words = filter.text.toLower().split(space, Qt::SkipEmptyParts);
    // The order is total (ties end at the id), so filtering the sorted list is sorting the filtered one.
    std::vector<int> sorted;
    if (order == nullptr || order->size() != entries.size()) {
        sorted = sortEntries(entries, filter.sort);
        order = &sorted;
    }
    std::vector<int> out;
    out.reserve(entries.size());
    for (const int i : *order) {
        const Entry& e = entries[static_cast<std::size_t>(i)];
        if (!filter.type.isEmpty() && !e.holder && e.type != filter.type)
            continue;
        if (!filter.types.isEmpty() && !e.holder && !filter.types.contains(e.type))
            continue;
        if (filter.minDiameter >= 0 && e.diameter < filter.minDiameter - 1e-9)
            continue;
        if (filter.maxDiameter >= 0 && e.diameter > filter.maxDiameter + 1e-9)
            continue;
        if (!filter.material.isEmpty() && !std::binary_search(e.materials.begin(), e.materials.end(), filter.material))
            continue;
        if (!std::all_of(words.begin(), words.end(), [&](const QString& w) { return e.search.contains(w); }))
            continue;
        out.push_back(i);
    }
    return out;
}

std::vector<int> sortEntries(const std::vector<Entry>& entries, SortKey sort)
{
    std::vector<int> out(entries.size());
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<int>(i);
    std::vector<int> ranks(entries.size(), 99);
    if (sort == SortKey::Type) {
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const auto t = api::cam_tools::toolTypeFromName(entries[i].type.toStdString());
            ranks[i] = t ? static_cast<int>(*t) : 99;
        }
    }
    const auto typeRank = [&](const Entry& e) {
        return ranks[static_cast<std::size_t>(&e - entries.data())];
    };
    const auto byName = [](const Entry& a, const Entry& b) {
        const int c = a.name.compare(b.name, Qt::CaseInsensitive);
        if (c != 0)
            return c < 0;
        const int d = a.name.compare(b.name, Qt::CaseSensitive);
        if (d != 0)
            return d < 0;
        return a.id < b.id;
    };
    std::stable_sort(out.begin(), out.end(), [&](int ia, int ib) {
        const Entry& a = entries[static_cast<std::size_t>(ia)];
        const Entry& b = entries[static_cast<std::size_t>(ib)];
        if (a.document != b.document)
            return a.document;
        switch (sort) {
        case SortKey::Name:
            break;
        case SortKey::Type:
            if (typeRank(a) != typeRank(b))
                return typeRank(a) < typeRank(b);
            [[fallthrough]];
        case SortKey::Diameter:
            if (a.diameter != b.diameter)
                return a.diameter < b.diameter;
            break;
        }
        return byName(a, b);
    });
    return out;
}

// ---------- fields ----------
std::vector<Field> toolFields(lib::ToolType type)
{
    std::vector<QString> extra;
    // A neck (after M3): the milling cutters that are made with one (thread mills, long-reach end mills, ...).
    const QString neckDiameter = QStringLiteral("neckDiameter"), neckLength = QStringLiteral("neckLength");
    switch (type) {
    case lib::ToolType::BallEndMill:
    case lib::ToolType::BullEndMill:
        extra = {QStringLiteral("cornerRadius"), neckDiameter, neckLength};
        break;
    case lib::ToolType::FaceMill:
        extra = {QStringLiteral("cornerRadius")};
        break;
    case lib::ToolType::ChamferMill:
    case lib::ToolType::EngraveCutter:
        extra = {QStringLiteral("tipAngle"), QStringLiteral("tipDiameter"), neckDiameter, neckLength};
        break;
    case lib::ToolType::SpotDrill:
        extra = {QStringLiteral("tipAngle"), QStringLiteral("tipDiameter")};
        break;
    case lib::ToolType::Drill:
        extra = {QStringLiteral("tipAngle")};
        break;
    case lib::ToolType::Tap:
        extra = {QStringLiteral("threadPitch"), QStringLiteral("threadSpec")};
        break;
    case lib::ToolType::ThreadMill:
        extra = {QStringLiteral("threadPitch"), QStringLiteral("tipAngle"), neckDiameter, neckLength};
        break;
    case lib::ToolType::FlatEndMill:
        extra = {neckDiameter, neckLength};
        break;
    case lib::ToolType::Reamer:
    case lib::ToolType::BoringBar:
        break;
    }
    // Centre cutting (after M3): the milling cutters, where it varies from tool to tool; a drill always cuts at its
    // centre and a tap, reamer or boring bar never does.
    if (lib::centreCuttingSettable(type))
        extra.push_back(QLatin1String(kCentreCutting));
    static const QStringList optional = {QStringLiteral("cornerRadius"), QStringLiteral("tipAngle"),
                                         QStringLiteral("tipDiameter"),  QStringLiteral("threadPitch"),
                                         QStringLiteral("threadSpec"),   QStringLiteral("neckDiameter"),
                                         QStringLiteral("neckLength"),   QLatin1String(kCentreCutting)};
    std::vector<Field> out;
    for (const Field& f : allToolFields()) {
        const QString key = QLatin1String(f.key);
        if (optional.contains(key) && std::find(extra.begin(), extra.end(), key) == extra.end())
            continue;
        out.push_back(f);
    }
    return out;
}

std::vector<Choice> centreCuttingChoices(lib::ToolType type, double tipDiameter)
{
    lib::Tool t;
    t.type = type;
    t.tipDiameter = tipDiameter;
    return {{QString(), lib::centreCuttingByDefault(t)
                            ? tr(QT_TRANSLATE_NOOP("ray::ui::ToolLibraryModel", "Type default (yes)"))
                            : tr(QT_TRANSLATE_NOOP("ray::ui::ToolLibraryModel", "Type default (no)"))},
            {QStringLiteral("true"), tr(QT_TRANSLATE_NOOP("ray::ui::ToolLibraryModel", "Yes"))},
            {QStringLiteral("false"), tr(QT_TRANSLATE_NOOP("ray::ui::ToolLibraryModel", "No"))}};
}

const std::vector<Field>& presetFields()
{
    static const std::vector<Field> fields = {
        RAY_F("material", "Material", "choice"),
        RAY_F("opClass", "Cut", "choice"),
        RAY_F("surfaceSpeed", "Surface speed", "speed"),
        RAY_F("chipLoad", "Chip load", "length"),
        RAY_F("maxRadialFraction", "Radial (× D)", "fraction"),
        RAY_F("maxAxialFraction", "Axial (× D)", "fraction"),
        RAY_F("rampAngle", "Ramp (°)", "angle"),
        RAY_F("comment", "Note", "text"),
    };
    return fields;
}

const std::vector<Field>& holderFields()
{
    static const std::vector<Field> fields = {
        RAY_F("name", "Name", "text"),
        RAY_F("vendor", "Maker", "text"),
        RAY_F("gaugeLength", "Gauge length", "length"),
        RAY_F("comment", "Comment", "multiline"),
    };
    return fields;
}

const std::vector<Field>& segmentFields()
{
    static const std::vector<Field> fields = {
        RAY_F("height", "Height", "length"),
        RAY_F("lowerDiameter", "Lower Ø", "length"),
        RAY_F("upperDiameter", "Upper Ø", "length"),
    };
    return fields;
}

#undef RAY_F

QString formatValue(const char* kind, double value, const Units& units)
{
    const QLatin1String k(kind);
    if (k == QLatin1String("length")) {
        if (units.inch)
            return num(value / 25.4) + QStringLiteral(" in");
        return num(value) + QStringLiteral(" mm");
    }
    if (k == QLatin1String("speed")) {
        if (units.inch)
            return num(value / 0.3048, 6) + QStringLiteral(" ft/min");
        return num(value) + QStringLiteral(" m/min");
    }
    if (k == QLatin1String("int"))
        return QString::number(std::llround(value));
    return num(value);
}

std::optional<double> parseValue(const char* kind, const QString& raw, const Units& units, QString* error)
{
    const QString text = raw.trimmed();
    const QLatin1String k(kind);
    const auto fail = [&](const char* why) -> std::optional<double> {
        if (error != nullptr)
            *error = tr(why);
        return std::nullopt;
    };
    if (text.isEmpty())
        return fail(QT_TRANSLATE_NOOP("ray::ui::ToolLibraryModel", "Enter a value"));
    if (k == QLatin1String("length")) {
        const double mm = units.parseLength ? units.parseLength(text) : std::nan("");
        if (!std::isfinite(mm))
            return fail(
                QT_TRANSLATE_NOOP("ray::ui::ToolLibraryModel", "Not a length: give a number, e.g. 6 mm or 0.25 in"));
        return mm;
    }
    if (k == QLatin1String("speed")) {
        static const QRegularExpression re(
            QStringLiteral("^([-+]?[0-9]*\\.?[0-9]+(?:[eE][-+]?[0-9]+)?)\\s*(m/min|sfm|ft/min)?$"),
            QRegularExpression::CaseInsensitiveOption);
        const auto m = re.match(text);
        if (!m.hasMatch())
            return fail(QT_TRANSLATE_NOOP("ray::ui::ToolLibraryModel",
                                          "Not a speed: give a number, e.g. 120 m/min or 400 ft/min"));
        const double v = QLocale::c().toDouble(m.captured(1));
        const QString unit = m.captured(2).toLower();
        const bool feet = unit.isEmpty() ? units.inch : unit != QLatin1String("m/min");
        return feet ? v * 0.3048 : v;
    }
    bool ok = false;
    QString number = text;
    if (k == QLatin1String("angle") && number.endsWith(QChar(0x00B0)))
        number.chop(1);
    const double v = QLocale::c().toDouble(number.trimmed(), &ok);
    if (!ok || !std::isfinite(v))
        return fail(QT_TRANSLATE_NOOP("ray::ui::ToolLibraryModel", "Not a number"));
    if (k == QLatin1String("int") && std::abs(v - std::round(v)) > 1e-9)
        return fail(QT_TRANSLATE_NOOP("ray::ui::ToolLibraryModel", "Give a whole number"));
    return v;
}

namespace {

Row rowOf(const QJsonObject& json, const std::vector<Field>& table, const Units& units)
{
    Row row;
    row.base = json;
    for (const Field& f : table) {
        const QString v = formatJson(f.kind, json.value(QLatin1String(f.key)), units);
        row.fields[QLatin1String(f.key)] = v;
        row.shown[QLatin1String(f.key)] = v;
    }
    return row;
}

QString defaultOpClass(lib::ToolType type)
{
    switch (type) {
    case lib::ToolType::ChamferMill:
        return QStringLiteral("chamfer");
    case lib::ToolType::FaceMill:
        return QStringLiteral("face");
    case lib::ToolType::SpotDrill:
        return QStringLiteral("spot");
    case lib::ToolType::Drill:
        return QStringLiteral("drill");
    case lib::ToolType::Tap:
        return QStringLiteral("tap");
    case lib::ToolType::Reamer:
        return QStringLiteral("ream");
    case lib::ToolType::BoringBar:
        return QStringLiteral("bore");
    case lib::ToolType::ThreadMill:
        return QStringLiteral("thread");
    case lib::ToolType::EngraveCutter:
        return QStringLiteral("engrave");
    case lib::ToolType::FlatEndMill:
    case lib::ToolType::BallEndMill:
    case lib::ToolType::BullEndMill:
        break;
    }
    return QStringLiteral("rough");
}

} // namespace

Values toolValues(const lib::Tool& tool, const Units& units)
{
    const QJsonObject json = parseObject(api::cam_tools::toolJson(tool));
    Values v;
    for (const Field& f : allToolFields()) {
        const QString text = formatJson(f.kind, json.value(QLatin1String(f.key)), units);
        v.fields[QLatin1String(f.key)] = text;
        v.shown[QLatin1String(f.key)] = text;
    }
    for (const QJsonValue& p : json.value(QStringLiteral("presets")).toArray())
        v.rows.push_back(rowOf(p.toObject(), presetFields(), units));
    return v;
}

Values holderValues(const lib::Holder& holder, const Units& units)
{
    const QJsonObject json = parseObject(api::cam_tools::holderJson(holder));
    Values v;
    for (const Field& f : holderFields()) {
        const QString text = formatJson(f.kind, json.value(QLatin1String(f.key)), units);
        v.fields[QLatin1String(f.key)] = text;
        v.shown[QLatin1String(f.key)] = text;
    }
    for (const QJsonValue& s : json.value(QStringLiteral("segments")).toArray())
        v.rows.push_back(rowOf(s.toObject(), segmentFields(), units));
    return v;
}

Row newPresetRow(lib::ToolType type, const QString& material, const Units& units)
{
    const bool tap = type == lib::ToolType::Tap;
    const bool hole = tap || type == lib::ToolType::Drill || type == lib::ToolType::SpotDrill ||
                      type == lib::ToolType::Reamer || type == lib::ToolType::BoringBar;
    const QJsonObject json{{QStringLiteral("material"), material},
                           {QStringLiteral("opClass"), defaultOpClass(type)},
                           {QStringLiteral("surfaceSpeed"), tap ? 10.0 : 100.0},
                           {QStringLiteral("chipLoad"), tap ? 0.0 : 0.05},
                           {QStringLiteral("maxRadialFraction"), hole ? 1.0 : 0.1},
                           {QStringLiteral("maxAxialFraction"), 1.0},
                           {QStringLiteral("rampAngle"), 0.0}};
    Row row = rowOf(json, presetFields(), units);
    row.shown.clear(); // new: nothing was shown, so every field is the editor's
    return row;
}

Row newSegmentRow(const Units& units)
{
    const QJsonObject json{{QStringLiteral("height"), 20.0},
                           {QStringLiteral("lowerDiameter"), 30.0},
                           {QStringLiteral("upperDiameter"), 30.0}};
    Row row = rowOf(json, segmentFields(), units);
    row.shown.clear();
    return row;
}

bool edited(const Values& v)
{
    if (v.rowsEdited || v.fields != v.shown)
        return true;
    for (const Row& r : v.rows) {
        if (r.fields != r.shown)
            return true;
    }
    return false;
}

namespace {

template <class FromJson>
Checked check(const Values& values, const QJsonObject& base, const Units& units, const std::vector<Field>& table,
              const std::vector<Field>& rowTable, const char* rowsKey, FromJson fromJson)
{
    Checked out;
    out.json = base;
    apply(values.fields, values.shown, table, out.json, units, QString(), out.problems);
    QJsonArray rows;
    QJsonArray fallbackRows;
    for (std::size_t i = 0; i < values.rows.size(); ++i) {
        const Row& row = values.rows[i];
        QJsonObject o = row.base;
        apply(row.fields, row.shown, rowTable, o, units, QStringLiteral("%1.%2.").arg(QLatin1String(rowsKey)).arg(i),
              out.problems);
        rows.append(o);
        fallbackRows.append(row.base);
    }
    out.json[QLatin1String(rowsKey)] = rows;
    QJsonObject fallback = base;
    fallback[QLatin1String(rowsKey)] = fallbackRows;
    // The schema's problems of the fields that read; one that did not read is already named.
    std::map<QString, QString> schema;
    const auto kindOf = [&](const QString& key) -> const char* {
        const QStringList parts = key.split(QLatin1Char('.'));
        if (parts.size() == 3) {
            const Field* f = findField(rowTable, parts[2]);
            return f ? f->kind : nullptr;
        }
        const Field* f = findField(table, key);
        return f ? f->kind : nullptr;
    };
    out.valid = schemaProblems(out.json, fallback, fromJson, kindOf, schema);
    for (auto& [key, message] : schema) {
        if (out.problems.count(key) == 0)
            out.problems[key] = message;
    }
    return out;
}

} // namespace

Checked checkTool(const Values& values, const QJsonObject& base, const Units& units)
{
    // Every tool field (not only the type's): a field hidden for this type keeps its value.
    return check(values, base, units, allToolFields(), presetFields(), "presets",
                 [](const std::string& json) { return api::cam_tools::toolFromJson(json); });
}

Checked checkHolder(const Values& values, const QJsonObject& base, const Units& units)
{
    return check(values, base, units, holderFields(), segmentFields(), "segments",
                 [](const std::string& json) { return api::cam_tools::holderFromJson(json); });
}

std::pair<QString, QString> splitProblem(const QString& message)
{
    if (!message.startsWith(QLatin1Char('/')))
        return {QString(), message};
    const qsizetype colon = message.indexOf(QStringLiteral(": "));
    if (colon < 0)
        return {QString(), message};
    QString pointer = message.left(colon).mid(1);
    pointer.replace(QLatin1Char('/'), QLatin1Char('.'));
    return {pointer, message.mid(colon + 2)};
}

} // namespace ray::ui::toolsui
