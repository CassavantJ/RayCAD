// The build harness of the naming tests (task T-081a). See harness.h.
#include "harness.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <mutex>
#include <set>
#include <span>
#include <thread>
#include <tuple>

namespace ray::naming_oracle {

namespace {

naming::TopoRef withoutOutlineIfAsked(naming::TopoRef ref)
{
    // RAYCAD_NAMING_NO_OUTLINE: references as a build before TopoRef::outline made them (T-081b round 2: answers must
    // be those of that build).
    static const bool strip = std::getenv("RAYCAD_NAMING_NO_OUTLINE") != nullptr;
    if (strip)
        ref.outline = naming::PickOutline{};
    // RAYCAD_NAMING_NO_PLACEMENT: references without TopoRef::placements, as a build before round 4 made them (the
    // moved-face rule then guesses every move, as round 3 did: the before/after of the round-4 change).
    static const bool unplaced = std::getenv("RAYCAD_NAMING_NO_PLACEMENT") != nullptr;
    if (unplaced)
        ref.placements.clear();
    return ref;
}

// A sweep's sketch frame as a rigid transform, frame to model (naming.h NamingInput::placement).
Xform placementOf(const Frame& f)
{
    const Vec3 y = f.yd();
    return Xform{{f.xd.x, y.x, f.n.x, f.o.x, f.xd.y, y.y, f.n.y, f.o.y, f.xd.z, y.z, f.n.z, f.o.z}};
}

kernel::ProfileLoop toKernel(const Loop& loop)
{
    kernel::ProfileLoop out;
    out.tag = loop.id;
    for (const Ent& e : loop.ents) {
        kernel::ProfileCurve c;
        c.tag = e.tag;
        switch (e.k) {
        case 'L':
            c.kind = kernel::ProfileCurve::Kind::Line;
            c.pts = {{e.x0, e.y0}, {e.x1, e.y1}};
            break;
        case 'A':
            c.kind = kernel::ProfileCurve::Kind::Arc;
            c.pts = {{e.x0, e.y0}, {e.x1, e.y1}, {e.cx, e.cy}};
            c.ccw = true;
            break;
        default:
            c.kind = kernel::ProfileCurve::Kind::Circle;
            c.pts = {{e.cx, e.cy}};
            c.radius = e.r;
            c.ccw = true;
            break;
        }
        out.curves.push_back(std::move(c));
    }
    return out;
}

std::vector<std::uint32_t> indices(const std::vector<SubId>& subs)
{
    std::vector<std::uint32_t> out;
    for (const SubId& s : subs)
        out.push_back(s.index);
    return out;
}

Vec3 centroidOf(const ElemSamples& es)
{
    Vec3 sum{0, 0, 0};
    double w = 0;
    for (std::size_t i = 0; i < es.pts.size(); ++i) {
        sum = add(sum, mul(es.pts[i], es.w[i]));
        w += es.w[i];
    }
    return w > 0 ? mul(sum, 1.0 / w) : Vec3{};
}

const char* kindName(SubKind k)
{
    return k == SubKind::Face ? "face" : k == SubKind::Edge ? "edge" : "vertex";
}

const char* stateName(naming::ResolveState s)
{
    switch (s) {
    case naming::ResolveState::Exact:
        return "Exact";
    case naming::ResolveState::Split:
        return "Split";
    case naming::ResolveState::Merged:
        return "Merged";
    case naming::ResolveState::Repaired:
        return "Repaired";
    case naming::ResolveState::Broken:
        return "Broken";
    }
    return "?";
}

std::string list(const std::vector<std::uint32_t>& v)
{
    std::string s = "{";
    for (std::size_t i = 0; i < v.size(); ++i)
        s += (i ? " " : "") + std::to_string(v[i]);
    return s + "}";
}

// The roles of an element's names, with sibling information, for diagnosis.
std::string namesOf(const naming::ElementMap& map, SubId s)
{
    std::string out;
    for (const Hash256& n : map.namesOf(s)) {
        const naming::TopoName* r = map.record(n);
        if (r == nullptr)
            continue;
        out += (out.empty() ? "" : ", ") + r->role + "/d" + std::to_string(r->discriminator.size()) + "/o" +
               std::to_string(r->ordinal) + "/n" + std::to_string(r->siblingCount) + "#" + n.hex().substr(0, 6) + "(";
        for (const Hash256& p : r->parents) {
            const naming::TopoName* pr = map.record(p);
            out += (pr != nullptr ? pr->role : std::string("?")) + "#" + p.hex().substr(0, 6) + " ";
        }
        out += ")";
        if (!r->discriminator.empty()) {
            out += "<";
            for (const Hash256& d : r->discriminator) {
                const naming::TopoName* dr = map.record(d);
                out += (dr != nullptr ? dr->role : std::string("?")) + "#" + d.hex().substr(0, 6) + " ";
            }
            out += ">";
        }
    }
    return out;
}

} // namespace

FeatureId featureIdOf(const std::string& id)
{
    Hasher h;
    h.str("t081a.feature");
    h.str(id);
    const Hash256 hash = h.finish();
    FeatureId out;
    std::copy_n(hash.bytes.begin(), out.value.bytes.size(), out.value.bytes.begin());
    return out;
}

BodyId testBody()
{
    BodyId out;
    for (std::size_t i = 0; i < out.value.bytes.size(); ++i)
        out.value.bytes[i] = static_cast<std::uint8_t>(0xB0 + i);
    return out;
}

Answer answerOf(naming::ResolveState state)
{
    switch (state) {
    case naming::ResolveState::Exact:
        return Answer::Exact;
    case naming::ResolveState::Split:
        return Answer::Split;
    case naming::ResolveState::Merged:
        return Answer::Merged;
    case naming::ResolveState::Repaired:
        return Answer::Repaired;
    case naming::ResolveState::Broken:
        return Answer::Broken;
    }
    return Answer::Broken;
}

void Builder::record(naming::NamingOutput out, const kernel::Shape& shape)
{
    lineage_ += out.lineage.size();
    map_ = std::move(out.map);
    shape_ = shape;
    have_ = true;
    if (keepHistory_)
        history_.push_back(map_.serialize());
}

void Builder::apply(const Feature& f)
{
    if (f.suppressed)
        return;
    bool ok = false;
    switch (f.type) {
    case FType::Import:
        if (f.base) {
            map_ = naming::nameImported(featureIdOf(f.id), f.base->shape);
            shape_ = f.base->shape;
            have_ = true;
            if (keepHistory_)
                history_.push_back(map_.serialize());
            ok = true;
        }
        break;
    case FType::Sweep:
        ok = sweep(f);
        break;
    case FType::Fillet:
    case FType::Chamfer:
        ok = blend(f);
        break;
    case FType::Edit: {
        Feature measured = f;
        if (edit(measured)) {
            ++built_;
            builtIds_.insert(f.id);
            builtFeatures_.push_back(std::move(measured));
        }
        return;
    }
    }
    if (ok) {
        ++built_;
        builtIds_.insert(f.id);
        builtFeatures_.push_back(f);
    }
}

Script builtOnly(const Script& script, const Builder& b)
{
    Script out;
    for (const Feature& f : script)
        if (!f.suppressed && b.builtIds().count(f.id) != 0)
            out.push_back(f);
    return out;
}

bool Builder::sweep(const Feature& f)
{
    const FeatureId fid = featureIdOf(f.id);
    bool any = false;
    bool first = true;
    for (const Loop& loop : f.loops) {
        JobContext ctx;
        const kernel::Profile profile{kernel::Plane{f.frame.o, f.frame.n, f.frame.xd}, {toKernel(loop)}};
        auto tool = kernel::extrude(profile, f.op == BOp::Cut ? -f.dist : f.dist, 0.0, ctx);
        if (!tool) {
            failed_.push_back(f.id + " loop " + std::to_string(loop.id) + ": " + tool.error().message);
            continue;
        }
        // The sweep's placement (naming.h NamingInput::placement): its sketch frame, as the document's extrude gives.
        naming::NamingInput toolInput{fid, f.role, {}, nullptr, &tool->op.shape, tool->origins};
        toolInput.placement = placementOf(f.frame);
        auto toolNames = naming::propagate(toolInput);
        if (!toolNames) {
            failed_.push_back(f.id + ": naming the tool failed: " + toolNames.error().message);
            continue;
        }
        if (!have_ || (f.op == BOp::New && first)) {
            record(std::move(*toolNames), tool->op.shape);
            any = true;
            first = false;
            continue;
        }
        first = false;
        const kernel::BoolOp op = f.op == BOp::Cut ? kernel::BoolOp::Cut : kernel::BoolOp::Union;
        auto result = kernel::boolean(op, shape_, tool->op.shape, ctx);
        if (!result) {
            failed_.push_back(f.id + " loop " + std::to_string(loop.id) + ": " + result.error().message);
            continue;
        }
        const std::array<const naming::ElementMap*, 2> maps{&map_, &toolNames->map};
        const std::array<const kernel::Shape*, 2> shapes{&shape_, &tool->op.shape};
        naming::NamingInput input{fid, f.role, maps, &result->history, &result->shape, {}, std::span(shapes)};
        input.placement = toolInput.placement;
        auto named = naming::propagate(input);
        if (!named) {
            failed_.push_back(f.id + ": naming failed: " + named.error().message);
            continue;
        }
        record(std::move(*named), result->shape);
        any = true;
    }
    return any;
}

bool Builder::blend(const Feature& f)
{
    if (!have_)
        return false;
    const auto ownersAt = std::make_shared<const Owners>(buildOwners(builtFeatures_));
    const Owners& owners = *ownersAt;
    auto samples = sampleShape(shape_);
    if (!samples) {
        failed_.push_back(f.id + ": sampling failed: " + samples.error().message);
        return false;
    }
    auto resolver = naming::makeResolver({naming::ResolverBody{testBody(), map_, shape_}});
    std::set<std::uint32_t> edges;
    if (mode_ == Mode::Author) {
        auto& picks = refs_[f.id];
        picks.clear();
        for (std::size_t k = 0; k < f.targets.size(); ++k) {
            if (!f.targets[k].picked)
                continue;
            for (auto& [e, desc] : pickTarget(f.targets[k], owners, **samples)) {
                const SubId edge{SubKind::Edge, e};
                picks.push_back({k, withoutOutlineIfAsked(resolver->makeRef(testBody(), edge)),
                                 PickRecord{edge, desc, *samples, ownersAt}});
                edges.insert(e);
                if (!desc.ok)
                    ++unverifiablePicks_;
            }
        }
    } else {
        const auto it = refs_.find(f.id);
        if (it != refs_.end())
            for (const Pick& p : it->second) {
                const naming::Resolved r = resolver->resolve(p.ref);
                if (p.record.truth.ok)
                    picks_.push_back({f.id, p.target, p.record.base.index, r,
                                      judgeReference(p.record, owners, **samples, r, "pick: ")});
                else
                    ++unjudgedPicks_;
                if (r.usable())
                    for (const SubId& s : r.elements)
                        if (s.kind == SubKind::Edge)
                            edges.insert(s.index);
            }
    }
    if (edges.empty()) {
        failed_.push_back(f.id + ": no resolvable edge reference");
        return false;
    }
    std::vector<SubId> picked;
    for (const std::uint32_t e : edges)
        picked.push_back(SubId{SubKind::Edge, e});
    JobContext ctx;
    auto op = f.type == FType::Fillet ? kernel::fillet(shape_, picked, f.size, ctx)
                                      : kernel::chamfer(shape_, picked, f.size, ctx);
    if (!op) {
        failed_.push_back(f.id + ": " + op.error().message);
        return false;
    }
    const std::array<const naming::ElementMap*, 1> maps{&map_};
    const std::array<const kernel::Shape*, 1> shapes{&shape_};
    const naming::NamingInput input{featureIdOf(f.id), f.role, maps, &op->history, &op->shape, {}, std::span(shapes)};
    auto named = naming::propagate(input);
    if (!named) {
        failed_.push_back(f.id + ": naming failed: " + named.error().message);
        return false;
    }
    record(std::move(*named), op->shape);
    return true;
}

namespace {

// The time limit of one direct edit's kernel call (RAYCAD_NAMING_EDIT_LIMIT_S): 60 s, ten times that in a build
// without NDEBUG (unoptimised OpenCASCADE).
std::chrono::seconds editLimit()
{
    if (const char* text = std::getenv("RAYCAD_NAMING_EDIT_LIMIT_S"))
        if (const int s = std::atoi(text); s > 0)
            return std::chrono::seconds(s);
#ifdef NDEBUG
    return std::chrono::seconds(60);
#else
    return std::chrono::seconds(600);
#endif
}

// Runs `call` with a JobContext that is cancelled after `limit`; `timedOut` says whether it was.
Result<kernel::OpResult> withLimit(const std::function<Result<kernel::OpResult>(JobContext&)>& call,
                                   std::chrono::seconds limit, bool& timedOut)
{
    CancelSource source;
    JobContext ctx;
    ctx.cancel = source.token();
    std::mutex lock;
    std::condition_variable wake;
    bool done = false;
    timedOut = false;
    std::thread watchdog([&] {
        std::unique_lock<std::mutex> guard(lock);
        if (!wake.wait_for(guard, limit, [&] { return done; }))
            timedOut = source.cancel();
    });
    Result<kernel::OpResult> out = call(ctx);
    {
        const std::lock_guard<std::mutex> guard(lock);
        done = true;
    }
    wake.notify_all();
    watchdog.join();
    return out;
}

} // namespace

bool Builder::edit(Feature& f)
{
    if (!have_)
        return false;
    const bool pickNow = mode_ == Mode::Author || refs_.find(f.id) == refs_.end();
    auto fail = [&](const std::string& outcome, const std::string& message) {
        ++editCounts_[f.role + ": " + outcome];
        failed_.push_back(f.id + ": " + message);
        if (pickNow)
            refs_.erase(f.id);
        return false;
    };
    const auto ownersAt = std::make_shared<const Owners>(buildOwners(builtFeatures_));
    const Owners& owners = *ownersAt;
    auto samples = sampleShape(shape_);
    if (!samples)
        return fail("other", "sampling failed: " + samples.error().message);
    auto resolver = naming::makeResolver({naming::ResolverBody{testBody(), map_, shape_}});
    std::set<std::uint32_t> faces;
    if (f.edit.kind != EKind::Unify) {
        if (pickNow) {
            auto& picks = refs_[f.id];
            picks.clear();
            const TruthBuilder tb(owners, **samples);
            for (std::size_t k = 0; k < f.edit.targets.size(); ++k)
                for (const std::uint32_t face : facesOn(f.edit.targets[k], owners, **samples)) {
                    const SubId s{SubKind::Face, face};
                    const TruthDesc d = tb.describe(s);
                    if (!d.ok || d.cons.size() != 1 || d.cons[0].alts != std::vector<std::string>{f.edit.targets[k]})
                        return fail("no target face", "face " + std::to_string(face) + " of " + f.edit.targets[k] +
                                                          " is not described by it alone");
                    picks.push_back({k, withoutOutlineIfAsked(resolver->makeRef(testBody(), s)),
                                     PickRecord{s, d, *samples, ownersAt}});
                    faces.insert(face);
                }
            if (faces.empty())
                return fail("no target face", "no face of the edit's targets");
        } else {
            const auto& recorded = refs_.find(f.id)->second;
            bool exact = !recorded.empty();
            for (const Pick& p : recorded) {
                const naming::Resolved r = resolver->resolve(p.ref);
                if (p.record.truth.ok)
                    picks_.push_back({f.id, p.target, p.record.base.index, r,
                                      judgeReference(p.record, owners, **samples, r, "pick: ")});
                else
                    ++unjudgedPicks_;
                exact = exact && r.state == naming::ResolveState::Exact;
                for (const SubId& s : r.elements)
                    if (s.kind == SubKind::Face)
                        faces.insert(s.index);
            }
            if (!exact || faces.empty())
                return fail("pick not Exact", "a face it edits did not resolve Exact");
        }
    }
    std::vector<SubId> picked;
    for (const std::uint32_t s : faces)
        picked.push_back(SubId{SubKind::Face, s});
    const kernel::Shape input = shape_;
    const EditSpec spec = f.edit;
    bool timedOut = false;
    auto op = withLimit(
        [&](JobContext& ctx) -> Result<kernel::OpResult> {
            switch (spec.kind) {
            case EKind::Delete:
                return kernel::deleteFaces(input, picked, ctx);
            case EKind::Offset:
                return kernel::offsetFaces(input, picked, spec.amount, ctx);
            case EKind::Resize:
                return kernel::resizeHole(input, picked.front(), 2.0 * spec.amount, ctx);
            case EKind::Unify:
                break;
            }
            return kernel::unifySameDomain(input, ctx);
        },
        editLimit(), timedOut);
    if (!op) {
        if (timedOut) {
            ++editTimeouts_;
            return fail("time limit", "past the time limit: " + op.error().message);
        }
        const bool refused =
            op.error().code == ErrorCode::KernelFailure || op.error().code == ErrorCode::InvalidArgument;
        return fail(refused ? "kernel refused" : "other", op.error().message);
    }
    auto after = sampleShape(op->shape);
    if (!after)
        return fail("other", "sampling the result failed: " + after.error().message);
    if (std::getenv("RAYCAD_NAMING_EDIT_DEBUG") != nullptr) {
        std::string text = f.id + " " + f.role + " on faces";
        for (const SubId& s : picked)
            text += " " + std::to_string(s.index);
        text += "\n  input " + input.contentHash().hex().substr(0, 16) + " result " +
                op->shape.contentHash().hex().substr(0, 16);
        for (const std::string& w : op->warnings)
            text += "\n  warning: " + w;
        auto face = [](const Samples& smp, SubId s) {
            const ElemSamples& es = smp.of(s.kind)[s.index];
            const std::uint32_t i = s.index;
            char buf[160];
            std::snprintf(buf, sizeof buf, "%u[t%d n(%.2f %.2f %.2f) (%.2f %.2f %.2f)-(%.2f %.2f %.2f)]", i, es.type,
                          es.nrm.x, es.nrm.y, es.nrm.z, es.boxMin.x, es.boxMin.y, es.boxMin.z, es.boxMax.x, es.boxMax.y,
                          es.boxMax.z);
            return std::string(buf);
        };
        for (const kernel::HistoryEntry& e : op->history.entries) {
            if (e.source.kind != SubKind::Face &&
                (e.source.kind != SubKind::Edge || std::getenv("RAYCAD_NAMING_EDIT_DEBUG")[0] != '2'))
                continue;
            const bool same = e.relation == kernel::HistoryEntry::Relation::Modified && e.results.size() == 1 &&
                              e.results[0] == e.source;
            if (same && std::getenv("RAYCAD_NAMING_EDIT_DEBUG")[0] != '3')
                continue;
            text += "\n  " +
                    std::string(e.relation == kernel::HistoryEntry::Relation::Modified    ? "Modified "
                                : e.relation == kernel::HistoryEntry::Relation::Generated ? "Generated "
                                                                                          : "Deleted ") +
                    (e.source.kind == SubKind::Edge ? "edge " : "") + face(**samples, e.source) + " ->";
            for (const SubId& r : e.results)
                text += " " + face(**after, r);
        }
        std::printf("%s\n", text.c_str());
    }
    if (const std::string why = measureEdit(f, builtFeatures_, **samples, **after); !why.empty())
        return fail("not modelled (" + why.substr(0, why.find(':')) + ")",
                    "the oracle cannot describe the result: " + why);
    const std::array<const naming::ElementMap*, 1> maps{&map_};
    const std::array<const kernel::Shape*, 1> shapes{&shape_};
    const naming::NamingInput in{featureIdOf(f.id), f.role, maps, &op->history, &op->shape, {}, std::span(shapes)};
    auto named = naming::propagate(in);
    if (!named)
        return fail("other", "naming failed: " + named.error().message);
    record(std::move(*named), op->shape);
    ++editCounts_[f.role + ": applied"];
    return true;
}

Builder buildScript(const Script& script, Mode mode, FeatRefs& refs, bool keepHistory)
{
    Builder b(mode, refs);
    b.keepHistory(keepHistory);
    for (const Feature& f : script)
        b.apply(f);
    return b;
}

std::vector<Ref> recordReferences(const Builder& b, std::shared_ptr<const Owners> owners,
                                  std::shared_ptr<const Samples> samples, std::map<std::string, int>* skipped)
{
    std::vector<Ref> refs;
    if (!b.have())
        return refs;
    const auto builder = std::make_shared<const TruthBuilder>(*owners, *samples);
    const TruthBuilder& tb = *builder;
    auto resolver = naming::makeResolver({naming::ResolverBody{testBody(), b.map(), b.shape()}});
    for (const SubKind kind : {SubKind::Face, SubKind::Edge, SubKind::Vertex}) {
        const auto& elems = samples->of(kind);
        for (std::uint32_t i = 0; i < elems.size(); ++i) {
            const SubId s{kind, i};
            const TruthDesc d = tb.describe(s);
            if (!d.ok) {
                if (skipped != nullptr)
                    ++(*skipped)[std::string(kindName(kind)) + ": " + d.why];
                continue;
            }
            // A sliver (no sample more than 0.02 mm inside it): which element of an edited shape holds it cannot be
            // told by position (oracle.h, isSliver), so the oracle cannot vouch for it.
            if (isSliver(samples->of(kind)[i], kind)) {
                if (skipped != nullptr)
                    ++(*skipped)[std::string(kindName(kind)) + ": a sliver"];
                continue;
            }
            if (coincidesWithAnother(d, s, *owners, *samples)) {
                if (skipped != nullptr)
                    ++(*skipped)[std::string(kindName(kind)) + ": coincides with another element it cannot be told "
                                                               "apart from"];
                continue;
            }
            if (b.map().namesOf(s).empty()) {
                if (skipped != nullptr)
                    ++(*skipped)[std::string(kindName(kind)) + ": element has no name"];
                continue;
            }
            refs.push_back(Ref{withoutOutlineIfAsked(resolver->makeRef(testBody(), s)),
                               PickRecord{s, d, samples, owners, builder}, namesOf(b.map(), s)});
        }
    }
    return refs;
}

namespace {

// The element of `kind` nearest to c by centroid, not in `exclude` (sorted), of surface/curve type `type` (-2: any),
// not a sliver (the judge leaves slivers out).
long nearestElement(const std::vector<ElemSamples>& N, SubKind kind, const Vec3& c,
                    const std::vector<std::uint32_t>& exclude, int type)
{
    long best = -1;
    double bd = 1e300;
    for (std::uint32_t i = 0; i < N.size(); ++i) {
        if (N[i].pts.empty() || (type != -2 && N[i].type != type) ||
            std::binary_search(exclude.begin(), exclude.end(), i) || isSliver(N[i], kind))
            continue;
        const double d = norm(sub(centroidOf(N[i]), c));
        if (d < bd) {
            bd = d;
            best = static_cast<long>(i);
        }
    }
    return best;
}

// Boxes of two elements more than 0.05 mm apart (cannot share samples).
bool outsideBoxes(const ElemSamples& a, const ElemSamples& b)
{
    constexpr double m = 0.05;
    return a.boxMax.x + m < b.boxMin.x || b.boxMax.x + m < a.boxMin.x || a.boxMax.y + m < b.boxMin.y ||
           b.boxMax.y + m < a.boxMin.y || a.boxMax.z + m < b.boxMin.z || b.boxMax.z + m < a.boxMin.z;
}

const char* answerText(Answer a)
{
    switch (a) {
    case Answer::Exact:
        return "Exact";
    case Answer::Split:
        return "Split";
    case Answer::Merged:
        return "Merged";
    case Answer::Repaired:
        return "Repaired";
    case Answer::Broken:
        return "Broken";
    }
    return "?";
}

ReviewedList loadReviewed(const char* file)
{
    ReviewedList out;
    const std::string path = std::string(RAY_ORACLE_DIR) + "/" + file;
    std::FILE* f = std::fopen(path.c_str(), "r");
    if (f == nullptr)
        return out;
    char buf[4096];
    while (std::fgets(buf, sizeof buf, f) != nullptr) {
        std::string line(buf);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            line.pop_back();
        if (line.empty() || line[0] == '#')
            continue;
        const auto sep = line.find(" => ");
        if (sep == std::string::npos)
            continue;
        ReviewedEntry e;
        std::string rest = line.substr(sep + 4);
        // "<signature> => <why>": the signature starts with an answer state.
        const auto sep2 = rest.find(" => ");
        bool stated = false;
        for (const char* st : {"Exact", "Split", "Merged", "Repaired", "Broken"})
            stated = stated || rest.compare(0, std::strlen(st), st) == 0;
        if (sep2 != std::string::npos && stated) {
            e.answer = rest.substr(0, sep2);
            e.why = rest.substr(sep2 + 4);
        } else {
            e.why = rest;
        }
        out.emplace(line.substr(0, sep), std::move(e));
    }
    std::fclose(f);
    return out;
}

struct SigElement {
    std::string kind;
    Vec3 c{};
    double measure = 0;
};

bool parseSignature(const std::string& sig, std::string& state, std::vector<SigElement>& els)
{
    els.clear();
    const auto sp = sig.find(' ');
    state = sig.substr(0, sp);
    if (state.empty())
        return false;
    std::size_t at = sp == std::string::npos ? sig.size() : sp + 1;
    while (at < sig.size()) {
        const auto open = sig.find('(', at);
        const auto close = sig.find(')', at);
        if (open == std::string::npos || close == std::string::npos || close < open)
            return false;
        SigElement e;
        e.kind = sig.substr(at, open - at);
        const std::string inside = sig.substr(open + 1, close - open - 1);
        double v[4] = {0, 0, 0, 0};
        const int n = std::sscanf(inside.c_str(), "%lf %lf %lf %lf", &v[0], &v[1], &v[2], &v[3]);
        if (n < 3 || (e.kind != "vertex" && n != 4) || (e.kind != "face" && e.kind != "edge" && e.kind != "vertex"))
            return false;
        e.c = Vec3{v[0], v[1], v[2]};
        e.measure = v[3];
        els.push_back(e);
        at = close + 1;
        while (at < sig.size() && sig[at] == ' ')
            ++at;
    }
    return true;
}

} // namespace

std::string answerSignature(Answer answer, const std::vector<SubId>& elements, const Samples& samples)
{
    struct El {
        int kind;
        Vec3 c;
        double measure;
    };
    std::vector<El> els;
    for (const SubId& s : elements) {
        const auto& N = samples.of(s.kind);
        if (s.index >= N.size()) {
            els.push_back({static_cast<int>(s.kind), Vec3{1e9, 1e9, 1e9}, -1});
            continue;
        }
        const ElemSamples& es = N[s.index];
        Vec3 c{0, 0, 0};
        double m = 0;
        if (!es.pts.empty() && es.w.size() == es.pts.size()) {
            for (const double w : es.w)
                m += w;
            c = m > 0 ? centroidOf(es) : es.pts.front();
        } else if (!es.pts.empty()) {
            for (const Vec3& p : es.pts)
                c = add(c, mul(p, 1.0 / static_cast<double>(es.pts.size())));
        } else if (!es.nodes.empty()) {
            for (const Vec3& p : es.nodes)
                c = add(c, mul(p, 1.0 / static_cast<double>(es.nodes.size())));
        }
        els.push_back({static_cast<int>(s.kind), c, s.kind == SubKind::Vertex ? 0.0 : m});
    }
    std::sort(els.begin(), els.end(), [](const El& a, const El& b) {
        return std::tie(a.kind, a.c.x, a.c.y, a.c.z) < std::tie(b.kind, b.c.x, b.c.y, b.c.z);
    });
    auto num = [](double v) {
        if (std::abs(v) < 5e-5)
            v = 0.0;
        char b[40];
        std::snprintf(b, sizeof b, "%.4f", v);
        return std::string(b);
    };
    std::string out = answerText(answer);
    for (const El& e : els) {
        const SubKind k = static_cast<SubKind>(e.kind);
        out += std::string(" ") + kindName(k) + "(" + num(e.c.x) + " " + num(e.c.y) + " " + num(e.c.z) +
               (k == SubKind::Vertex ? std::string() : " " + num(e.measure)) + ")";
    }
    return out;
}

std::string answerSignature(const naming::Resolved& resolved, const Samples& samples)
{
    return answerSignature(answerOf(resolved.state), resolved.elements, samples);
}

bool sameAnswer(const std::string& a, const std::string& b)
{
    std::string sa, sb;
    std::vector<SigElement> ea, eb;
    if (a.empty() || b.empty() || !parseSignature(a, sa, ea) || !parseSignature(b, sb, eb))
        return false;
    if (sa != sb || ea.size() != eb.size())
        return false;
    std::vector<bool> used(eb.size(), false);
    for (const SigElement& x : ea) {
        bool found = false;
        for (std::size_t j = 0; j < eb.size() && !found; ++j) {
            const SigElement& y = eb[j];
            if (used[j] || x.kind != y.kind || norm(sub(x.c, y.c)) > 0.02 ||
                std::abs(x.measure - y.measure) > 0.01 * std::max(x.measure, y.measure) + 1e-3)
                continue;
            used[j] = true;
            found = true;
        }
        if (!found)
            return false;
    }
    return true;
}

const ReviewedList& reviewedTerritory()
{
    static const ReviewedList list = loadReviewed("reviewed_territory.txt");
    return list;
}

const ReviewedList& reviewedUnjudged()
{
    static const ReviewedList list = loadReviewed("reviewed_unjudged.txt");
    return list;
}

const ReviewedList& reviewedConflicts()
{
    static const ReviewedList list = loadReviewed("reviewed_conflicts.txt");
    return list;
}

const ReviewedEntry* reviewedAnswer(const ReviewedList& list, const std::string& id, const std::string& signature)
{
    const auto [lo, hi] = list.equal_range(id);
    for (auto it = lo; it != hi; ++it)
        if (sameAnswer(it->second.answer, signature))
            return &it->second;
    return nullptr;
}

std::string reviewNote(const ReviewedList& list, const std::string& id, const std::string& signature)
{
    if (const ReviewedEntry* e = reviewedAnswer(list, id, signature))
        return " [reviewed: " + e->why + "]";
    std::string others;
    const auto [lo, hi] = list.equal_range(id);
    for (auto it = lo; it != hi; ++it)
        others +=
            (others.empty() ? "" : "; ") + (it->second.answer.empty() ? "(no answer recorded)" : it->second.answer);
    return " [NOT REVIEWED; answer " + signature + (others.empty() ? "" : "; listed only for: " + others) + "]";
}

RefResult judgeReference(const PickRecord& pick, const Owners& now, const Samples& smp, const naming::Resolved& res,
                         const std::string& prefix)
{
    RefResult out;
    const Expectation x = expect(pick, now, smp);
    const SubKind kind = pick.base.kind;
    out.answer = answerOf(res.state);
    out.signature = answerSignature(res, smp);
    std::vector<std::uint32_t> X;
    bool kindOk = true;
    for (const SubId& s : res.elements) {
        kindOk = kindOk && s.kind == kind;
        X.push_back(s.index);
    }
    if (kindOk)
        out.judgement = judgeAnswer(x, pick, now, smp, out.answer, X);
    else
        out.judgement.verdict = Verdict::WrongOther;
    out.expected = x.expected.elems;
    out.merged = x.expected.merged;
    out.noAnchor = x.noAnchor;
    out.crossing = x.crossing;
    out.congruent = x.congruentIndex;
    // Congruence cross-check: an element lies exactly where the picked one was, and the oracle expects something
    // else. When nothing the element depends on moved, that is a disagreement (gated); when it moved, another element
    // may well have taken its place (spike S4 P23), and it is only counted.
    // The dangerous kind is the one where the judge accepted the answer: oracle and resolver agree on something
    // other than the element lying in place.
    if (x.congruentIndex >= 0 && x.noAnchor.empty() &&
        !(out.expected.size() == 1 && out.expected[0] == static_cast<std::uint32_t>(x.congruentIndex))) {
        const bool accepted = out.judgement.verdict == Verdict::Correct && out.answer != Answer::Broken;
        out.conflict = x.moved ? 3 : accepted ? 1 : 2;
    }
    if (!x.noAnchor.empty())
        return out;

    // ---- mutation test of the oracle: mutants of its own expected answer, through the same judge ----
    const std::string cls = prefix + (pick.truth.weak ? "weak: " : "");
    const auto& NN = smp.of(kind);
    auto mutant = [&](const std::string& name, Answer a, std::vector<std::uint32_t> m) {
        Judgement j = judgeAnswer(x, pick, now, smp, a, m);
        std::string suffix;
        if (!j.newGround.empty()) {
            // A legitimate answer under the new-ground rule: its negative instead, the extra pieces moved to the
            // nearest element off the picked element's carriers (or, with no extra piece, the piece holding the most
            // picked ground left out), which must still be wrong (T-081b round 2).
            const std::vector<std::uint32_t>& E0 = out.expected;
            std::vector<std::uint32_t> skip(x.logical.begin(), x.logical.end());
            skip.insert(skip.end(), m.begin(), m.end());
            std::sort(skip.begin(), skip.end());
            skip.erase(std::unique(skip.begin(), skip.end()), skip.end());
            std::vector<std::uint32_t> neg;
            bool extra = false;
            for (const std::uint32_t e : m) {
                if (!std::binary_search(E0.begin(), E0.end(), e) && x.inLogical(e)) {
                    extra = true;
                    if (const long o = nearestElement(NN, kind, centroidOf(NN[e]), skip, -2); o >= 0)
                        neg.push_back(static_cast<std::uint32_t>(o));
                } else {
                    neg.push_back(e);
                }
            }
            if (!extra && !neg.empty()) {
                std::size_t most = 0;
                double best = -1.0;
                for (std::size_t i = 0; i < neg.size(); ++i)
                    if (const double s = shareOnElement(pick.picked(), NN[neg[i]], kind); s > best) {
                        best = s;
                        most = i;
                    }
                neg.erase(neg.begin() + static_cast<long>(most));
            }
            if (neg.empty())
                if (const long o = nearestElement(NN, kind, centroidOf(pick.picked()), skip, -2); o >= 0)
                    neg.push_back(static_cast<std::uint32_t>(o));
            m = neg;
            j = judgeAnswer(x, pick, now, smp, a, m);
            suffix = " (new ground: negative)";
        }
        RefResult::Mutant mu;
        mu.cls = cls + name + suffix;
        const bool flagged = isWrong(j.verdict) || j.unjudged;
        mu.caught = flagged || !j.accepted.empty();
        mu.byReviewOnly = !flagged && !j.accepted.empty();
        if (!flagged)
            mu.detail = std::string(answerText(a)) + " " + list(m) + " judged " + verdictName(j.verdict) +
                        (j.accepted.empty() ? "" : " (new territory: " + j.accepted + ")");
        out.mutants.push_back(std::move(mu));
    };
    const auto& N = smp.of(kind);
    const Vec3 c = centroidOf(x.mapped.pts.empty() ? pick.picked() : x.mapped);
    const std::vector<std::uint32_t>& E = out.expected;
    if (E.empty()) {
        const long g = nearestElement(N, kind, c, {}, -2);
        if (g >= 0)
            mutant("ghost", Answer::Exact, {static_cast<std::uint32_t>(g)});
        return out;
    }
    const Answer best = E.size() == 1 ? (x.expected.merged ? Answer::Merged : Answer::Exact) : Answer::Split;
    const Judgement self = judgeAnswer(x, pick, now, smp, best, E);
    if (self.verdict != Verdict::Correct || !self.accepted.empty())
        out.selfCheck = std::string(answerText(best)) + " " + list(E) + " judged " + verdictName(self.verdict);
    if (const long o = nearestElement(N, kind, centroidOf(N[E[0]]), E, N[E[0]].type); o >= 0) {
        std::vector<std::uint32_t> m = E;
        m[0] = static_cast<std::uint32_t>(o);
        mutant("other element", best, m);
    }
    std::vector<std::uint32_t> siblings;
    for (const std::uint32_t e : x.logical)
        if (!std::binary_search(E.begin(), E.end(), e) && !isSliver(N[e], kind))
            siblings.push_back(e);
    if (!siblings.empty()) {
        mutant("sibling piece", Answer::Exact, {siblings.front()});
        std::vector<std::uint32_t> m = E;
        m.push_back(siblings.front());
        mutant("superset with a sibling", Answer::Split, m);
    }
    if (const long s = nearestElement(N, kind, c, E, -2); s >= 0) {
        std::vector<std::uint32_t> m = E;
        m.push_back(static_cast<std::uint32_t>(s));
        mutant("superset with a neighbour", Answer::Split, m);
    }
    if (!x.mapped.pts.empty()) {
        long inPlace = -1;
        double share = 0;
        for (std::uint32_t i = 0; i < N.size(); ++i) {
            if (std::binary_search(E.begin(), E.end(), i) || N[i].pts.empty() || outsideBoxes(x.mapped, N[i]) ||
                isSliver(N[i], kind))
                continue;
            const double s = shareOnElement(x.mapped, N[i], kind);
            if (s > share) {
                share = s;
                inPlace = static_cast<long>(i);
            }
        }
        if (inPlace >= 0)
            mutant("in place", Answer::Exact, {static_cast<std::uint32_t>(inPlace)});
    }
    if (E.size() >= 2 && !x.twins) {
        mutant("drop piece", Answer::Split, std::vector<std::uint32_t>(E.begin(), E.end() - 1));
        mutant("split as exact", Answer::Exact, {E.front()});
    }
    if (x.expected.merged && E.size() == 1)
        mutant("merged as exact", Answer::Exact, E);
    return out;
}

void Tally::add(Answer a, Verdict v)
{
    ++n;
    ++state[static_cast<std::size_t>(a)];
    ++verdict[static_cast<std::size_t>(v)];
}

void Tally::merge(const Tally& o)
{
    n += o.n;
    for (std::size_t i = 0; i < state.size(); ++i)
        state[i] += o.state[i];
    for (std::size_t i = 0; i < verdict.size(); ++i)
        verdict[i] += o.verdict[i];
    existing += o.existing;
    existingOk += o.existingOk;
    weak += o.weak;
    picks += o.picks;
    picksUnjudged += o.picksUnjudged;
    repairedUsable += o.repairedUsable;
    noAnchor += o.noAnchor;
    crossings += o.crossings;
    crossingsAtSeam += o.crossingsAtSeam;
    accepted += o.accepted;
    acceptedNewGround += o.acceptedNewGround;
    acceptedNewGroundReviewed += o.acceptedNewGroundReviewed;
    acceptedReviewed += o.acceptedReviewed;
    conflicts += o.conflicts;
    conflictsReviewed += o.conflictsReviewed;
    unjudgedReviewed += o.unjudgedReviewed;
    conflictsOther += o.conflictsOther;
    congruentGhosts += o.congruentGhosts;
    injectivity += o.injectivity;
    twins += o.twins;
    selfCheckFailures += o.selfCheckFailures;
    for (const auto& [k, v] : o.mutants) {
        auto& m = mutants[k];
        for (std::size_t i = 0; i < 3; ++i)
            m[i] += v[i];
    }
}

long Tally::wrong() const
{
    long w = 0;
    for (const Verdict v :
         {Verdict::WrongOther, Verdict::WrongIncomplete, Verdict::WrongGhost, Verdict::WrongSilentMerge})
        w += verdict[static_cast<std::size_t>(v)];
    return w;
}

long Tally::mutantsMade() const
{
    long made = 0;
    for (const auto& [k, v] : mutants)
        made += v[0];
    return made;
}

long Tally::mutantsCaught() const
{
    long caught = 0;
    for (const auto& [k, v] : mutants)
        caught += v[1];
    return caught;
}

std::string dumpElements(const Builder& b, const Samples& samples, SubKind kind)
{
    std::string out;
    const auto& E = samples.of(kind);
    for (std::uint32_t i = 0; i < E.size(); ++i) {
        char buf[200];
        std::snprintf(buf, sizeof buf, "    %s %u (%.3f %.3f %.3f)-(%.3f %.3f %.3f): ", kindName(kind), i,
                      E[i].boxMin.x, E[i].boxMin.y, E[i].boxMin.z, E[i].boxMax.x, E[i].boxMax.y, E[i].boxMax.z);
        out += buf + namesOf(b.map(), SubId{kind, i}) + "\n";
    }
    return out;
}

std::string percent(long a, long n)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f", n != 0 ? 100.0 * static_cast<double>(a) / static_cast<double>(n) : 0.0);
    return buf;
}

