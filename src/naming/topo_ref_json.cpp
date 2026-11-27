// The JSON form of a TopoRef (naming.h): one writer and one reader for every caller (feature props, the
// model.reference action, CAM operations). The reader is a small strict RFC 8259 parser for this one schema.
#include "naming_internal.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace ray::naming {

namespace {

constexpr std::int64_t kExact = std::int64_t{1} << 53;

// ---------------------------------------------------------------- writing (canonical: members in key order)
void writeInteger(std::string& out, std::int64_t v)
{
    if (v > -kExact && v < kExact) {
        out += std::to_string(v);
    } else {
        out += '"';
        out += std::to_string(v);
        out += '"';
    }
}

void writeHashes(std::string& out, const std::vector<Hash256>& hashes)
{
    out += '[';
    for (std::size_t i = 0; i < hashes.size(); ++i) {
        if (i > 0)
            out += ',';
        out += '"';
        out += hashes[i].hex();
        out += '"';
    }
    out += ']';
}

void writePoint(std::string& out, const std::array<std::int64_t, 3>& p)
{
    out += '[';
    for (std::size_t i = 0; i < 3; ++i) {
        if (i > 0)
            out += ',';
        writeInteger(out, p[i]);
    }
    out += ']';
}

void writeTerritory(std::string& out, const Territory& t)
{
    out += "{\"box\":[";
    for (std::size_t i = 0; i < 6; ++i) {
        if (i > 0)
            out += ',';
        writeInteger(out, i < 3 ? t.qMin[i] : t.qMax[i - 3]);
    }
    out += "],\"measure\":";
    writeInteger(out, t.qMeasure);
    out += ",\"points\":[";
    for (std::size_t i = 0; i < t.points.size(); ++i) {
        if (i > 0)
            out += ',';
        writePoint(out, t.points[i]);
    }
    out += "]}";
}

void writeOutline(std::string& out, const PickOutline& o)
{
    out += "{\"chords\":[";
    for (std::size_t i = 0; i < o.chords.size(); ++i) {
        if (i > 0)
            out += ',';
        out += '[';
        for (std::size_t j = 0; j < 7; ++j) {
            if (j > 0)
                out += ',';
            writeInteger(out, o.chords[i][j]);
        }
        out += ']';
    }
    out += "]}";
}

void writeIntegers(std::string& out, std::span<const std::int64_t> values)
{
    out += '[';
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i > 0)
            out += ',';
        writeInteger(out, values[i]);
    }
    out += ']';
}

void writePlacements(std::string& out, const std::vector<FeaturePlacement>& placements)
{
    out += '[';
    for (std::size_t i = 0; i < placements.size(); ++i) {
        if (i > 0)
            out += ',';
        out += "{\"feature\":\"";
        out += placements[i].feature.value.toString();
        out += "\",\"q\":";
        writeIntegers(out, placements[i].q);
        out += '}';
    }
    out += ']';
}

void writeSamples(std::string& out, const PickSamples& s)
{
    // (Version 1, read from an older reference, is written back as it was: nine integers per carrier, no grounds.)
    const bool v2 = s.version >= 2;
    out += "{\"carriers\":[";
    for (std::size_t i = 0; i < s.carriers.size(); ++i) {
        const SampleCarrier& c = s.carriers[i];
        if (i > 0)
            out += ',';
        const std::array<std::int64_t, 10> v{static_cast<std::int64_t>(c.type),
                                             c.qOrigin[0],
                                             c.qOrigin[1],
                                             c.qOrigin[2],
                                             c.qAxis[0],
                                             c.qAxis[1],
                                             c.qAxis[2],
                                             c.qRadius,
                                             c.owner,
                                             c.ground};
        writeIntegers(out, std::span<const std::int64_t>(v.data(), v2 ? 10 : 9));
    }
    out += "],\"ends\":";
    writeIntegers(out, s.ends);
    out += ",\"firstEnd\":";
    writeInteger(out, s.firstEndCarriers);
    if (v2) {
        out += ",\"grounds\":[";
        for (std::size_t i = 0; i < s.grounds.size(); ++i) {
            if (i > 0)
                out += ',';
            out += "{\"feature\":\"";
            out += s.grounds[i].feature.value.toString();
            out += "\",\"moved\":";
            writeIntegers(out, s.grounds[i].moved);
            out += '}';
        }
        out += "],\"neighbourGrounds\":[";
        for (std::size_t i = 0; i < s.neighbourGrounds.size(); ++i) {
            if (i > 0)
                out += ',';
            writeInteger(out, s.neighbourGrounds[i]);
        }
        out += ']';
    }
    out += ",\"neighbourOwners\":[";
    for (std::size_t i = 0; i < s.neighbourOwners.size(); ++i) {
        if (i > 0)
            out += ',';
        writeInteger(out, s.neighbourOwners[i]);
    }
    out += "],\"own\":";
    writeInteger(out, s.ownCarriers);
    out += ",\"placements\":";
    writePlacements(out, s.placements);
    out += ",\"points\":[";
    for (std::size_t i = 0; i < s.points.size(); ++i) {
        if (i > 0)
            out += ',';
        writePoint(out, s.points[i]);
    }
    out += "],\"spacing\":";
    writeInteger(out, s.qSpacing);
    out += ",\"version\":";
    writeInteger(out, s.version);
    if (v2)
        out += s.whole ? ",\"whole\":true" : ",\"whole\":false";
    out += '}';
}

