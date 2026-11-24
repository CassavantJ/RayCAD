// DXF import (task T-091). See dxf.h for what is read and what is left out.
#include "../core/from_chars_double.h"
#include "dxf.h"

#include "loops_curves.h"
#include "model.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <numbers>
#include <optional>
#include <string>
#include <system_error>

namespace ray::sketch {

using namespace detail;

namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kGrid = 1e-6; // mm: cell size of the point-merging grid (larger than kDxfMergeTolerance)

Error formatError(std::string message)
{
    return Error{ErrorCode::FormatError, std::move(message), {"DXF import"}};
}

std::string upper(std::string_view s)
{
    std::string out(s);
    for (char& ch : out) {
        if (ch >= 'a' && ch <= 'z') {
            ch = static_cast<char>(ch - 'a' + 'A');
        }
    }
    return out;
}

std::string_view trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

std::optional<double> toNumber(std::string_view v)
{
    v = trim(v);
    if (!v.empty() && v.front() == '+') {
        v.remove_prefix(1);
    }
    double out = 0;
    const auto [ptr, ec] = compat::fromChars(v.data(), v.data() + v.size(), out);
    if (ec != std::errc() || ptr != v.data() + v.size() || !std::isfinite(out)) {
        return std::nullopt;
    }
    return out;
}

std::optional<long long> toInteger(std::string_view v)
{
    v = trim(v);
    if (!v.empty() && v.front() == '+') {
        v.remove_prefix(1);
    }
    long long out = 0;
    const auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), out);
    if (ec == std::errc() && ptr == v.data() + v.size()) {
        return out;
    }
    const auto d = toNumber(v);
    if (d && *d == std::floor(*d) && std::fabs(*d) < 1e15) {
        return static_cast<long long>(*d);
    }
    return std::nullopt;
}

// ---------- Group code pairs ----------

struct Pair {
    int code = 0;
    std::string_view value;
};

class Tokens {
public:
    explicit Tokens(std::string_view data) : d_(data)
    {
        if (d_.substr(0, 3) == "\xEF\xBB\xBF") {
            pos_ = 3;
        }
        sep_ = d_.find('\n') != std::string_view::npos ? '\n' : '\r';
    }

    // The next pair; false at the end of the input or on an error (then error() is set).
    bool next(Pair& out)
    {
        if (pending_) {
            pending_ = false;
            out = last_;
            return true;
        }
        if (error_) {
            return false;
        }
        std::string_view codeLine;
        if (!readLine(codeLine)) {
            return false;
        }
        codeLine = trim(codeLine);
        if (codeLine.empty() && pos_ >= d_.size()) {
            return false; // a blank last line
        }
        int code = 0;
        const auto [ptr, ec] = std::from_chars(codeLine.data(), codeLine.data() + codeLine.size(), code);
        if (ec != std::errc() || ptr != codeLine.data() + codeLine.size() || code < -5 || code > 1071) {
            error_ = formatError("Line " + std::to_string(line_) +
                                 " should hold a DXF group code (a number from -5 "
                                 "to 1071); the file is damaged or not a DXF file.");
            return false;
        }
        std::string_view value;
        if (!readLine(value)) {
            error_ = formatError("The DXF file ends after group code " + std::to_string(code) + " on line " +
                                 std::to_string(line_) + "; it is cut off.");
            return false;
        }
        last_ = {code, trim(value)};
        out = last_;
        return true;
    }
    // Puts the pair just read back; the next call to next() returns it again.
    void back() { pending_ = true; }
    const std::optional<Error>& error() const { return error_; }
    std::size_t line() const { return line_; }

private:
    bool readLine(std::string_view& out)
    {
        if (pos_ >= d_.size()) {
            return false;
        }
        std::size_t end = d_.find(sep_, pos_);
        if (end == std::string_view::npos) {
            end = d_.size();
        }
        out = d_.substr(pos_, end - pos_);
        pos_ = end + 1;
        ++line_;
        return true;
    }

    std::string_view d_;
    std::size_t pos_ = 0;
    std::size_t line_ = 0;
    char sep_ = '\n';
    Pair last_;
    bool pending_ = false;
    std::optional<Error> error_;
};

struct Record {
    std::string type; // upper case
    std::vector<Pair> codes;

    std::optional<std::string_view> str(int code) const
    {
        for (const Pair& p : codes) {
            if (p.code == code) {
                return p.value;
            }
        }
        return std::nullopt;
    }
    // A number; `fallback` when absent; nullopt (bad) when present but not a number.
    std::optional<double> num(int code, double fallback) const
    {
        const auto s = str(code);
        return s ? toNumber(*s) : std::optional<double>(fallback);
    }
    long long integer(int code, long long fallback) const
    {
        const auto s = str(code);
        if (!s) {
            return fallback;
        }
        return toInteger(*s).value_or(fallback);
    }
    std::string layer() const { return std::string(str(8).value_or("0")); }
};

// Reads the pairs of one record after its (0, type) pair, up to the next code 0.
void readRecordBody(Tokens& t, Record& r)
{
    Pair p;
    while (t.next(p)) {
        if (p.code == 0) {
            t.back();
            return;
        }
        r.codes.push_back(p);
    }
}

// A stream of entity records: from the ENTITIES section, or from a stored block.
class RecordSource {
public:
    virtual ~RecordSource() = default;
    virtual bool next(Record& out) = 0;
    virtual void back() = 0;
};

class SectionRecords : public RecordSource {
public:
    explicit SectionRecords(Tokens& t) : t_(t) {}
    bool next(Record& out) override
    {
        if (pending_) {
            pending_ = false;
            out = last_;
            return true;
        }
        if (ended_) {
            return false;
        }
        Pair p;
        while (t_.next(p)) {
            if (p.code != 0) {
                continue; // stray pair between records
            }
            last_ = Record{upper(p.value), {}};
            if (last_.type == "ENDSEC") {
                ended_ = true;
                return false;
            }
            if (last_.type == "EOF" || last_.type == "SECTION") {
                t_.back(); // the section lost its ENDSEC; let the caller see this pair
                ended_ = true;
                return false;
            }
            readRecordBody(t_, last_);
            out = last_;
            return true;
        }
        return false;
    }
    void back() override { pending_ = true; }
    bool ended() const { return ended_; }

private:
    Tokens& t_;
    Record last_;
    bool pending_ = false;
    bool ended_ = false;
};