namespace {

// Adds one judged reference to the tally and the log.
void account(const std::string& id, const RefResult& rr, const naming::Resolved& res, bool weak, Tally& tally,
             JudgeLog& log, const std::function<std::string()>& verbose)
{
    const Verdict v = rr.judgement.verdict;
    tally.add(rr.answer, v);
    if (weak)
        ++tally.weak;
    if (rr.answer == Answer::Repaired && res.usable())
        ++tally.repairedUsable;
    if (!rr.expected.empty()) {
        ++tally.existing;
        if (rr.answer == Answer::Exact || rr.answer == Answer::Split || rr.answer == Answer::Merged)
            ++tally.existingOk;
    }
    if (!rr.noAnchor.empty())
        ++tally.noAnchor;
    if (!rr.crossing.empty()) {
        ++tally.crossings;
        if (rr.crossing.compare(0, 5, "seam:") == 0)
            ++tally.crossingsAtSeam;
    }
    if (rr.judgement.twin)
        ++tally.twins;
    auto addWrong = [&](const std::string& line) {
        if (log.wrongs.size() < log.maxWrongs)
            log.wrongs.push_back(line);
    };
    const std::string answer = std::string(answerText(rr.answer)) + " " + list(indices(res.elements));
    if (!rr.judgement.accepted.empty()) {
        ++tally.accepted;
        if (reviewedAnswer(reviewedTerritory(), id, rr.signature) != nullptr)
            ++tally.acceptedReviewed;
        log.notes.push_back("ACCEPTED " + id + ": " + rr.judgement.accepted +
                            reviewNote(reviewedTerritory(), id, rr.signature));
    }
    if (!rr.judgement.newGround.empty()) {
        ++tally.acceptedNewGround;
        // Every acceptance by the new-ground rule must be reviewed (round 3), on the same list as the new-territory
        // rule's (reviewed_territory.txt).
        if (reviewedAnswer(reviewedTerritory(), id, rr.signature) != nullptr)
            ++tally.acceptedNewGroundReviewed;
        log.notes.push_back("NEW GROUND " + id + ": " + rr.judgement.newGround +
                            reviewNote(reviewedTerritory(), id, rr.signature));
    }
    if (rr.conflict == 1) {
        ++tally.conflicts;
        if (reviewedAnswer(reviewedConflicts(), id, rr.signature) != nullptr)
            ++tally.conflictsReviewed;
        log.notes.push_back("CONFLICT " + id + ": congruent in place " + std::to_string(rr.congruent) +
                            ", oracle expects " + list(rr.expected) + ", resolver " + answer +
                            reviewNote(reviewedConflicts(), id, rr.signature));
        if (std::getenv("RAYCAD_NAMING_VERBOSE") != nullptr)
            log.notes.push_back(verbose());
    }
    if (v == Verdict::Unjudged) {
        if (reviewedAnswer(reviewedUnjudged(), id, rr.signature) != nullptr)
            ++tally.unjudgedReviewed;
        log.notes.push_back("UNJUDGED " + id + ": resolver " + answer + ", " +
                            (rr.noAnchor.empty() ? std::string("whether the element also continues another piece of "
                                                               "the logical element cannot be told")
                                                 : rr.noAnchor) +
                            reviewNote(reviewedUnjudged(), id, rr.signature));
    }
    if (rr.conflict == 2)
        ++tally.conflictsOther;
    if (rr.conflict == 3)
        ++tally.congruentGhosts;
    if (!rr.selfCheck.empty()) {
        ++tally.selfCheckFailures;
        addWrong("ORACLE SELF-CHECK FAILED " + id + ": " + rr.selfCheck);
    }
    for (const RefResult::Mutant& mu : rr.mutants) {
        auto& m = tally.mutants[mu.cls];
        ++m[0];
        if (mu.caught)
            ++m[1];
        if (mu.byReviewOnly)
            ++m[2];
        if (!mu.caught)
            addWrong("MUTANT NOT CAUGHT " + id + " [" + mu.cls + "]: " + mu.detail + "; oracle expects " +
                     list(rr.expected));
    }
    // RAYCAD_NAMING_EXPLAIN="<kind> <index>" (e.g. "face 21"): the reference is printed whatever its verdict.
    const char* explainOnly = std::getenv("RAYCAD_NAMING_EXPLAIN");
    const bool explainThis = explainOnly != nullptr && id.size() > std::strlen(explainOnly) &&
                             id.compare(id.size() - std::strlen(explainOnly), std::string::npos, explainOnly) == 0 &&
                             id[id.size() - std::strlen(explainOnly) - 1] == '|';
    if (isWrong(v) || v == Verdict::Unjudged || explainThis) {
        addWrong(id + ": " + verdictName(v) + "; resolver " + answer + " (" + res.reason + "), oracle expects " +
                 list(rr.expected) + (rr.merged ? " merged" : "") +
                 (rr.noAnchor.empty() ? "" : " (" + rr.noAnchor + ")") +
                 (rr.crossing.empty() ? "" : " (sliding crossing, " + rr.crossing + ")") + ", congruent " +
                 std::to_string(rr.congruent));
        if (std::getenv("RAYCAD_NAMING_VERBOSE") != nullptr && log.wrongs.size() < log.maxWrongs)
            log.wrongs.push_back(verbose());
    }
    if (std::getenv("RAYCAD_NAMING_VERDICTS") != nullptr)
        log.trace.push_back(id + " " + answer + " " + verdictName(v) + " E" + list(rr.expected) +
                            (rr.crossing.empty() ? "" : " [crossing " + rr.crossing + "]") + " | " + res.reason);
}

} // namespace