const char* kindName(kernel::SubKind kind)
{
    switch (kind) {
    case kernel::SubKind::Vertex:
        return "vertex";
    case kernel::SubKind::Edge:
        return "edge";
    case kernel::SubKind::Face:
    case kernel::SubKind::Solid:
        break;
    }
    return "face";
}

// ---------------------------------------------------------------- reading
struct Node {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    bool b = false;
    std::string text; // a number as written, or a string's value
    std::vector<Node> items;
    std::vector<std::string> keys; // object members: keys[i] names values[i] (a vector of an incomplete type only)
    std::vector<Node> values;
    const Node* find(std::string_view key) const
    {
        for (std::size_t i = 0; i < keys.size(); ++i)
            if (keys[i] == key)
                return &values[i];
        return nullptr;
    }
};

class Parser {
public:
    explicit Parser(std::string_view s) : s_(s) {}

    bool parse(Node& out)
    {
        skip();
        if (!value(out, 0))
            return false;
        skip();
        return i_ == s_.size();
    }

private:
    void skip()
    {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r'))
            ++i_;
    }
    bool literal(std::string_view word)
    {
        if (s_.substr(i_, word.size()) != word)
            return false;
        i_ += word.size();
        return true;
    }
    bool value(Node& out, int depth)
    {
        if (depth > 32 || i_ >= s_.size())
            return false;
        const char c = s_[i_];
        if (c == '{')
            return object(out, depth);
        if (c == '[')
            return array(out, depth);
        if (c == '"') {
            out.type = Node::Type::String;
            return string(out.text);
        }
        if (c == 't' || c == 'f') {
            out.type = Node::Type::Bool;
            out.b = c == 't';
            return literal(c == 't' ? "true" : "false");
        }
        if (c == 'n') {
            out.type = Node::Type::Null;
            return literal("null");
        }
        out.type = Node::Type::Number;
        return number(out.text);
    }
    bool object(Node& out, int depth)
    {
        out.type = Node::Type::Object;
        ++i_;
        skip();
        if (i_ < s_.size() && s_[i_] == '}') {
            ++i_;
            return true;
        }
        for (;;) {
            skip();
            std::string key;
            if (i_ >= s_.size() || s_[i_] != '"' || !string(key))
                return false;
            for (const std::string& k : out.keys)
                if (k == key)
                    return false; // duplicate key
            skip();
            if (i_ >= s_.size() || s_[i_] != ':')
                return false;
            ++i_;
            skip();
            Node v;
            if (!value(v, depth + 1))
                return false;
            out.keys.push_back(std::move(key));
            out.values.push_back(std::move(v));
            skip();
            if (i_ < s_.size() && s_[i_] == ',') {
                ++i_;
                continue;
            }
            if (i_ < s_.size() && s_[i_] == '}') {
                ++i_;
                return true;
            }
            return false;
        }
    }
    bool array(Node& out, int depth)
    {
        out.type = Node::Type::Array;
        ++i_;
        skip();
        if (i_ < s_.size() && s_[i_] == ']') {
            ++i_;
            return true;
        }
        for (;;) {
            skip();
            Node v;
            if (!value(v, depth + 1))
                return false;
            out.items.push_back(std::move(v));
            skip();
            if (i_ < s_.size() && s_[i_] == ',') {
                ++i_;
                continue;
            }
            if (i_ < s_.size() && s_[i_] == ']') {
                ++i_;
                return true;
            }
            return false;
        }
    }
    // Strings of this schema are ASCII (hex, UUIDs, member names); escapes are read, non-ASCII bytes kept as they are.
    bool string(std::string& out)
    {
        ++i_;
        while (i_ < s_.size()) {
            const char c = s_[i_++];
            if (c == '"')
                return true;
            if (static_cast<unsigned char>(c) < 0x20)
                return false;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i_ >= s_.size())
                return false;
            const char e = s_[i_++];
            switch (e) {
            case '"':
            case '\\':
            case '/':
                out += e;
                break;
            case 'b':
                out += '\b';
                break;
            case 'f':
                out += '\f';
                break;
            case 'n':
                out += '\n';
                break;
            case 'r':
                out += '\r';
                break;
            case 't':
                out += '\t';
                break;
            case 'u': {
                if (i_ + 4 > s_.size())
                    return false;
                unsigned code = 0;
                for (int k = 0; k < 4; ++k) {
                    const char h = s_[i_++];
                    const int d = h >= '0' && h <= '9'   ? h - '0'
                                  : h >= 'a' && h <= 'f' ? h - 'a' + 10
                                  : h >= 'A' && h <= 'F' ? h - 'A' + 10
                                                         : -1;
                    if (d < 0)
                        return false;
                    code = code * 16 + static_cast<unsigned>(d);
                }
                out += code < 0x80 ? static_cast<char>(code) : '?'; // never valid in this schema anyway
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }
    bool number(std::string& out)
    {
        const std::size_t start = i_;
        if (i_ < s_.size() && s_[i_] == '-')
            ++i_;
        if (i_ >= s_.size() || s_[i_] < '0' || s_[i_] > '9')
            return false;
        if (s_[i_] == '0') {
            ++i_;
        } else {
            while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9')
                ++i_;
        }
        if (i_ < s_.size() && s_[i_] == '.') {
            ++i_;
            if (i_ >= s_.size() || s_[i_] < '0' || s_[i_] > '9')
                return false;
            while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9')
                ++i_;
        }
        if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
            ++i_;
            if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-'))
                ++i_;
            if (i_ >= s_.size() || s_[i_] < '0' || s_[i_] > '9')
                return false;
            while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9')
                ++i_;
        }
        out = std::string(s_.substr(start, i_ - start));
        return true;
    }

    std::string_view s_;
    std::size_t i_ = 0;
};

tl::unexpected<Error> fieldError(const std::string& field, std::string what)
{
    return tl::unexpected(Error{ErrorCode::InvalidArgument, std::move(what), {field}});
}

Status onlyKeys(const Node& object, std::initializer_list<std::string_view> keys, const std::string& prefix)
{
    for (const std::string& k : object.keys) {
        bool known = false;
        for (const std::string_view key : keys)
            known = known || key == k;
        if (!known)
            return fieldError(prefix + k, "is not a property of a reference");
    }
    return {};
}

// A whole number: a JSON number without fraction or exponent (or with a zero fraction, as a canonical writer of
// doubles may never produce but a hand may), or a decimal string of one.
std::optional<std::int64_t> wholeNumber(const Node& v)
{
    std::string_view s;
    if (v.type == Node::Type::Number || v.type == Node::Type::String)
        s = v.text;
    else
        return std::nullopt;
    bool negative = false;
    std::size_t i = 0;
    if (!s.empty() && s[0] == '-') {
        negative = true;
        i = 1;
    }
    std::size_t digits = 0;
    std::int64_t out = 0;
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i, ++digits) {
        if (digits >= 19)
            return std::nullopt;
        out = out * 10 + (s[i] - '0');
    }
    if (digits == 0)
        return std::nullopt;
    if (v.type == Node::Type::Number && i < s.size()) {
        // "12.0" or "12e0" style: only a fraction of zeros is a whole number.
        if (s[i] != '.')
            return std::nullopt;
        for (++i; i < s.size(); ++i)
            if (s[i] != '0')
                return std::nullopt;
    }
    if (i != s.size())
        return std::nullopt;
    if (v.type == Node::Type::Number && !(out < kExact))
        return std::nullopt; // a JSON number beyond 2^53 is not exact
    return negative ? -out : out;
}

