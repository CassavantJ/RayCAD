// The setup sheet's HTML (task T-076, src/post/setupsheet.cpp): the goldens (a fixed date and a fixed picture give the
// same bytes on every run and every OS), what a sheet must show, the escaping of every text a user or a library gives
// (hostile names), a self-contained document (no external URL, the picture embedded), the notice, and one document
// for several setups with each on a new page.
#include "post_test_util.h"
#include "setupsheet.h"

#include <ray/cam.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <span>

using namespace ray;
using namespace ray::post;
using namespace ray::post::test;
using Catch::Matchers::ContainsSubstring;

namespace {

// A real 2 x 2 PNG (red, dark green / blue, white), as a picture the renderer only embeds.
constexpr std::array<unsigned char, 78> kPng = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x08, 0x02, 0x00, 0x00, 0x00, 0xfd, 0xd4, 0x9a,
    0x73, 0x00, 0x00, 0x00, 0x15, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0xf8, 0xcf, 0xc0, 0xc0,
    0xb0, 0x00, 0x88, 0x19, 0xfe, 0xff, 0xff, 0xff, 0x1f, 0x00, 0x1c, 0x97, 0x05, 0x9c, 0x42, 0x7c,
    0xc7, 0x03, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};

std::vector<std::byte> png()
{
    std::vector<std::byte> out;
    for (unsigned char c : kPng)
        out.push_back(static_cast<std::byte>(c));
    return out;
}

PostInfo shippedPost()
{
    PostInfo info{};
    info.id = "haas-ngc";
    info.title = "Haas NGC (Next Generation Control)";
    info.vendor = "Haas";
    info.dialect = "haas-ngc";
    info.fileExtension = ".nc";
    info.apiVersion = 1;
    return info;
}

lib::Material aluminium()
{
    lib::Material m{};
    m.name = "6061-T6";
    m.group = "Aluminium";
    m.hardnessHb = 95;
    return m;
}

// A facing pass, a pocket with stock left, and six peck-drilled holes: three tools, the second used twice.
Program sampleProgram(UnitSystem units)
{
    Program p;
    p.units = units;
    p.setup.name = "Op 1 - top";
    p.setup.stock = cam::BoxStock{0, 0, 0, 0, 0, 1.5};
    p.setup.wcs.originMode = cam::Wcs::OriginMode::StockCorner;
    p.setup.wcs.stockCornerIndex = 0;

    lib::Tool face = makeTool("50 mm face mill", lib::ToolType::FaceMill, 50, 5);
    face.vendor = "Acme";
    face.productId = "FM50-5";
    cam::Toolpath& f = p.section("Face top", "face", 1, face, 6000);
    p.ops.back().cutting.feedCut = 1500;
    p.ops.back().cutting.feedPlunge = 500;
    p.ops.back().paramsJson = R"({"pattern":"zigzag","stepdown":1.5,"stepover":0.7})";
    f.moves = {rapid(-mm(30), 0, mm(25)),
               rapid(-mm(30), 0, mm(2)),
               linear(-mm(30), 0, 0, mmPerMin(500)),
               linear(mm(150), 0, 0, mmPerMin(1500)),
               linear(mm(150), mm(40), 0, mmPerMin(1500)),
               linear(-mm(30), mm(40), 0, mmPerMin(1500)),
               linear(-mm(30), mm(80), 0, mmPerMin(1500)),
               linear(mm(150), mm(80), 0, mmPerMin(1500)),
               rapid(mm(150), mm(80), mm(25))};

    lib::Tool mill = makeTool("10 mm 3-flute end mill", lib::ToolType::FlatEndMill, 10, 3);
    cam::Toolpath& pk = p.section("Rough pocket", "pocket2d", 2, mill, 8000);
    p.ops.back().cutting.feedCut = 1200;
    p.ops.back().cutting.feedPlunge = 400;
    p.ops.back().paramsJson =
        R"({"cutMode":"climb","entry":{"stepdown":9},"stepdown":2,"stepover":0.4,"stockToLeaveFloor":0.1,"stockToLeaveWall":0.25})";
    p.ops.back().tool.stickOut = 32;
    pk.moves = {rapid(mm(40), mm(30), mm(25)),
                rapid(mm(40), mm(30), mm(2)),
                linear(mm(40), mm(30), -mm(2), mmPerMin(400)),
                arc(false, mm(50), mm(40), -mm(2), mm(40), mm(40), 0, mmPerMin(1200)),
                linear(mm(80), mm(40), -mm(4), mmPerMin(1200)),
                rapid(mm(80), mm(40), mm(25))};
    pk.warnings = {"An area of 0.8 mm\xc2\xb2 could not be cleared with this tool"};

    cam::Toolpath& fin = p.section("Finish pocket walls", "contour2d", 2, mill, 9000);
    p.ops.back().cutting.feedCut = 900;
    p.ops.back().cutting.coolant = cam::CuttingData::Coolant::Mist;
    p.ops.back().paramsJson = R"({"stockToLeave":0,"stockToLeaveAxial":0})";
    p.ops.back().tool.stickOut = 35;
    fin.moves = {rapid(mm(40), mm(30), mm(25)), linear(mm(40), mm(30), -mm(4), mmPerMin(300)),
                 linear(mm(80), mm(30), -mm(4), mmPerMin(900)), rapid(mm(80), mm(30), mm(25))};

    lib::Tool drill = makeTool("6.6 mm jobber drill", lib::ToolType::Drill, 6.6, 2);
    drill.tipAngleDeg = 118;
    cam::Toolpath& d = p.section("Drill 6.6 thru", "drill", 3, drill, 1500);
    p.ops.back().cutting.feedCut = 150;
    p.ops.back().cutting.feedPlunge = 150;
    p.ops.back().paramsJson = R"({"cycle":"peck","peck":2})";
    p.holders.back().name = "BT40 ER32 collet chuck";
    for (int i = 0; i < 6; ++i) {
        d.moves.push_back(levels(
            cycle(cam::MoveKind::CyclePeck, mm(20 + 20 * i), mm(20), -mm(12), mm(2), mmPerMin(150), mm(2)), 0, mm(25)));
    }
    d.moves.push_back(cycleCancel());
    d.moves.push_back(rapid(mm(120), mm(20), mm(25)));
    return p;
}

