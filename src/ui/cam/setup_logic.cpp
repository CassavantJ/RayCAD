#include "setup_logic.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonArray>
#include <QtCore/QLocale>

#include <algorithm>
#include <cmath>

namespace ray::ui::camui {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("ray::ui::camui", text);
}

Vec3 add(Vec3 a, Vec3 b)
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
Vec3 mul(Vec3 a, double s)
{
    return {a.x * s, a.y * s, a.z * s};
}

// The worse of two statuses for a setup row: broken > failed > generating > stale > up to date.
int severity(cam::OpStatus s)
{
    switch (s) {
    case cam::OpStatus::ReferenceBroken:
        return 5;
    case cam::OpStatus::Failed:
        return 4;
    case cam::OpStatus::Generating:
        return 3;
    case cam::OpStatus::Stale:
        return 2;
    case cam::OpStatus::UpToDate:
        return 1;
    case cam::OpStatus::Suppressed:
        return 0;
    }
    return 0;
}

constexpr const char* kLengthKeys[] = {"xNeg", "xPos", "yNeg", "yPos", "zNeg", "zPos"};

} // namespace

// ---------- Badges ----------
QString statusKey(cam::OpStatus status)
{
    switch (status) {
    case cam::OpStatus::UpToDate:
        return QStringLiteral("upToDate");
    case cam::OpStatus::Stale:
        return QStringLiteral("stale");
    case cam::OpStatus::Generating:
        return QStringLiteral("generating");
    case cam::OpStatus::Failed:
        return QStringLiteral("failed");
    case cam::OpStatus::ReferenceBroken:
        return QStringLiteral("broken");
    case cam::OpStatus::Suppressed:
        return QStringLiteral("suppressed");
    }
    return QStringLiteral("stale");
}

QString statusText(cam::OpStatus status, double progress)
{
    switch (status) {
    case cam::OpStatus::UpToDate:
        return tr("Up to date");
    case cam::OpStatus::Stale:
        return tr("Stale");
    case cam::OpStatus::Generating:
        if (progress >= 0 && progress <= 1)
            return tr("Generating %1%").arg(static_cast<int>(std::floor(progress * 100.0)));
        return tr("Generating");
    case cam::OpStatus::Failed:
        return tr("Failed");
    case cam::OpStatus::ReferenceBroken:
        return tr("Broken reference");
    case cam::OpStatus::Suppressed:
        return tr("Suppressed");
    }
    return {};
}

std::optional<Badge> setupBadge(const std::optional<Status>& own, const std::vector<const cam::OpState*>& operations)
{
    if (own && !*own) {
        const Error& e = own->error();
        return Badge{e.code == ErrorCode::ReferenceBroken ? cam::OpStatus::ReferenceBroken : cam::OpStatus::Failed,
                     QString::fromStdString(e.message)};
    }
    std::optional<Badge> worst;
    bool allSuppressed = !operations.empty();
    for (const cam::OpState* op : operations) {
        if (op == nullptr)
            continue;
        if (op->status != cam::OpStatus::Suppressed)
            allSuppressed = false;
        if (!worst || severity(op->status) > severity(worst->status))
            worst = Badge{op->status, QString::fromStdString(op->message)};
    }
    if (allSuppressed)
        return Badge{cam::OpStatus::Suppressed, {}};
    return worst;
}

// ---------- Origin ----------
QString originKey(const cam::Wcs& wcs)
{
    switch (wcs.originMode) {
    case cam::Wcs::OriginMode::StockCorner:
        return QStringLiteral("corner%1").arg(std::clamp(wcs.stockCornerIndex, 0, 7));
    case cam::Wcs::OriginMode::StockTopCenter:
        return QStringLiteral("topCenter");
    case cam::Wcs::OriginMode::ModelPoint:
        return QStringLiteral("modelPoint");
    }
    return QStringLiteral("corner0");
}