class StoredRecords : public RecordSource {
public:
    explicit StoredRecords(const std::vector<Record>& records) : r_(records) {}
    bool next(Record& out) override
    {
        if (i_ >= r_.size()) {
            return false;
        }
        out = r_[i_++];
        return true;
    }
    void back() override
    {
        if (i_ > 0) {
            --i_;
        }
    }

private:
    const std::vector<Record>& r_;
    std::size_t i_ = 0;
};

// ---------- 2D affine maps ----------

struct Xf {
    double a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0; // x' = a x + b y + tx, y' = c x + d y + ty

    Vec2 apply(Vec2 p) const { return {a * p.x + b * p.y + tx, c * p.x + d * p.y + ty}; }
    Xf operator*(const Xf& o) const // this after o
    {
        return {a * o.a + b * o.c, a * o.b + b * o.d,        c * o.a + d * o.c,
                c * o.b + d * o.d, a * o.tx + b * o.ty + tx, c * o.tx + d * o.ty + ty};
    }
    double det() const { return a * d - b * c; }
    // The uniform scale of a similarity (rotation, mirror and uniform scale), or nullopt.
    std::optional<double> similarityScale() const
    {
        const double s1 = dmath::hypot(a, c);
        const double s2 = dmath::hypot(b, d);
        if (!(s1 > 0) || std::fabs(s1 - s2) > 1e-9 * s1 || std::fabs(a * b + c * d) > 1e-9 * s1 * s2) {
            return std::nullopt;
        }
        return s1;
    }
    static Xf translate(Vec2 t) { return {1, 0, 0, 1, t.x, t.y}; }
    static Xf scale(double sx, double sy) { return {sx, 0, 0, sy, 0, 0}; }
};

double cosDeg(double deg)
{
    const double r = std::fmod(std::fmod(deg, 360.0) + 360.0, 360.0);
    if (r == 0) {
        return 1;
    }
    if (r == 90 || r == 270) {
        return 0;
    }
    if (r == 180) {
        return -1;
    }
    return dmath::cos(r * kPi / 180);
}
double sinDeg(double deg)
{
    const double r = std::fmod(std::fmod(deg, 360.0) + 360.0, 360.0);
    if (r == 0 || r == 180) {
        return 0;
    }
    if (r == 90) {
        return 1;
    }
    if (r == 270) {
        return -1;
    }
    return dmath::sin(r * kPi / 180);
}
Xf rotateDeg(double deg)
{
    const double c = cosDeg(deg);
    const double s = sinDeg(deg);
    return {c, -s, s, c, 0, 0};
}

// The object coordinate system of a 2D entity with extrusion direction n: identity for +Z, a mirror in x for -Z
// (the arbitrary axis algorithm of the DXF reference gives Ax = (-1, 0, 0), Ay = (0, 1, 0) there), nullopt for an
// entity not parallel to the XY plane.
std::optional<Xf> ocs(const Record& r)
{
    const auto nx = r.num(210, 0);
    const auto ny = r.num(220, 0);
    const auto nz = r.num(230, 1);
    if (!nx || !ny || !nz) {
        return std::nullopt;
    }
    const double len = dmath::sqrt(*nx * *nx + *ny * *ny + *nz * *nz);
    if (!(len > 0) || std::fabs(*nx) > 1e-9 * len || std::fabs(*ny) > 1e-9 * len) {
        return std::nullopt;
    }
    return *nz > 0 ? Xf{} : Xf::scale(-1, 1);
}

// ---------- The importer ----------

struct Block {
    Vec2 base;
    long long flags = 0;
    std::vector<Record> records;
};

struct LayerInfo {
    bool off = false;
    bool frozen = false;
};

class Importer {
public:
    Importer(std::string_view bytes, UnitSystem assume, const DxfLimits& limits) : assume_(assume), lim_(limits)
    {
        Hasher h;
        h.str("raycad.sketch.dxf.import");
        h.bytes(std::as_bytes(std::span(bytes.data(), bytes.size())));
        seed_ = h.finish();
    }

    Result<DxfImport> run(std::string_view bytes);

private:
    UnitSystem assume_;
    const DxfLimits& lim_;
    DxfImport out_;
    Hash256 seed_;
    std::uint64_t serial_ = 0;
    std::map<std::string, Block> blocks_;
    std::map<std::string, LayerInfo> layers_;
    std::map<std::string, std::size_t> imported_;
    std::map<std::string, std::size_t> ignored_;
    std::map<std::string, std::size_t> warnings_;
    bool unitsFixed_ = false;
    Xf root_;
    std::size_t expansion_ = 0;
    bool expansionWarned_ = false;
    std::optional<Error> fatal_;
    std::map<std::pair<long long, long long>, std::vector<std::size_t>> grid_;
    std::vector<std::pair<Vec2, SketchEntId>> points_;

    void warn(const std::string& message) { ++warnings_[message]; }
    void ignore(const std::string& type) { ++ignored_[type]; }

    // ----- units -----
    void fixUnits()
    {
        if (unitsFixed_) {
            return;
        }
        unitsFixed_ = true;
        static const std::map<int, std::pair<double, const char*>> kUnits = {
            {1, {25.4, "inch"}},
            {2, {304.8, "foot"}},
            {3, {1609344.0, "mile"}},
            {4, {1.0, "millimetre"}},
            {5, {10.0, "centimetre"}},
            {6, {1000.0, "metre"}},
            {7, {1e6, "kilometre"}},
            {8, {25.4e-6, "microinch"}},
            {9, {0.0254, "mil"}},
            {10, {914.4, "yard"}},
            {11, {1e-7, "angstrom"}},
            {12, {1e-6, "nanometre"}},
            {13, {1e-3, "micron"}},
            {14, {100.0, "decimetre"}},
            {15, {1e4, "decametre"}},
            {16, {1e5, "hectometre"}},
            {17, {1e12, "gigametre"}},
            {18, {1.495978707e14, "astronomical unit"}},
            {19, {9.4607304725808e18, "light year"}},
            {20, {3.0856775814913673e19, "parsec"}},
            {21, {1200.0 / 3937.0 * 1000.0, "US survey foot"}},
            {22, {1200.0 / 3937.0 * 1000.0 / 12.0, "US survey inch"}},
            {23, {1200.0 / 3937.0 * 1000.0 * 3.0, "US survey yard"}},
            {24, {1200.0 / 3937.0 * 1000.0 * 5280.0, "US survey mile"}}};
        const auto it = kUnits.find(out_.insunits);
        if (it != kUnits.end()) {
            out_.mmPerUnit = it->second.first;
            out_.unitSource = "$INSUNITS";
        } else {
            if (out_.insunits > 0) {
                warn("$INSUNITS " + std::to_string(out_.insunits) + " is not a known length unit; it was ignored");
            }
            if (out_.measurement == 0 || out_.measurement == 1) {
                out_.mmPerUnit = out_.measurement == 0 ? 25.4 : 1.0;
                out_.unitSource = "$MEASUREMENT";
            } else {
                out_.mmPerUnit = assume_ == UnitSystem::Inch ? 25.4 : 1.0;
                out_.unitSource = "assumed";
            }
        }
        root_ = Xf::scale(out_.mmPerUnit, out_.mmPerUnit);
    }

