// The build harness of the naming tests (task T-081a). See harness.h.
#include "harness.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <set>

namespace ray::naming_oracle {

namespace {

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

double congruenceTolerance(SubKind kind)
{
    return kind == SubKind::Vertex ? 1e-3 : 0.05;
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
        const naming::NamingInput toolInput{fid, f.role, {}, nullptr, &tool->op.shape, tool->origins};
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
        const naming::NamingInput input{fid, f.role, maps, &result->history, &result->shape, {}};
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
    const Owners owners = buildOwners(builtFeatures_);
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
                picks.push_back({k, resolver->makeRef(testBody(), SubId{SubKind::Edge, e}), desc});
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
                PickOutcome po{f.id, p.target, r, {}, Verdict::Correct};
                if (p.truth.ok) {
                    po.expected = evalTruth(p.truth, owners, **samples);
                    po.verdict = judge(answerOf(r.state), indices(r.elements), po.expected, p.truth.weak, -1);
                    picks_.push_back(po);
                }
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
    const naming::NamingInput input{featureIdOf(f.id), f.role, maps, &op->history, &op->shape, {}};
    auto named = naming::propagate(input);
    if (!named) {
        failed_.push_back(f.id + ": naming failed: " + named.error().message);
        return false;
    }
    record(std::move(*named), op->shape);
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

std::vector<Ref> recordReferences(const Builder& b, const Owners& owners, const Samples& samples,
                                  std::map<std::string, int>* skipped)
{
    std::vector<Ref> refs;
    if (!b.have())
        return refs;
    const TruthBuilder tb(owners, samples);
    auto resolver = naming::makeResolver({naming::ResolverBody{testBody(), b.map(), b.shape()}});
    for (const SubKind kind : {SubKind::Face, SubKind::Edge, SubKind::Vertex}) {
        const auto& elems = samples.of(kind);
        for (std::uint32_t i = 0; i < elems.size(); ++i) {
            const SubId s{kind, i};
            const TruthDesc d = tb.describe(s);
            if (!d.ok) {
                if (skipped != nullptr)
                    ++(*skipped)[std::string(kindName(kind)) + ": " + d.why];
                continue;
            }
            if (b.map().namesOf(s).empty()) {
                if (skipped != nullptr)
                    ++(*skipped)[std::string(kindName(kind)) + ": element has no name"];
                continue;
            }
            refs.push_back(Ref{s, resolver->makeRef(testBody(), s), d, elems[i], namesOf(b.map(), s)});
        }
    }
    return refs;
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
    conflicts += o.conflicts;
    mutants += o.mutants;
    mutantsCaught += o.mutantsCaught;
    weak += o.weak;
    newTerritory += o.newTerritory;
    unjudged += o.unjudged;
    repairedUsable += o.repairedUsable;
}

long Tally::wrong() const
{
    long w = 0;
    for (const Verdict v :
         {Verdict::WrongOther, Verdict::WrongIncomplete, Verdict::WrongGhost, Verdict::WrongSilentMerge})
        w += verdict[static_cast<std::size_t>(v)];
    return w;
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

void judgeAll(const std::vector<Ref>& refs, const Builder& replay, const Owners& owners, const Samples& samples,
              const Samples& baseline, Tally& tally, std::vector<std::string>& wrongs, std::size_t maxWrongs,
              const std::string& label)
{
    if (!replay.have())
        return;
    auto resolver = naming::makeResolver({naming::ResolverBody{testBody(), replay.map(), replay.shape()}});
    // Does a baseline element still exist after the edit? By its own description (computed once); an element that
    // is not a reference has none and counts as existing.
    std::map<SubId, std::size_t> refOf;
    for (std::size_t i = 0; i < refs.size(); ++i)
        refOf[refs[i].base] = i;
    std::map<SubId, bool> survived;
    const std::function<bool(SubId)> survives = [&](SubId s) {
        const auto it = refOf.find(s);
        if (it == refOf.end())
            return true;
        const auto known = survived.find(s);
        if (known != survived.end())
            return known->second;
        const bool alive = !evalTruth(refs[it->second].truth, owners, samples).elems.empty();
        survived[s] = alive;
        return alive;
    };
    for (const Ref& r : refs) {
        const naming::Resolved res = resolver->resolve(r.ref);
        const TruthEval te = evalTruth(r.truth, owners, samples);
        const std::vector<std::uint32_t> cong =
            congruent(r.picked, r.ref.kind, samples, congruenceTolerance(r.ref.kind));
        const long congIdx = cong.size() == 1 ? static_cast<long>(cong[0]) : -1;
        const Answer answer = answerOf(res.state);
        const Verdict strict = judge(answer, indices(res.elements), te, r.truth.weak, congIdx);
        bool newTerritory = false;
        Verdict v = reconsider(strict, answer, indices(res.elements), r.truth, te, owners, samples, baseline, r.base,
                               survives, newTerritory);
        if (newTerritory)
            ++tally.newTerritory;
        // An answer element the mesher left without triangles cannot be described: not judged (counted).
        bool unsampled = false;
        for (const SubId& e : res.elements)
            unsampled = unsampled || e.index >= samples.of(e.kind).size() || samples.of(e.kind)[e.index].pts.empty();
        if (unsampled && isWrong(v)) {
            ++tally.unjudged;
            v = Verdict::Correct;
        }
        tally.add(answer, v);
        if (r.truth.weak)
            ++tally.weak;
        if (answer == Answer::Repaired && res.usable())
            ++tally.repairedUsable;
        if (!te.elems.empty()) {
            ++tally.existing;
            if (answer == Answer::Exact || answer == Answer::Split || answer == Answer::Merged)
                ++tally.existingOk;
        }
        if (!r.truth.weak && congIdx >= 0 && !(te.elems.size() == 1 && te.elems[0] == cong[0]))
            ++tally.conflicts;
        // Mutation test of the oracle: a correct single answer replaced by the nearest other element of the same
        // kind and type must be judged wrong by the full oracle (with the new-territory rule); a correct Split with
        // one piece removed must be judged wrong by the description. Weak descriptions only know the set of pieces,
        // so their mutants are not counted.
        auto full = [&](const std::vector<std::uint32_t>& x) {
            bool unused = false;
            return reconsider(judge(answer, x, te, r.truth.weak, congIdx), answer, x, r.truth, te, owners, samples,
                              baseline, r.base, survives, unused);
        };
        if (strict == Verdict::Correct && res.elements.size() == 1 && answer != Answer::Broken && !r.truth.weak) {
            const auto& E = samples.of(r.ref.kind);
            const std::uint32_t self = res.elements.front().index;
            const Vec3 c = centroidOf(E[self]);
            long best = -1;
            double bd = 1e300;
            for (std::uint32_t i = 0; i < E.size(); ++i) {
                if (i == self || E[i].type != E[self].type || E[i].pts.empty())
                    continue;
                const double d = norm(sub(centroidOf(E[i]), c));
                if (d < bd) {
                    bd = d;
                    best = static_cast<long>(i);
                }
            }
            if (best >= 0) {
                ++tally.mutants;
                if (isWrong(full({static_cast<std::uint32_t>(best)})))
                    ++tally.mutantsCaught;
                else if (wrongs.size() < maxWrongs)
                    wrongs.push_back(label + ": MUTANT NOT CAUGHT: " + kindName(r.ref.kind) + " " +
                                     std::to_string(r.base.index) + " [" + r.truth.cat + "] answer " +
                                     list(indices(res.elements)) + " mutant " + std::to_string(best) +
                                     ", oracle expects " + list(te.elems) + "; " +
                                     explain(r.truth, owners, samples, best));
            }
        } else if (strict == Verdict::Correct && res.elements.size() > 1 && !r.truth.weak) {
            std::vector<std::uint32_t> fewer = indices(res.elements);
            fewer.pop_back();
            ++tally.mutants;
            if (isWrong(judge(answer, fewer, te, false, -1)))
                ++tally.mutantsCaught;
        }
        if (isWrong(v) && wrongs.size() < maxWrongs) {
            wrongs.push_back(label + ": " + kindName(r.ref.kind) + " " + std::to_string(r.base.index) + " [" +
                             r.truth.cat + "] " + verdictName(v) + "; resolver " + stateName(res.state) + " " +
                             list(indices(res.elements)) + " (" + res.reason + "), oracle expects " + list(te.elems) +
                             (te.merged ? " merged" : "") + ", congruent " + list(cong));
            if (std::getenv("RAYCAD_NAMING_VERBOSE") != nullptr) {
                const long x = !cong.empty()           ? static_cast<long>(cong[0])
                               : !res.elements.empty() ? static_cast<long>(res.elements[0].index)
                                                       : -1;
                wrongs.push_back("    " + explain(r.truth, owners, samples, x));
                std::string got;
                for (const SubId& e : res.elements)
                    got += " [" + namesOf(replay.map(), e) + "]";
                wrongs.push_back("    picked names [" + r.names + "]; answer names" + got);
                std::string want;
                for (const std::uint32_t e : te.elems)
                    want += " " + std::to_string(e) + "[" + namesOf(replay.map(), SubId{r.ref.kind, e}) + "]";
                wrongs.push_back("    expected names" + want);
                auto box = [](const ElemSamples& es) {
                    char b[160];
                    std::snprintf(b, sizeof b, "(%.3f %.3f %.3f)-(%.3f %.3f %.3f) n%zu", es.boxMin.x, es.boxMin.y,
                                  es.boxMin.z, es.boxMax.x, es.boxMax.y, es.boxMax.z, es.nodes.size());
                    return std::string(b);
                };
                std::string boxes = "    picked " + box(r.picked);
                for (const std::uint32_t e : te.elems)
                    boxes += "; " + std::to_string(e) + " " + box(samples.of(r.ref.kind)[e]);
                wrongs.push_back(boxes);
            }
        }
    }
    for (const PickOutcome& p : replay.picks()) {
        tally.add(answerOf(p.resolved.state), p.verdict);
        if (isWrong(p.verdict) && wrongs.size() < maxWrongs)
            wrongs.push_back(label + ": pick of " + p.feature + " target " + std::to_string(p.target) + " " +
                             verdictName(p.verdict) + "; resolver " + stateName(p.resolved.state) + " " +
                             list(indices(p.resolved.elements)) + ", oracle expects " + list(p.expected.elems));
    }
}

} // namespace ray::naming_oracle
