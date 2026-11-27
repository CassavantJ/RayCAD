// The spike S4 part built as document features (task T-096): every S4 sweep becomes a datum plane, a sketch and an
// extrude, every blend a fillet or chamfer whose edges are TopoRefs picked on the document's own build. The 47 S4
// edit scenarios are applied to the document through its commands (feature.edit / add / remove / reorder /
// suppress), the document is rebuilt, and every reference recorded on the baseline is resolved and judged by the
// T-081a oracle (tests/naming/oracle, linked, not copied). A second run builds the round boss F04 as a revolve.
//
// What is judged, as in tests/naming/oracle/s4_test.cpp: every element of the final body the oracle can describe,
// and every blend pick resolved just before its blend feature. Two differences come from the document model, not
// from naming: one extrude sweeps all loops of its sketch (the harness does one boolean per loop too), and a feature
// that fails stops the body's chain (later features report NotBuilt), where the harness skips the failed feature and
// goes on. The gates are the s4_test gates (zero wrong resolutions, mutants caught, nothing unjudged, every
// acceptance and conflict on the reviewed lists); the 95 % bar is printed per scenario ("BELOW"), not gated, exactly
// as s4_test does.
//
// RAYCAD_S4DOC_ONLY=<scenario id> runs one scenario; RAYCAD_S4DOC_TABLE=<file> also writes the table there.
#include "harness.h"
#include "s4_part.h"

#include "features/feature_props.h"
#include "features/sketch_json.h"
#include "golden_geometry.h"
#include "json.h"
#include "test_dispatcher.h"

#include <ray/document.h>
#include <ray/naming.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace o = ray::naming_oracle;
using ray::BodyId;
using ray::FeatureId;
using ray::Hasher;
using ray::Uuid;
using ray::doc::BuildResultPtr;
using ray::doc::Command;
using ray::doc::DocState;
using ray::doc::DocStatePtr;
using ray::doc::FeatureStatus;
using ray::kernel::SubId;
using ray::kernel::SubKind;

Uuid derived(const std::string& key)
{
    Hasher h;
    h.str("t096.s4doc");
    h.str(key);
    const ray::Hash256 d = h.finish();
    Uuid id;
    std::copy_n(d.bytes.begin(), id.bytes.size(), id.bytes.begin());
    id.bytes[6] = static_cast<std::uint8_t>((id.bytes[6] & 0x0F) | 0x40);
    id.bytes[8] = static_cast<std::uint8_t>((id.bytes[8] & 0x3F) | 0x80);
    return id;
}

FeatureId fid(const std::string& scriptId, const char* role)
{
    return FeatureId{derived("feature/" + scriptId + "/" + role)};
}

BodyId partBody()
{
    return BodyId{derived("body")};
}

ray::SketchEntId entityId(const std::string& scriptId, std::uint64_t loop, const std::string& what)
{
    return ray::SketchEntId{derived("entity/" + scriptId + "/" + std::to_string(loop) + "/" + what)};
}

std::string quote(const std::string& s)
{
    return "\"" + s + "\"";
}

std::string num(double v)
{
    return ray::doc::json::write(ray::doc::json::Value::number(v));
}

// ---------- The document form of a script ----------

// Entity tags of each loop of each sweep in the baseline script: the region selectors (a region is picked once, when
// the user makes the extrude, and kept through later sketch edits).
using Selectors = std::map<std::pair<std::string, std::uint64_t>, std::vector<std::uint64_t>>;

Selectors selectorsOf(const o::Script& script)
{
    Selectors out;
    for (const o::Feature& f : script) {
        if (f.type != o::FType::Sweep) {
            continue;
        }
        for (const o::Loop& l : f.loops) {
            std::vector<std::uint64_t>& tags = out[{f.id, l.id}];
            for (const o::Ent& e : l.ents) {
                tags.push_back(e.tag);
            }
        }
    }
    return out;
}

std::string entName(const o::Ent& e)
{
    return "t" + std::to_string(e.tag);
}

// The sketch model of a sweep: each loop's entities with shared corner points.
ray::sketch::Sketch sketchOf(const o::Feature& f)
{
    namespace sk = ray::sketch;
    sk::Sketch s;
    for (const o::Loop& l : f.loops) {
        const std::size_t n = l.ents.size();
        auto corner = [&](std::size_t i) { // the point where ent i ends and ent i+1 starts
            return entityId(f.id, l.id,
                            "p" + std::to_string(l.ents[i].tag) + "_" + std::to_string(l.ents[(i + 1) % n].tag));
        };
        for (std::size_t i = 0; i < n; ++i) {
            const o::Ent& e = l.ents[i];
            const ray::SketchEntId id = entityId(f.id, l.id, entName(e));
            if (e.k == 'C') {
                const ray::SketchEntId c = entityId(f.id, l.id, entName(e) + "c");
                s.entities.push_back(sk::Entity{c, sk::Point{ray::Vec2{e.cx, e.cy}}, false});
                s.entities.push_back(sk::Entity{id, sk::Circle{c, e.r}, false});
                continue;
            }
            s.entities.push_back(sk::Entity{corner(i), sk::Point{ray::Vec2{e.x1, e.y1}}, false});
            const ray::SketchEntId a = corner((i + n - 1) % n);
            const ray::SketchEntId b = corner(i);
            if (e.k == 'L') {
                s.entities.push_back(sk::Entity{id, sk::Line{a, b}, false});
            } else {
                const ray::SketchEntId c = entityId(f.id, l.id, entName(e) + "c");
                s.entities.push_back(sk::Entity{c, sk::Point{ray::Vec2{e.cx, e.cy}}, false});
                s.entities.push_back(sk::Entity{id, sk::Arc{c, a, b, true}, false});
            }
        }
    }
    return s;
}