    // ----- sketch output -----
    SketchEntId newId()
    {
        Hasher h;
        h.bytes(std::as_bytes(std::span(seed_.bytes)));
        h.i64(static_cast<std::int64_t>(serial_++));
        const Hash256 hash = h.finish();
        Uuid u;
        std::copy_n(hash.bytes.begin(), 16, u.bytes.begin());
        u.bytes[6] = static_cast<std::uint8_t>((u.bytes[6] & 0x0F) | 0x80);
        u.bytes[8] = static_cast<std::uint8_t>((u.bytes[8] & 0x3F) | 0x80);
        return SketchEntId{u};
    }
    bool room()
    {
        if (out_.sketch.entities.size() >= lim_.maxEntities) {
            if (!fatal_) {
                fatal_ = formatError("The DXF file has more than " + std::to_string(lim_.maxEntities) +
                                     " entities, the most a sketch import takes.");
            }
            return false;
        }
        return true;
    }
    bool inRange(Vec2 p) const { return std::fabs(p.x) <= lim_.maxCoordinate && std::fabs(p.y) <= lim_.maxCoordinate; }
    std::pair<long long, long long> cell(Vec2 p) const
    {
        return {static_cast<long long>(std::floor(p.x / kGrid)), static_cast<long long>(std::floor(p.y / kGrid))};
    }
    SketchEntId pointAt(Vec2 p, bool construction, const std::string& layer)
    {
        const auto [cx, cy] = cell(p);
        for (long long dx = -1; dx <= 1; ++dx) {
            for (long long dy = -1; dy <= 1; ++dy) {
                const auto it = grid_.find({cx + dx, cy + dy});
                if (it == grid_.end()) {
                    continue;
                }
                for (const std::size_t i : it->second) {
                    if (dist(points_[i].first, p) <= kDxfMergeTolerance) {
                        return points_[i].second;
                    }
                }
            }
        }
        const SketchEntId id = newPoint(p, construction, layer);
        grid_[{cx, cy}].push_back(points_.size());
        points_.emplace_back(p, id);
        return id;
    }
    SketchEntId newPoint(Vec2 p, bool construction, const std::string& layer)
    {
        const SketchEntId id = newId();
        out_.sketch.entities.push_back(Entity{id, Point{p}, construction});
        out_.layers.emplace_back(id, layer);
        return id;
    }
    void addEntity(Entity e, const std::string& layer)
    {
        out_.layers.emplace_back(e.id, layer);
        out_.sketch.entities.push_back(std::move(e));
    }
    static bool isConstruction(const std::string& layer) { return upper(layer) == kDxfConstructionLayer; }

    // Each returns false when the geometry could not be added (a warning says why).
    bool line(Vec2 a, Vec2 b, const std::string& layer)
    {
        if (!inRange(a) || !inRange(b)) {
            warn("entities beyond " + std::to_string(static_cast<long long>(lim_.maxCoordinate)) +
                 " mm from the origin were left out");
            return false;
        }
        if (dist(a, b) <= kDxfMergeTolerance) {
            warn("lines and polyline segments of zero length were left out");
            return false;
        }
        if (!room()) {
            return false;
        }
        const bool c = isConstruction(layer);
        const SketchEntId pa = pointAt(a, c, layer);
        const SketchEntId pb = pointAt(b, c, layer);
        if (pa == pb) {
            warn("lines and polyline segments of zero length were left out");
            return false;
        }
        addEntity(Entity{newId(), Line{pa, pb}, c}, layer);
        return true;
    }
    bool arc(Vec2 centre, Vec2 start, Vec2 end, bool ccw, const std::string& layer)
    {
        if (!inRange(centre) || !inRange(start) || !inRange(end)) {
            warn("entities beyond " + std::to_string(static_cast<long long>(lim_.maxCoordinate)) +
                 " mm from the origin were left out");
            return false;
        }
        if (!(dist(centre, start) > kDxfMergeTolerance) || dist(start, end) <= kDxfMergeTolerance) {
            warn("arcs of zero radius or zero length were left out");
            return false;
        }
        if (!room()) {
            return false;
        }
        const bool c = isConstruction(layer);
        const SketchEntId pc = pointAt(centre, c, layer);
        const SketchEntId ps = pointAt(start, c, layer);
        const SketchEntId pe = pointAt(end, c, layer);
        if (pc == ps || pc == pe || ps == pe) {
            warn("arcs of zero radius or zero length were left out");
            return false;
        }
        addEntity(Entity{newId(), Arc{pc, ps, pe, ccw}, c}, layer);
        return true;
    }
    bool circle(Vec2 centre, double r, const std::string& layer)
    {
        if (!inRange(centre) || !(r < lim_.maxCoordinate)) {
            warn("entities beyond " + std::to_string(static_cast<long long>(lim_.maxCoordinate)) +
                 " mm from the origin were left out");
            return false;
        }
        if (!(r > kDxfMergeTolerance)) {
            warn("circles with a radius of zero or less were left out");
            return false;
        }
        if (!room()) {
            return false;
        }
        const bool c = isConstruction(layer);
        addEntity(Entity{newId(), Circle{pointAt(centre, c, layer), r}, c}, layer);
        return true;
    }
    bool spline(const std::vector<Vec2>& poles, int degree, bool closed, const std::string& layer)
    {
        for (const Vec2& p : poles) {
            if (!inRange(p)) {
                warn("entities beyond " + std::to_string(static_cast<long long>(lim_.maxCoordinate)) +
                     " mm from the origin were left out");
                return false;
            }
        }
        if (!room()) {
            return false;
        }
        const bool c = isConstruction(layer);
        Spline s;
        s.degree = degree;
        s.closed = closed;
        for (const Vec2& p : poles) {
            s.controlPoints.push_back(pointAt(p, c, layer));
        }
        addEntity(Entity{newId(), std::move(s), c}, layer);
        return true;
    }

