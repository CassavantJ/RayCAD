// A post run after the script is loaded: the ProgramContext and Section objects, the handler calls in program order,
// and the move objects of every event (post_api.d.ts). Values reach the script in output units (inch or mm),
// converted from integer nanometres by one division; the script hands them back to `out`, which snaps them to
// integers again (format.h).
#include "engine_program.h"

#include "format.h"

#include <algorithm>
#include <cmath>

namespace ray::post::detail {
namespace {

// A JSValue freed at scope exit.
class Owned {
public:
    Owned(JSContext* ctx, JSValue v) : ctx_(ctx), v_(v) {}
    ~Owned() { JS_FreeValue(ctx_, v_); }
    Owned(const Owned&) = delete;
    Owned& operator=(const Owned&) = delete;
    JSValue get() const { return v_; }

private:
    JSContext* ctx_;
    JSValue v_;
};

void setNum(JSContext* c, JSValueConst obj, const char* key, double v)
{
    JS_DefinePropertyValueStr(c, obj, key, JS_NewFloat64(c, v), JS_PROP_C_W_E);
}
void setInt(JSContext* c, JSValueConst obj, const char* key, std::int64_t v)
{
    JS_DefinePropertyValueStr(c, obj, key, JS_NewFloat64(c, static_cast<double>(v)), JS_PROP_C_W_E);
}
void setBool(JSContext* c, JSValueConst obj, const char* key, bool v)
{
    JS_DefinePropertyValueStr(c, obj, key, JS_NewBool(c, v), JS_PROP_C_W_E);
}
void setStr(JSContext* c, JSValueConst obj, const char* key, std::string_view v)
{
    JS_DefinePropertyValueStr(c, obj, key, JS_NewStringLen(c, v.data(), v.size()), JS_PROP_C_W_E);
}
void setVal(JSContext* c, JSValueConst obj, const char* key, JSValue v)
{
    JS_DefinePropertyValueStr(c, obj, key, v, JS_PROP_C_W_E);
}

std::string_view toolTypeName(lib::ToolType t)
{
    switch (t) {
    case lib::ToolType::FlatEndMill:
        return "flatEndMill";
    case lib::ToolType::BallEndMill:
        return "ballEndMill";
    case lib::ToolType::BullEndMill:
        return "bullEndMill";
    case lib::ToolType::ChamferMill:
        return "chamferMill";
    case lib::ToolType::FaceMill:
        return "faceMill";
    case lib::ToolType::SpotDrill:
        return "spotDrill";
    case lib::ToolType::Drill:
        return "drill";
    case lib::ToolType::Tap:
        return "tap";
    case lib::ToolType::Reamer:
        return "reamer";
    case lib::ToolType::BoringBar:
        return "boringBar";
    case lib::ToolType::ThreadMill:
        return "threadMill";
    case lib::ToolType::EngraveCutter:
        return "engraveCutter";
    }
    return "flatEndMill";
}

std::string_view coolantName(cam::CuttingData::Coolant c)
{
    switch (c) {
    case cam::CuttingData::Coolant::Off:
        return "off";
    case cam::CuttingData::Coolant::Flood:
        return "flood";
    case cam::CuttingData::Coolant::Mist:
        return "mist";
    case cam::CuttingData::Coolant::ThroughTool:
        return "through";
    case cam::CuttingData::Coolant::Air:
        return "air";
    }
    return "off";
}

JSValue toolInfo(JSContext* c, const Units& u, const PostSection& s)
{
    JSValue t = JS_NewObject(c);
    const cam::ToolRef& ref = s.op->tool;
    setInt(c, t, "number", ref.toolNumber);
    setInt(c, t, "lengthOffset", ref.lengthOffset);
    setInt(c, t, "diameterOffset", ref.diameterOffset);
    setStr(c, t, "type", toolTypeName(s.tool->type));
    setNum(c, t, "diameter", u.mm(s.tool->diameter));
    setNum(c, t, "cornerRadius", u.mm(s.tool->cornerRadius));
    setNum(c, t, "fluteLength", u.mm(s.tool->fluteLength));
    setInt(c, t, "flutes", s.tool->flutes);
    setStr(c, t, "description", s.tool->name);
    setStr(c, t, "holderDescription", s.holder ? std::string_view(s.holder->name) : std::string_view());
    setNum(c, t, "stickOut", u.mm(ref.stickOut));
    setStr(c, t, "threadSpec", s.tool->threadSpec);
    return t;
}

JSValue point(JSContext* c, const Units& u, Nm x, Nm y, Nm z)
{
    JSValue p = JS_NewObject(c);
    setNum(c, p, "x", u.len(x));
    setNum(c, p, "y", u.len(y));
    setNum(c, p, "z", u.len(z));
    return p;
}

JSValue propertyValue(JSContext* c, const PropertyValue& v)
{
    if (const bool* b = std::get_if<bool>(&v)) {
        return JS_NewBool(c, *b);
    }
    if (const double* d = std::get_if<double>(&v)) {
        return JS_NewFloat64(c, *d);
    }
    const std::string& s = std::get<std::string>(v);
    return JS_NewStringLen(c, s.data(), s.size());
}

// The handler calls of one section's moves.
class ScriptSink final : public MotionSink {
public:
    ScriptSink(Session& s, const Units& u, std::uint32_t section, JobContext& job, std::size_t movesBefore,
               std::size_t totalMoves)
        : s_(s), c_(s.context()), u_(u), section_(section), job_(job), movesBefore_(movesBefore),
          totalMoves_(std::max<std::size_t>(totalMoves, 1))
    {
    }