// The datum plane props of a sweep's frame (the S4 frames are principal planes, offset, maybe flipped).
std::string datumProps(const o::Frame& fr)
{
    auto is = [](const ray::Vec3& a, double x, double y, double z) {
        return std::abs(a.x - x) < 1e-12 && std::abs(a.y - y) < 1e-12 && std::abs(a.z - z) < 1e-12;
    };
    if (is(fr.n, 0, 0, 1) && is(fr.xd, 1, 0, 0) && fr.o.x == 0 && fr.o.y == 0) {
        return R"({"base":{"principal":"xy"},"offset":)" + num(fr.o.z) + "}";
    }
    if (is(fr.n, 1, 0, 0) && is(fr.xd, 0, 1, 0) && fr.o.y == 0 && fr.o.z == 0) {
        return R"({"base":{"principal":"yz"},"offset":)" + num(fr.o.x) + "}";
    }
    if (is(fr.n, -1, 0, 0) && is(fr.xd, 0, -1, 0) && fr.o.y == 0 && fr.o.z == 0) {
        return R"({"base":{"principal":"yz"},"offset":)" + num(fr.o.x) + R"(,"flip":true})";
    }
    return {};
}

// How the script's features become document features: as written, with the round boss F04 as a revolve (T-096), or
// with every "hole" sweep (circles of one radius) as a hole feature drilling at sketch points (T-097).
enum class Variant { Plain, RevolveF04, Holes };

// A "hole" sweep the hole feature can make: every loop one circle, all of one radius.
bool holeSweep(const o::Feature& f)
{
    if (f.role != "hole" || f.loops.empty()) {
        return false;
    }
    for (const o::Loop& l : f.loops) {
        if (l.ents.size() != 1 || l.ents[0].k != 'C' || l.ents[0].r != f.loops[0].ents[0].r) {
            return false;
        }
    }
    return true;
}

struct Generated {
    std::vector<ray::doc::Feature> features;
    std::map<std::string, FeatureId> opOf; // script feature -> the doc feature that builds it
};