cam::CycleTime timeOf(const Program& p)
{
    std::vector<cam::CycleTimeOperation> ops;
    for (std::size_t i = 0; i < p.ops.size(); ++i)
        ops.push_back(cam::CycleTimeOperation{&p.paths[i], p.ops[i].tool.toolNumber});
    return cam::estimateCycleTime(ops, p.machine);
}

struct Sheet {
    Program program;
    PostJob job;
    PostInfo post = shippedPost();
    lib::Material material = aluminium();
    SetupSheetInput input;

    explicit Sheet(UnitSystem units) : program(sampleProgram(units))
    {
        job = program.job();
        job.fixedDate = "2026-11-23";
        job.userName = "";
        input.job = &job;
        input.time = timeOf(program);
        input.fixtureImagePng = png();
        input.stock = Box3{{0, 0, -11.5}, {120, 80, 0}};
        input.post = &post;
        input.postShipped = true;
        input.material = &material;
        input.partName = "Bracket";
    }
    Sheet(const Sheet&) = delete;
};

std::string goldenFile(const char* name)
{
    return std::string(RAY_GOLDEN_SETUPSHEET_DIR) + "/" + name;
}

void checkGolden(const char* name, const std::string& produced)
{
    if (const char* dir = std::getenv("RAYCAD_GOLDEN_PRODUCED")) {
        // Laid out like the suite: tools/update_golden.py --produced <dir> --suite setupsheet.
        const std::filesystem::path folder(dir);
        std::filesystem::create_directories(folder);
        std::ofstream(folder / name, std::ios::binary) << produced;
    }
    const std::string golden = readFile(goldenFile(name));
    INFO("golden " << name);
    CHECK_FALSE(golden.empty());
    CHECK(produced == golden);
}

std::size_t count(const std::string& text, const std::string& what)
{
    std::size_t n = 0;
    for (std::size_t p = text.find(what); p != std::string::npos; p = text.find(what, p + what.size()))
        ++n;
    return n;
}

} // namespace