Result<std::vector<Hash256>> readHashes(const Node& object, std::string_view key, const std::string& field,
                                        bool required)
{
    std::vector<Hash256> out;
    const Node* v = object.find(key);
    if (v == nullptr) {
        if (required)
            return fieldError(field, "is required");
        return out;
    }
    const char* bad = "must be an array of hashes (64 lower-case hex digits)";
    if (v->type != Node::Type::Array)
        return fieldError(field, bad);
    for (const Node& item : v->items) {
        if (item.type != Node::Type::String || item.text.size() != 64)
            return fieldError(field, bad);
        Hash256 h;
        for (std::size_t i = 0; i < 32; ++i) {
            int byte = 0;
            for (std::size_t k = 0; k < 2; ++k) {
                const char c = item.text[2 * i + k];
                const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
                if (d < 0)
                    return fieldError(field, bad);
                byte = byte * 16 + d;
            }
            h.bytes[i] = static_cast<std::uint8_t>(byte);
        }
        out.push_back(h);
    }
    return out;
}

Result<PickOutline> readOutline(const Node& v, const std::string& field)
{
    if (v.type != Node::Type::Object)
        return fieldError(field, "must be a JSON object");
    if (auto ok = onlyKeys(v, {"chords"}, field + "."); !ok)
        return tl::unexpected(ok.error());
    PickOutline o;
    const Node* chords = v.find("chords");
    const char* bad = "must be an array of chords, each seven whole numbers";
    if (chords == nullptr || chords->type != Node::Type::Array)
        return fieldError(field + ".chords", bad);
    if (chords->items.size() > kMaxOutlineChords)
        return fieldError(field + ".chords", "must hold at most " + std::to_string(kMaxOutlineChords) + " chords");
    for (const Node& c : chords->items) {
        if (c.type != Node::Type::Array || c.items.size() != 7)
            return fieldError(field + ".chords", bad);
        std::array<std::int64_t, 7> q{};
        for (std::size_t i = 0; i < 7; ++i) {
            const auto n = wholeNumber(c.items[i]);
            if (!n || (i == 6 && *n < 0))
                return fieldError(field + ".chords", bad);
            q[i] = *n;
        }
        o.chords.push_back(q);
    }
    return o;
}