// `blendEdges`: script blend id -> the JSON array of its edge references (authored on the baseline); blends without
// an entry are left out (authoring adds them one at a time).
Generated generate(const o::Script& script, const Selectors& selectors,
                   const std::map<std::string, std::string>& blendEdges, Variant variant)
{
    Generated out;
    auto add = [&](const FeatureId& id, const std::string& type, const std::string& name, const std::string& props,
                   bool suppressed) {
        ray::doc::Feature f;
        f.id = id;
        f.type = type;
        f.name = name;
        f.propsJson = ray::doc::canonicalJson(props);
        f.suppressed = suppressed;
        out.features.push_back(std::move(f));
    };
    const std::string body = quote(partBody().value.toString());
    for (const o::Feature& f : script) {
        if (f.type == o::FType::Sweep) {
            const FeatureId datum = fid(f.id, "datum");
            const FeatureId sketch = fid(f.id, "sketch");
            const FeatureId op = fid(f.id, "op");
            out.opOf[f.id] = op;
            const char* operation = f.op == o::BOp::New ? "new" : f.op == o::BOp::Join ? "join" : "cut";
            if (variant == Variant::Holes && holeSweep(f)) {
                // Simple flat-bottomed holes at the circle centres; "through" for the script's through cuts.
                add(datum, "datum_plane", "Datum " + f.id, datumProps(f.frame), f.suppressed);
                namespace sk = ray::sketch;
                sk::Sketch s;
                for (const o::Loop& l : f.loops) {
                    const o::Ent& c = l.ents[0];
                    s.entities.push_back(
                        sk::Entity{entityId(f.id, l.id, entName(c) + "c"), sk::Point{ray::Vec2{c.cx, c.cy}}, false});
                }
                std::sort(s.entities.begin(), s.entities.end(),
                          [](const sk::Entity& a, const sk::Entity& b) { return a.id < b.id; });
                add(sketch, "sketch", "Sketch " + f.id,
                    R"({"plane":{"datum":)" + quote(datum.value.toString()) + R"(},"sketch":)" +
                        ray::doc::json::write(ray::doc::features::sketchToJson(s)) + "}",
                    f.suppressed);
                const bool through = f.dist >= 200;
                add(op, "hole", f.id,
                    R"({"sketch":)" + quote(sketch.value.toString()) + R"(,"target":)" + body +
                        R"(,"kind":"simple","diameter":)" + num(2 * f.loops[0].ents[0].r) +
                        (through ? std::string(R"(,"end":"through")")
                                 : R"(,"end":"blind","bottom":"flat","depth":)" + num(f.dist)) +
                        "}",
                    f.suppressed);
                continue;
            }
            if (variant == Variant::RevolveF04 && f.id == "F04") {
                // The round boss as a revolve: a rectangle from the axis out to r, H to H + h, turned about a
                // construction line on the axis, on a plane through the axis parallel to XZ.
                const o::Ent& c = f.loops.front().ents.front();
                const double z0 = f.frame.o.z, h = f.dist;
                add(datum, "datum_plane", "Datum " + f.id, R"({"base":{"principal":"xz"},"offset":)" + num(-c.cy) + "}",
                    f.suppressed);
                namespace sk = ray::sketch;
                sk::Sketch s;
                auto pid = [&](const char* n) {
                    return entityId(f.id, 1, n);
                };
                const double x0 = c.cx, x1 = c.cx + c.r;
                s.entities.push_back(sk::Entity{pid("p0"), sk::Point{{x0, z0}}, false});
                s.entities.push_back(sk::Entity{pid("p1"), sk::Point{{x1, z0}}, false});
                s.entities.push_back(sk::Entity{pid("p2"), sk::Point{{x1, z0 + h}}, false});
                s.entities.push_back(sk::Entity{pid("p3"), sk::Point{{x0, z0 + h}}, false});
                s.entities.push_back(sk::Entity{pid("bottom"), sk::Line{pid("p0"), pid("p1")}, false});
                s.entities.push_back(sk::Entity{pid("side"), sk::Line{pid("p1"), pid("p2")}, false});
                s.entities.push_back(sk::Entity{pid("top"), sk::Line{pid("p2"), pid("p3")}, false});
                s.entities.push_back(sk::Entity{pid("inner"), sk::Line{pid("p3"), pid("p0")}, false});
                s.entities.push_back(sk::Entity{pid("a0"), sk::Point{{x0, z0 - 1}}, false});
                s.entities.push_back(sk::Entity{pid("a1"), sk::Point{{x0, z0 + h + 1}}, false});
                s.entities.push_back(sk::Entity{pid("axis"), sk::Line{pid("a0"), pid("a1")}, true});
                add(sketch, "sketch", "Sketch " + f.id,
                    R"({"plane":{"datum":)" + quote(datum.value.toString()) + R"(},"sketch":)" +
                        ray::doc::json::write(ray::doc::features::sketchToJson(s)) + "}",
                    f.suppressed);
                const std::string region =
                    "[" + quote(pid("bottom").value.toString()) + "," + quote(pid("side").value.toString()) + "," +
                    quote(pid("top").value.toString()) + "," + quote(pid("inner").value.toString()) + "]";
                add(op, "revolve", f.id,
                    R"({"sketch":)" + quote(sketch.value.toString()) + R"(,"regions":[)" + region +
                        R"(],"axis":{"entity":)" + quote(pid("axis").value.toString()) + R"(},"operation":")" +
                        operation + R"(","target":)" + body + "}",
                    f.suppressed);
                continue;
            }
            add(datum, "datum_plane", "Datum " + f.id, datumProps(f.frame), f.suppressed);
            add(sketch, "sketch", "Sketch " + f.id,
                R"({"plane":{"datum":)" + quote(datum.value.toString()) + R"(},"sketch":)" +
                    ray::doc::json::write(ray::doc::features::sketchToJson(sketchOf(f))) + "}",
                f.suppressed);
            std::string regions;
            for (const o::Loop& l : f.loops) {
                // The baseline's selector; a feature the baseline does not have is picked by its own outline.
                const auto it = selectors.find({f.id, l.id});
                std::vector<std::uint64_t> own;
                for (const o::Ent& e : l.ents) {
                    own.push_back(e.tag);
                }
                std::string ids;
                for (const std::uint64_t tag : it != selectors.end() ? it->second : own) {
                    o::Ent e;
                    e.tag = tag;
                    ids += (ids.empty() ? "" : ",") + quote(entityId(f.id, l.id, entName(e)).value.toString());
                }
                regions += (regions.empty() ? "[" : ",[") + ids + "]";
            }
            add(op, "extrude", f.id,
                R"({"sketch":)" + quote(sketch.value.toString()) + R"(,"regions":[)" + regions + R"(],"distance":)" +
                    num(f.dist) + R"(,"direction":)" + (f.op == o::BOp::Cut ? R"("reverse")" : R"("normal")") +
                    R"(,"operation":")" + operation + R"(","target":)" + body + "}",
                f.suppressed);
        } else if (f.type == o::FType::Fillet || f.type == o::FType::Chamfer) {
            const auto it = blendEdges.find(f.id);
            if (it == blendEdges.end()) {
                continue;
            }
            const FeatureId op = fid(f.id, "op");
            out.opOf[f.id] = op;
            const bool fillet = f.type == o::FType::Fillet;
            add(op, fillet ? "fillet" : "chamfer", f.id,
                R"({"edges":)" + it->second + (fillet ? R"(,"radius":)" : R"(,"distance":)") + num(f.size) + "}",
                f.suppressed);
        }
    }
    return out;
}

// ---------- Commands ----------

ray::Result<DocStatePtr> runCommand(const DocStatePtr& state, const std::string& name, const std::string& args)
{
    const ray::doc::ICommandHandler* handler = ray::doc::builtinCommandRegistry()->find(name);
    if (handler == nullptr) {
        return tl::unexpected(ray::Error{ray::ErrorCode::NotFound, "no command " + name, {}});
    }
    auto next = handler->apply(state, args);
    if (!next) {
        return tl::unexpected(ray::Error{next.error().code, name + ": " + next.error().message, {}});
    }
    return *next;
}

std::string featureArgs(const ray::doc::Feature& f, bool withType)
{
    return "{" + std::string(R"("id":)") + quote(f.id.value.toString()) + R"(,"name":)" + quote(f.name) +
           (withType ? R"(,"type":)" + quote(f.type) : std::string()) + R"(,"props":)" + f.propsJson + "}";
}

