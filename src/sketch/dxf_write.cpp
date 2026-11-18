// DXF export (task T-091). See dxf.h. The file layout (sections, tables, block records, owner handles and subclass
// markers) follows the R2000 part of the public DXF reference.
#include "dxf.h"

#include "loops_curves.h"
#include "model.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <set>

namespace ray::sketch {

using namespace detail;

namespace {

constexpr double kPi = std::numbers::pi;

std::string number(double v)
{
    if (v == 0) {
        v = 0; // no "-0"
    }
    char buf[64];
    const auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), v);
    std::string s(buf, ec == std::errc() ? end : buf);
    if (s.find_first_of(".eEn") == std::string::npos) {
        s += ".0";
    }
    return s;
}

std::string hex(unsigned v)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%X", v);
    return buf;
}

// Degrees in [0, 360) of the direction from c to p; exact at multiples of 90.
double degreesOf(Vec2 c, Vec2 p)
{
    double deg = dmath::atan2(p.y - c.y, p.x - c.x) * 180 / kPi;
    const double quarter = std::round(deg / 90) * 90;
    if (std::fabs(deg - quarter) < 1e-10) {
        deg = quarter;
    }
    if (deg < 0) {
        deg += 360;
    }
    if (deg >= 360) {
        deg -= 360;
    }
    return deg;
}

class Out {
public:
    void pair(int code, std::string_view value)
    {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%3d", code);
        text_ += buf;
        text_ += '\n';
        text_ += value;
        text_ += '\n';
    }
    void num(int code, double v) { pair(code, number(v)); }
    void integer(int code, long long v) { pair(code, std::to_string(v)); }
    std::string& text() { return text_; }

private:
    std::string text_;
};

// Fixed handles of the tables, blocks and objects; entities are numbered from kFirstEntity.
namespace h {
constexpr unsigned BlockRecordTable = 0x1, LayerTable = 0x2, StyleTable = 0x3, LtypeTable = 0x5, ViewTable = 0x6,
                   UcsTable = 0x7, VportTable = 0x8, AppidTable = 0x9, DimstyleTable = 0xA, RootDict = 0xC,
                   GroupDict = 0xD, PlotStyleDict = 0xE, PlotStyleNormal = 0xF, Layer0 = 0x10, LayerConstruction = 0x11,
                   AppidAcad = 0x12, LtypeByBlock = 0x14, LtypeByLayer = 0x15, LtypeContinuous = 0x16,
                   StyleStandard = 0x17, PaperSpaceRecord = 0x1B, PaperSpaceBlock = 0x1C, PaperSpaceEnd = 0x1D,
                   ModelSpaceRecord = 0x1F, ModelSpaceBlock = 0x20, ModelSpaceEnd = 0x21, DimstyleStandard = 0x27,
                   VportActive = 0x29;
constexpr unsigned kFirstEntity = 0x100;
} // namespace h

struct Bounds {
    bool any = false;
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    void add(Vec2 p)
    {
        if (!any) {
            x0 = x1 = p.x;
            y0 = y1 = p.y;
            any = true;
            return;
        }
        x0 = std::min(x0, p.x);
        y0 = std::min(y0, p.y);
        x1 = std::max(x1, p.x);
        y1 = std::max(y1, p.y);
    }
};

class Writer {
public:
    explicit Writer(double scale) : scale_(scale) {}