TEST_CASE("setup sheet helpers: escaping, base64, durations, work offsets, origins", "[post][setupsheet]")
{
    CHECK(sheet::escapeHtml("a<b>&\"c'") == "a&lt;b&gt;&amp;&quot;c&#39;");
    CHECK(sheet::escapeHtml(std::string("x\x01y\tz\n", 6)) == "x y\tz\n");
    CHECK(sheet::escapeHtml("\xc3\x98 6") == "\xc3\x98 6"); // UTF-8 passes through
    const auto b64 = [](std::string_view s) {
        std::vector<std::byte> v;
        for (char c : s)
            v.push_back(static_cast<std::byte>(c));
        return sheet::base64(v);
    };
    // RFC 4648 section 10.
    CHECK(b64("") == "");
    CHECK(b64("f") == "Zg==");
    CHECK(b64("fo") == "Zm8=");
    CHECK(b64("foo") == "Zm9v");
    CHECK(b64("foob") == "Zm9vYg==");
    CHECK(b64("fooba") == "Zm9vYmE=");
    CHECK(b64("foobar") == "Zm9vYmFy");
    CHECK(sheet::duration(7.4) == "0:00:07");
    CHECK(sheet::duration(3723) == "1:02:03");
    CHECK(sheet::duration(100000) == "27:46:40");
    CHECK(sheet::duration(-1).empty());
    CHECK(sheet::workOffsetCode(1) == "G54");
    CHECK(sheet::workOffsetCode(6) == "G59");
    CHECK(sheet::workOffsetCode(7) == "G54.1 P1");
    cam::Wcs w;
    CHECK(sheet::originWords(w) == "Top front left corner of the stock");
    w.stockCornerIndex = 2;
    CHECK(sheet::originWords(w) == "Top back right corner of the stock");
    w.stockCornerIndex = 5;
    CHECK(sheet::originWords(w) == "Bottom front right corner of the stock");
    w.originMode = cam::Wcs::OriginMode::StockTopCenter;
    CHECK(sheet::originWords(w) == "Centre of the top face of the stock");
    CHECK(sheet::styleSheet().find("color: black") != std::string_view::npos);
}

TEST_CASE("setup sheet goldens: the same bytes on every run and every OS", "[post][setupsheet][golden]")
{
    {
        Sheet s(UnitSystem::Metric);
        const auto html = renderSetupSheetHtml(s.input);
        REQUIRE(html.has_value());
        checkGolden("sheet_metric.html", *html);
        CHECK(*renderSetupSheetHtml(s.input) == *html); // again, in the same process
    }
    {
        // A whole job: setup 1 in inches, then a second setup (the part turned over) on a round bar, with a post from
        // the user's own library that is not labelled, on a machine checked against the real one.
        Sheet a(UnitSystem::Inch);
        Sheet b(UnitSystem::Inch);
        b.program.setup.name = "Op 2 - bottom";
        b.program.setup.programNumber = 1002;
        b.program.setup.wcs.workOffset = 2;
        b.program.setup.wcs.originMode = cam::Wcs::OriginMode::StockTopCenter;
        b.program.setup.wcs.flipX = true;
        b.program.setup.stock = cam::CylinderStock{90, 30, 2, 0};
        b.program.machine.verified = true;
        b.input.stock = Box3{{-45, -45, -30}, {45, 45, 0}};
        b.input.postShipped = false;
        b.input.material = nullptr;
        b.input.fixtureImagePng.clear();
        b.job = b.program.job();
        b.job.fixedDate = "2026-11-23";
        b.input.job = &b.job;
        const std::array<SetupSheetInput, 2> both{a.input, b.input};
        const auto html = renderSetupSheetsHtml(both);
        REQUIRE(html.has_value());
        checkGolden("job_inch.html", *html);
    }
}