// Turns `from` into a snapshot with exactly the features `to` lists, through the generic feature commands; returns the
// number of commands run.
ray::Result<DocStatePtr> applyDiff(DocStatePtr state, const std::vector<ray::doc::Feature>& to, int* commands)
{
    int count = 0;
    auto run = [&](const std::string& name, const std::string& args) -> ray::Status {
        auto next = runCommand(state, name, args);
        if (!next) {
            return tl::unexpected(next.error());
        }
        state = *next;
        ++count;
        return {};
    };
    auto find = [](const std::vector<ray::doc::Feature>& list, const FeatureId& id) -> const ray::doc::Feature* {
        for (const auto& f : list) {
            if (f.id == id) {
                return &f;
            }
        }
        return nullptr;
    };
    for (const ray::doc::Feature& f : std::vector<ray::doc::Feature>(state->features)) {
        if (find(to, f.id) == nullptr) {
            if (auto st = run("feature.remove", R"({"id":)" + quote(f.id.value.toString()) + "}"); !st) {
                return tl::unexpected(st.error());
            }
        }
    }
    for (const ray::doc::Feature& f : to) {
        const ray::doc::Feature* now = find(state->features, f.id);
        if (now == nullptr) {
            if (auto st = run("feature.add", featureArgs(f, true)); !st) {
                return tl::unexpected(st.error());
            }
        } else if (now->propsJson != f.propsJson) {
            if (auto st = run("feature.edit",
                              R"({"id":)" + quote(f.id.value.toString()) + R"(,"props":)" + f.propsJson + "}");
                !st) {
                return tl::unexpected(st.error());
            }
        }
    }
    for (std::size_t i = 0; i < to.size(); ++i) {
        if (state->features[i].id != to[i].id) {
            if (auto st = run("feature.reorder", R"({"id":)" + quote(to[i].id.value.toString()) + R"(,"index":)" +
                                                     std::to_string(i) + "}");
                !st) {
                return tl::unexpected(st.error());
            }
        }
        if (state->features[i].suppressed != to[i].suppressed) {
            if (auto st = run("feature.suppress", R"({"id":)" + quote(to[i].id.value.toString()) + R"(,"suppressed":)" +
                                                      (to[i].suppressed ? "true" : "false") + "}");
                !st) {
                return tl::unexpected(st.error());
            }
        }
    }
    if (commands != nullptr) {
        *commands = count;
    }
    return state;
}

// ---------- Rebuild environment (one per worker thread; no Catch2 assertions off the main thread) ----------

struct Env {
    explicit Env(int workers) : jobs(ray::makeJobSystem(ui, workers))
    {
        rebuilder = ray::doc::makeRebuilder(*jobs, ray::doc::builtinFeatureRegistry(), ray::doc::makeObjectPool());
    }
    ray::Result<BuildResultPtr> build(const DocStatePtr& state)
    {
        ray::JobContext ctx;
        return rebuilder->rebuild(state, ctx);
    }
    ray::test::TestDispatcher ui;
    std::unique_ptr<ray::IJobSystem> jobs;
    std::shared_ptr<ray::doc::IRebuilder> rebuilder;
};

bool built(const ray::doc::BuildResult& b, const FeatureId& id)
{
    for (const auto& r : b.reports) {
        if (r.id == id) {
            return r.status == FeatureStatus::Ok || r.status == FeatureStatus::Warning;
        }
    }
    return false;
}

const ray::doc::FeatureReport* reportOf(const ray::doc::BuildResult& b, const FeatureId& id)
{
    for (const auto& r : b.reports) {
        if (r.id == id) {
            return &r;
        }
    }
    return nullptr;
}

// The script features whose document feature built, in script order.
o::Script builtScript(const o::Script& script, const Generated& g, const ray::doc::BuildResult& b)
{
    o::Script out;
    for (const o::Feature& f : script) {
        const auto it = g.opOf.find(f.id);
        if (!f.suppressed && it != g.opOf.end() && built(b, it->second)) {
            out.push_back(f);
        }
    }
    return out;
}

const ray::doc::BuiltBody* bodyIn(const ray::doc::BuildResult& b)
{
    const auto it = b.bodies.find(partBody());
    return it == b.bodies.end() ? nullptr : &it->second;
}

// ---------- Judging (the accounting of tests/naming/oracle/harness.cpp judgeAll) ----------

std::string list(const std::vector<std::uint32_t>& v)
{
    std::string s = "{";
    for (std::size_t i = 0; i < v.size(); ++i) {
        s += (i ? " " : "") + std::to_string(v[i]);
    }
    return s + "}";
}

std::string roleNames(const ray::naming::ElementMap& map, SubId s)
{
    std::string out;
    for (const ray::Hash256& n : map.namesOf(s)) {
        const ray::naming::TopoName* r = map.record(n);
        out += (out.empty() ? "" : ", ") + (r != nullptr ? r->role : std::string("?")) + "#" + n.hex().substr(0, 6);
    }
    return out;
}

const char* kindName(SubKind k)
{
    return k == SubKind::Face ? "face" : k == SubKind::Edge ? "edge" : "vertex";
}