    void line(Vec2 a, Vec2 b, bool construction)
    {
        a = s(a);
        b = s(b);
        start("LINE", construction);
        e_.pair(100, "AcDbLine");
        xy(10, a);
        xy(11, b);
    }
    void arc(Vec2 c, Vec2 from, Vec2 to, bool ccw, bool construction)
    {
        if (!ccw) {
            std::swap(from, to);
        }
        const double r = dist(from, c) * scale_;
        const double a0 = degreesOf(c, from);
        const double a1 = degreesOf(c, to);
        c = s(c);
        start("ARC", construction);
        e_.pair(100, "AcDbCircle");
        xy(10, c);
        e_.num(40, r);
        e_.pair(100, "AcDbArc");
        e_.num(50, a0);
        e_.num(51, a1);
        bounds_.add({c.x - r, c.y - r});
        bounds_.add({c.x + r, c.y + r});
    }
    void circle(Vec2 c, double r, bool construction)
    {
        c = s(c);
        r *= scale_;
        start("CIRCLE", construction);
        e_.pair(100, "AcDbCircle");
        xy(10, c);
        e_.num(40, r);
        bounds_.add({c.x - r, c.y - r});
        bounds_.add({c.x + r, c.y + r});
    }
    void point(Vec2 p, bool construction)
    {
        p = s(p);
        start("POINT", construction);
        e_.pair(100, "AcDbPoint");
        xy(10, p);
    }
    void spline(std::vector<Vec2> poles, int degree, bool closed, bool construction)
    {
        for (Vec2& q : poles) {
            q = s(q);
        }
        const int n = static_cast<int>(poles.size());
        std::vector<double> knots;
        if (closed) {
            // Periodic uniform: the first `degree` control points repeat at the end, knots 0, 1, 2, ...
            for (int i = 0; i < degree; ++i) {
                poles.push_back(poles[static_cast<std::size_t>(i)]);
            }
            for (int i = 0; i <= n + 2 * degree; ++i) {
                knots.push_back(static_cast<double>(i));
            }
        } else {
            knots = clampedUniformKnots(n, degree);
        }
        start("SPLINE", construction);
        e_.pair(100, "AcDbSpline");
        e_.num(210, 0);
        e_.num(220, 0);
        e_.num(230, 1);
        e_.integer(70, closed ? 1 + 2 + 8 : 8);
        e_.integer(71, degree);
        e_.integer(72, static_cast<long long>(knots.size()));
        e_.integer(73, static_cast<long long>(poles.size()));
        e_.integer(74, 0);
        e_.num(42, 1e-10);
        e_.num(43, 1e-10);
        for (const double k : knots) {
            e_.num(40, k);
        }
        for (const Vec2& q : poles) {
            xy(10, q);
        }
    }

    bool usesConstruction() const { return construction_; }
    std::string document(UnitSystem units)
    {
        Out o;
        o.pair(999, "RayCAD sketch export");
        header(o, units);
        o.pair(0, "SECTION");
        o.pair(2, "CLASSES");
        o.pair(0, "ENDSEC");
        tables(o);
        blocks(o);
        o.pair(0, "SECTION");
        o.pair(2, "ENTITIES");
        o.text() += e_.text();
        o.pair(0, "ENDSEC");
        objects(o);
        o.pair(0, "EOF");
        return std::move(o.text());
    }

private:
    double scale_;
    Out e_;
    unsigned next_ = h::kFirstEntity;
    Bounds bounds_;
    bool construction_ = false;

    Vec2 s(Vec2 p) const { return {p.x * scale_, p.y * scale_}; }
    void xy(int code, Vec2 p)
    {
        e_.num(code, p.x);
        e_.num(code + 10, p.y);
        e_.num(code + 20, 0);
        bounds_.add(p);
    }
    void start(const char* type, bool construction)
    {
        construction_ = construction_ || construction;
        e_.pair(0, type);
        e_.pair(5, hex(next_++));
        e_.pair(330, hex(h::ModelSpaceRecord));
        e_.pair(100, "AcDbEntity");
        e_.pair(8, construction ? kDxfConstructionLayer : "0");
    }

    static void table(Out& o, const char* name, unsigned handle, int count)
    {
        o.pair(0, "TABLE");
        o.pair(2, name);
        o.pair(5, hex(handle));
        o.pair(330, "0");
        o.pair(100, "AcDbSymbolTable");
        o.integer(70, count);
    }
    static void record(Out& o, const char* type, unsigned handle, unsigned owner, const char* subclass)
    {
        o.pair(0, type);
        o.pair(5, hex(handle));
        o.pair(330, hex(owner));
        o.pair(100, "AcDbSymbolTableRecord");
        o.pair(100, subclass);
    }