    // ----- sections -----
    Status header(Tokens& t);
    // Each returns whether the section ended with its ENDSEC.
    bool tables(Tokens& t);
    bool blocks(Tokens& t);
    void entities(RecordSource& src, const Xf& xf, int depth, const std::string& insertLayer);

    // ----- entities -----
    void entity(const Record& r, RecordSource& src, const Xf& xf, int depth, const std::string& insertLayer);
    void lwpolyline(const Record& r, const Xf& xf, const std::string& layer);
    void polyline(const Record& r, RecordSource& src, const Xf& xf, const std::string& layer);
    void bulgeSegment(Vec2 a, Vec2 b, double bulge, const Xf& xf, const std::string& layer, bool& ok);
    void splineEntity(const Record& r, const Xf& xf, const std::string& layer, bool& ok);
    void ellipse(const Record& r, const Xf& xf, const std::string& layer, bool& ok);
    void insert(const Record& r, RecordSource& src, const Xf& xf, int depth, const std::string& layer);
    void arcEntity(const Record& r, const Xf& xf, const std::string& layer, bool full, bool& ok);
    void counted(const std::string& type, bool ok)
    {
        if (ok) {
            ++imported_[type];
        }
    }
};

Status Importer::header(Tokens& t)
{
    std::string var;
    Pair p;
    while (t.next(p)) {
        if (p.code == 0) {
            if (upper(p.value) != "ENDSEC") {
                t.back(); // the header lost its ENDSEC
            }
            return {};
        }
        if (p.code == 9) {
            var = upper(p.value);
        } else if (var == "$ACADVER" && p.code == 1) {
            out_.version = std::string(p.value);
        } else if (var == "$INSUNITS" && p.code == 70) {
            out_.insunits = static_cast<int>(toInteger(p.value).value_or(-1));
        } else if (var == "$MEASUREMENT" && p.code == 70) {
            out_.measurement = static_cast<int>(toInteger(p.value).value_or(-1));
        }
    }
    if (t.error()) {
        return tl::unexpected(*t.error());
    }
    return tl::unexpected(formatError("The DXF file ends inside its HEADER section; it is cut off."));
}

bool Importer::tables(Tokens& t)
{
    SectionRecords src(t);
    Record r;
    while (src.next(r)) {
        if (r.type == "LAYER") {
            const std::string name = upper(r.str(2).value_or(""));
            LayerInfo info;
            info.frozen = (r.integer(70, 0) & 1) != 0;
            info.off = r.integer(62, 7) < 0;
            layers_[name] = info;
        }
    }
    return src.ended();
}

bool Importer::blocks(Tokens& t)
{
    SectionRecords src(t);
    Record r;
    while (src.next(r)) {
        if (r.type != "BLOCK") {
            continue;
        }
        Block b;
        b.base = {r.num(10, 0).value_or(0), r.num(20, 0).value_or(0)};
        b.flags = r.integer(70, 0);
        const std::string name = upper(r.str(2).value_or(r.str(3).value_or("")));
        Record e;
        while (src.next(e)) {
            if (e.type == "ENDBLK") {
                break;
            }
            if (e.type == "BLOCK") {
                src.back(); // ENDBLK missing
                break;
            }
            b.records.push_back(std::move(e));
        }
        if (!name.empty()) {
            blocks_[name] = std::move(b);
        }
    }
    return src.ended();
}

void Importer::entities(RecordSource& src, const Xf& xf, int depth, const std::string& insertLayer)
{
    Record r;
    while (!fatal_ && src.next(r)) {
        if (depth > 0) {
            if (++expansion_ > lim_.maxBlockExpansion) {
                if (!expansionWarned_) {
                    expansionWarned_ = true;
                    warn("block references expand to more than " + std::to_string(lim_.maxBlockExpansion) +
                         " entities; the rest were left out");
                }
                return;
            }
        }
        entity(r, src, xf, depth, insertLayer);
    }
}

void Importer::entity(const Record& r, RecordSource& src, const Xf& xf, int depth, const std::string& insertLayer)
{
    std::string layer = r.layer();
    if (depth > 0 && layer == "0") {
        layer = insertLayer; // entities on layer 0 in a block take the layer of the reference
    }
    if (r.integer(67, 0) == 1) {
        ignore(r.type);
        warn("entities in paper space were left out");
        if (r.type == "POLYLINE" || r.type == "INSERT") {
            Record v; // skip what follows the entity
            while (src.next(v)) {
                if (v.type == "SEQEND") {
                    break;
                }
                if (v.type != "VERTEX" && v.type != "ATTRIB") {
                    src.back();
                    break;
                }
            }
        }
        return;
    }
    const auto upperLayer = upper(layer);
    if (const auto it = layers_.find(upperLayer); it != layers_.end() && (it->second.off || it->second.frozen)) {
        warn("entities on layers that are off or frozen were imported");
    }
    bool ok = true;
    if (r.type == "LINE") {
        const auto x0 = r.num(10, 0), y0 = r.num(20, 0), z0 = r.num(30, 0);
        const auto x1 = r.num(11, 0), y1 = r.num(21, 0), z1 = r.num(31, 0);
        if (!x0 || !y0 || !x1 || !y1 || !z0 || !z1) {
            warn("damaged LINE entities were left out");
            return;
        }
        if (*z0 != 0 || *z1 != 0) {
            warn("Z coordinates were dropped (the geometry was projected onto the XY plane)");
        }
        counted("LINE", line(xf.apply({*x0, *y0}), xf.apply({*x1, *y1}), layer));
    } else if (r.type == "POINT") {
        const auto x = r.num(10, 0), y = r.num(20, 0);
        if (!x || !y) {
            warn("damaged POINT entities were left out");
            return;
        }
        const Vec2 p = xf.apply({*x, *y});
        if (!inRange(p) || !room()) {
            return;
        }
        newPoint(p, isConstruction(layer), layer);
        counted("POINT", true);
    } else if (r.type == "CIRCLE" || r.type == "ARC") {
        arcEntity(r, xf, layer, r.type == "CIRCLE", ok);
        counted(r.type, ok);
    } else if (r.type == "LWPOLYLINE") {
        lwpolyline(r, xf, layer);
    } else if (r.type == "POLYLINE") {
        polyline(r, src, xf, layer);
    } else if (r.type == "SPLINE") {
        splineEntity(r, xf, layer, ok);
        counted("SPLINE", ok);
    } else if (r.type == "ELLIPSE") {
        ellipse(r, xf, layer, ok);
        counted("ELLIPSE", ok);
    } else if (r.type == "INSERT") {
        insert(r, src, xf, depth, layer);
    } else if (r.type == "VERTEX" || r.type == "SEQEND" || r.type == "ATTRIB") {
        // Belongs to a POLYLINE or INSERT that was left out.
    } else {
        ignore(r.type);
    }
}