void account(const std::string& id, const o::RefResult& rr, const ray::naming::Resolved& res, bool weak,
             o::Tally& tally, o::JudgeLog& log, const std::string& detail)
{
    const o::Verdict v = rr.judgement.verdict;
    tally.add(rr.answer, v);
    if (weak) {
        ++tally.weak;
    }
    if (rr.answer == o::Answer::Repaired && res.usable()) {
        ++tally.repairedUsable;
    }
    if (!rr.expected.empty()) {
        ++tally.existing;
        if (rr.answer == o::Answer::Exact || rr.answer == o::Answer::Split || rr.answer == o::Answer::Merged) {
            ++tally.existingOk;
        }
    }
    if (!rr.noAnchor.empty()) {
        ++tally.noAnchor;
    }
    if (rr.judgement.twin) {
        ++tally.twins;
    }
    std::vector<std::uint32_t> answered;
    for (const SubId& s : res.elements) {
        answered.push_back(s.index);
    }
    const std::string answer = std::string(o::verdictName(v)) + " / " +
                               (res.state == ray::naming::ResolveState::Exact      ? "Exact"
                                : res.state == ray::naming::ResolveState::Split    ? "Split"
                                : res.state == ray::naming::ResolveState::Merged   ? "Merged"
                                : res.state == ray::naming::ResolveState::Repaired ? "Repaired"
                                                                                   : "Broken") +
                               " " + list(answered);
    auto addWrong = [&](const std::string& line) {
        if (log.wrongs.size() < log.maxWrongs) {
            log.wrongs.push_back(line);
        }
    };
    if (!rr.judgement.accepted.empty()) {
        ++tally.accepted;
        if (!rr.ambiguous && o::reviewedAnswer(o::reviewedTerritory(), id, rr.signature) != nullptr) {
            ++tally.acceptedReviewed;
        }
        log.notes.push_back("ACCEPTED " + id + ": " + rr.judgement.accepted +
                            o::reviewNote(o::reviewedTerritory(), id, rr.signature));
    }
    if (rr.conflict == 1) {
        ++tally.conflicts;
        if (!rr.ambiguous && o::reviewedAnswer(o::reviewedConflicts(), id, rr.signature) != nullptr) {
            ++tally.conflictsReviewed;
        }
        log.notes.push_back("CONFLICT " + id + ": congruent in place " + std::to_string(rr.congruent) +
                            ", oracle expects " + list(rr.expected) + ", resolver " + answer +
                            o::reviewNote(o::reviewedConflicts(), id, rr.signature));
    }
    if (rr.conflict == 2) {
        ++tally.conflictsOther;
    }
    if (rr.conflict == 3) {
        ++tally.congruentGhosts;
    }
    if (!rr.selfCheck.empty()) {
        ++tally.selfCheckFailures;
        addWrong("ORACLE SELF-CHECK FAILED " + id + ": " + rr.selfCheck);
    }
    for (const o::RefResult::Mutant& mu : rr.mutants) {
        auto& m = tally.mutants[mu.cls];
        ++m[0];
        if (mu.caught) {
            ++m[1];
        }
        if (mu.byReviewOnly) {
            ++m[2];
        }
        if (!mu.caught) {
            addWrong("MUTANT NOT CAUGHT " + id + " [" + mu.cls + "]: " + mu.detail);
        }
    }
    if (o::isWrong(v) || v == o::Verdict::Unjudged) {
        addWrong(id + ": " + answer + " (" + res.reason + "), oracle expects " + list(rr.expected) +
                 (rr.merged ? " merged" : "") + (rr.noAnchor.empty() ? "" : " (" + rr.noAnchor + ")") + ", congruent " +
                 std::to_string(rr.congruent) + "\n" + detail);
    }
}

struct Pick {
    std::size_t target = 0;
    ray::naming::TopoRef ref;
    o::PickRecord record;
};
using Picks = std::map<std::string, std::vector<Pick>>;

struct Baseline {
    o::Script script;
    Selectors selectors;
    std::map<std::string, std::string> blendEdges;
    Picks picks;
    DocStatePtr state;
    Generated generated;
    BuildResultPtr build;
    std::vector<o::Ref> refs;
    int commands = 0;
    std::vector<std::string> problems;
};

// Builds the baseline document feature by feature through the typed add commands, picking blend edges on the
// document's own build as the harness does on its.
Baseline author(Env& env, Variant variant)
{
    Baseline b;
    b.script = o::s4::makeScript(o::s4::baseParams());
    b.selectors = selectorsOf(b.script);
    b.state = std::make_shared<const DocState>();
    o::Script soFar;
    for (const o::Feature& f : b.script) {
        if (f.type == o::FType::Fillet || f.type == o::FType::Chamfer) {
            auto build = env.build(b.state);
            const ray::doc::BuiltBody* body = build ? bodyIn(**build) : nullptr;
            if (body == nullptr) {
                b.problems.push_back(f.id + ": no body to pick on");
                continue;
            }
            auto samples = o::sampleShape(body->shape);
            if (!samples) {
                b.problems.push_back(f.id + ": sampling failed");
                continue;
            }
            const auto owners = std::make_shared<const o::Owners>(o::buildOwners(soFar));
            auto resolver = ray::naming::makeResolver({ray::naming::ResolverBody{partBody(), body->map, body->shape}});
            std::string edges;
            std::set<std::uint32_t> seen;
            for (std::size_t k = 0; k < f.targets.size(); ++k) {
                if (!f.targets[k].picked) {
                    continue;
                }
                for (auto& [e, desc] : o::pickTarget(f.targets[k], *owners, **samples)) {
                    const SubId edge{SubKind::Edge, e};
                    const ray::naming::TopoRef ref = resolver->makeRef(partBody(), edge);
                    b.picks[f.id].push_back(Pick{k, ref, o::PickRecord{edge, desc, *samples, owners}});
                    if (seen.insert(e).second) {
                        edges +=
                            (edges.empty() ? "" : ",") + ray::doc::json::write(ray::doc::features::topoRefToJson(ref));
                    }
                }
            }
            b.blendEdges[f.id] = "[" + edges + "]";
        }
        // Add this feature's document features with the typed commands.
        o::Script one{f};
        const Generated g = generate(one, b.selectors, b.blendEdges, variant);
        for (const ray::doc::Feature& df : g.features) {
            const std::string args = featureArgs(df, false);
            auto next = runCommand(b.state, "feature." + df.type + ".add", args);
            if (!next) {
                b.problems.push_back(next.error().message);
                continue;
            }
            b.state = *next;
            ++b.commands;
        }
        soFar.push_back(f);
    }
    b.generated = generate(b.script, b.selectors, b.blendEdges, variant);
    auto build = env.build(b.state);
    if (!build) {
        b.problems.push_back("baseline rebuild failed: " + build.error().message);
        return b;
    }
    b.build = *build;
    for (const auto& r : b.build->reports) {
        if (r.status != FeatureStatus::Ok && r.status != FeatureStatus::Warning) {
            b.problems.push_back("baseline feature " + r.message);
        }
    }
    const ray::doc::BuiltBody* body = bodyIn(*b.build);
    if (body == nullptr) {
        b.problems.push_back("no baseline body");
        return b;
    }
    auto samples = o::sampleShape(body->shape);
    if (!samples) {
        b.problems.push_back("baseline sampling failed");
        return b;
    }
    const auto owners = std::make_shared<const o::Owners>(o::buildOwners(builtScript(b.script, b.generated, *b.build)));
    const o::TruthBuilder tb(*owners, **samples);
    auto resolver = ray::naming::makeResolver({ray::naming::ResolverBody{partBody(), body->map, body->shape}});
    for (const SubKind kind : {SubKind::Face, SubKind::Edge, SubKind::Vertex}) {
        for (std::uint32_t i = 0; i < (*samples)->of(kind).size(); ++i) {
            const SubId s{kind, i};
            const o::TruthDesc d = tb.describe(s);
            if (!d.ok || body->map.namesOf(s).empty()) {
                continue;
            }
            b.refs.push_back(o::Ref{resolver->makeRef(partBody(), s), o::PickRecord{s, d, *samples, owners},
                                    roleNames(body->map, s)});
        }
    }
    return b;
}