    void header(Out& o, UnitSystem units)
    {
        const bool inch = units == UnitSystem::Inch;
        o.pair(0, "SECTION");
        o.pair(2, "HEADER");
        o.pair(9, "$ACADVER");
        o.pair(1, "AC1015");
        o.pair(9, "$HANDSEED");
        o.pair(5, hex(next_));
        o.pair(9, "$INSBASE");
        o.num(10, 0);
        o.num(20, 0);
        o.num(30, 0);
        o.pair(9, "$EXTMIN");
        o.num(10, bounds_.x0);
        o.num(20, bounds_.y0);
        o.num(30, 0);
        o.pair(9, "$EXTMAX");
        o.num(10, bounds_.x1);
        o.num(20, bounds_.y1);
        o.num(30, 0);
        o.pair(9, "$INSUNITS");
        o.integer(70, inch ? 1 : 4);
        o.pair(9, "$MEASUREMENT");
        o.integer(70, inch ? 0 : 1);
        o.pair(0, "ENDSEC");
    }

    void tables(Out& o) const
    {
        o.pair(0, "SECTION");
        o.pair(2, "TABLES");

        table(o, "VPORT", h::VportTable, 1);
        record(o, "VPORT", h::VportActive, h::VportTable, "AcDbViewportTableRecord");
        o.pair(2, "*Active");
        o.integer(70, 0);
        o.num(10, 0);
        o.num(20, 0);
        o.num(11, 1);
        o.num(21, 1);
        const double cx = (bounds_.x0 + bounds_.x1) / 2;
        const double cy = (bounds_.y0 + bounds_.y1) / 2;
        const double height = std::max({bounds_.y1 - bounds_.y0, (bounds_.x1 - bounds_.x0) / 1.5, 1.0}) * 1.1;
        o.num(12, cx);
        o.num(22, cy);
        o.num(13, 0);
        o.num(23, 0);
        o.num(14, 10);
        o.num(24, 10);
        o.num(15, 10);
        o.num(25, 10);
        o.num(16, 0);
        o.num(26, 0);
        o.num(36, 1);
        o.num(17, 0);
        o.num(27, 0);
        o.num(37, 0);
        o.num(40, height);
        o.num(41, 1.5);
        o.num(42, 50);
        o.num(43, 0);
        o.num(44, 0);
        o.num(50, 0);
        o.num(51, 0);
        o.integer(71, 0);
        o.integer(72, 100);
        o.integer(73, 1);
        o.integer(74, 3);
        o.integer(75, 0);
        o.integer(76, 0);
        o.integer(77, 0);
        o.integer(78, 0);
        o.pair(0, "ENDTAB");

        table(o, "LTYPE", h::LtypeTable, 3);
        for (const auto& [handle, name] : {std::pair{h::LtypeByBlock, "ByBlock"}, std::pair{h::LtypeByLayer, "ByLayer"},
                                           std::pair{h::LtypeContinuous, "Continuous"}}) {
            record(o, "LTYPE", handle, h::LtypeTable, "AcDbLinetypeTableRecord");
            o.pair(2, name);
            o.integer(70, 0);
            o.pair(3, handle == h::LtypeContinuous ? "Solid line" : "");
            o.integer(72, 65);
            o.integer(73, 0);
            o.num(40, 0);
        }
        o.pair(0, "ENDTAB");

        table(o, "LAYER", h::LayerTable, construction_ ? 2 : 1);
        for (const auto& [handle, name, colour] :
             {std::tuple{h::Layer0, "0", 7}, std::tuple{h::LayerConstruction, kDxfConstructionLayer, 8}}) {
            if (handle == h::LayerConstruction && !construction_) {
                continue;
            }
            record(o, "LAYER", handle, h::LayerTable, "AcDbLayerTableRecord");
            o.pair(2, name);
            o.integer(70, 0);
            o.integer(62, colour);
            o.pair(6, "Continuous");
            o.integer(370, -3);
            o.pair(390, hex(h::PlotStyleNormal));
        }
        o.pair(0, "ENDTAB");

        table(o, "STYLE", h::StyleTable, 1);
        record(o, "STYLE", h::StyleStandard, h::StyleTable, "AcDbTextStyleTableRecord");
        o.pair(2, "Standard");
        o.integer(70, 0);
        o.num(40, 0);
        o.num(41, 1);
        o.num(50, 0);
        o.integer(71, 0);
        o.num(42, 2.5);
        o.pair(3, "txt");
        o.pair(4, "");
        o.pair(0, "ENDTAB");

        table(o, "VIEW", h::ViewTable, 0);
        o.pair(0, "ENDTAB");
        table(o, "UCS", h::UcsTable, 0);
        o.pair(0, "ENDTAB");

        table(o, "APPID", h::AppidTable, 1);
        record(o, "APPID", h::AppidAcad, h::AppidTable, "AcDbRegAppTableRecord");
        o.pair(2, "ACAD");
        o.integer(70, 0);
        o.pair(0, "ENDTAB");

        table(o, "DIMSTYLE", h::DimstyleTable, 1);
        o.pair(100, "AcDbDimStyleTable");
        o.integer(71, 0);
        o.pair(0, "DIMSTYLE");
        o.pair(105, hex(h::DimstyleStandard));
        o.pair(330, hex(h::DimstyleTable));
        o.pair(100, "AcDbSymbolTableRecord");
        o.pair(100, "AcDbDimStyleTableRecord");
        o.pair(2, "Standard");
        o.integer(70, 0);
        o.pair(0, "ENDTAB");

        table(o, "BLOCK_RECORD", h::BlockRecordTable, 2);
        record(o, "BLOCK_RECORD", h::ModelSpaceRecord, h::BlockRecordTable, "AcDbBlockTableRecord");
        o.pair(2, "*Model_Space");
        record(o, "BLOCK_RECORD", h::PaperSpaceRecord, h::BlockRecordTable, "AcDbBlockTableRecord");
        o.pair(2, "*Paper_Space");
        o.pair(0, "ENDTAB");

        o.pair(0, "ENDSEC");
    }