void Importer::arcEntity(const Record& r, const Xf& xf, const std::string& layer, bool full, bool& ok)
{
    ok = false;
    const auto cx = r.num(10, 0), cy = r.num(20, 0), rad = r.num(40, 0);
    const auto a0 = r.num(50, 0), a1 = r.num(51, 360);
    if (!cx || !cy || !rad || !a0 || !a1) {
        warn("damaged " + r.type + " entities were left out");
        return;
    }
    const auto frame = ocs(r);
    if (!frame) {
        warn("entities not parallel to the XY plane were left out");
        return;
    }
    const Xf m = xf * *frame;
    const auto s = m.similarityScale();
    if (!s) {
        warn("arcs and circles in blocks inserted with unequal X and Y scales were left out (they would be "
             "ellipses)");
        return;
    }
    const Vec2 c{*cx, *cy};
    double sweep = std::fmod(*a1 - *a0, 360.0);
    if (sweep < 0) {
        sweep += 360;
    }
    if (full || sweep == 0) {
        if (!full) {
            warn("ARC entities whose start and end angles are equal were read as full circles");
        }
        ok = circle(m.apply(c), *rad * *s, layer);
        return;
    }
    const Vec2 start = c + *rad * Vec2{cosDeg(*a0), sinDeg(*a0)};
    const Vec2 end = c + *rad * Vec2{cosDeg(*a1), sinDeg(*a1)};
    if (!(*rad > 0)) {
        warn("arcs of zero radius or zero length were left out");
        return;
    }
    ok = arc(m.apply(c), m.apply(start), m.apply(end), m.det() > 0, layer);
}

void Importer::bulgeSegment(Vec2 a, Vec2 b, double bulge, const Xf& xf, const std::string& layer, bool& ok)
{
    if (dist(a, b) == 0) {
        return; // a repeated vertex
    }
    if (bulge == 0) {
        ok = line(xf.apply(a), xf.apply(b), layer) && ok;
        return;
    }
    if (!xf.similarityScale()) {
        warn("polyline arcs in blocks inserted with unequal X and Y scales were left out (they would be ellipses)");
        ok = false;
        return;
    }
    // Centre of the arc from a to b with bulge = tan(sweep / 4), sweep positive counter-clockwise.
    const Vec2 d = b - a;
    const Vec2 centre = lerp(a, b, 0.5) + ((1 - bulge * bulge) / (4 * bulge)) * perpLeft(d);
    ok = arc(xf.apply(centre), xf.apply(a), xf.apply(b), (bulge > 0) == (xf.det() > 0), layer) && ok;
}

void Importer::lwpolyline(const Record& r, const Xf& xf, const std::string& layer)
{
    struct V {
        Vec2 p;
        double bulge = 0;
    };
    std::vector<V> vs;
    bool bad = false;
    for (const Pair& p : r.codes) {
        if (p.code == 10) {
            const auto x = toNumber(p.value);
            bad = bad || !x;
            vs.push_back({{x.value_or(0), 0}, 0});
        } else if (p.code == 20 && !vs.empty()) {
            const auto y = toNumber(p.value);
            bad = bad || !y;
            vs.back().p.y = y.value_or(0);
        } else if (p.code == 42 && !vs.empty()) {
            const auto b = toNumber(p.value);
            bad = bad || !b;
            vs.back().bulge = b.value_or(0);
        }
    }
    const auto frame = ocs(r);
    if (bad) {
        warn("damaged LWPOLYLINE entities were left out");
        return;
    }
    if (!frame) {
        warn("entities not parallel to the XY plane were left out");
        return;
    }
    const bool closed = (r.integer(70, 0) & 1) != 0;
    const Xf m = xf * *frame;
    bool ok = true;
    for (std::size_t i = 0; i + 1 < vs.size(); ++i) {
        bulgeSegment(vs[i].p, vs[i + 1].p, vs[i].bulge, m, layer, ok);
    }
    if (closed && vs.size() > 1) {
        bulgeSegment(vs.back().p, vs.front().p, vs.back().bulge, m, layer, ok);
    }
    counted("LWPOLYLINE", ok && vs.size() > 1);
}

void Importer::polyline(const Record& r, RecordSource& src, const Xf& xf, const std::string& layer)
{
    struct V {
        Vec2 p;
        double z = 0;
        double bulge = 0;
    };
    std::vector<V> vs;
    bool bad = false;
    Record v;
    while (src.next(v)) {
        if (v.type == "SEQEND") {
            break;
        }
        if (v.type != "VERTEX") {
            src.back(); // SEQEND missing
            break;
        }
        const long long flags = v.integer(70, 0);
        if (flags & 16) {
            continue; // spline frame control point: not on the curve
        }
        const auto x = v.num(10, 0), y = v.num(20, 0), z = v.num(30, 0), b = v.num(42, 0);
        bad = bad || !x || !y || !z || !b;
        vs.push_back({{x.value_or(0), y.value_or(0)}, z.value_or(0), b.value_or(0)});
    }
    const long long flags = r.integer(70, 0);
    if (flags & (16 | 64)) {
        ignore("POLYLINE (mesh)");
        warn("polygon and polyface meshes were left out");
        return;
    }
    if (bad) {
        warn("damaged POLYLINE entities were left out");
        return;
    }
    const bool closed = (flags & 1) != 0;
    bool ok = true;
    Xf m = xf;
    if (flags & 8) { // 3D polyline: WCS, straight segments
        for (const V& p : vs) {
            if (p.z != 0) {
                warn("Z coordinates were dropped (the geometry was projected onto the XY plane)");
                break;
            }
        }
        for (V& p : vs) {
            p.bulge = 0;
        }
    } else {
        const auto frame = ocs(r);
        if (!frame) {
            warn("entities not parallel to the XY plane were left out");
            return;
        }
        m = xf * *frame;
    }
    for (std::size_t i = 0; i + 1 < vs.size(); ++i) {
        bulgeSegment(vs[i].p, vs[i + 1].p, vs[i].bulge, m, layer, ok);
    }
    if (closed && vs.size() > 1) {
        bulgeSegment(vs.back().p, vs.front().p, vs.back().bulge, m, layer, ok);
    }
    counted("POLYLINE", ok && vs.size() > 1);
}

