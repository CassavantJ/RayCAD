// The non-visual part of the CAM workspace (task T-044, src/ui/cam/setup_logic.h): status badges, the origin
// choices, the triad's geometry and the setup command's arguments. No window: these run in every tier and on every
// platform without a screen.
#include "cam/setup_logic.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QLocale>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace ray;
using namespace ray::ui::camui;
using Catch::Approx;

namespace {

double len(Vec3 v)
{
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

Vec3 minus(Vec3 a, Vec3 b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

// Lengths as the panel's fields give them: "<number> mm" or "<number> in", or a plain number in mm.
std::optional<double> length(const QString& text, QString* error)
{
    QString t = text.trimmed();
    double scale = 1;
    if (t.endsWith(QLatin1String(" in"))) {
        scale = 25.4;
        t.chop(3);
    } else if (t.endsWith(QLatin1String(" mm"))) {
        t.chop(3);
    }
    bool ok = false;
    const double v = QLocale::c().toDouble(t, &ok);
    if (!ok) {
        *error = QStringLiteral("'%1' is not a length").arg(text);
        return std::nullopt;
    }
    return v * scale;
}

SetupInput blockInput()
{
    SetupInput in;
    in.id = QStringLiteral("a0000000-0000-4000-8000-000000000001");
    in.name = QStringLiteral("Setup 1");
    in.partBodies = {QStringLiteral("b0000000-0000-4000-8000-000000000001")};
    in.stock = QStringLiteral("box");
    for (const char* key : {"xNeg", "xPos", "yNeg", "yPos", "zNeg", "zPos"})
        in.lengths[QLatin1String(key)] = QStringLiteral("0 mm");
    in.origin = QStringLiteral("corner0");
    in.workOffset = 1;
    in.programNumber = QStringLiteral("1001");
    in.machine = QStringLiteral("90000000-0000-4000-8000-000000000001");
    in.post = QStringLiteral("generic-iso");
    return in;
}

} // namespace

TEST_CASE("status badges: every engine status has a key and a plain text", "[cam][ui]")
{
    CHECK(statusKey(cam::OpStatus::UpToDate) == QStringLiteral("upToDate"));
    CHECK(statusKey(cam::OpStatus::Stale) == QStringLiteral("stale"));
    CHECK(statusKey(cam::OpStatus::Generating) == QStringLiteral("generating"));
    CHECK(statusKey(cam::OpStatus::Failed) == QStringLiteral("failed"));
    CHECK(statusKey(cam::OpStatus::ReferenceBroken) == QStringLiteral("broken"));
    CHECK(statusKey(cam::OpStatus::Suppressed) == QStringLiteral("suppressed"));
    CHECK(statusText(cam::OpStatus::UpToDate) == QStringLiteral("Up to date"));
    CHECK(statusText(cam::OpStatus::ReferenceBroken) == QStringLiteral("Broken reference"));
    CHECK(statusText(cam::OpStatus::Generating) == QStringLiteral("Generating"));
    CHECK(statusText(cam::OpStatus::Generating, 0.426) == QStringLiteral("Generating 42%"));
    CHECK(statusText(cam::OpStatus::Generating, 1.0) == QStringLiteral("Generating 100%"));
}

TEST_CASE("a setup's badge: its own resolution first, then the worst of its operations", "[cam][ui]")
{
    const cam::OpState up{cam::OpStatus::UpToDate, "", nullptr};
    const cam::OpState stale{cam::OpStatus::Stale, "The toolpath is out of date", nullptr};
    const cam::OpState generating{cam::OpStatus::Generating, "Generating", nullptr};
    const cam::OpState failed{cam::OpStatus::Failed, "Test generator failed", nullptr};
    const cam::OpState broken{cam::OpStatus::ReferenceBroken, "The Z face is gone", nullptr};
    const cam::OpState off{cam::OpStatus::Suppressed, "Suppressed", nullptr};

    CHECK_FALSE(setupBadge(std::nullopt, {}).has_value());
    CHECK_FALSE(setupBadge(Status{}, {}).has_value()); // resolves, no operations: nothing to show
    CHECK(setupBadge(Status{}, {&up})->status == cam::OpStatus::UpToDate);
    CHECK(setupBadge(Status{}, {&up, &stale})->status == cam::OpStatus::Stale);
    CHECK(setupBadge(Status{}, {&stale, &generating, &up})->status == cam::OpStatus::Generating);
    const auto f = setupBadge(Status{}, {&generating, &failed, &stale});
    REQUIRE(f);
    CHECK(f->status == cam::OpStatus::Failed);
    CHECK(f->message == QStringLiteral("Test generator failed"));
    CHECK(setupBadge(Status{}, {&failed, &broken})->status == cam::OpStatus::ReferenceBroken);
    // Suppressed operations count only when every one is.
    CHECK(setupBadge(Status{}, {&off, &up})->status == cam::OpStatus::UpToDate);
    CHECK(setupBadge(Status{}, {&off, &off})->status == cam::OpStatus::Suppressed);
    // The setup's own broken reference wins over its operations.
    const Status brokenOwn = tl::unexpected(Error{ErrorCode::ReferenceBroken, "The Z face of setup 'Setup 1'", {}});
    const auto own = setupBadge(brokenOwn, {&up});
    REQUIRE(own);
    CHECK(own->status == cam::OpStatus::ReferenceBroken);
    CHECK(own->message == QStringLiteral("The Z face of setup 'Setup 1'"));
    const Status invalid = tl::unexpected(Error{ErrorCode::InvalidArgument, "is not flat", {}});
    CHECK(setupBadge(invalid, {})->status == cam::OpStatus::Failed);
}

TEST_CASE("origin choices map to the WCS origin and back", "[cam][ui]")
{
    for (int k = 0; k < 8; ++k) {
        cam::Wcs wcs;
        wcs.originMode = cam::Wcs::OriginMode::StockCorner;
        wcs.stockCornerIndex = k;
        const QString key = originKey(wcs);
        CHECK(key == markerKey(k));
        cam::Wcs::OriginMode mode{};
        int corner = -1;
        REQUIRE(originFromKey(key, &mode, &corner));
        CHECK(mode == cam::Wcs::OriginMode::StockCorner);
        CHECK(corner == k);
    }
    cam::Wcs top;
    top.originMode = cam::Wcs::OriginMode::StockTopCenter;
    CHECK(originKey(top) == QStringLiteral("topCenter"));
    CHECK(markerKey(8) == QStringLiteral("topCenter"));
    cam::Wcs::OriginMode mode{};
    int corner = 0;
    CHECK(originFromKey(QStringLiteral("modelPoint"), &mode, &corner));
    CHECK(mode == cam::Wcs::OriginMode::ModelPoint);
    CHECK_FALSE(originFromKey(QStringLiteral("corner8"), &mode, &corner));
    CHECK_FALSE(originFromKey(QStringLiteral("centre"), &mode, &corner));
}

TEST_CASE("the triad: three axes of the given length from the origin, heads at the tips", "[cam][ui]")
{
    const Vec3 o{10, 20, 30};
    const Triad t = triadGeometry(o, Vec3{0, 1, 0}, Vec3{-1, 0, 0}, Vec3{0, 0, 1}, 15);
    for (std::size_t i = 0; i < 3; ++i) {
        REQUIRE(t.segments[i].size() == 6); // the shaft and two head strokes
        CHECK(len(minus(t.segments[i][0], o)) == Approx(0));
        CHECK(len(minus(t.segments[i][1], o)) == Approx(15));
        CHECK(len(minus(t.tips[i], o)) == Approx(15));
        // Both head strokes start at the tip and point back towards the origin.
        for (std::size_t s : {2u, 4u}) {
            CHECK(len(minus(t.segments[i][s], t.tips[i])) == Approx(0));
            CHECK(len(minus(t.segments[i][s + 1], o)) < 15);
        }
    }
    CHECK(len(minus(t.tips[0], Vec3{10, 35, 30})) == Approx(0)); // X along model Y here
    CHECK(len(minus(t.tips[1], Vec3{-5, 20, 30})) == Approx(0));
    CHECK(triadLength(Box3{Vec3{0, 0, -10}, Vec3{120, 80, 0}}) == Approx(40));
    CHECK(triadLength(Box3{Vec3{0, 0, 0}, Vec3{3, 3, 3}}) == Approx(5));        // at least 5 mm
    CHECK(triadLength(Box3{Vec3{0, 0, 0}, Vec3{3000, 10, 10}}) == Approx(200)); // at most 200 mm
}

TEST_CASE("the setup arguments: the defaults make a zero-offset box at corner 0", "[cam][ui]")
{
    SetupInput in = blockInput();
    const ArgsResult r = setupArgs(in, length, true);
    INFO(r.error.toStdString());
    REQUIRE(r.error.isEmpty());
    const QJsonObject& a = r.args;
    CHECK(a.value(QStringLiteral("id")).toString() == in.id);
    CHECK(a.value(QStringLiteral("partBodies")).toArray().size() == 1);
    CHECK(a.value(QStringLiteral("fixtureBodies")).toArray().isEmpty());
    const QJsonObject stock = a.value(QStringLiteral("stock")).toObject();
    CHECK(stock.value(QStringLiteral("type")).toString() == QStringLiteral("box"));
    CHECK(stock.value(QStringLiteral("xNeg")).toDouble() == 0.0);
    const QJsonObject wcs = a.value(QStringLiteral("wcs")).toObject();
    CHECK(wcs.value(QStringLiteral("origin")).toObject().value(QStringLiteral("corner")).toInt() == 0);
    CHECK(wcs.value(QStringLiteral("workOffset")).toInt() == 1);
    CHECK_FALSE(wcs.contains(QStringLiteral("zFace")));
    CHECK_FALSE(wcs.contains(QStringLiteral("flipX"))); // written only when set
    CHECK(a.value(QStringLiteral("programNumber")).toInt() == 1001);
    CHECK(a.value(QStringLiteral("machine")).toString() == in.machine);
    CHECK_FALSE(a.contains(QStringLiteral("library")));
}

TEST_CASE("the setup arguments: picks, flips, stock kinds, units and the reasons a value is refused", "[cam][ui]")
{
    SECTION("references and flips")
    {
        SetupInput in = blockInput();
        in.zFace = QJsonObject{{QStringLiteral("body"), in.partBodies.front()},
                               {QStringLiteral("kind"), QStringLiteral("face")}};
        in.xEdge = QJsonObject{{QStringLiteral("body"), in.partBodies.front()},
                               {QStringLiteral("kind"), QStringLiteral("edge")}};
        in.flipX = true;
        in.flipZ = true;
        in.origin = QStringLiteral("topCenter");
        const QJsonObject wcs = setupArgs(in, length, true).args.value(QStringLiteral("wcs")).toObject();
        CHECK(wcs.value(QStringLiteral("zFace")).toObject() == in.zFace);
        CHECK(wcs.value(QStringLiteral("xEdge")).toObject() == in.xEdge);
        CHECK(wcs.value(QStringLiteral("flipX")).toBool());
        CHECK(wcs.value(QStringLiteral("flipZ")).toBool());
        CHECK(wcs.value(QStringLiteral("origin")).toObject().value(QStringLiteral("mode")).toString() ==
              QStringLiteral("stockTopCenter"));
    }
    SECTION("a model point needs its vertex")
    {
        SetupInput in = blockInput();
        in.origin = QStringLiteral("modelPoint");
        ArgsResult r = setupArgs(in, length, true);
        CHECK(r.field == QStringLiteral("origin"));
        CHECK(r.args.isEmpty());
        in.originVertex = QJsonObject{{QStringLiteral("kind"), QStringLiteral("vertex")}};
        r = setupArgs(in, length, true);
        REQUIRE(r.error.isEmpty());
        CHECK(r.args.value(QStringLiteral("wcs")).toObject().contains(QStringLiteral("originVertex")));
    }
    SECTION("offsets in the field's unit, and refused ones name their field")
    {
        SetupInput in = blockInput();
        in.lengths[QStringLiteral("zPos")] = QStringLiteral("0.1 in");
        in.lengths[QStringLiteral("xNeg")] = QStringLiteral("2");
        ArgsResult r = setupArgs(in, length, true);
        REQUIRE(r.error.isEmpty());
        CHECK(r.args.value(QStringLiteral("stock")).toObject().value(QStringLiteral("zPos")).toDouble() ==
              Approx(2.54));
        CHECK(r.args.value(QStringLiteral("stock")).toObject().value(QStringLiteral("xNeg")).toDouble() == Approx(2));
        in.lengths[QStringLiteral("yPos")] = QStringLiteral("-1 mm");
        r = setupArgs(in, length, true);
        CHECK(r.field == QStringLiteral("yPos"));
        CHECK(!r.error.isEmpty());
        in.lengths[QStringLiteral("yPos")] = QStringLiteral("width / 0");
        r = setupArgs(in, length, true);
        CHECK(r.field == QStringLiteral("yPos"));
        CHECK(r.error.contains(QStringLiteral("not a length")));
    }
    SECTION("cylinder and solid stock")
    {
        SetupInput in = blockInput();
        in.stock = QStringLiteral("cylinder");
        in.axis = QStringLiteral("x");
        in.lengths[QStringLiteral("diameter")] = QStringLiteral("50 mm");
        in.lengths[QStringLiteral("length")] = QStringLiteral("2 in");
        ArgsResult r = setupArgs(in, length, true);
        REQUIRE(r.error.isEmpty());
        const QJsonObject cyl = r.args.value(QStringLiteral("stock")).toObject();
        CHECK(cyl.value(QStringLiteral("type")).toString() == QStringLiteral("cylinder"));
        CHECK(cyl.value(QStringLiteral("axis")).toString() == QStringLiteral("x"));
        CHECK(cyl.value(QStringLiteral("length")).toDouble() == Approx(50.8));
        in.lengths[QStringLiteral("diameter")] = QString();
        CHECK(setupArgs(in, length, true).field == QStringLiteral("diameter"));
        in.stock = QStringLiteral("solid");
        CHECK(setupArgs(in, length, true).field == QStringLiteral("solidBody"));
        in.solidBody = in.partBodies.front();
        CHECK(setupArgs(in, length, true).field == QStringLiteral("solidBody")); // a part cannot be its own stock
        in.solidBody = QStringLiteral("b0000000-0000-4000-8000-000000000002");
        r = setupArgs(in, length, true);
        REQUIRE(r.error.isEmpty());
        CHECK(r.args.value(QStringLiteral("stock")).toObject().value(QStringLiteral("body")).toString() ==
              in.solidBody);
    }
    SECTION("a library machine is embedded in the same step")
    {
        SetupInput in = blockInput();
        in.libraryMachine =
            QJsonObject{{QStringLiteral("id"), in.machine}, {QStringLiteral("name"), QStringLiteral("Mill")}};
        const ArgsResult r = setupArgs(in, length, true);
        REQUIRE(r.error.isEmpty());
        const QJsonArray machines =
            r.args.value(QStringLiteral("library")).toObject().value(QStringLiteral("machines")).toArray();
        REQUIRE(machines.size() == 1);
        CHECK(machines.at(0).toObject().value(QStringLiteral("id")).toString() == in.machine);
    }
    SECTION("the stock material, a library one embedded in the same step as the machine")
    {
        SetupInput in = blockInput();
        CHECK_FALSE(setupArgs(in, length, true).args.contains(QStringLiteral("material")));  // none: not given
        CHECK(setupArgs(in, length, false).args.value(QStringLiteral("material")).isNull()); // an edit clears it
        in.material = QStringLiteral("b72c5171-c258-44d2-91ae-ee049b636621");
        CHECK(setupArgs(in, length, true).args.value(QStringLiteral("material")).toString() == in.material);
        CHECK_FALSE(setupArgs(in, length, true).args.contains(QStringLiteral("library"))); // embedded already
        in.libraryMaterial =
            QJsonObject{{QStringLiteral("id"), in.material}, {QStringLiteral("name"), QStringLiteral("6061-T6")}};
        in.libraryMachine =
            QJsonObject{{QStringLiteral("id"), in.machine}, {QStringLiteral("name"), QStringLiteral("Mill")}};
        const QJsonObject library = setupArgs(in, length, false).args.value(QStringLiteral("library")).toObject();
        REQUIRE(library.value(QStringLiteral("materials")).toArray().size() == 1);
        CHECK(library.value(QStringLiteral("materials")).toArray().at(0).toObject().value(QStringLiteral("id")) ==
              in.material);
        CHECK(library.value(QStringLiteral("machines")).toArray().size() == 1);
    }
    SECTION("the name, the bodies and the program number")
    {
        SetupInput in = blockInput();
        in.name = QStringLiteral("  ");
        CHECK(setupArgs(in, length, true).field == QStringLiteral("name"));
        in = blockInput();
        in.partBodies.clear();
        CHECK(setupArgs(in, length, true).field == QStringLiteral("bodies"));
        in = blockInput();
        in.programNumber = QStringLiteral("12.5");
        CHECK(setupArgs(in, length, true).field == QStringLiteral("programNumber"));
        in.programNumber = QStringLiteral("O1001");
        CHECK(setupArgs(in, length, true).field == QStringLiteral("programNumber"));
        in = blockInput();
        in.workOffset = 0;
        CHECK(setupArgs(in, length, true).field == QStringLiteral("workOffset"));
        // An edit gives every member, the post too.
        in = blockInput();
        in.post.clear();
        CHECK(setupArgs(in, length, false).args.contains(QStringLiteral("post")));
        CHECK_FALSE(setupArgs(in, length, true).args.contains(QStringLiteral("post")));
    }
}