Result<Territory> readTerritory(const Node& v, const std::string& field)
{
    if (v.type != Node::Type::Object)
        return fieldError(field, "must be a JSON object");
    if (auto ok = onlyKeys(v, {"box", "measure", "points"}, field + "."); !ok)
        return tl::unexpected(ok.error());
    Territory t;
    if (const Node* box = v.find("box")) {
        if (box->type != Node::Type::Array || box->items.size() != 6)
            return fieldError(field + ".box", "must be six whole numbers");
        for (std::size_t i = 0; i < 6; ++i) {
            const auto n = wholeNumber(box->items[i]);
            if (!n)
                return fieldError(field + ".box", "must be six whole numbers");
            (i < 3 ? t.qMin[i] : t.qMax[i - 3]) = *n;
        }
        for (std::size_t i = 0; i < 3; ++i)
            if (t.qMin[i] > t.qMax[i])
                return fieldError(field + ".box", "must have each minimum at most its maximum");
    }
    if (const Node* m = v.find("measure")) {
        const auto n = wholeNumber(*m);
        if (!n || *n < 0)
            return fieldError(field + ".measure", "must be a whole number, 0 or more");
        t.qMeasure = *n;
    }
    if (const Node* pts = v.find("points")) {
        const char* bad = "must be an array of points, each three whole numbers";
        if (pts->type != Node::Type::Array)
            return fieldError(field + ".points", bad);
        if (pts->items.size() > kTerritoryPoints)
            return fieldError(field + ".points", "must hold at most " + std::to_string(kTerritoryPoints) + " points");
        for (const Node& p : pts->items) {
            if (p.type != Node::Type::Array || p.items.size() != 3)
                return fieldError(field + ".points", bad);
            std::array<std::int64_t, 3> q{};
            for (std::size_t i = 0; i < 3; ++i) {
                const auto n = wholeNumber(p.items[i]);
                if (!n)
                    return fieldError(field + ".points", bad);
                q[i] = *n;
            }
            t.points.push_back(q);
        }
    }
    return t;
}

Result<std::vector<FeaturePlacement>> readPlacements(const Node& list, const std::string& field)
{
    std::vector<FeaturePlacement> out;
    if (list.type != Node::Type::Array)
        return fieldError(field, "must be an array");
    for (std::size_t i = 0; i < list.items.size(); ++i) {
        const Node& item = list.items[i];
        const std::string at = field + "[" + std::to_string(i) + "]";
        if (item.type != Node::Type::Object)
            return fieldError(at, "must be a JSON object");
        if (auto ok = onlyKeys(item, {"feature", "q"}, at + "."); !ok)
            return tl::unexpected(ok.error());
        FeaturePlacement p;
        const Node* feature = item.find("feature");
        if (feature == nullptr || feature->type != Node::Type::String)
            return fieldError(at + ".feature", "must be a lower-case UUID");
        const Uuid id = Uuid::fromString(feature->text);
        if (id.toString() != feature->text || id == Uuid{})
            return fieldError(at + ".feature", "must be a lower-case UUID other than the nil one");
        p.feature = FeatureId{id};
        const Node* q = item.find("q");
        if (q == nullptr || q->type != Node::Type::Array || q->items.size() != 12)
            return fieldError(at + ".q", "must be twelve whole numbers");
        for (std::size_t k = 0; k < 12; ++k) {
            const auto n = wholeNumber(q->items[k]);
            if (!n)
                return fieldError(at + ".q", "must be twelve whole numbers");
            p.q[k] = *n;
        }
        if (!out.empty() && !(out.back().feature < p.feature))
            return fieldError(field, "must be sorted by feature, each feature once");
        out.push_back(p);
    }
    return out;
}