TEST_CASE("a setup sheet shows what the operator needs", "[post][setupsheet]")
{
    Sheet s(UnitSystem::Metric);
    const auto r = renderSetupSheetHtml(s.input);
    REQUIRE(r.has_value());
    const std::string& html = *r;
    // Program, part, file, units, work offset and its origin, stock and material.
    CHECK_THAT(html, ContainsSubstring("Program <b>1001</b>"));
    CHECK_THAT(html, ContainsSubstring("host test part (rev A)"));
    CHECK_THAT(html, ContainsSubstring(">Bracket<"));
    CHECK_THAT(html, ContainsSubstring(">bracket<")); // the file (PostJob::documentName)
    CHECK_THAT(html, ContainsSubstring("Millimetres (G21)"));
    CHECK_THAT(html, ContainsSubstring(">G54<"));
    CHECK_THAT(html, ContainsSubstring("Top front left corner of the stock"));
    CHECK_THAT(html, ContainsSubstring("X 0 to 120<br>Y 0 to 80<br>Z -11.5 to 0 mm"));
    CHECK_THAT(html, ContainsSubstring("Block 120 &#215; 80 &#215; 11.5 mm"));
    CHECK_THAT(html, ContainsSubstring("6061-T6 (Aluminium), 95 HB"));
    CHECK_THAT(html, ContainsSubstring("2026-11-23"));
    // The picture, embedded as it was given.
    CHECK_THAT(html, ContainsSubstring("<img src=\"data:image/png;base64," + sheet::base64(png()) + "\""));
    // The tools: one row per tool number with diameter, flutes, stick-out, holder and the operations using it.
    CHECK(count(html, "<tr><td><b>T") == 3);
    CHECK_THAT(html, ContainsSubstring("<b>T2</b></td><td><b>10 mm 3-flute end mill</b><br>Flat end mill</td><td>10 "
                                       "mm</td><td>3</td><td>32, 35 mm</td><td>CAT40 ER32</td><td>H2 D2</td><td>2. "
                                       "Rough pocket<br>3. Finish pocket walls</td>"));
    CHECK_THAT(html, ContainsSubstring("Drill, 118&#176; point"));
    CHECK_THAT(html, ContainsSubstring("BT40 ER32 collet chuck"));
    CHECK_THAT(html, ContainsSubstring("Acme FM50-5"));
    // The operations: type, depth, feeds and speeds, stock to leave, time, warnings.
    CHECK_THAT(html, ContainsSubstring("Peck drilling (full retract), 6 holes"));
    CHECK_THAT(html, ContainsSubstring("Z -12<br>peck 2"));
    CHECK_THAT(html, ContainsSubstring("Z -4<br>stepdown 2")); // the top-level stepdown, not the entry's
    CHECK_THAT(html, ContainsSubstring("8000 rpm CW<br>Flood coolant"));
    CHECK_THAT(html, ContainsSubstring("cut 1200 mm/min<br>plunge 400 mm/min<br>0.05 mm per tooth"));
    CHECK_THAT(html, ContainsSubstring("walls 0.25<br>floor 0.1"));
    CHECK_THAT(html, ContainsSubstring("walls 0<br>floor 0"));
    CHECK_THAT(html, ContainsSubstring("Warning: An area of 0.8 mm\xc2\xb2 could not be cleared with this tool"));
    REQUIRE(s.input.time.operations.size() == 4);
    CHECK_THAT(html, ContainsSubstring("<td>" + sheet::duration(s.input.time.operations[3].totalSec) + "</td>"));
    // The total, as an estimate, with what it assumed.
    CHECK_THAT(html, ContainsSubstring("Estimated cycle time " + sheet::duration(s.input.time.totalSec)));
    CHECK_THAT(html, ContainsSubstring("An estimate from the toolpaths and the machine definition"));
    CHECK(s.input.time.toolChanges == 3);
    CHECK_THAT(html, ContainsSubstring("3 tool changes"));
    for (const std::string& note : s.input.time.notes)
        CHECK_THAT(html, ContainsSubstring(sheet::escapeHtml(note)));
    // The notice, worded as the post dialog words it.
    CHECK_THAT(html, ContainsSubstring("This post has not been proven on a real machine, and the machine definition "
                                       "(Test mill) has not been checked against one."));
    CHECK_THAT(html, ContainsSubstring("Dry-run the program before cutting: single block, feed hold in reach"));
    CHECK_THAT(html, ContainsSubstring("Sign-off"));
}