namespace {
thread_local Inspector inspector;
} // namespace

void setInspector(Inspector f)
{
    inspector = std::move(f);
}

void judgeAll(const std::vector<Ref>& refs, const Builder& replay, const Owners& owners, const Samples& samples,
              Tally& tally, JudgeLog& log, const std::string& key)
{
    if (!replay.have())
        return;
    auto resolver = naming::makeResolver({naming::ResolverBody{testBody(), replay.map(), replay.shape()}});
    // Injectivity: two distinct references answered Exact on one element cannot both be right.
    std::map<SubId, std::vector<std::string>> exactOn;
    for (const Ref& r : refs) {
        const naming::Resolved res = resolver->resolve(r.ref);
        const RefResult rr = judgeReference(r.record, owners, samples, res, "");
        const std::string id = key + "|" + kindName(r.record.base.kind) + " " + std::to_string(r.record.base.index);
        if (res.state == naming::ResolveState::Exact && res.elements.size() == 1)
            exactOn[res.elements.front()].push_back(id);
        if (inspector)
            inspector(id, r, res, rr, owners, samples);
        account(id, rr, res, r.record.truth.weak, tally, log, [&] {
            const SubKind kind = r.record.base.kind;
            std::string out = "    " + explain(r.record.truth, owners, samples,
                                               rr.congruent >= 0       ? rr.congruent
                                               : !res.elements.empty() ? static_cast<long>(res.elements[0].index)
                                                                       : -1);
            std::string got;
            for (const SubId& e : res.elements)
                got += " [" + namesOf(replay.map(), e) + "]";
            out += "\n    picked names [" + r.names + "]; answer names" + got + "\n    expected names";
            for (const std::uint32_t e : rr.expected)
                out += " " + std::to_string(e) + "[" + namesOf(replay.map(), SubId{kind, e}) + "]";
            auto box = [](const ElemSamples& es) {
                char b[160];
                std::snprintf(b, sizeof b, "(%.3f %.3f %.3f)-(%.3f %.3f %.3f) n%zu", es.boxMin.x, es.boxMin.y,
                              es.boxMin.z, es.boxMax.x, es.boxMax.y, es.boxMax.z, es.nodes.size());
                return std::string(b);
            };
            out += "\n    picked " + box(r.record.picked());
            for (const std::uint32_t e : rr.expected)
                out += "; " + std::to_string(e) + " " + box(samples.of(kind)[e]);
            const Expectation x = expect(r.record, owners, samples);
            out += "\n    mapped pick " + (x.mapped.pts.empty() ? std::string("(none)") : box(x.mapped)) +
                   (x.moved ? " moved" : "") + "; strict " + list(x.strict.elems) + "; logical:";
            // How much of the element lies on the picked element as it was (unmapped), and of it on the element.
            auto shares = [&](const ElemSamples& es) {
                char b[80];
                std::snprintf(b, sizeof b, " on-pick %.0f%% pick-on %.0f%%",
                              100.0 * shareOnElement(es, r.record.picked(), kind),
                              100.0 * shareOnElement(r.record.picked(), es, kind));
                return std::string(b);
            };
            for (std::size_t i = 0; i < x.logical.size(); ++i)
                out += " " + std::to_string(x.logical[i]) + "/c" + std::to_string(x.component[i]) +
                       (x.anchored[i] != 0 ? "/anchored" : "") + (x.isClaimed(x.logical[i]) ? "/claimed" : "") + " " +
                       box(samples.of(kind)[x.logical[i]]) + shares(samples.of(kind)[x.logical[i]]);
            // Which baseline elements the answer lies on (its territory at pick time), by share of its samples.
            auto onBaseline = [&](const ElemSamples& es) {
                std::string s;
                const auto& B = r.record.baseline->of(kind);
                for (std::uint32_t i = 0; i < B.size(); ++i) {
                    if (outsideBoxes(es, B[i]))
                        continue;
                    const double sh = shareOnElement(es, B[i], kind);
                    if (sh >= 0.02) {
                        char b[40];
                        std::snprintf(b, sizeof b, " b%u:%.0f%%", i, 100.0 * sh);
                        s += b;
                    }
                }
                return s.empty() ? std::string(" on no baseline element") : " on baseline" + s;
            };
            for (const SubId& e : res.elements)
                if (e.kind == kind && e.index < samples.of(kind).size())
                    out += "\n    answer " + std::to_string(e.index) + " " + box(samples.of(kind)[e.index]) +
                           shares(samples.of(kind)[e.index]) + onBaseline(samples.of(kind)[e.index]);
            for (const std::uint32_t e : x.logical)
                out += "\n    logical " + std::to_string(e) + onBaseline(samples.of(kind)[e]);
            return out;
        });
    }
    for (const auto& [element, ids] : exactOn) {
        if (ids.size() < 2)
            continue;
        ++tally.injectivity;
        std::string all;
        for (const std::string& id : ids)
            all += " " + id;
        if (log.wrongs.size() < log.maxWrongs)
            log.wrongs.push_back("INJECTIVITY: " + std::string(kindName(element.kind)) + " " +
                                 std::to_string(element.index) + " answered Exact for" + all);
    }
    for (const PickOutcome& p : replay.picks()) {
        ++tally.picks;
        const std::string id =
            key + "|pick " + p.feature + "/" + std::to_string(p.target) + " edge " + std::to_string(p.edge);
        account(id, p.result, p.resolved, false, tally, log, [] { return std::string(); });
    }
    tally.picksUnjudged += replay.unjudgedPicks();
}