bool originFromKey(const QString& key, cam::Wcs::OriginMode* mode, int* corner)
{
    if (key == QLatin1String("topCenter")) {
        *mode = cam::Wcs::OriginMode::StockTopCenter;
        *corner = 0;
        return true;
    }
    if (key == QLatin1String("modelPoint")) {
        *mode = cam::Wcs::OriginMode::ModelPoint;
        *corner = 0;
        return true;
    }
    if (key.size() == 7 && key.startsWith(QLatin1String("corner"))) {
        const int k = key.at(6).digitValue();
        if (k >= 0 && k <= 7) {
            *mode = cam::Wcs::OriginMode::StockCorner;
            *corner = k;
            return true;
        }
    }
    return false;
}

QString markerKey(int index)
{
    return index >= 0 && index < 8 ? QStringLiteral("corner%1").arg(index) : QStringLiteral("topCenter");
}

// ---------- Triad ----------
Triad triadGeometry(Vec3 origin, Vec3 x, Vec3 y, Vec3 z, double length)
{
    Triad out;
    const std::array<Vec3, 3> axes{x, y, z};
    for (std::size_t i = 0; i < 3; ++i) {
        const Vec3 dir = axes[i];
        const Vec3 side = axes[(i + 1) % 3]; // the head lies in the plane of this axis and the next
        const Vec3 tip = add(origin, mul(dir, length));
        const Vec3 back = add(tip, mul(dir, -length / 5.0));
        std::vector<Vec3>& s = out.segments[i];
        s = {origin, tip, tip, add(back, mul(side, length / 12.0)), tip, add(back, mul(side, -length / 12.0))};
        out.tips[i] = tip;
    }
    return out;
}

double triadLength(const Box3& stock)
{
    const double extent =
        std::max({stock.max.x - stock.min.x, stock.max.y - stock.min.y, stock.max.z - stock.min.z, 0.0});
    return std::clamp(extent / 3.0, 5.0, 200.0);
}