struct ScenarioResult {
    o::Tally tally;
    o::JudgeLog log;
    std::vector<std::string> failed;
    int built = 0;
    int features = 0;
    int commands = 0;
    double seconds = 0;
    long picksSkipped = 0; // blends not built: their picks are not resolved by the document
};

ScenarioResult runScenario(Env& env, const Baseline& base, const o::s4::Scenario& sc, Variant variant,
                           const std::string& key)
{
    ScenarioResult out;
    out.log.maxWrongs = 40;
    o::s4::Params p = o::s4::baseParams();
    if (sc.p) {
        sc.p(p);
    }
    o::Script script = o::s4::makeScript(p);
    if (sc.s) {
        sc.s(script);
    }
    // The blends keep their baseline edges; their size comes from the scenario.
    const Generated g = generate(script, base.selectors, base.blendEdges, variant);
    const auto start = std::chrono::steady_clock::now();
    auto state = applyDiff(base.state, g.features, &out.commands);
    if (!state) {
        out.failed.push_back("applying the edit failed: " + state.error().message);
        return out;
    }
    auto build = env.build(*state);
    out.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (!build) {
        out.failed.push_back("rebuild failed: " + build.error().message);
        return out;
    }
    const ray::doc::BuildResult& b = **build;
    for (const auto& r : b.reports) {
        ++out.features;
        if (r.status == FeatureStatus::Ok || r.status == FeatureStatus::Warning) {
            ++out.built;
        } else if (r.status == FeatureStatus::Failed) {
            out.failed.push_back(r.message);
        }
    }
    const ray::doc::BuiltBody* body = bodyIn(b);
    if (body == nullptr) {
        out.failed.push_back("no body");
        return out;
    }
    auto samples = o::sampleShape(body->shape);
    if (!samples) {
        out.failed.push_back("sampling failed: " + samples.error().message);
        return out;
    }
    const o::Owners owners = o::buildOwners(builtScript(script, g, b));
    auto resolver = ray::naming::makeResolver({ray::naming::ResolverBody{partBody(), body->map, body->shape}});
    std::map<SubId, std::vector<std::string>> exactOn;
    for (const o::Ref& r : base.refs) {
        const ray::naming::Resolved res = resolver->resolve(r.ref);
        const o::RefResult rr = o::judgeReference(r.record, owners, **samples, res, "");
        const std::string id = key + "|" + kindName(r.record.base.kind) + " " + std::to_string(r.record.base.index);
        if (res.state == ray::naming::ResolveState::Exact && res.elements.size() == 1) {
            exactOn[res.elements.front()].push_back(id);
        }
        std::string detail;
        if (o::isWrong(rr.judgement.verdict) || rr.judgement.verdict == o::Verdict::Unjudged) {
            detail = "    " +
                     o::explain(r.record.truth, owners, **samples,
                                rr.congruent >= 0       ? rr.congruent
                                : !res.elements.empty() ? static_cast<long>(res.elements[0].index)
                                                        : -1) +
                     "\n    picked names [" + r.names + "]";
            for (const SubId& e : res.elements) {
                detail += "\n    answer " + std::to_string(e.index) + " [" + roleNames(body->map, e) + "]";
            }
        }
        account(id, rr, res, r.record.truth.weak, out.tally, out.log, detail);
    }
    for (const auto& [element, ids] : exactOn) {
        if (ids.size() < 2) {
            continue;
        }
        ++out.tally.injectivity;
        std::string all;
        for (const std::string& i : ids) {
            all += " " + i;
        }
        out.log.wrongs.push_back("INJECTIVITY: " + std::string(kindName(element.kind)) + " " +
                                 std::to_string(element.index) + " answered Exact for" + all);
    }
    // Blend picks: resolved on the body as it is just before the blend, as the fillet or chamfer resolves them.
    const DocStatePtr& s = *state;
    for (const o::Feature& f : script) {
        if ((f.type != o::FType::Fillet && f.type != o::FType::Chamfer) || f.suppressed) {
            continue;
        }
        const auto pickIt = base.picks.find(f.id);
        const auto opIt = g.opOf.find(f.id);
        if (pickIt == base.picks.end() || opIt == g.opOf.end()) {
            continue;
        }
        const ray::doc::FeatureReport* report = reportOf(b, opIt->second);
        if (report == nullptr || report->status == FeatureStatus::NotBuilt) {
            out.picksSkipped += static_cast<long>(pickIt->second.size());
            continue;
        }
        auto prefix = std::make_shared<DocState>(*s);
        const auto at = std::find_if(prefix->features.begin(), prefix->features.end(),
                                     [&](const ray::doc::Feature& x) { return x.id == opIt->second; });
        prefix->features.erase(at, prefix->features.end());
        auto before = env.build(prefix);
        const ray::doc::BuiltBody* pb = before ? bodyIn(**before) : nullptr;
        if (pb == nullptr) {
            out.picksSkipped += static_cast<long>(pickIt->second.size());
            continue;
        }
        auto ps = o::sampleShape(pb->shape);
        if (!ps) {
            out.picksSkipped += static_cast<long>(pickIt->second.size());
            continue;
        }
        o::Script soFar;
        for (const o::Feature& x : script) {
            if (x.id == f.id) {
                break;
            }
            const auto it = g.opOf.find(x.id);
            if (!x.suppressed && it != g.opOf.end() && built(**before, it->second)) {
                soFar.push_back(x);
            }
        }
        const o::Owners ownersAt = o::buildOwners(soFar);
        auto pr = ray::naming::makeResolver({ray::naming::ResolverBody{partBody(), pb->map, pb->shape}});
        for (const Pick& pk : pickIt->second) {
            const ray::naming::Resolved r = pr->resolve(pk.ref);
            if (!pk.record.truth.ok) {
                ++out.tally.picksUnjudged;
                continue;
            }
            ++out.tally.picks;
            const o::RefResult rr = o::judgeReference(pk.record, ownersAt, **ps, r, "pick: ");
            account(key + "|pick " + f.id + "/" + std::to_string(pk.target) + " edge " +
                        std::to_string(pk.record.base.index),
                    rr, r, false, out.tally, out.log, {});
        }
    }
    return out;
}