// An array of exactly `n` whole numbers into `out`.
bool readIntegers(const Node* v, std::size_t n, std::int64_t* out)
{
    if (v == nullptr || v->type != Node::Type::Array || v->items.size() != n)
        return false;
    for (std::size_t i = 0; i < n; ++i) {
        const auto w = wholeNumber(v->items[i]);
        if (!w)
            return false;
        out[i] = *w;
    }
    return true;
}

Result<PickSamples> readSamples(const Node& v, const std::string& field, std::size_t neighbours)
{
    if (v.type != Node::Type::Object)
        return fieldError(field, "must be a JSON object");
    PickSamples s;
    const Node* version = v.find("version");
    const auto ver = version != nullptr ? wholeNumber(*version) : std::nullopt;
    if (!ver || *ver < 1 || *ver > kPickSamplesVersion)
        return fieldError(field + ".version", "must be 1 or " + std::to_string(kPickSamplesVersion));
    s.version = static_cast<std::uint32_t>(*ver);
    const bool v2 = s.version >= 2;
    if (auto ok = v2 ? onlyKeys(v,
                                {"carriers", "ends", "firstEnd", "grounds", "neighbourGrounds", "neighbourOwners",
                                 "own", "placements", "points", "spacing", "version", "whole"},
                                field + ".")
                     : onlyKeys(v,
                                {"carriers", "ends", "firstEnd", "neighbourOwners", "own", "placements", "points",
                                 "spacing", "version"},
                                field + ".");
        !ok)
        return tl::unexpected(ok.error());
    if (v2) {
        if (const Node* whole = v.find("whole")) {
            if (whole->type != Node::Type::Bool)
                return fieldError(field + ".whole", "must be true or false");
            s.whole = whole->b;
        }
        if (const Node* grounds = v.find("grounds")) {
            if (grounds->type != Node::Type::Array)
                return fieldError(field + ".grounds", "must be an array");
            for (std::size_t i = 0; i < grounds->items.size(); ++i) {
                const Node& item = grounds->items[i];
                const std::string at = field + ".grounds[" + std::to_string(i) + "]";
                if (item.type != Node::Type::Object)
                    return fieldError(at, "must be a JSON object");
                if (auto ok = onlyKeys(item, {"feature", "moved"}, at + "."); !ok)
                    return tl::unexpected(ok.error());
                const Node* feature = item.find("feature");
                if (feature == nullptr || feature->type != Node::Type::String)
                    return fieldError(at + ".feature", "must be a lower-case UUID");
                const Uuid id = Uuid::fromString(feature->text);
                if (id.toString() != feature->text || id == Uuid{})
                    return fieldError(at + ".feature", "must be a lower-case UUID other than the nil one");
                SampleGround g;
                g.feature = FeatureId{id};
                if (!readIntegers(item.find("moved"), 12, g.moved.data()))
                    return fieldError(at + ".moved", "must be twelve whole numbers");
                if (!s.grounds.empty() && !(s.grounds.back().feature < g.feature))
                    return fieldError(field + ".grounds", "must be sorted by feature, each feature once");
                s.grounds.push_back(g);
            }
        }
    }
    if (const Node* placements = v.find("placements")) {
        auto p = readPlacements(*placements, field + ".placements");
        if (!p)
            return tl::unexpected(p.error());
        s.placements = std::move(*p);
    }
    // -1, an index into placements, or -2 - such an index (SampleCarrier::owner).
    const auto owner = [&](std::int64_t o) {
        const std::int64_t n = static_cast<std::int64_t>(s.placements.size());
        return o < n && o >= -1 - n;
    };
    const Node* points = v.find("points");
    const char* badPoints = "must be an array of points, each three whole numbers";
    // (Version 2 may have none: an element that had none to give.)
    if (points == nullptr || points->type != Node::Type::Array || (points->items.empty() && !v2))
        return fieldError(field + ".points", badPoints);
    if (points->items.size() > kMaxPickSamples)
        return fieldError(field + ".points", "must hold at most " + std::to_string(kMaxPickSamples) + " points");
    for (const Node& p : points->items) {
        std::array<std::int64_t, 3> q{};
        if (!readIntegers(&p, 3, q.data()))
            return fieldError(field + ".points", badPoints);
        s.points.push_back(q);
    }
    if (const Node* ends = v.find("ends"); ends != nullptr && !readIntegers(ends, 6, s.ends.data()))
        return fieldError(field + ".ends", "must be six whole numbers");
    if (const Node* spacing = v.find("spacing")) {
        const auto n = wholeNumber(*spacing);
        if (!n || *n < 0)
            return fieldError(field + ".spacing", "must be a whole number, 0 or more");
        s.qSpacing = *n;
    }
    // -1 or an index into grounds (version 2).
    const auto ground = [&](std::int64_t g) {
        return g >= -1 && g < static_cast<std::int64_t>(s.grounds.size());
    };
    if (const Node* carriers = v.find("carriers")) {
        const char* bad =
            v2 ? "must be an array of carriers, each ten whole numbers (type, origin, axis, radius, "
                 "owner, ground)"
               : "must be an array of carriers, each nine whole numbers (type, origin, axis, radius, owner)";
        const std::size_t n = v2 ? 10 : 9;
        if (carriers->type != Node::Type::Array || carriers->items.size() > kMaxSampleCarriers)
            return fieldError(field + ".carriers", bad);
        for (const Node& c : carriers->items) {
            std::array<std::int64_t, 10> q{};
            q[9] = -1;
            if (!readIntegers(&c, n, q.data()) || q[0] < 0 || q[0] > 2 || !owner(q[8]) || !ground(q[9]))
                return fieldError(field + ".carriers", bad);
            SampleCarrier carrier;
            carrier.type = static_cast<SampleCarrier::Type>(q[0]);
            carrier.qOrigin = {q[1], q[2], q[3]};
            carrier.qAxis = {q[4], q[5], q[6]};
            carrier.qRadius = q[7];
            carrier.owner = static_cast<std::int32_t>(q[8]);
            carrier.ground = static_cast<std::int32_t>(q[9]);
            s.carriers.push_back(carrier);
        }
    }
    if (const Node* grounds = v2 ? v.find("neighbourGrounds") : nullptr) {
        const char* bad = "must hold one ground (-1 or an index into grounds) per pick neighbour";
        if (grounds->type != Node::Type::Array || (!grounds->items.empty() && grounds->items.size() != neighbours))
            return fieldError(field + ".neighbourGrounds", bad);
        for (const Node& g : grounds->items) {
            const auto n = wholeNumber(g);
            if (!n || !ground(*n))
                return fieldError(field + ".neighbourGrounds", bad);
            s.neighbourGrounds.push_back(static_cast<std::int32_t>(*n));
        }
    }
    s.ownCarriers = static_cast<std::uint32_t>(s.carriers.size());
    if (const Node* own = v.find("own")) {
        const auto n = wholeNumber(*own);
        if (!n || *n < 0 || *n > static_cast<std::int64_t>(s.carriers.size()))
            return fieldError(field + ".own", "must be a whole number, at most the number of carriers");
        s.ownCarriers = static_cast<std::uint32_t>(*n);
    }
    if (const Node* first = v.find("firstEnd")) {
        const auto n = wholeNumber(*first);
        if (!n || *n < 0 || *n > static_cast<std::int64_t>(s.carriers.size() - s.ownCarriers))
            return fieldError(field + ".firstEnd", "must be a whole number, at most the number of end carriers");
        s.firstEndCarriers = static_cast<std::uint32_t>(*n);
    }
    if (const Node* owners = v.find("neighbourOwners")) {
        const char* bad = "must hold one owner (-1 or an index into placements) per pick neighbour";
        if (owners->type != Node::Type::Array || (!owners->items.empty() && owners->items.size() != neighbours))
            return fieldError(field + ".neighbourOwners", bad);
        for (const Node& o : owners->items) {
            const auto n = wholeNumber(o);
            if (!n || !owner(*n))
                return fieldError(field + ".neighbourOwners", bad);
            s.neighbourOwners.push_back(static_cast<std::int32_t>(*n));
        }
    }
    return s;
}

} // namespace