TEST_CASE("a setup sheet is one self-contained file, black on white", "[post][setupsheet]")
{
    Sheet s(UnitSystem::Metric);
    const std::string html = *renderSetupSheetHtml(s.input);
    CHECK(html.rfind("<!DOCTYPE html>\n", 0) == 0);
    CHECK_THAT(html, ContainsSubstring("<meta charset=\"utf-8\">"));
    // No external resource: no URL scheme but the embedded picture's data: URL, no stylesheet link, no script.
    CHECK(html.find("http:") == std::string::npos);
    CHECK(html.find("https:") == std::string::npos);
    CHECK(html.find("file:") == std::string::npos);
    CHECK(html.find("url(") == std::string::npos);
    CHECK(html.find("<link") == std::string::npos);
    CHECK(html.find("<script") == std::string::npos);
    CHECK(html.find("@import") == std::string::npos);
    std::size_t srcs = 0;
    for (std::size_t p = html.find("src="); p != std::string::npos; p = html.find("src=", p + 4)) {
        ++srcs;
        CHECK(html.compare(p, 21, "src=\"data:image/png;b") == 0);
    }
    CHECK(srcs == 1);
    // The style is inline and prints black on white.
    CHECK_THAT(html, ContainsSubstring("<style>\n" + std::string(sheet::styleSheet()) + "</style>"));
    CHECK_THAT(html, ContainsSubstring("color: black; background-color: white;"));
    // Without a picture, no image at all.
    s.input.fixtureImagePng.clear();
    const std::string bare = *renderSetupSheetHtml(s.input);
    CHECK(bare.find("<img") == std::string::npos);
    CHECK_THAT(bare, ContainsSubstring("No picture."));
}

TEST_CASE("every text from the user or a library is escaped in a setup sheet", "[post][setupsheet]")
{
    const std::string evil = "<script>alert(\"x\")</script>&amp;'\"><img src=x onerror=alert(1)>";
    const std::string escaped = sheet::escapeHtml(evil);
    Sheet s(UnitSystem::Metric);
    s.program.setup.name = evil;
    s.program.setup.programComment = evil;
    s.program.setup.postId = evil;
    s.program.machine.name = evil;
    s.program.machine.vendor = evil;
    s.program.machine.model = evil;
    for (cam::Operation& op : s.program.ops)
        op.name = evil;
    for (lib::Tool& t : s.program.tools) {
        t.name = evil;
        t.vendor = evil;
        t.productId = evil;
        t.threadSpec = evil;
        t.type = lib::ToolType::Tap;
    }
    for (lib::Holder& h : s.program.holders)
        h.name = evil;
    for (cam::Toolpath& p : s.program.paths)
        p.warnings = {evil};
    s.program.ops.front().type = evil;
    s.program.ops.front().paramsJson = R"({"cycle":"<b>"})";
    s.job = s.program.job();
    s.job.documentName = evil;
    s.job.userName = evil;
    s.job.fixedDate = evil;
    s.input.job = &s.job;
    s.post.title = evil;
    s.post.id = evil;
    s.post.verifiedOn = {evil};
    s.material.name = evil;
    s.material.group = evil;
    s.input.partName = evil;
    s.input.time.notes = {evil};
    for (const bool several : {false, true}) {
        const auto r = several ? renderSetupSheetsHtml(std::span<const SetupSheetInput>(&s.input, 1))
                               : renderSetupSheetHtml(s.input);
        REQUIRE(r.has_value());
        const std::string& html = *r;
        CHECK(html.find("<script") == std::string::npos);
        CHECK(html.find("<img src=x") == std::string::npos);
        CHECK(html.find("onerror=alert(1)>") == std::string::npos);
        CHECK(html.find("'\"><img") == std::string::npos);
        CHECK(count(html, escaped) >= 20);
        // The only tags are the sheet's own.
        const std::regex tag("<([a-zA-Z!/][a-zA-Z0-9]*)");
        std::set<std::string> tags;
        for (auto it = std::sregex_iterator(html.begin(), html.end(), tag); it != std::sregex_iterator(); ++it)
            tags.insert((*it)[1].str());
        const std::set<std::string> allowed{"!DOCTYPE", "html",   "/html",  "head",   "/head", "meta",   "title",
                                            "/title",   "style",  "/style", "body",   "/body", "h1",     "/h1",
                                            "h2",       "/h2",    "p",      "/p",     "b",     "/b",     "br",
                                            "table",    "/table", "thead",  "/thead", "tbody", "/tbody", "tr",
                                            "/tr",      "th",     "/th",    "td",     "/td",   "img"};
        for (const std::string& t : tags) {
            INFO("tag <" << t);
            CHECK(allowed.count(t) == 1);
        }
    }
}