    bool onEvent(const MotionEvent& e) override
    {
        if ((++events_ & 1023) == 0) {
            // The interrupt handler also sees a cancel, but only while script code runs; check between calls too.
            if (job_.cancel.cancelled()) {
                s_.setStop(Stop::Cancel);
                s_.setHostError(Error{ErrorCode::Cancelled, s_.scriptName() + ": posting was cancelled", {}});
                return false;
            }
            job_.progress.set(static_cast<double>(movesBefore_ + e.move) / static_cast<double>(totalMoves_), "Posting");
        }
        s_.writer().enterMove(section_, e.move);
        const bool ok = dispatch(e);
        s_.writer().leaveMove();
        return ok;
    }

private:
    using A = Session::Atom;

    bool callWith(Handler h, JSValue obj, bool required)
    {
        JSValueConst argv[2] = {obj, s_.out()};
        const bool ok = s_.call(h, 2, argv, required);
        JS_FreeValue(c_, obj);
        return ok;
    }

    bool dispatch(const MotionEvent& e)
    {
        std::array<JSAtom, 16> atoms{};
        std::array<JSValue, 16> values{};
        int n = 0;
        auto put = [&](A a, JSValue v) {
            atoms[static_cast<std::size_t>(n)] = s_.atom(a);
            values[static_cast<std::size_t>(n)] = v;
            ++n;
        };
        auto num = [&](A a, double v) {
            put(a, JS_NewFloat64(c_, v));
        };
        auto object = [&] {
            return JS_NewObjectFrom(c_, n, atoms.data(), values.data());
        };
        switch (e.kind) {
        case EventKind::Rapid:
            num(A::aX, u_.len(e.x));
            num(A::aY, u_.len(e.y));
            num(A::aZ, u_.len(e.z));
            return callWith(kOnRapid, object(), true);
        case EventKind::Linear:
            num(A::aX, u_.len(e.x));
            num(A::aY, u_.len(e.y));
            num(A::aZ, u_.len(e.z));
            num(A::aFeed, u_.feed(e.feed));
            put(A::aRole, s_.roleString(e.role));
            put(A::aComp, JS_NewInt32(c_, e.comp));
            return callWith(kOnLinear, object(), true);
        case EventKind::Arc:
            num(A::aX, u_.len(e.x));
            num(A::aY, u_.len(e.y));
            num(A::aZ, u_.len(e.z));
            put(A::aCw, JS_NewBool(c_, e.cw));
            put(A::aPlane, s_.planeString(e.plane));
            num(A::aCx, u_.len(e.cx));
            num(A::aCy, u_.len(e.cy));
            num(A::aCz, u_.len(e.cz));
            num(A::aI, u_.len(e.i));
            num(A::aJ, u_.len(e.j));
            num(A::aK, u_.len(e.k));
            num(A::aRadius, u_.lenD(e.radiusNm));
            num(A::aSweep, e.sweepDegrees);
            num(A::aFeed, u_.feed(e.feed));
            put(A::aComp, JS_NewInt32(c_, e.comp));
            return callWith(kOnArc, object(), true);
        case EventKind::Cycle:
            put(A::aKind, s_.cycleString(e.cycle));
            num(A::aX, u_.len(e.x));
            num(A::aY, u_.len(e.y));
            num(A::aTop, u_.len(e.top));
            num(A::aBottom, u_.len(e.z));
            num(A::aRetract, u_.len(e.retract));
            num(A::aClearance, u_.len(e.clearance));
            put(A::aReturnLevel, s_.returnLevelString(e.returnLevel));
            num(A::aFeed, u_.feed(e.feed));
            num(A::aPeck, u_.len(e.peck));
            num(A::aPeckClearance, u_.len(e.peckClearance));
            num(A::aDwell, static_cast<double>(e.dwellMs) / 1000.0);
            num(A::aPitch, u_.len(e.pitchNm));
            put(A::aLeftHand, JS_NewBool(c_, e.leftHand));
            put(A::aFirst, JS_NewBool(c_, e.first));
            return callWith(kOnCycle, object(), true);
        case EventKind::CycleEnd: {
            JSValueConst argv[1] = {s_.out()};
            return s_.call(kOnCycleEnd, 1, argv, false);
        }
        case EventKind::Dwell:
            return callWith(kOnDwell, JS_NewFloat64(c_, static_cast<double>(e.dwellMs) / 1000.0), true);
        }
        return true;
    }