std::string toJson(const TopoRef& ref)
{
    std::string out;
    out.reserve(1024);
    out += "{\"ambiguous\":[";
    for (std::size_t i = 0; i < ref.ambiguous.size(); ++i) {
        if (i > 0)
            out += ',';
        out += ref.ambiguous[i] ? "true" : "false";
    }
    out += "],\"body\":\"";
    out += ref.body.value.toString();
    out += "\",\"families\":";
    writeHashes(out, ref.families);
    const Fingerprint& f = ref.hint;
    out += ",\"hint\":{\"area\":";
    writeInteger(out, f.qArea);
    out += ",\"ax\":";
    writeInteger(out, f.qAxisX);
    out += ",\"ay\":";
    writeInteger(out, f.qAxisY);
    out += ",\"az\":";
    writeInteger(out, f.qAxisZ);
    out += ",\"cx\":";
    writeInteger(out, f.qCx);
    out += ",\"cy\":";
    writeInteger(out, f.qCy);
    out += ",\"cz\":";
    writeInteger(out, f.qCz);
    out += ",\"neighbours\":";
    writeInteger(out, f.neighbourCount);
    out += ",\"radius\":";
    writeInteger(out, f.qRadius);
    out += ",\"type\":";
    writeInteger(out, static_cast<std::int64_t>(f.surface));
    out += "},\"kind\":\"";
    out += kindName(ref.kind);
    out += "\",\"names\":";
    writeHashes(out, ref.names);
    out += ",\"neighbourNames\":";
    writeHashes(out, ref.neighbourNames);
    if (ref.outline.known()) {
        out += ",\"outline\":";
        writeOutline(out, ref.outline);
    }
    if (!ref.pickNeighbours.empty()) {
        out += ",\"pickNeighbours\":[";
        for (std::size_t i = 0; i < ref.pickNeighbours.size(); ++i) {
            const PickNeighbour& n = ref.pickNeighbours[i];
            if (i > 0)
                out += ',';
            out += "{\"names\":";
            writeHashes(out, n.names);
            out += ",\"relation\":\"";
            out += n.relation == PickNeighbour::Relation::Sibling ? "sibling" : "carrier";
            out += "\",\"territory\":";
            writeTerritory(out, n.territory);
            out += '}';
        }
        out += ']';
    }
    if (!ref.placements.empty()) {
        out += ",\"placements\":";
        writePlacements(out, ref.placements);
    }
    out += ",\"repairConfirmed\":";
    out += ref.repairConfirmed ? "true" : "false";
    if (ref.samples.known() || ref.samples.version >= 2) {
        out += ",\"samples\":";
        writeSamples(out, ref.samples);
    }
    out += ",\"splitParents\":";
    writeHashes(out, ref.splitParents);
    if (ref.territory.known()) {
        out += ",\"territory\":";
        writeTerritory(out, ref.territory);
    }
    out += '}';
    return out;
}