void printTally(const char* title, const Tally& t)
{
    auto v = [&](Verdict x) {
        return t.verdict[static_cast<std::size_t>(x)];
    };
    std::printf("\n%s: %ld resolutions (%ld blend picks, %ld weak references): Exact %s%%, Split %s%%, Merged %s%%, "
                "Repaired %s%%, Broken %s%%\n",
                title, t.n, t.picks, t.weak, percent(t.state[0], t.n).c_str(), percent(t.state[1], t.n).c_str(),
                percent(t.state[2], t.n).c_str(), percent(t.state[3], t.n).c_str(), percent(t.state[4], t.n).c_str());
    std::printf("  counts: Exact %ld, Split %ld, Merged %ld, Repaired %ld, Broken %ld\n", t.state[0], t.state[1],
                t.state[2], t.state[3], t.state[4]);
    std::printf("  verdicts: correct %ld, broken and really gone %ld, missed %ld, NOT JUDGED %ld, WRONG %ld (other "
                "element %ld, incomplete %ld, ghost %ld, unflagged merge %ld)\n",
                v(Verdict::Correct), v(Verdict::BrokenGone), v(Verdict::Missed), v(Verdict::Unjudged), t.wrong(),
                v(Verdict::WrongOther), v(Verdict::WrongIncomplete), v(Verdict::WrongGhost),
                v(Verdict::WrongSilentMerge));
    std::printf("  among references whose element still exists: %s%% Exact, Split or Merged (%ld of %ld)\n",
                percent(t.existingOk, t.existing).c_str(), t.existingOk, t.existing);
    std::printf(
        "  continuation unknown (no anchor): %ld references (%ld answered other than Broken: NOT JUDGED, %ld of "
        "them reviewed); blend picks not judged (no description): %ld\n",
        t.noAnchor, t.unjudged(), t.unjudgedReviewed, t.picksUnjudged);
    std::printf("  sliding crossings told by the oracle (oracle.h Expectation::crossing): %ld (%ld at a seam, %ld by "
                "position)\n",
                t.crossings, t.crossingsAtSeam, t.crossings - t.crossingsAtSeam);
    std::printf("  accepted with new ground (flagged answers, oracle.cpp newGroundRule): %ld\n", t.acceptedNewGround);
    std::printf("  new-territory acceptances: %ld (%ld on the reviewed list, %ld NOT REVIEWED)\n", t.accepted,
                t.acceptedReviewed, t.accepted - t.acceptedReviewed);
    std::printf(
        "  congruence cross-check (an element lies exactly where the picked one was, and E is not it): %ld with "
        "the answer judged correct though nothing moved (%ld reviewed, %ld NOT REVIEWED); %ld with a Broken or "
        "wrong answer (counted only); %ld after a move (counted only)\n",
        t.conflicts, t.conflictsReviewed, t.conflicts - t.conflictsReviewed, t.conflictsOther, t.congruentGhosts);
    std::printf("  injectivity violations: %ld; oracle self-check failures: %ld; usable Repaired: %ld; Exact on one of "
                "coincident vertices (accepted): %ld\n",
                t.injectivity, t.selfCheckFailures, t.repairedUsable, t.twins);
    std::printf("  mutants: %ld of %ld caught\n", t.mutantsCaught(), t.mutantsMade());
    for (const auto& [k, m] : t.mutants)
        std::printf("    %-40s %8ld of %8ld caught (%ld only by the review gate)\n", k.c_str(), m[1], m[0], m[2]);
}

} // namespace ray::naming_oracle