unsigned workerCount()
{
    return std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
}

void runAll(Variant variant)
{
    const char* label = variant == Variant::RevolveF04 ? "S4 document, F04 revolved"
                        : variant == Variant::Holes    ? "S4 document, holes as hole features"
                                                       : "S4 document";
    Env authorEnv(4);
    const Baseline base = author(authorEnv, variant);
    for (const std::string& p : base.problems) {
        std::printf("baseline problem: %s\n", p.c_str());
    }
    REQUIRE(base.problems.empty());
    REQUIRE(base.build != nullptr);
    const ray::doc::BuiltBody* body = bodyIn(*base.build);
    REQUIRE(body != nullptr);
    long nPicks = 0;
    for (const auto& [f, v] : base.picks) {
        nPicks += static_cast<long>(v.size());
    }
    std::printf("\n%s baseline: %zu document features from %zu script features (%d commands), %u faces, %u edges, "
                "%u vertices; %zu references, %ld blend picks\n",
                label, base.state->features.size(), base.script.size(), base.commands, body->shape.count(SubKind::Face),
                body->shape.count(SubKind::Edge), body->shape.count(SubKind::Vertex), base.refs.size(), nPicks);
    CHECK(base.refs.size() >= 600);

    std::vector<o::s4::Scenario> list = o::s4::scenarios();
    REQUIRE(list.size() == 47);
    if (const char* only = std::getenv("RAYCAD_S4DOC_ONLY")) {
        list.erase(std::remove_if(list.begin(), list.end(), [&](const o::s4::Scenario& s) { return s.id != only; }),
                   list.end());
        REQUIRE(!list.empty());
    }
    std::vector<ScenarioResult> results(list.size());
    std::vector<std::unique_ptr<Env>> envs;
    for (unsigned t = 0; t < workerCount(); ++t) {
        envs.push_back(std::make_unique<Env>(2));
    }
    std::atomic<std::size_t> next{0};
    auto worker = [&](Env* env) {
        // As in a session: the baseline was built before the edit, so a scenario's rebuild is incremental.
        (void)env->build(base.state);
        for (std::size_t i = next++; i < list.size(); i = next++) {
            results[i] = runScenario(*env, base, list[i], variant, "S4 " + list[i].id);
        }
    };
    std::vector<std::thread> threads;
    for (auto& env : envs) {
        threads.emplace_back(worker, env.get());
    }
    for (std::thread& t : threads) {
        t.join();
    }

    o::Tally total;
    std::string table = std::string("\n### ") + label +
                        "\n\n| scenario | built/features | failed | commands | rebuild s | refs | Exact % | Split % | "
                        "Merged % | Repaired % | Broken % | gone / missed | exists: E+S+M % (bar 95) | WRONG | not "
                        "judged | conflicts | accepted |\n|-|-|-|-|-|-|-|-|-|-|-|-|-|-|-|-|-|\n";
    for (std::size_t i = 0; i < list.size(); ++i) {
        const o::Tally& t = results[i].tally;
        total.merge(t);
        const bool below = t.existing > 0 && 100 * t.existingOk < 95 * t.existing;
        char row[512];
        std::snprintf(
            row, sizeof row,
            "| %s | %d/%d | %zu | %d | %.2f | %ld | %s | %s | %s | %s | %s | %ld / %ld | %s%s | %ld | %ld | "
            "%ld | %ld |\n",
            list[i].id.c_str(), results[i].built, results[i].features, results[i].failed.size(), results[i].commands,
            results[i].seconds, t.n, o::percent(t.state[0], t.n).c_str(), o::percent(t.state[1], t.n).c_str(),
            o::percent(t.state[2], t.n).c_str(), o::percent(t.state[3], t.n).c_str(),
            o::percent(t.state[4], t.n).c_str(), t.verdict[static_cast<std::size_t>(o::Verdict::BrokenGone)],
            t.verdict[static_cast<std::size_t>(o::Verdict::Missed)], o::percent(t.existingOk, t.existing).c_str(),
            below ? " BELOW" : "", t.wrong(), t.unjudged(), t.conflicts, t.accepted);
        table += row;
    }
    std::printf("%s", table.c_str());
    for (std::size_t i = 0; i < list.size(); ++i) {
        for (const std::string& f : results[i].failed) {
            std::printf("  %s: feature failed: %s\n", list[i].id.c_str(), f.c_str());
        }
        if (results[i].picksSkipped > 0) {
            std::printf("  %s: %ld blend picks not resolved (their blend was not built)\n", list[i].id.c_str(),
                        results[i].picksSkipped);
        }
        for (const std::string& w : results[i].log.wrongs) {
            std::printf("  %s\n", w.c_str());
        }
    }
    for (const ScenarioResult& r : results) {
        for (const std::string& n : r.log.notes) {
            std::printf("  %s\n", n.c_str());
        }
    }
    o::printTally(label, total);
    if (const char* file = std::getenv("RAYCAD_S4DOC_TABLE")) {
        if (std::FILE* f = std::fopen(file, "a")) {
            std::fprintf(f, "%s", table.c_str());
            std::fclose(f);
        }
    }
    CHECK(total.mutantsCaught() == total.mutantsMade());
    CHECK(total.repairedUsable == 0);
    CHECK(total.selfCheckFailures == 0);
    CHECK(total.injectivity == 0);
    CHECK(total.unjudged() == 0);
    CHECK(total.accepted == total.acceptedReviewed);
    CHECK(total.conflicts == total.conflictsReviewed);
    if (list.front().id == "N00") {
        CHECK(results[0].tally.state[0] == results[0].tally.n);
        CHECK(results[0].commands == 0);
    }
    REQUIRE(total.wrong() == 0);
}

} // namespace