TEST_CASE("the notice on a setup sheet follows the post and the machine", "[post][setupsheet]")
{
    Sheet s(UnitSystem::Metric);
    const auto html = [&] {
        return *renderSetupSheetHtml(s.input);
    };
    CHECK_THAT(html(), ContainsSubstring("table class=\"notice\""));
    // A proven post on a checked machine: no notice, and the controls it was proven on are listed.
    s.post.verifiedOn = {"Haas VF-2, NGC 100.21"};
    s.program.machine.verified = true;
    CHECK(html().find("table class=\"notice\"") == std::string::npos);
    CHECK_THAT(html(), ContainsSubstring("Post proven on</td><td class=\"value\">Haas VF-2, NGC 100.21"));
    // An unchecked machine alone.
    s.program.machine.verified = false;
    CHECK_THAT(html(), ContainsSubstring("The machine definition (Test mill) has not been checked against the real "
                                         "machine."));
    CHECK(html().find("This post has not been proven") == std::string::npos);
    // A post of the user's own library is not labelled either way (as in the post dialog).
    s.program.machine.verified = true;
    s.post.verifiedOn.clear();
    s.input.postShipped = false;
    CHECK(html().find("table class=\"notice\"") == std::string::npos);
    s.input.postShipped = true;
    CHECK_THAT(html(), ContainsSubstring("<b>This post has not been proven on a real machine.</b>"));
    // No post chosen at all.
    s.input.post = nullptr;
    CHECK_THAT(html(), ContainsSubstring("No post was chosen for this setup"));
    // One tool number for two different tools.
    s.input.post = &s.post;
    s.program.tools[3].id = ToolId{Uuid::fromString("6a1c53f2-4f2b-4d6e-9b4e-7c0d6a1f2e31")};
    s.program.ops[3].tool.toolNumber = 2;
    s.job = s.program.job();
    s.job.fixedDate = "2026-11-23";
    CHECK_THAT(html(),
               ContainsSubstring("Tool number T2 is given to different tools in this program (10 mm 3-flute end "
                                 "mill, 6.6 mm jobber drill)"));
}

TEST_CASE("one document for several setups starts each on a new page", "[post][setupsheet]")
{
    Sheet a(UnitSystem::Metric);
    Sheet b(UnitSystem::Metric);
    Sheet c(UnitSystem::Metric);
    const std::array<SetupSheetInput, 3> all{a.input, b.input, c.input};
    const auto r = renderSetupSheetsHtml(all);
    REQUIRE(r.has_value());
    CHECK(count(*r, "<h1") == 3);
    CHECK(count(*r, "page-break-before: always") == 2);
    CHECK(count(*r, "<html") == 1);
    CHECK(count(*r, "<style>") == 1);
    CHECK_THAT(*r, ContainsSubstring("<title>Setup sheets: bracket</title>"));
    // One setup alone is the single sheet's body.
    const auto one = renderSetupSheetsHtml(std::span<const SetupSheetInput>(&a.input, 1));
    REQUIRE(one.has_value());
    CHECK(count(*one, "page-break-before") == 0);
    // The three setups share program number 1001: each sheet says so, naming the others.
    CHECK(count(*r, "Program number 1001 is also the number of Op 1 - top, Op 1 - top in this job") == 3);
    b.program.setup.programNumber = 1002;
    c.program.setup.programNumber = 1003;
    CHECK(renderSetupSheetsHtml(all)->find("is also the number of") == std::string::npos);
    // Refused: nothing, or an input without a job.
    CHECK(renderSetupSheetsHtml({}).error().code == ErrorCode::InvalidArgument);
    SetupSheetInput none{};
    CHECK(renderSetupSheetHtml(none).error().code == ErrorCode::InvalidArgument);
}