// A rational quadratic Bezier segment that is a circular arc: its centre, or nullopt.
std::optional<Vec2> circularArcCentre(const std::vector<HPoint>& seg)
{
    if (seg.size() != 3) {
        return std::nullopt;
    }
    const Vec2 p0{seg[0].x / seg[0].w, seg[0].y / seg[0].w};
    const Vec2 p1{seg[1].x / seg[1].w, seg[1].y / seg[1].w};
    const Vec2 p2{seg[2].x / seg[2].w, seg[2].y / seg[2].w};
    const double w = seg[1].w / dmath::sqrt(seg[0].w * seg[2].w);
    const double l0 = dist(p0, p1);
    const double l1 = dist(p1, p2);
    const double chord = dist(p0, p2);
    if (!(l0 > 0) || !(chord > 0) || std::fabs(l0 - l1) > 1e-9 * l0 || std::fabs(w - chord / (2 * l0)) > 1e-9) {
        return std::nullopt;
    }
    const Vec2 m = lerp(p0, p2, 0.5);
    const double h = dist(p1, m);
    if (!(h > 1e-12 * chord)) {
        return std::nullopt;
    }
    const double a = chord / 2;
    return m + (a * a / (h * h)) * (m - p1);
}

void Importer::splineEntity(const Record& r, const Xf& xf, const std::string& layer, bool& ok)
{
    ok = false;
    std::vector<double> knots;
    std::vector<double> weights;
    std::vector<Vec2> poles;
    std::size_t fitPoints = 0;
    bool bad = false;
    bool zDropped = false;
    for (const Pair& p : r.codes) {
        if (p.code == 40 || p.code == 41 || p.code == 10 || p.code == 20 || p.code == 30) {
            const auto v = toNumber(p.value);
            bad = bad || !v;
            const double x = v.value_or(0);
            if (p.code == 40) {
                knots.push_back(x);
            } else if (p.code == 41) {
                weights.push_back(x);
            } else if (p.code == 10) {
                if (poles.size() >= lim_.maxSplinePoles) {
                    warn("SPLINE entities with more than " + std::to_string(lim_.maxSplinePoles) +
                         " control points were left out");
                    return;
                }
                poles.push_back({x, 0});
            } else if (p.code == 20 && !poles.empty()) {
                poles.back().y = x;
            } else if (p.code == 30 && x != 0) {
                zDropped = true;
            }
        } else if (p.code == 11) {
            ++fitPoints;
        }
    }
    const long long flags = r.integer(70, 0);
    const long long degree = r.integer(71, 3);
    if (bad) {
        warn("damaged SPLINE entities were left out");
        return;
    }
    if (poles.empty()) {
        warn(fitPoints > 0 ? "SPLINE entities given only by fit points were left out (their curve is not defined "
                             "by the DXF reference)"
                           : "damaged SPLINE entities were left out");
        return;
    }
    if (degree < 1 || degree > 10) {
        warn("SPLINE entities of degree " + std::to_string(degree) + " were left out (degrees 1 to 10 are read)");
        return;
    }
    const int p = static_cast<int>(degree);
    const std::size_t n = poles.size();
    if (n < static_cast<std::size_t>(p) + 1 || knots.size() != n + static_cast<std::size_t>(p) + 1) {
        warn("SPLINE entities whose knots do not match their control points were left out");
        return;
    }
    if (zDropped) {
        warn("Z coordinates were dropped (the geometry was projected onto the XY plane)");
    }
    for (Vec2& q : poles) {
        q = xf.apply(q);
    }
    bool rational = false;
    if (!weights.empty()) {
        if (weights.size() != n) {
            warn("SPLINE entities whose weights do not match their control points were left out");
            return;
        }
        for (const double w : weights) {
            if (!(w > 0)) {
                warn("SPLINE entities with weights of zero or less were left out");
                return;
            }
            rational = rational || std::fabs(w - weights.front()) > 1e-12 * weights.front();
        }
    }
    if (p == 1) {
        // A degree-1 B-spline is the polyline through its control points, whatever its knots or weights.
        bool any = false;
        for (std::size_t i = 0; i + 1 < n; ++i) {
            if (dist(poles[i], poles[i + 1]) > kDxfMergeTolerance) {
                any = line(poles[i], poles[i + 1], layer) || any;
            }
        }
        ok = any;
        return;
    }
    const double lo = knots.front();
    const double hi = knots.back();
    const double range = hi - lo;
    if (!(range > 0)) {
        warn("SPLINE entities whose knots do not match their control points were left out");
        return;
    }
    const auto near = [&](double a, double b) {
        return std::fabs(a - b) <= 1e-9 * range;
    };
    if (!rational) {
        // Clamped uniform: one sketch spline as it is.
        const double a = knots[static_cast<std::size_t>(p)];
        const double b = knots[n];
        bool clampedUniform = b > a;
        for (std::size_t i = 0; clampedUniform && i < knots.size(); ++i) {
            double expect = 0;
            if (i <= static_cast<std::size_t>(p)) {
                expect = a;
            } else if (i >= n) {
                expect = b;
            } else {
                expect = a + (b - a) * static_cast<double>(i - static_cast<std::size_t>(p)) /
                                 static_cast<double>(n - static_cast<std::size_t>(p));
            }
            clampedUniform = near(knots[i], expect);
        }
        if (clampedUniform) {
            ok = spline(poles, p, false, layer);
            return;
        }
        // Periodic uniform with the first `p` control points repeated at the end: one closed sketch spline.
        if ((flags & 2) && n >= 2 * static_cast<std::size_t>(p) + 1) {
            bool periodic = true;
            const double step = range / static_cast<double>(knots.size() - 1);
            for (std::size_t i = 0; periodic && i < knots.size(); ++i) {
                periodic = near(knots[i], lo + step * static_cast<double>(i));
            }
            double size = 0;
            for (const Vec2& q : poles) {
                size = std::max({size, std::fabs(q.x), std::fabs(q.y)});
            }
            for (std::size_t i = 0; periodic && i < static_cast<std::size_t>(p); ++i) {
                periodic = dist(poles[i], poles[n - static_cast<std::size_t>(p) + i]) <= 1e-12 * std::max(size, 1.0);
            }
            if (periodic) {
                ok = spline(std::vector<Vec2>(poles.begin(), poles.end() - p), p, true, layer);
                return;
            }
        }
    }
    std::vector<HPoint> hp;
    for (std::size_t i = 0; i < n; ++i) {
        const double w = rational ? weights[i] : 1.0;
        hp.push_back({poles[i].x * w, poles[i].y * w, w});
    }
    const auto segments = bezierDecompose(std::move(hp), knots, p);
    if (!segments || segments->empty()) {
        warn("SPLINE entities whose knots do not match their control points were left out");
        return;
    }
    if (rational) {
        // Only circular arcs (rational quadratic spans) have an exact form in the sketch.
        std::vector<std::pair<Vec2, std::vector<Vec2>>> arcs;
        for (const auto& seg : *segments) {
            const auto centre = p == 2 ? circularArcCentre(seg) : std::nullopt;
            if (!centre) {
                warn("rational SPLINE entities that are not made of circular arcs were left out (the sketch has no "
                     "exact form for them)");
                return;
            }
            arcs.push_back({*centre,
                            {{seg[0].x / seg[0].w, seg[0].y / seg[0].w},
                             {seg[1].x / seg[1].w, seg[1].y / seg[1].w},
                             {seg[2].x / seg[2].w, seg[2].y / seg[2].w}}});
        }
        bool all = true;
        for (const auto& [centre, pts] : arcs) {
            const bool ccw = cross(pts[1] - pts[0], pts[2] - pts[1]) > 0;
            all = arc(centre, pts[0], pts[2], ccw, layer) && all;
        }
        ok = all;
        return;
    }
    bool all = true;
    for (const auto& seg : *segments) {
        std::vector<Vec2> pts;
        for (const HPoint& q : seg) {
            pts.push_back({q.x, q.y});
        }
        all = spline(pts, p, false, layer) && all;
    }
    ok = all;
}