TEST_CASE("geometry goldens: the S4 part as document features", "[golden-geometry]")
{
    const std::pair<Variant, const char*> parts[] = {{Variant::Plain, "s4_document"},
                                                     {Variant::RevolveF04, "s4_document_revolve"},
                                                     {Variant::Holes, "s4_document_holes"}};
    for (const auto& [variant, name] : parts) {
        Env env(4);
        const Baseline base = author(env, variant);
        REQUIRE(base.problems.empty());
        const ray::doc::BuiltBody* body = bodyIn(*base.build);
        REQUIRE(body != nullptr);
        ray::doc::test::checkGoldenGeometry(name, body->shape, body->map);
    }
}

TEST_CASE("the S4 part as document features: no reference resolves to a wrong element in the 47 scenarios", "[s4doc]")
{
    runAll(Variant::Plain);
}

TEST_CASE("the S4 part with its round boss revolved: no reference resolves to a wrong element", "[s4doc-revolve]")
{
    runAll(Variant::RevolveF04);
}

// Task T-097: the S4 part with its 11 hole sweeps made by the hole feature (simple, flat-bottomed or through) at
// sketch points, judged by the same oracle on the same 47 scenarios.
TEST_CASE("the S4 part with hole features: no reference resolves to a wrong element", "[s4doc-hole]")
{
    runAll(Variant::Holes);
}

TEST_CASE("harness and document give the resolver the same placements, feature kind by feature kind",
          "[s4doc-parity][doc]")
{
    // T-081b round 6, item 10 (the lead's decision): the naming oracle's harness must give the resolver exactly what the
    // document gives. The S4 part with hole features is built both ways from one script: as document features (an
    // extrude per sweep, a hole feature per drilled sweep, fillets and chamfers) and as the harness's script. For every
    // script feature, the document's element map holds a placement exactly when the harness's does.
    Env env(2);
    const Baseline b = author(env, Variant::Holes);
    for (const std::string& p : b.problems) {
        INFO(p);
    }
    REQUIRE(b.problems.empty());
    REQUIRE(b.build != nullptr);
    const ray::doc::BuiltBody* body = bodyIn(*b.build);
    REQUIRE(body != nullptr);
    o::FeatRefs refs;
    const o::Builder harness = o::buildScript(b.script, o::Mode::Author, refs);
    REQUIRE(harness.failed().empty());
    std::size_t placed = 0, compared = 0;
    for (const o::Feature& f : b.script) {
        const auto op = b.generated.opOf.find(f.id);
        if (op == b.generated.opOf.end()) {
            continue;
        }
        ++compared;
        const bool inDocument = body->map.placement(op->second) != nullptr;
        const bool inHarness = harness.map().placement(o::featureIdOf(f.id)) != nullptr;
        INFO(f.id << " (" << f.role << "): document " << inDocument << ", harness " << inHarness);
        CHECK(inDocument == inHarness);
        placed += inDocument ? 1 : 0;
    }
    CHECK(compared >= 6);
    CHECK(placed >= 3); // the base, the sweeps and the holes are placed; blends are not
}