    Session& s_;
    JSContext* c_;
    const Units& u_;
    std::uint32_t section_;
    JobContext& job_;
    std::size_t movesBefore_;
    std::size_t totalMoves_;
    std::uint64_t events_ = 0;
};

} // namespace

Result<PostOutput> runProgram(Session& s, const Declaration& decl, const PostJob& job, JobContext& jobCtx,
                              const ProgramPlan& plan)
{
    JSContext* c = s.context();
    const Units& u = plan.units;
    const auto sectionCount = job.sections.size();

    for (const std::string& w : plan.warnings) {
        s.writer().warn(w);
    }

    // ---------- ToolInfo, Section and ProgramContext objects ----------
    Owned sections(c, JS_NewArray(c));
    Owned tools(c, JS_NewArray(c));
    std::vector<int> toolNumbers; // first-use order
    for (std::size_t i = 0; i < sectionCount; ++i) {
        const PostSection& ps = job.sections[i];
        const SectionStats& st = plan.stats[i];
        const int number = ps.op->tool.toolNumber;
        JSValue sec = JS_NewObject(c);
        setInt(c, sec, "index", static_cast<std::int64_t>(i));
        setStr(c, sec, "name", ps.op->name);
        setStr(c, sec, "opType", ps.op->type);
        setVal(c, sec, "tool", toolInfo(c, u, ps));
        setBool(c, sec, "toolChanged", plan.toolChanged[i]);
        JSValue next = JS_NULL;
        for (std::size_t k = i + 1; k < sectionCount; ++k) {
            if (job.sections[k].op->tool.toolNumber != number) {
                next = toolInfo(c, u, job.sections[k]);
                break;
            }
        }
        setVal(c, sec, "nextTool", next);
        setNum(c, sec, "rpm", ps.op->cutting.spindleRpm);
        setBool(c, sec, "clockwise", ps.op->cutting.clockwise);
        setStr(c, sec, "coolant", coolantName(ps.op->cutting.coolant));
        setVal(c, sec, "start", point(c, u, ps.toolpath->startX, ps.toolpath->startY, ps.toolpath->startZ));
        setNum(c, sec, "clearanceZ", u.mm(ps.op->heights.clearance));
        setNum(c, sec, "minZ", u.len(st.minZ));
        setNum(c, sec, "maxZ", u.len(st.maxZ));
        setNum(c, sec, "cutDistance", u.lenD(st.cutLengthNm));
        setNum(c, sec, "estimatedSeconds", st.seconds);
        JS_SetPropertyUint32(c, sections.get(), static_cast<std::uint32_t>(i), sec);
        if (std::find(toolNumbers.begin(), toolNumbers.end(), number) == toolNumbers.end()) {
            JS_SetPropertyUint32(c, tools.get(), static_cast<std::uint32_t>(toolNumbers.size()), toolInfo(c, u, ps));
            toolNumbers.push_back(number);
        }
    }

    Owned program(c, JS_NewObject(c));
    const JSValue p = program.get();
    setInt(c, p, "programNumber", job.setup->programNumber);
    setStr(c, p, "programComment", job.setup->programComment);
    setStr(c, p, "documentName", job.documentName);
    setStr(c, p, "setupName", job.setup->name);
    setStr(c, p, "userName", job.userName);
    setStr(c, p, "units", u.inch ? "inch" : "mm");
    setInt(c, p, "workOffset", job.setup->wcs.workOffset);
    setVal(c, p, "date", plan.date ? JS_NewStringLen(c, plan.date->data(), plan.date->size()) : JS_NULL);
    {
        const lib::MachineDef& m = *job.machine;
        JSValue machine = JS_NewObject(c);
        setStr(c, machine, "name", m.name);
        setStr(c, machine, "model", m.model);
        setNum(c, machine, "maxRpm", m.maxRpm);
        setBool(c, machine, "rigidTapping", m.rigidTapping);
        setBool(c, machine, "hasToolSetter", m.hasToolSetter);
        // T-040: the control's limits as the machine definition knows them (0 = not known) and its dialect.
        setStr(c, machine, "controlDialect", m.controlDialect);
        setInt(c, machine, "maxBlockNumber", m.maxBlockNumber);
        setInt(c, machine, "maxProgramNameLength", m.maxProgramNameLength);
        JSValue travel = JS_NewObject(c);
        for (const lib::AxisDef& a : m.axes) {
            JSValue range = JS_NewArray(c);
            JS_SetPropertyUint32(c, range, 0, JS_NewFloat64(c, u.mm(a.minTravel)));
            JS_SetPropertyUint32(c, range, 1, JS_NewFloat64(c, u.mm(a.maxTravel)));
            const char letter[2] = {a.letter, 0};
            setVal(c, travel, letter, range);
        }
        setVal(c, machine, "travel", travel);
        setVal(c, p, "machine", machine);
    }
    {
        JSValue props = JS_NewObject(c);
        for (const auto& [key, value] : plan.properties) {
            setVal(c, props, key.c_str(), propertyValue(c, value));
        }
        setVal(c, p, "properties", props);
    }
    setVal(c, p, "tools", JS_DupValue(c, tools.get()));
    setVal(c, p, "sections", JS_DupValue(c, sections.get()));

    // ---------- The program ----------
    auto failed = [&]() -> Result<PostOutput> {
        return tl::unexpected(s.error());
    };
    {
        JSValueConst argv[2] = {p, s.out()};
        if (!s.call(kOnProgramStart, 2, argv, false)) {
            return failed();
        }
    }
    std::size_t movesBefore = 0;
    for (std::size_t i = 0; i < sectionCount; ++i) {
        const PostSection& ps = job.sections[i];
        Owned sec(c, JS_GetPropertyUint32(c, sections.get(), static_cast<std::uint32_t>(i)));
        JSValueConst argv[3] = {sec.get(), p, s.out()};
        // Lines of onSectionStart map to the section's first move, those of onSectionEnd to its last (a tool change
        // or a move to the start point is where that section begins); program start and end lines are not mapped.
        const std::size_t moveCount = ps.toolpath->moves.size();
        s.writer().setToolChangeBlockPending(plan.toolChanged[i]);
        if (moveCount > 0) {
            s.writer().enterMove(static_cast<std::uint32_t>(i), 0);
        }
        const bool started = s.call(kOnSectionStart, 3, argv, false);
        s.writer().leaveMove();
        s.writer().setToolChangeBlockPending(false);
        if (!started) {
            return failed();
        }
        ScriptSink sink(s, u, static_cast<std::uint32_t>(i), jobCtx, movesBefore, plan.totalMoves);
        const Result<bool> moved = expandSection(*ps.toolpath, job.machine, decl.motion, sink);
        if (!moved) {
            Error e = moved.error();
            e.message = s.scriptName() + ": section " + std::to_string(i) + " (" + ps.op->name + "): " + e.message;
            return tl::unexpected(std::move(e));
        }
        if (!*moved) {
            return failed();
        }
        movesBefore += moveCount;
        if (moveCount > 0) {
            s.writer().enterMove(static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(moveCount - 1));
        }
        const bool ended = s.call(kOnSectionEnd, 3, argv, false);
        s.writer().leaveMove();
        if (!ended) {
            return failed();
        }
    }
    {
        JSValueConst argv[2] = {p, s.out()};
        if (!s.call(kOnProgramEnd, 2, argv, false)) {
            return failed();
        }
    }
    jobCtx.progress.set(1.0, "Posting");
    return s.writer().take();
}

} // namespace ray::post::detail