// ---------- Setup arguments ----------
ArgsResult setupArgs(const SetupInput& in, const LengthFn& length, bool adding)
{
    ArgsResult out;
    const auto fail = [&](const QString& message, const QString& field = QString()) {
        out.args = QJsonObject();
        out.error = message;
        out.field = field;
        return out;
    };
    const auto lengthOf = [&](const char* key, double* mm) -> bool {
        const auto it = in.lengths.find(QLatin1String(key));
        const QString text = it == in.lengths.end() ? QString() : it->second.trimmed();
        if (text.isEmpty()) {
            *mm = 0;
            return true;
        }
        QString error;
        const auto v = length(text, &error);
        if (!v) {
            out.error = error.isEmpty() ? tr("Give a length") : error;
            out.field = QLatin1String(key);
            return false;
        }
        *mm = *v;
        return true;
    };
    if (in.name.trimmed().isEmpty())
        return fail(tr("Give the setup a name"), QStringLiteral("name"));
    if (in.partBodies.isEmpty())
        return fail(tr("Choose at least one part body"), QStringLiteral("bodies"));
    QJsonObject a;
    a.insert(QStringLiteral("id"), in.id);
    a.insert(QStringLiteral("name"), in.name.trimmed());
    a.insert(QStringLiteral("partBodies"), QJsonArray::fromStringList(in.partBodies));
    a.insert(QStringLiteral("fixtureBodies"), QJsonArray::fromStringList(in.fixtureBodies));

    QJsonObject stock;
    if (in.stock == QLatin1String("cylinder")) {
        double diameter = 0, len = 0, endOffset = 0;
        if (!lengthOf("diameter", &diameter) || !lengthOf("length", &len) || !lengthOf("endOffset", &endOffset)) {
            const ArgsResult keep = out;
            return fail(keep.error, keep.field);
        }
        if (!(diameter > 0))
            return fail(tr("The diameter must be more than 0"), QStringLiteral("diameter"));
        if (!(len > 0))
            return fail(tr("The length must be more than 0"), QStringLiteral("length"));
        if (endOffset < 0)
            return fail(tr("The end offset cannot be negative"), QStringLiteral("endOffset"));
        const QString axis = in.axis.isEmpty() ? QStringLiteral("z") : in.axis;
        stock = QJsonObject{{QStringLiteral("type"), QStringLiteral("cylinder")},
                            {QStringLiteral("axis"), axis},
                            {QStringLiteral("diameter"), diameter},
                            {QStringLiteral("length"), len},
                            {QStringLiteral("endOffset"), endOffset}};
    } else if (in.stock == QLatin1String("solid")) {
        if (in.solidBody.isEmpty())
            return fail(tr("Choose the stock body"), QStringLiteral("solidBody"));
        if (in.partBodies.contains(in.solidBody))
            return fail(tr("The stock body cannot also be a part body"), QStringLiteral("solidBody"));
        stock = QJsonObject{{QStringLiteral("type"), QStringLiteral("solid")}, {QStringLiteral("body"), in.solidBody}};
    } else {
        stock.insert(QStringLiteral("type"), QStringLiteral("box"));
        for (const char* key : kLengthKeys) {
            double mm = 0;
            if (!lengthOf(key, &mm)) {
                const ArgsResult keep = out;
                return fail(keep.error, keep.field);
            }
            if (mm < 0)
                return fail(tr("An offset cannot be negative"), QLatin1String(key));
            stock.insert(QLatin1String(key), mm);
        }
    }
    a.insert(QStringLiteral("stock"), stock);

    QJsonObject wcs;
    cam::Wcs::OriginMode mode = cam::Wcs::OriginMode::StockCorner;
    int corner = 0;
    if (!originFromKey(in.origin, &mode, &corner))
        return fail(tr("Choose the origin"), QStringLiteral("origin"));
    if (mode == cam::Wcs::OriginMode::StockCorner)
        wcs.insert(QStringLiteral("origin"), QJsonObject{{QStringLiteral("mode"), QStringLiteral("stockCorner")},
                                                         {QStringLiteral("corner"), corner}});
    else if (mode == cam::Wcs::OriginMode::StockTopCenter)
        wcs.insert(QStringLiteral("origin"), QJsonObject{{QStringLiteral("mode"), QStringLiteral("stockTopCenter")}});
    else {
        if (in.originVertex.isEmpty())
            return fail(tr("Pick the origin vertex in the view"), QStringLiteral("origin"));
        wcs.insert(QStringLiteral("origin"), QJsonObject{{QStringLiteral("mode"), QStringLiteral("modelPoint")}});
        wcs.insert(QStringLiteral("originVertex"), in.originVertex);
    }
    if (in.workOffset < 1 || in.workOffset > 99)
        return fail(tr("The work offset must be 1 (G54) to 99"), QStringLiteral("workOffset"));
    wcs.insert(QStringLiteral("workOffset"), in.workOffset);
    if (!in.zFace.isEmpty())
        wcs.insert(QStringLiteral("zFace"), in.zFace);
    if (!in.xEdge.isEmpty())
        wcs.insert(QStringLiteral("xEdge"), in.xEdge);
    if (in.flipX)
        wcs.insert(QStringLiteral("flipX"), true);
    if (in.flipZ)
        wcs.insert(QStringLiteral("flipZ"), true);
    a.insert(QStringLiteral("wcs"), wcs);

    bool ok = false;
    const QString number = in.programNumber.trimmed();
    const qlonglong program = QLocale::c().toLongLong(number, &ok);
    if (!ok || program < 0 || program > 99999999)
        return fail(tr("The program number must be a whole number from 0 to 99999999"),
                    QStringLiteral("programNumber"));
    a.insert(QStringLiteral("programNumber"), static_cast<double>(program));
    a.insert(QStringLiteral("machine"), in.machine.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(in.machine));
    if (!in.post.isEmpty() || !adding)
        a.insert(QStringLiteral("post"), in.post);
    // The stock material: given when there is one, and as null when an edit takes it away.
    if (!in.material.isEmpty() || !adding)
        a.insert(QStringLiteral("material"),
                 in.material.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(in.material));
    QJsonObject library;
    if (!in.libraryMachine.isEmpty())
        library.insert(QStringLiteral("machines"), QJsonArray{in.libraryMachine});
    if (!in.libraryMaterial.isEmpty())
        library.insert(QStringLiteral("materials"), QJsonArray{in.libraryMaterial});
    if (!library.isEmpty())
        a.insert(QStringLiteral("library"), library);
    out.args = a;
    out.error.clear();
    out.field.clear();
    return out;
}

} // namespace ray::ui::camui