    static void blocks(Out& o)
    {
        o.pair(0, "SECTION");
        o.pair(2, "BLOCKS");
        for (const auto& [block, end, owner, name, paper] :
             {std::tuple{h::ModelSpaceBlock, h::ModelSpaceEnd, h::ModelSpaceRecord, "*Model_Space", false},
              std::tuple{h::PaperSpaceBlock, h::PaperSpaceEnd, h::PaperSpaceRecord, "*Paper_Space", true}}) {
            o.pair(0, "BLOCK");
            o.pair(5, hex(block));
            o.pair(330, hex(owner));
            o.pair(100, "AcDbEntity");
            if (paper) {
                o.integer(67, 1);
            }
            o.pair(8, "0");
            o.pair(100, "AcDbBlockBegin");
            o.pair(2, name);
            o.integer(70, 0);
            o.num(10, 0);
            o.num(20, 0);
            o.num(30, 0);
            o.pair(3, name);
            o.pair(1, "");
            o.pair(0, "ENDBLK");
            o.pair(5, hex(end));
            o.pair(330, hex(owner));
            o.pair(100, "AcDbEntity");
            if (paper) {
                o.integer(67, 1);
            }
            o.pair(8, "0");
            o.pair(100, "AcDbBlockEnd");
        }
        o.pair(0, "ENDSEC");
    }

    static void objects(Out& o)
    {
        o.pair(0, "SECTION");
        o.pair(2, "OBJECTS");
        o.pair(0, "DICTIONARY");
        o.pair(5, hex(h::RootDict));
        o.pair(330, "0");
        o.pair(100, "AcDbDictionary");
        o.integer(281, 1);
        o.pair(3, "ACAD_GROUP");
        o.pair(350, hex(h::GroupDict));
        o.pair(3, "ACAD_PLOTSTYLENAME");
        o.pair(350, hex(h::PlotStyleDict));
        o.pair(0, "DICTIONARY");
        o.pair(5, hex(h::GroupDict));
        o.pair(330, hex(h::RootDict));
        o.pair(100, "AcDbDictionary");
        o.integer(281, 1);
        o.pair(0, "ACDBDICTIONARYWDFLT");
        o.pair(5, hex(h::PlotStyleDict));
        o.pair(330, hex(h::RootDict));
        o.pair(100, "AcDbDictionary");
        o.integer(281, 1);
        o.pair(3, "Normal");
        o.pair(350, hex(h::PlotStyleNormal));
        o.pair(100, "AcDbDictionaryWithDefault");
        o.pair(340, hex(h::PlotStyleNormal));
        o.pair(0, "ACDBPLACEHOLDER");
        o.pair(5, hex(h::PlotStyleNormal));
        o.pair(330, hex(h::PlotStyleDict));
        o.pair(0, "ENDSEC");
    }
};

} // namespace