Result<TopoRef> topoRefFromJson(std::string_view json)
{
    Node root;
    if (!Parser(json).parse(root))
        return tl::unexpected(Error{ErrorCode::InvalidArgument, "is not valid JSON", {}});
    if (root.type != Node::Type::Object)
        return tl::unexpected(
            Error{ErrorCode::InvalidArgument, "must be a reference to a face, edge or vertex (a JSON object)", {}});
    if (auto ok = onlyKeys(root,
                           {"ambiguous", "body", "families", "hint", "kind", "names", "neighbourNames", "outline",
                            "pickNeighbours", "placements", "repairConfirmed", "samples", "splitParents", "territory"},
                           "");
        !ok)
        return tl::unexpected(ok.error());
    TopoRef out;
    {
        const Node* body = root.find("body");
        if (body == nullptr)
            return fieldError("body", "is required");
        if (body->type != Node::Type::String)
            return fieldError("body", "must be a lower-case UUID");
        const Uuid id = Uuid::fromString(body->text);
        if (id.toString() != body->text || id == Uuid{})
            return fieldError("body", "must be a lower-case UUID other than the nil one");
        out.body = BodyId{id};
    }
    {
        const Node* kind = root.find("kind");
        if (kind == nullptr)
            return fieldError("kind", "is required");
        if (kind->type == Node::Type::String && kind->text == "face")
            out.kind = kernel::SubKind::Face;
        else if (kind->type == Node::Type::String && kind->text == "edge")
            out.kind = kernel::SubKind::Edge;
        else if (kind->type == Node::Type::String && kind->text == "vertex")
            out.kind = kernel::SubKind::Vertex;
        else
            return fieldError("kind", "must be one of face, edge, vertex");
    }
    auto names = readHashes(root, "names", "names", true);
    if (!names)
        return tl::unexpected(names.error());
    if (names->empty())
        return fieldError("names", "must list at least one name");
    out.names = std::move(*names);
    auto families = readHashes(root, "families", "families", false);
    if (!families)
        return tl::unexpected(families.error());
    out.families = std::move(*families);
    if (!out.families.empty() && out.families.size() != out.names.size())
        return fieldError("families", "must have one entry per name");
    if (const Node* a = root.find("ambiguous"); a != nullptr && !(a->type == Node::Type::Array && a->items.empty())) {
        if (a->type != Node::Type::Array || a->items.size() != out.names.size())
            return fieldError("ambiguous", "must have one true or false per name");
        for (const Node& b : a->items) {
            if (b.type != Node::Type::Bool)
                return fieldError("ambiguous", "must have one true or false per name");
            out.ambiguous.push_back(b.b);
        }
    }
    auto split = readHashes(root, "splitParents", "splitParents", false);
    if (!split)
        return tl::unexpected(split.error());
    out.splitParents = std::move(*split);
    auto neighbours = readHashes(root, "neighbourNames", "neighbourNames", false);
    if (!neighbours)
        return tl::unexpected(neighbours.error());
    out.neighbourNames = std::move(*neighbours);
    out.hint = detail::emptyFingerprint();
    if (const Node* hint = root.find("hint")) {
        if (hint->type != Node::Type::Object)
            return fieldError("hint", "must be a JSON object");
        if (auto ok =
                onlyKeys(*hint, {"area", "ax", "ay", "az", "cx", "cy", "cz", "neighbours", "radius", "type"}, "hint.");
            !ok)
            return tl::unexpected(ok.error());
        struct Slot {
            const char* key;
            std::int64_t* target;
        };
        std::int64_t type = static_cast<std::int64_t>(kernel::SurfaceType::Other);
        std::int64_t neighbourCount = 0;
        const Slot slots[] = {{"type", &type},          {"area", &out.hint.qArea},
                              {"cx", &out.hint.qCx},    {"cy", &out.hint.qCy},
                              {"cz", &out.hint.qCz},    {"radius", &out.hint.qRadius},
                              {"ax", &out.hint.qAxisX}, {"ay", &out.hint.qAxisY},
                              {"az", &out.hint.qAxisZ}, {"neighbours", &neighbourCount}};
        for (const Slot& s : slots) {
            const Node* v = hint->find(s.key);
            if (v == nullptr) {
                if (std::string_view(s.key) != "type")
                    *s.target = 0;
                continue;
            }
            const auto n = wholeNumber(*v);
            if (!n)
                return fieldError(std::string("hint.") + s.key, "must be a whole number");
            *s.target = *n;
        }
        if (type < 0 || type > static_cast<std::int64_t>(kernel::SurfaceType::Other) || neighbourCount < 0 ||
            neighbourCount > 65535)
            return fieldError("hint", "is not a valid fingerprint");
        out.hint.surface = static_cast<kernel::SurfaceType>(type);
        out.hint.neighbourCount = static_cast<std::uint16_t>(neighbourCount);
    }
    if (const Node* c = root.find("repairConfirmed")) {
        if (c->type != Node::Type::Bool)
            return fieldError("repairConfirmed", "must be true or false");
        out.repairConfirmed = c->b;
    }
    if (const Node* t = root.find("territory")) {
        auto territory = readTerritory(*t, "territory");
        if (!territory)
            return tl::unexpected(territory.error());
        out.territory = std::move(*territory);
    }
    if (const Node* o = root.find("outline")) {
        auto outline = readOutline(*o, "outline");
        if (!outline)
            return tl::unexpected(outline.error());
        out.outline = std::move(*outline);
    }
    if (const Node* list = root.find("pickNeighbours")) {
        if (list->type != Node::Type::Array)
            return fieldError("pickNeighbours", "must be an array");
        if (list->items.size() > kMaxPickNeighbours)
            return fieldError("pickNeighbours",
                              "must hold at most " + std::to_string(kMaxPickNeighbours) + " neighbours");
        for (std::size_t i = 0; i < list->items.size(); ++i) {
            const Node& item = list->items[i];
            const std::string at = "pickNeighbours[" + std::to_string(i) + "]";
            if (item.type != Node::Type::Object)
                return fieldError(at, "must be a JSON object");
            if (auto ok = onlyKeys(item, {"names", "relation", "territory"}, at + "."); !ok)
                return tl::unexpected(ok.error());
            PickNeighbour n;
            auto nnames = readHashes(item, "names", at + ".names", true);
            if (!nnames)
                return tl::unexpected(nnames.error());
            if (nnames->empty())
                return fieldError(at + ".names", "must list at least one name");
            n.names = std::move(*nnames);
            const Node* relation = item.find("relation");
            if (relation == nullptr)
                return fieldError(at + ".relation", "is required");
            if (relation->type == Node::Type::String && relation->text == "sibling")
                n.relation = PickNeighbour::Relation::Sibling;
            else if (relation->type == Node::Type::String && relation->text == "carrier")
                n.relation = PickNeighbour::Relation::SameCarrier;
            else
                return fieldError(at + ".relation", "must be one of sibling, carrier");
            const Node* territory = item.find("territory");
            if (territory == nullptr)
                return fieldError(at + ".territory", "is required");
            auto t = readTerritory(*territory, at + ".territory");
            if (!t)
                return tl::unexpected(t.error());
            n.territory = std::move(*t);
            out.pickNeighbours.push_back(std::move(n));
        }
    }
    if (const Node* list = root.find("placements")) {
        auto placements = readPlacements(*list, "placements");
        if (!placements)
            return tl::unexpected(placements.error());
        out.placements = std::move(*placements);
    }
    if (const Node* s = root.find("samples")) {
        auto samples = readSamples(*s, "samples", out.pickNeighbours.size());
        if (!samples)
            return tl::unexpected(samples.error());
        out.samples = std::move(*samples);
    }
    return out;
}

} // namespace ray::naming
