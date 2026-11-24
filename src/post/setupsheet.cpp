// The setup sheet's HTML (task T-076; setupsheet.h says what a sheet holds and why).
#include "setupsheet.h"

#include "format.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace ray::post {

namespace detail {
extern const unsigned char kSetupSheetCss[];
extern const std::size_t kSetupSheetCssSize;
} // namespace detail

namespace sheet {

std::string escapeHtml(std::string_view text)
{
    std::string out;
    out.reserve(text.size() + text.size() / 8);
    for (const char c : text) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        case '\'':
            out += "&#39;";
            break;
        default:
            if ((static_cast<unsigned char>(c) < 0x20 && c != '\t' && c != '\n') || c == 0x7f)
                out += ' ';
            else
                out += c;
        }
    }
    return out;
}

std::string base64(std::span<const std::byte> bytes)
{
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 3 <= bytes.size(); i += 3) {
        const unsigned v = (std::to_integer<unsigned>(bytes[i]) << 16) |
                           (std::to_integer<unsigned>(bytes[i + 1]) << 8) | std::to_integer<unsigned>(bytes[i + 2]);
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
        out += kAlphabet[v & 63];
    }
    const std::size_t rest = bytes.size() - i;
    if (rest > 0) {
        unsigned v = std::to_integer<unsigned>(bytes[i]) << 16;
        if (rest == 2)
            v |= std::to_integer<unsigned>(bytes[i + 1]) << 8;
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += rest == 2 ? kAlphabet[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

std::string duration(double seconds)
{
    if (!std::isfinite(seconds) || seconds < 0 || seconds > 1e9)
        return {};
    const long long total = std::llround(seconds);
    const long long h = total / 3600, m = (total / 60) % 60, s = total % 60;
    std::string out = std::to_string(h) + ":";
    out += (m < 10 ? "0" : "") + std::to_string(m) + ":";
    out += (s < 10 ? "0" : "") + std::to_string(s);
    return out;
}

std::string workOffsetCode(int workOffset)
{
    if (workOffset >= 1 && workOffset <= 6)
        return "G" + std::to_string(53 + workOffset);
    if (workOffset > 6)
        return "G54.1 P" + std::to_string(workOffset - 6);
    return "G54";
}

std::string originWords(const cam::Wcs& wcs)
{
    switch (wcs.originMode) {
    case cam::Wcs::OriginMode::StockCorner: {
        // cam.h: seen from +Z, corner k is X max for k = 1, 2, 5, 6, Y max for 2, 3, 6, 7 and Z max for 0..3.
        const int k = std::clamp(wcs.stockCornerIndex, 0, 7);
        const bool xMax = k == 1 || k == 2 || k == 5 || k == 6;
        const bool yMax = k == 2 || k == 3 || k == 6 || k == 7;
        const bool top = k <= 3;
        return std::string(top ? "Top " : "Bottom ") + (yMax ? "back " : "front ") + (xMax ? "right" : "left") +
               " corner of the stock";
    }
    case cam::Wcs::OriginMode::StockTopCenter:
        return "Centre of the top face of the stock";
    case cam::Wcs::OriginMode::ModelPoint:
        return "A point picked on the part (see the stock's extent below)";
    }
    return {};
}

std::string_view styleSheet()
{
    return std::string_view(reinterpret_cast<const char*>(detail::kSetupSheetCss), detail::kSetupSheetCssSize);
}

} // namespace sheet

namespace {

using sheet::escapeHtml;

constexpr const char* kDash = "&#8212;";

// ---------- numbers in the program's units ----------
std::string lengthNumber(Nm nm, UnitSystem units)
{
    std::string out;
    const NumberFormat f{units == UnitSystem::Metric ? 3 : 4, true, false, true, 1};
    fmt::appendGrid(out, nm, f, units == UnitSystem::Metric ? fmt::kMicroGrid : fmt::kInchGrid);
    return out;
}

std::optional<Nm> snapMm(double mm)
{
    std::int64_t nm = 0;
    if (!fmt::snapToGrid(mm, nm, fmt::kMicroGrid))
        return std::nullopt;
    return nm;
}

std::string unitName(UnitSystem units)
{
    return units == UnitSystem::Metric ? " mm" : " in";
}

std::string lengthNumberMm(double mm, UnitSystem units)
{
    const auto nm = snapMm(mm);
    return nm ? lengthNumber(*nm, units) : std::string(kDash);
}

std::string lengthMm(double mm, UnitSystem units)
{
    const auto nm = snapMm(mm);
    return nm ? lengthNumber(*nm, units) + unitName(units) : std::string(kDash);
}

// A feed in mm/min as the program's units per minute: whole mm/min, or in/min to one decimal.
std::string feedText(double mmPerMin, UnitSystem units)
{
    const auto nm = snapMm(mmPerMin);
    if (!nm)
        return kDash;
    std::string out;
    if (units == UnitSystem::Metric) {
        fmt::appendGrid(out, *nm, NumberFormat{0, true, false, true, 1}, fmt::kMicroGrid);
        return out + " mm/min";
    }
    fmt::appendGrid(out, *nm, NumberFormat{1, true, false, true, 1}, fmt::kInchGrid);
    return out + " in/min";
}

std::string plainNumber(double value, int decimals)
{
    const std::string s = formatNumber(value, NumberFormat{decimals, true, false, true, 1});
    return s.empty() ? std::string(kDash) : s;
}

// ---------- the operation's parameters (canonical JSON; only top-level keys are read) ----------
// Each top-level member's raw JSON text by key; an empty map for anything that is not an object.
std::map<std::string, std::string> topLevel(std::string_view json)
{
    std::map<std::string, std::string> out;
    std::size_t p = 0;
    const auto ws = [&] {
        while (p < json.size() && (json[p] == ' ' || json[p] == '\t' || json[p] == '\n' || json[p] == '\r'))
            ++p;
    };
    // Past one JSON string starting at p (on its opening quote); false when it does not end.
    const auto skipString = [&]() -> bool {
        ++p;
        while (p < json.size()) {
            if (json[p] == '\\') {
                p += 2;
                continue;
            }
            if (json[p] == '"') {
                ++p;
                return true;
            }
            ++p;
        }
        return false;
    };
    ws();
    if (p >= json.size() || json[p] != '{')
        return {};
    ++p;
    while (true) {
        ws();
        if (p >= json.size() || json[p] == '}')
            return out;
        if (json[p] != '"')
            return {};
        const std::size_t keyStart = p + 1;
        if (!skipString())
            return {};
        const std::string key(json.substr(keyStart, p - keyStart - 1));
        ws();
        if (p >= json.size() || json[p] != ':')
            return {};
        ++p;
        ws();
        const std::size_t valueStart = p;
        int depth = 0;
        while (p < json.size()) {
            const char c = json[p];
            if (c == '"') {
                if (!skipString())
                    return {};
                continue;
            }
            if (c == '{' || c == '[')
                ++depth;
            else if (c == '}' || c == ']') {
                if (depth == 0)
                    break;
                --depth;
            } else if (c == ',' && depth == 0)
                break;
            ++p;
        }
        std::size_t valueEnd = p;
        while (valueEnd > valueStart && (json[valueEnd - 1] == ' ' || json[valueEnd - 1] == '\n' ||
                                         json[valueEnd - 1] == '\r' || json[valueEnd - 1] == '\t'))
            --valueEnd;
        out[key] = std::string(json.substr(valueStart, valueEnd - valueStart));
        if (p < json.size() && json[p] == ',')
            ++p;
    }
}

std::optional<double> numberOf(const std::map<std::string, std::string>& params, const char* key)
{
    const auto it = params.find(key);
    if (it == params.end())
        return std::nullopt;
    double d = 0;
    const char* first = it->second.data();
    const char* last = first + it->second.size();
    const auto r = std::from_chars(first, last, d);
    if (r.ec != std::errc() || r.ptr != last || !std::isfinite(d))
        return std::nullopt;
    return d;
}

// A plain string member (an enum value: no escapes in it); "" otherwise.
std::string stringOf(const std::map<std::string, std::string>& params, const char* key)
{
    const auto it = params.find(key);
    if (it == params.end() || it->second.size() < 2 || it->second.front() != '"' || it->second.back() != '"' ||
        it->second.find('\\') != std::string::npos)
        return {};
    return it->second.substr(1, it->second.size() - 2);
}

// ---------- words ----------
std::string operationWords(const cam::Operation& op, const std::map<std::string, std::string>& params)
{
    const std::string& t = op.type;
    if (t == "face")
        return "Facing";
    if (t == "adaptive2d")
        return "Adaptive roughing";
    if (t == "contour2d")
        return "Contour";
    if (t == "pocket2d")
        return "Pocket";
    if (t == "slot")
        return "Slot";
    if (t == "chamfer2d")
        return "Chamfer";
    if (t == "engrave")
        return "Engraving";
    if (t == "threadmill")
        return "Thread milling";
    if (t == "drill") {
        const std::string cycle = stringOf(params, "cycle");
        if (cycle == "spot")
            return "Spot drilling";
        if (cycle == "peck")
            return "Peck drilling (full retract)";
        if (cycle == "chipBreak")
            return "Peck drilling (chip break)";
        if (cycle == "tap")
            return "Tapping";
        if (cycle == "bore")
            return "Boring";
        if (cycle == "ream")
            return "Reaming";
        return "Drilling";
    }
    return escapeHtml(t);
}

std::string toolTypeWords(const lib::Tool& tool, UnitSystem units)
{
    switch (tool.type) {
    case lib::ToolType::FlatEndMill:
        return "Flat end mill";
    case lib::ToolType::BallEndMill:
        return "Ball end mill";
    case lib::ToolType::BullEndMill:
        return "Bull nose end mill" +
               (tool.cornerRadius > 0 ? ", corner radius " + lengthMm(tool.cornerRadius, units) : std::string());
    case lib::ToolType::ChamferMill:
        return "Chamfer mill" +
               (tool.tipAngleDeg > 0 ? ", " + plainNumber(tool.tipAngleDeg, 1) + "&#176; included" : std::string());
    case lib::ToolType::FaceMill:
        return "Face mill";
    case lib::ToolType::SpotDrill:
        return "Spot drill" +
               (tool.tipAngleDeg > 0 ? ", " + plainNumber(tool.tipAngleDeg, 1) + "&#176; point" : std::string());
    case lib::ToolType::Drill:
        return "Drill" +
               (tool.tipAngleDeg > 0 ? ", " + plainNumber(tool.tipAngleDeg, 1) + "&#176; point" : std::string());
    case lib::ToolType::Tap:
        return "Tap" + (tool.threadSpec.empty() ? std::string() : " " + escapeHtml(tool.threadSpec));
    case lib::ToolType::Reamer:
        return "Reamer";
    case lib::ToolType::BoringBar:
        return "Boring bar";
    case lib::ToolType::ThreadMill:
        return "Thread mill" + (tool.threadSpec.empty() ? std::string() : " " + escapeHtml(tool.threadSpec));
    case lib::ToolType::EngraveCutter:
        return "Engraving cutter";
    }
    return {};
}

bool isHoleTool(lib::ToolType t)
{
    return t == lib::ToolType::SpotDrill || t == lib::ToolType::Drill || t == lib::ToolType::Tap ||
           t == lib::ToolType::Reamer || t == lib::ToolType::BoringBar;
}

const char* coolantWords(cam::CuttingData::Coolant c)
{
    switch (c) {
    case cam::CuttingData::Coolant::Off:
        return "No coolant";
    case cam::CuttingData::Coolant::Flood:
        return "Flood coolant";
    case cam::CuttingData::Coolant::Mist:
        return "Mist";
    case cam::CuttingData::Coolant::ThroughTool:
        return "Coolant through the tool";
    case cam::CuttingData::Coolant::Air:
        return "Air blast";
    }
    return "";
}

bool isCycle(cam::MoveKind k)
{
    return k == cam::MoveKind::CycleDrill || k == cam::MoveKind::CyclePeck || k == cam::MoveKind::CycleChipBreak ||
           k == cam::MoveKind::CycleTap || k == cam::MoveKind::CycleBore || k == cam::MoveKind::CycleReam;
}

// The lowest point the tool reaches while cutting (feed moves and hole bottoms), if it cuts at all.
std::optional<Nm> deepestZ(const cam::Toolpath& path)
{
    std::optional<Nm> low;
    for (const cam::Move& m : path.moves) {
        if (m.kind == cam::MoveKind::Rapid || m.kind == cam::MoveKind::Dwell || m.kind == cam::MoveKind::CycleCancel)
            continue;
        low = low ? std::min(*low, m.z) : m.z;
    }
    return low;
}

std::size_t holeCount(const cam::Toolpath& path)
{
    return static_cast<std::size_t>(
        std::count_if(path.moves.begin(), path.moves.end(), [](const cam::Move& m) { return isCycle(m.kind); }));
}

std::string stockShapeWords(const cam::StockDef& stock, const Box3& box, UnitSystem units)
{
    const std::string x = lengthNumberMm(box.max.x - box.min.x, units);
    const std::string y = lengthNumberMm(box.max.y - box.min.y, units);
    const std::string z = lengthNumberMm(box.max.z - box.min.z, units);
    const std::string boxText = x + " &#215; " + y + " &#215; " + z + unitName(units) + " (X &#215; Y &#215; Z)";
    if (const auto* b = std::get_if<cam::BoxStock>(&stock)) {
        std::string out = "Block " + boxText;
        if (b->xNeg != 0 || b->xPos != 0 || b->yNeg != 0 || b->yPos != 0 || b->zNeg != 0 || b->zPos != 0) {
            out += "<br>Beyond the part: top " + lengthNumberMm(b->zPos, units) + ", bottom " +
                   lengthNumberMm(b->zNeg, units) + ", left " + lengthNumberMm(b->xNeg, units) + ", right " +
                   lengthNumberMm(b->xPos, units) + ", front " + lengthNumberMm(b->yNeg, units) + ", back " +
                   lengthNumberMm(b->yPos, units) + unitName(units);
        }
        return out;
    }
    if (const auto* c = std::get_if<cam::CylinderStock>(&stock)) {
        const char* axis = c->axis == 0 ? "X" : c->axis == 1 ? "Y" : "Z";
        return "Round bar &#216;" + lengthNumberMm(c->diameter, units) + " &#215; " + lengthNumberMm(c->length, units) +
               unitName(units) + " long, along " + axis;
    }
    if (std::holds_alternative<cam::SolidStock>(stock))
        return "A body of the model, inside " + boxText;
    return "What the earlier setup left, inside " + boxText;
}

// ---------- the document ----------
class Html {
public:
    void raw(std::string_view s) { out_ += s; }
    void text(std::string_view s) { out_ += escapeHtml(s); }
    void line(std::string_view s)
    {
        out_ += s;
        out_ += '\n';
    }
    std::string& str() { return out_; }

private:
    std::string out_;
};

// A row of the facts table: label and value pairs, `value` already HTML.
void factRow(Html& h, std::string_view l1, const std::string& v1, std::string_view l2, const std::string& v2)
{
    h.raw("<tr><td class=\"label\">");
    h.raw(l1);
    h.raw("</td><td class=\"value\">");
    h.raw(v1.empty() ? std::string(kDash) : v1);
    h.raw("</td><td class=\"label\">");
    h.raw(l2);
    h.raw("</td><td class=\"value\">");
    h.raw(l2.empty() ? std::string() : (v2.empty() ? std::string(kDash) : v2));
    h.line("</td></tr>");
}

struct ToolUse {
    int number = 0;
    std::vector<std::size_t> sections; // indices into job->sections, in program order
};

void writeSheet(Html& h, const SetupSheetInput& in, bool newPage, const std::vector<std::string>& jobNotices)
{
    const PostJob& job = *in.job;
    const cam::Setup& setup = *job.setup;
    const UnitSystem units = job.outputUnits;
    const lib::MachineDef* machine = job.machine;

    // ---- title and facts ----
    h.raw(newPage ? "<h1 style=\"page-break-before: always\">" : "<h1>");
    h.raw("Setup sheet: ");
    h.text(setup.name.empty() ? std::string("Setup") : setup.name);
    h.line("</h1>");
    h.raw("<p class=\"sub\">Program <b>");
    h.raw(std::to_string(setup.programNumber));
    h.raw("</b>");
    if (!setup.programComment.empty()) {
        h.raw(" &#8212; ");
        h.text(setup.programComment);
    }
    h.raw(", work offset <b>");
    h.raw(sheet::workOffsetCode(setup.wcs.workOffset));
    h.raw("</b>, ");
    h.raw(units == UnitSystem::Metric ? "millimetres (G21)" : "inches (G20)");
    h.line("</p>");

    h.line("<table class=\"facts\" width=\"100%\" cellspacing=\"0\" cellpadding=\"2\" border=\"0\">");
    std::string machineText;
    if (machine != nullptr) {
        machineText = escapeHtml(machine->name);
        std::string model = machine->vendor;
        if (!machine->model.empty())
            model += (model.empty() ? "" : " ") + machine->model;
        if (!model.empty() && model != machine->name)
            machineText += " (" + escapeHtml(model) + ")";
    }
    std::string postText;
    if (in.post != nullptr) {
        postText = escapeHtml(in.post->title.empty() ? in.post->id : in.post->title);
        if (!in.post->id.empty() && in.post->id != in.post->title)
            postText += " (" + escapeHtml(in.post->id) + ")";
    } else if (!setup.postId.empty()) {
        postText = escapeHtml(setup.postId);
    }
    factRow(h, "Program number", std::to_string(setup.programNumber), "Machine", machineText);
    factRow(h, "Program name", escapeHtml(setup.programComment), "Post", postText);
    factRow(h, "Part", escapeHtml(in.partName), "Units",
            units == UnitSystem::Metric ? "Millimetres (G21)" : "Inches (G20)");
    factRow(h, "File", escapeHtml(job.documentName), "Date", job.fixedDate ? escapeHtml(*job.fixedDate) : "");
    if (!job.userName.empty())
        factRow(h, "Setup", escapeHtml(setup.name), "Programmer", escapeHtml(job.userName));
    else
        factRow(h, "Setup", escapeHtml(setup.name), "", "");
    if (in.post != nullptr && !in.post->verifiedOn.empty()) {
        std::string proven;
        for (const std::string& v : in.post->verifiedOn)
            proven += (proven.empty() ? "" : "; ") + escapeHtml(v);
        factRow(h, "Post proven on", proven, "", "");
    }
    h.line("</table>");

    // ---- the tools, by number (needed for the notice too) ----
    std::map<int, ToolUse> tools;
    for (std::size_t i = 0; i < job.sections.size(); ++i) {
        const PostSection& s = job.sections[i];
        if (s.op == nullptr)
            continue;
        ToolUse& use = tools[s.op->tool.toolNumber];
        use.number = s.op->tool.toolNumber;
        use.sections.push_back(i);
    }

    // ---- the notice ----
    std::vector<std::string> notices;
    const bool postUnproven = in.post != nullptr && in.postShipped && in.post->verifiedOn.empty();
    const bool machineUnchecked = machine == nullptr || !machine->verified;
    const std::string machineName = machine != nullptr && !machine->name.empty() ? escapeHtml(machine->name) : "none";
    if (postUnproven && machineUnchecked)
        notices.push_back("This post has not been proven on a real machine, and the machine definition (" +
                          machineName + ") has not been checked against one.");
    else if (postUnproven)
        notices.push_back("This post has not been proven on a real machine.");
    else if (machineUnchecked)
        notices.push_back("The machine definition (" + machineName +
                          ") has not been checked against the real machine.");
    for (const std::string& n : jobNotices)
        notices.push_back(n);
    if (in.post == nullptr)
        notices.push_back("No post was chosen for this setup: the program this sheet describes may not exist yet.");
    for (const auto& [number, use] : tools) {
        std::set<std::string> names;
        std::set<std::string> ids;
        for (std::size_t i : use.sections) {
            const lib::Tool* t = job.sections[i].tool;
            if (t != nullptr) {
                ids.insert(t->id.value.toString());
                names.insert(t->name);
            }
        }
        if (ids.size() > 1) {
            std::string list;
            for (const std::string& n : names)
                list += (list.empty() ? "" : ", ") + escapeHtml(n);
            notices.push_back("Tool number T" + std::to_string(number) +
                              " is given to different tools in this program (" + list +
                              "): the machine holds one tool per pocket.");
        }
    }
    if (!notices.empty()) {
        h.line("<p></p>");
        h.line("<table class=\"notice\" width=\"100%\" border=\"2\" cellspacing=\"0\" cellpadding=\"5\"><tr><td>");
        for (const std::string& n : notices)
            h.line("<p><b>" + n + "</b></p>");
        if (postUnproven || machineUnchecked)
            h.line("<p>Dry-run the program before cutting: single block, feed hold in reach, above the part (an air "
                   "cut) or without a tool. Check the work offsets, tool lengths and tool changes.</p>");
        h.line("</td></tr></table>");
    }

    // ---- work offset, stock and the picture ----
    const Box3& b = in.stock;
    h.line("<h2>Work offset and stock</h2>");
    h.line("<table width=\"100%\" cellspacing=\"0\" cellpadding=\"0\" border=\"0\"><tr>");
    // The picture as large as its column allows on both Letter and A4 (A4's text is about 698 CSS px wide), the facts
    // beside it.
    h.raw("<td class=\"picture\" width=\"440\">");
    h.raw("<table class=\"picture\" border=\"1\" cellspacing=\"0\" cellpadding=\"0\"><tr><td>");
    if (!in.fixtureImagePng.empty()) {
        h.raw("<img src=\"data:image/png;base64,");
        h.raw(sheet::base64(in.fixtureImagePng));
        h.raw("\" width=\"432\" height=\"288\" alt=\"The part in this setup\">");
    } else {
        h.raw("<p>No picture.</p>");
    }
    h.line("</td></tr></table></td>");
    h.line("<td>");
    h.line("<table class=\"facts\" width=\"100%\" cellspacing=\"0\" cellpadding=\"2\" border=\"0\">");
    const auto fact = [&h](std::string_view label, const std::string& value) {
        h.raw("<tr><td class=\"label\">");
        h.raw(label);
        h.raw("</td></tr><tr><td class=\"value\">");
        h.raw(value.empty() ? std::string(kDash) : value);
        h.line("</td></tr>");
    };
    fact("Work offset", sheet::workOffsetCode(setup.wcs.workOffset));
    fact("Origin (X0 Y0 Z0)", sheet::originWords(setup.wcs));
    fact("Stock, in work coordinates", "X " + lengthNumberMm(b.min.x, units) + " to " + lengthNumberMm(b.max.x, units) +
                                           "<br>Y " + lengthNumberMm(b.min.y, units) + " to " +
                                           lengthNumberMm(b.max.y, units) + "<br>Z " + lengthNumberMm(b.min.z, units) +
                                           " to " + lengthNumberMm(b.max.z, units) + unitName(units));
    if (setup.wcs.flipX || setup.wcs.flipZ) {
        std::string flips;
        if (setup.wcs.flipX)
            flips = "X and Y turned round (X flipped)";
        if (setup.wcs.flipZ)
            flips += std::string(flips.empty() ? "" : "; ") + "Z flipped";
        fact("Axes", flips);
    }
    fact("Stock", stockShapeWords(setup.stock, b, units));
    std::string material;
    if (in.material != nullptr) {
        material = escapeHtml(in.material->name);
        if (!in.material->group.empty() && in.material->group != in.material->name)
            material += " (" + escapeHtml(in.material->group) + ")";
        if (in.material->hardnessHb > 0)
            material += ", " + plainNumber(in.material->hardnessHb, 0) + " HB";
    }
    fact("Material", material.empty() ? std::string("Not chosen") : material);
    h.line("</table></td></tr></table>");

    // ---- tools ----
    h.line("<h2>Tools</h2>");
    h.line("<table class=\"grid\" width=\"100%\" border=\"1\" cellspacing=\"0\" cellpadding=\"2\">");
    h.line("<thead><tr><th>Tool</th><th>Description</th><th>Diameter</th><th>Flutes</th><th>Stick-out</th>"
           "<th>Holder</th><th>Offsets</th><th>Used by</th></tr></thead>");
    h.line("<tbody>");
    for (const auto& [number, use] : tools) {
        const PostSection& first = job.sections[use.sections.front()];
        const lib::Tool* tool = first.tool;
        std::set<Nm> stickOuts;
        std::set<std::pair<int, int>> offsets;
        std::vector<std::string> holders;
        std::string usedBy;
        for (std::size_t i : use.sections) {
            const PostSection& s = job.sections[i];
            if (s.op->tool.stickOut > 0) {
                if (const auto nm = snapMm(s.op->tool.stickOut))
                    stickOuts.insert(*nm);
            }
            offsets.insert({s.op->tool.lengthOffset, s.op->tool.diameterOffset});
            const std::string holder = s.holder != nullptr ? escapeHtml(s.holder->name) : std::string();
            if (std::find(holders.begin(), holders.end(), holder) == holders.end())
                holders.push_back(holder);
            usedBy += (usedBy.empty() ? "" : "<br>") + std::to_string(i + 1) + ". " + escapeHtml(s.op->name);
        }
        h.raw("<tr><td><b>T" + std::to_string(number) + "</b></td><td>");
        if (tool != nullptr) {
            h.raw("<b>");
            h.text(tool->name.empty() ? std::string("Unnamed tool") : tool->name);
            h.raw("</b><br>");
            h.raw(toolTypeWords(*tool, units));
            std::string vendor = tool->vendor;
            if (!tool->productId.empty())
                vendor += (vendor.empty() ? "" : " ") + tool->productId;
            if (!vendor.empty()) {
                h.raw("<br>");
                h.text(vendor);
            }
            h.raw("</td><td>");
            h.raw(lengthMm(tool->diameter, units));
            h.raw("</td><td>");
            h.raw(tool->flutes > 0 ? std::to_string(tool->flutes) : std::string(kDash));
        } else {
            h.raw("Not in the document</td><td>");
            h.raw(kDash);
            h.raw("</td><td>");
            h.raw(kDash);
        }
        h.raw("</td><td>");
        if (stickOuts.empty()) {
            h.raw("Not set");
        } else {
            std::string list;
            for (Nm s : stickOuts)
                list += (list.empty() ? "" : ", ") + lengthNumber(s, units);
            h.raw(list + unitName(units));
        }
        h.raw("</td><td>");
        std::string holderText;
        for (const std::string& holder : holders)
            holderText += (holderText.empty() ? "" : "<br>") + (holder.empty() ? std::string("None chosen") : holder);
        h.raw(holderText);
        h.raw("</td><td>");
        std::string offsetText;
        for (const auto& [len, dia] : offsets)
            offsetText += (offsetText.empty() ? "" : "<br>") + ("H" + std::to_string(len) + " D" + std::to_string(dia));
        h.raw(offsetText);
        h.raw("</td><td>");
        h.raw(usedBy);
        h.line("</td></tr>");
    }
    h.line("</tbody></table>");

    // ---- operations ----
    h.line("<h2>Operations</h2>");
    h.line("<table class=\"grid\" width=\"100%\" border=\"1\" cellspacing=\"0\" cellpadding=\"2\">");
    h.line("<thead><tr><th>#</th><th>Operation</th><th>Tool</th><th>Deepest Z</th><th>Spindle</th>"
           "<th>Feed</th><th>Stock to leave</th><th>Est. time</th></tr></thead>");
    h.line("<tbody>");
    for (std::size_t i = 0; i < job.sections.size(); ++i) {
        const PostSection& s = job.sections[i];
        if (s.op == nullptr)
            continue;
        const cam::Operation& op = *s.op;
        const auto params = topLevel(op.paramsJson);
        h.raw("<tr><td>" + std::to_string(i + 1) + "</td><td><b>");
        h.text(op.name.empty() ? std::string("Operation") : op.name);
        h.raw("</b><br>");
        h.raw(operationWords(op, params));
        if (s.toolpath != nullptr && op.type == "drill") {
            const std::size_t holes = holeCount(*s.toolpath);
            h.raw(", " + std::to_string(holes) + (holes == 1 ? " hole" : " holes"));
        }
        h.raw("</td><td><b>T" + std::to_string(op.tool.toolNumber) + "</b>");
        if (s.tool != nullptr)
            h.raw("<br>&#216;" + lengthNumberMm(s.tool->diameter, units));
        h.raw("</td><td>");
        const std::optional<Nm> low = s.toolpath != nullptr ? deepestZ(*s.toolpath) : std::nullopt;
        h.raw(low ? "Z " + lengthNumber(*low, units) : std::string(kDash));
        for (const char* key : {"stepdown", "stepDown"}) {
            if (const auto d = numberOf(params, key); d && *d > 0) {
                h.raw("<br>stepdown " + lengthNumberMm(*d, units));
                break;
            }
        }
        if (const auto peck = numberOf(params, "peck"); peck && *peck > 0 && op.type == "drill")
            h.raw("<br>peck " + lengthNumberMm(*peck, units));
        h.raw("</td><td>");
        if (op.cutting.spindleRpm > 0)
            h.raw(plainNumber(op.cutting.spindleRpm, 0) + " rpm " + (op.cutting.clockwise ? "CW" : "CCW"));
        else
            h.raw("Spindle off");
        h.raw("<br>");
        h.raw(coolantWords(op.cutting.coolant));
        h.raw("</td><td>");
        std::string feeds;
        if (op.cutting.feedCut > 0)
            feeds = "cut " + feedText(op.cutting.feedCut, units);
        if (op.cutting.feedPlunge > 0 && op.cutting.feedPlunge != op.cutting.feedCut)
            feeds += (feeds.empty() ? "" : "<br>") + ("plunge " + feedText(op.cutting.feedPlunge, units));
        if (s.tool != nullptr && !isHoleTool(s.tool->type) && s.tool->flutes > 0 && op.cutting.spindleRpm > 0 &&
            op.cutting.feedCut > 0) {
            const double perTooth = op.cutting.feedCut / (op.cutting.spindleRpm * s.tool->flutes);
            if (const auto nm = snapMm(perTooth))
                feeds += "<br>" + lengthNumber(*nm, units) + unitName(units) + " per tooth";
        }
        h.raw(feeds.empty() ? std::string(kDash) : feeds);
        h.raw("</td><td>");
        std::optional<double> wall = numberOf(params, "stockToLeaveWall");
        if (!wall)
            wall = numberOf(params, "stockToLeave");
        std::optional<double> floor = numberOf(params, "stockToLeaveFloor");
        if (!floor)
            floor = numberOf(params, "stockToLeaveAxial");
        if (!floor)
            floor = numberOf(params, "floorStock");
        std::string leave;
        if (wall)
            leave = "walls " + lengthNumberMm(*wall, units);
        if (floor)
            leave += (leave.empty() ? "" : "<br>") + ("floor " + lengthNumberMm(*floor, units));
        h.raw(leave.empty() ? std::string(kDash) : leave);
        h.raw("</td><td>");
        const std::string t = i < in.time.operations.size() ? sheet::duration(in.time.operations[i].totalSec) : "";
        h.raw(t.empty() ? std::string(kDash) : t);
        h.line("</td></tr>");
        if (s.toolpath != nullptr) {
            for (const std::string& w : s.toolpath->warnings) {
                h.raw("<tr><td></td><td class=\"warning\" colspan=\"7\">Warning: ");
                h.text(w);
                h.line("</td></tr>");
            }
        }
    }
    h.line("</tbody></table>");

    // ---- cycle time ----
    h.line("<h2>Cycle time</h2>");
    const std::string total = sheet::duration(in.time.totalSec);
    if (job.sections.empty() || total.empty()) {
        h.line("<p>No estimate.</p>");
    } else {
        h.line("<p class=\"total\">Estimated cycle time " + total + "</p>");
        std::string parts =
            "Cutting " + sheet::duration(in.time.cutSec) + ", rapids " + sheet::duration(in.time.rapidSec);
        if (in.time.dwellSec > 0)
            parts += ", dwells " + sheet::duration(in.time.dwellSec);
        if (in.time.toolChanges > 0)
            parts += ", " + std::to_string(in.time.toolChanges) +
                     (in.time.toolChanges == 1 ? " tool change " : " tool changes ") +
                     sheet::duration(in.time.toolChangeSec);
        h.line("<p>" + parts + ".</p>");
        h.line("<p>An estimate from the toolpaths and the machine definition: the real time depends on the control's "
               "acceleration and look-ahead and on the feed and rapid overrides. Time the first part.</p>");
        for (const std::string& note : in.time.notes) {
            h.raw("<p class=\"small\">");
            h.text(note);
            h.line("</p>");
        }
    }

    // ---- sign-off ----
    h.line("<h2>Sign-off</h2>");
    h.line("<table class=\"grid\" width=\"100%\" border=\"1\" cellspacing=\"0\" cellpadding=\"2\">");
    h.line("<tr><th width=\"34%\">Set up by</th><th width=\"33%\">Date</th><th width=\"33%\">First part checked by</th>"
           "</tr>");
    h.line("<tr><td class=\"sign\">&#160;</td><td class=\"sign\">&#160;</td><td class=\"sign\">&#160;</td></tr>");
    h.line("</table>");
    h.raw("<p class=\"small\">Made with RayCAD");
    if (job.fixedDate) {
        h.raw(" on ");
        h.text(*job.fixedDate);
    }
    h.line(". Check this sheet against the program before running it.</p>");
}

Status checkInput(const SetupSheetInput& in)
{
    if (in.job == nullptr || in.job->setup == nullptr)
        return tl::unexpected(Error{ErrorCode::InvalidArgument, "The setup sheet has no post job or setup", {}});
    for (const PostSection& s : in.job->sections) {
        if (s.op == nullptr)
            return tl::unexpected(
                Error{ErrorCode::InvalidArgument, "A section of the setup sheet has no operation", {}});
    }
    return std::monostate{};
}

void documentStart(Html& h, const std::string& title)
{
    h.line("<!DOCTYPE html>");
    h.line("<html lang=\"en\">");
    h.line("<head>");
    h.line("<meta charset=\"utf-8\">");
    h.line("<meta name=\"generator\" content=\"RayCAD\">");
    h.raw("<title>");
    h.text(title);
    h.line("</title>");
    h.raw("<style>\n");
    h.raw(sheet::styleSheet());
    h.line("</style>");
    h.line("</head>");
    h.line("<body>");
}

void documentEnd(Html& h)
{
    h.line("</body>");
    h.line("</html>");
}

} // namespace

Result<std::string> renderSetupSheetHtml(const SetupSheetInput& in)
{
    if (auto ok = checkInput(in); !ok)
        return tl::unexpected(ok.error());
    Html h;
    std::string title = "Setup sheet: " + in.job->setup->name;
    if (!in.job->documentName.empty())
        title += " (" + in.job->documentName + ")";
    documentStart(h, title);
    writeSheet(h, in, false, {});
    documentEnd(h);
    return std::move(h.str());
}

Result<std::string> renderSetupSheetsHtml(std::span<const SetupSheetInput> inputs)
{
    if (inputs.empty())
        return tl::unexpected(Error{ErrorCode::InvalidArgument, "No setup to make a sheet of", {}});
    for (const SetupSheetInput& in : inputs) {
        if (auto ok = checkInput(in); !ok)
            return tl::unexpected(ok.error());
    }
    Html h;
    std::string title = "Setup sheets";
    if (!inputs.front().job->documentName.empty())
        title += ": " + inputs.front().job->documentName;
    documentStart(h, title);
    // Two setups of one job with the same program number: the control keeps one program per number.
    std::map<int, std::vector<std::size_t>> byNumber;
    for (std::size_t i = 0; i < inputs.size(); ++i)
        byNumber[inputs[i].job->setup->programNumber].push_back(i);
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        std::vector<std::string> notices;
        const int number = inputs[i].job->setup->programNumber;
        const std::vector<std::size_t>& same = byNumber[number];
        if (same.size() > 1) {
            std::string others;
            for (std::size_t j : same) {
                if (j != i)
                    others += (others.empty() ? "" : ", ") + escapeHtml(inputs[j].job->setup->name);
            }
            notices.push_back("Program number " + std::to_string(number) + " is also the number of " + others +
                              " in this job: the control keeps one program per number, so give each setup its own.");
        }
        writeSheet(h, inputs[i], i > 0, notices);
    }
    documentEnd(h);
    return std::move(h.str());
}

} // namespace ray::post