Result<std::string> writeDxf(const Sketch& input, UnitSystem units)
{
    if (auto ok = validateSketch(input); !ok) {
        return tl::unexpected(ok.error());
    }
    const Sketch sk = canonicalSketch(input);
    const EntityIndex index(sk);
    std::set<SketchEntId> used;
    for (const Entity& e : sk.entities) {
        if (const auto* l = std::get_if<Line>(&e.geom)) {
            used.insert({l->a, l->b});
        } else if (const auto* a = std::get_if<Arc>(&e.geom)) {
            used.insert({a->center, a->start, a->end});
        } else if (const auto* c = std::get_if<Circle>(&e.geom)) {
            used.insert(c->center);
        } else if (const auto* sp = std::get_if<Spline>(&e.geom)) {
            used.insert(sp->controlPoints.begin(), sp->controlPoints.end());
        } else if (const auto* sl = std::get_if<Slot>(&e.geom)) {
            used.insert({sl->a, sl->b});
        }
    }
    const auto pt = [&](SketchEntId id) {
        return *pointOf(index, id);
    };
    Writer w(units == UnitSystem::Inch ? 1 / 25.4 : 1.0);
    for (const Entity& e : sk.entities) {
        const bool c = e.construction;
        if (const auto* p = std::get_if<Point>(&e.geom)) {
            if (!used.count(e.id)) {
                w.point(p->p, c);
            }
        } else if (const auto* l = std::get_if<Line>(&e.geom)) {
            w.line(pt(l->a), pt(l->b), c);
        } else if (const auto* a = std::get_if<Arc>(&e.geom)) {
            w.arc(pt(a->center), pt(a->start), pt(a->end), a->ccw, c);
        } else if (const auto* ci = std::get_if<Circle>(&e.geom)) {
            w.circle(pt(ci->center), ci->radius, c);
        } else if (const auto* sp = std::get_if<Spline>(&e.geom)) {
            std::vector<Vec2> poles;
            for (SketchEntId id : sp->controlPoints) {
                poles.push_back(pt(id));
            }
            w.spline(std::move(poles), sp->degree, sp->closed, c);
        } else if (const auto* sl = std::get_if<Slot>(&e.geom)) {
            // The slot's outline, as closedLoops builds it: side a->b, cap around b, side b->a, cap around a.
            const Vec2 A = pt(sl->a);
            const Vec2 B = pt(sl->b);
            const Vec2 n = (sl->width / 2 / dist(A, B)) * perpLeft(B - A);
            w.line(A - n, B - n, c);
            w.arc(B, B - n, B + n, true, c);
            w.line(B + n, A + n, c);
            w.arc(A, A + n, A - n, true, c);
        }
    }
    return w.document(units);
}

Status exportDxf(const std::string& path, const Sketch& sketch, UnitSystem units)
{
    auto text = writeDxf(sketch, units);
    if (!text) {
        return tl::unexpected(text.error());
    }
    const std::filesystem::path p(std::u8string(reinterpret_cast<const char8_t*>(path.data()), path.size()));
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) {
        return tl::unexpected(Error{ErrorCode::IoError, "The DXF file could not be created: " + path, {"DXF export"}});
    }
    out.write(text->data(), static_cast<std::streamsize>(text->size()));
    out.close();
    if (!out) {
        return tl::unexpected(Error{ErrorCode::IoError, "The DXF file could not be written: " + path, {"DXF export"}});
    }
    return {};
}

} // namespace ray::sketch