void Importer::ellipse(const Record& r, const Xf& xf, const std::string& layer, bool& ok)
{
    ok = false;
    const auto cx = r.num(10, 0), cy = r.num(20, 0), mx = r.num(11, 1), my = r.num(21, 0);
    const auto ratio = r.num(40, 1), t0 = r.num(41, 0), t1 = r.num(42, 2 * kPi), nz = r.num(230, 1);
    const auto nx = r.num(210, 0), ny = r.num(220, 0);
    if (!cx || !cy || !mx || !my || !ratio || !t0 || !t1 || !nz || !nx || !ny) {
        warn("damaged ELLIPSE entities were left out");
        return;
    }
    if (std::fabs(*nx) > 1e-9 || std::fabs(*ny) > 1e-9 || *nz == 0) {
        warn("entities not parallel to the XY plane were left out");
        return;
    }
    if (std::fabs(*ratio - 1) > 1e-9) {
        warn("ELLIPSE entities that are not circles were left out (the sketch has no ellipse)");
        ignore("ELLIPSE");
        return;
    }
    const auto s = xf.similarityScale();
    if (!s) {
        warn("arcs and circles in blocks inserted with unequal X and Y scales were left out (they would be "
             "ellipses)");
        return;
    }
    const Vec2 c{*cx, *cy};
    const Vec2 major{*mx, *my};
    const double rad = norm(major);
    if (!(rad > 0)) {
        warn("damaged ELLIPSE entities were left out");
        return;
    }
    // Minor axis = extrusion x major * ratio: counter-clockwise for +Z, clockwise for -Z.
    const Vec2 minor = *nz > 0 ? perpLeft(major) : -1.0 * perpLeft(major);
    double sweep = *t1 - *t0;
    if (sweep >= 2 * kPi - 1e-12 || sweep == 0) {
        ok = circle(xf.apply(c), rad * *s, layer);
        return;
    }
    const auto at = [&](double t) {
        return c + dmath::cos(t) * major + dmath::sin(t) * minor;
    };
    ok = arc(xf.apply(c), xf.apply(at(*t0)), xf.apply(at(*t1)), (*nz > 0) == (xf.det() > 0), layer);
}

void Importer::insert(const Record& r, RecordSource& src, const Xf& xf, int depth, const std::string& layer)
{
    // Attributes follow an INSERT with 66 = 1; they are text.
    if (r.integer(66, 0) == 1) {
        Record v;
        while (src.next(v)) {
            if (v.type == "SEQEND") {
                break;
            }
            if (v.type != "ATTRIB") {
                src.back();
                break;
            }
        }
    }
    const std::string name = upper(r.str(2).value_or(""));
    const auto it = blocks_.find(name);
    if (it == blocks_.end()) {
        warn("references to missing blocks were left out");
        ignore("INSERT");
        return;
    }
    const Block& b = it->second;
    if (b.flags & (4 | 32)) {
        warn("external references (xrefs) were left out");
        ignore("INSERT");
        return;
    }
    if (depth + 1 > lim_.maxBlockDepth) {
        warn("block references nested deeper than " + std::to_string(lim_.maxBlockDepth) + " levels were left out");
        ignore("INSERT");
        return;
    }
    const auto x = r.num(10, 0), y = r.num(20, 0), sx = r.num(41, 1), sy = r.num(42, 1), rot = r.num(50, 0);
    const auto cs = r.num(44, 0), rs = r.num(45, 0);
    const auto frame = ocs(r);
    if (!x || !y || !sx || !sy || !rot || !cs || !rs) {
        warn("damaged INSERT entities were left out");
        return;
    }
    if (!frame) {
        warn("entities not parallel to the XY plane were left out");
        return;
    }
    if (*sx == 0 || *sy == 0) {
        warn("block references with a scale of zero were left out");
        return;
    }
    const long long cols = std::max(1LL, r.integer(70, 1));
    const long long rows = std::max(1LL, r.integer(71, 1));
    for (long long row = 0; row < rows && !fatal_ && !expansionWarned_; ++row) {
        for (long long col = 0; col < cols && !fatal_ && !expansionWarned_; ++col) {
            // Every copy counts against the expansion budget, so a huge array of an empty block ends too.
            if (++expansion_ > lim_.maxBlockExpansion) {
                expansionWarned_ = true;
                warn("block references expand to more than " + std::to_string(lim_.maxBlockExpansion) +
                     " entities; the rest were left out");
                return;
            }
            const Xf m = xf * *frame * Xf::translate({*x, *y}) * rotateDeg(*rot) *
                         Xf::translate({static_cast<double>(col) * *cs, static_cast<double>(row) * *rs}) *
                         Xf::scale(*sx, *sy) * Xf::translate({-b.base.x, -b.base.y});
            StoredRecords inner(b.records);
            entities(inner, m, depth + 1, layer);
            ++imported_["INSERT"];
        }
    }
}

Result<DxfImport> Importer::run(std::string_view bytes)
{
    if (bytes.size() > lim_.maxFileBytes) {
        return tl::unexpected(formatError("The DXF file is larger than " + std::to_string(lim_.maxFileBytes >> 20) +
                                          " MiB, the most a sketch import reads."));
    }
    if (bytes.substr(0, 18) == "AutoCAD Binary DXF") {
        return tl::unexpected(formatError("Binary DXF files cannot be read; save the drawing as ASCII DXF."));
    }
    Tokens t(bytes);
    bool sawSection = false;
    bool sawEof = false;
    bool strayWarned = false;
    Pair p;
    while (!fatal_ && t.next(p)) {
        if (p.code == 999) {
            continue;
        }
        if (p.code == 0 && upper(p.value) == "EOF") {
            sawEof = true;
            break;
        }
        if (p.code != 0 || upper(p.value) != "SECTION") {
            if (!strayWarned) {
                strayWarned = true;
                warn("data outside any section was skipped");
            }
            continue;
        }
        Pair name;
        if (!t.next(name)) {
            break;
        }
        if (name.code != 2) {
            return tl::unexpected(formatError("Line " + std::to_string(t.line()) +
                                              ": a DXF SECTION must be followed by its name (group code 2)."));
        }
        sawSection = true;
        const std::string section = upper(name.value);
        if (section == "HEADER") {
            if (auto ok = header(t); !ok) {
                return tl::unexpected(ok.error());
            }
            continue;
        }
        bool ended = true;
        if (section == "TABLES") {
            ended = tables(t);
        } else if (section == "BLOCKS") {
            ended = blocks(t);
        } else if (section == "ENTITIES") {
            fixUnits();
            SectionRecords src(t);
            entities(src, root_, 0, "0");
            if (!src.ended() && !t.error() && !fatal_) {
                return tl::unexpected(formatError("The DXF file ends inside its ENTITIES section; it is cut off."));
            }
        } else {
            SectionRecords skip(t);
            Record r;
            while (skip.next(r)) {
            }
            ended = skip.ended();
        }
        if (!ended && !t.error()) {
            return tl::unexpected(formatError("The DXF file ends inside its " + section + " section; it is cut off."));
        }
    }
    if (t.error()) {
        return tl::unexpected(*t.error());
    }
    if (fatal_) {
        return tl::unexpected(*fatal_);
    }
    if (!sawSection) {
        return tl::unexpected(formatError("This is not a DXF file: it has no sections."));
    }
    fixUnits();
    if (!sawEof) {
        warn("the file has no EOF marker");
    }
    if (!out_.version.empty()) {
        const std::string v = upper(out_.version);
        if (v.size() == 6 && v.substr(0, 2) == "AC" && v > "AC1032") {
            warn("the file is DXF version " + out_.version + ", newer than 2018 (AC1032); it was read as 2018");
        }
    }
    for (const auto& [type, count] : ignored_) {
        out_.ignored.emplace_back(type, count);
        warn(std::to_string(count) + " " + type + " entit" + (count == 1 ? "y was" : "ies were") +
             " ignored (not sketch geometry)");
    }
    for (const auto& [type, count] : imported_) {
        out_.imported.emplace_back(type, count);
    }
    for (const auto& [message, count] : warnings_) {
        std::string line = message;
        line[0] = static_cast<char>(line[0] >= 'a' && line[0] <= 'z' ? line[0] - 'a' + 'A' : line[0]);
        out_.warnings.push_back(count > 1 ? line + " (" + std::to_string(count) + " times)." : line + ".");
    }
    out_.sketch = canonicalSketch(std::move(out_.sketch));
    std::sort(out_.layers.begin(), out_.layers.end());
    return std::move(out_);
}

} // namespace

Result<DxfImport> readDxf(std::string_view bytes, UnitSystem assumeIfUnitless, const DxfLimits& limits)
{
    Importer importer(bytes, assumeIfUnitless, limits);
    return importer.run(bytes);
}

Result<DxfImport> importDxfFile(const std::string& utf8Path, UnitSystem assumeIfUnitless, const DxfLimits& limits)
{
    const std::filesystem::path path(std::u8string(reinterpret_cast<const char8_t*>(utf8Path.data()), utf8Path.size()));
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        return tl::unexpected(
            Error{ErrorCode::NotFound, "The DXF file could not be found: " + utf8Path, {"DXF import"}});
    }
    if (size > limits.maxFileBytes) {
        return tl::unexpected(formatError("The DXF file is larger than " + std::to_string(limits.maxFileBytes >> 20) +
                                          " MiB, the most a sketch import reads."));
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return tl::unexpected(
            Error{ErrorCode::IoError, "The DXF file could not be opened: " + utf8Path, {"DXF import"}});
    }
    std::string bytes(static_cast<std::size_t>(size), '\0');
    in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (in.gcount() != static_cast<std::streamsize>(bytes.size())) {
        return tl::unexpected(Error{ErrorCode::IoError, "The DXF file could not be read: " + utf8Path, {"DXF import"}});
    }
    return readDxf(bytes, assumeIfUnitless, limits);
}

Result<Sketch> importDxf(const std::string& path, UnitSystem assumeIfUnitless)
{
    auto result = importDxfFile(path, assumeIfUnitless);
    if (!result) {
        return tl::unexpected(result.error());
    }
    for (const std::string& w : result->warnings) {
        ray::log(LogLevel::Warn, "sketch", "DXF import: " + w);
    }
    return std::move(result->sketch);
}

} // namespace ray::sketch
