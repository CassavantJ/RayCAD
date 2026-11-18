// The build harness of the naming tests (task T-081a): runs a script (oracle.h) through the kernel operations and
// the naming code, the way the document rebuilder will (one propagate() per kernel operation), and judges
// references with the oracle. This is the only part of tests/naming/oracle that uses the naming module; the oracle
// itself (oracle.h, its own library) does not.
//
// How a feature is built:
//  - Import: the base shape; names from nameImported.
//  - Sweep: each loop is its own region: extrude it (Cut along -n by dist, New and Join along +n), name the tool
//    from its sweep origins (propagate with no input), then boolean it into the body and name the result
//    (propagate with the body's and the tool's maps). The first New loop, or the first loop when there is no body
//    yet, becomes the body.
//  - Fillet, chamfer: its edges are references. While authoring (the baseline build), the oracle picks the edges of
//    every picked target and a TopoRef is made of each; while replaying (an edited script), each TopoRef is
//    resolved, judged by the oracle, and the edges of every usable() answer are blended. With none usable the
//    feature fails, as it would in the product.
// A failing feature is skipped and recorded in Build::failed; later features continue on the body.
#pragma once
#include "oracle.h"

#include <ray/naming.h>

#include <map>
#include <set>
#include <string>
#include <vector>

namespace ray::naming_oracle {

FeatureId featureIdOf(const std::string& id);
BodyId testBody();

struct Pick {
    std::size_t target = 0;
    naming::TopoRef ref;
    TruthDesc truth;
};
using FeatRefs = std::map<std::string, std::vector<Pick>>;

struct PickOutcome {
    std::string feature;
    std::size_t target = 0;
    naming::Resolved resolved;
    TruthEval expected;
    Verdict verdict = Verdict::Correct;
};

Answer answerOf(naming::ResolveState state);

enum class Mode { Author, Replay };

class Builder {
public:
    Builder(Mode mode, FeatRefs& refs) : mode_(mode), refs_(refs) {}
    // Builds one feature on the current body. Blend picks are made and judged with the owners of the features
    // built so far.
    void apply(const Feature& f);

    bool have() const { return have_; }
    const kernel::Shape& shape() const { return shape_; }
    const naming::ElementMap& map() const { return map_; }
    const std::vector<std::string>& failed() const { return failed_; }
    const std::vector<PickOutcome>& picks() const { return picks_; }
    int built() const { return built_; }
    // Ids of the features that built (a failed feature leaves the body as it was).
    const std::set<std::string>& builtIds() const { return builtIds_; }
    int unverifiablePicks() const { return unverifiablePicks_; }
    std::size_t lineageEvents() const { return lineage_; }
    // Every map the build produced, serialised, in order (for determinism checks).
    const std::vector<std::vector<std::byte>>& history() const { return history_; }
    void keepHistory(bool on) { keepHistory_ = on; }

private:
    bool sweep(const Feature& f);
    bool blend(const Feature& f);
    Script builtFeatures_;
    void record(naming::NamingOutput out, const kernel::Shape& shape);

    Mode mode_;
    FeatRefs& refs_;
    bool have_ = false;
    kernel::Shape shape_;
    naming::ElementMap map_;
    std::vector<std::string> failed_;
    std::vector<PickOutcome> picks_;
    int built_ = 0;
    std::set<std::string> builtIds_;
    int unverifiablePicks_ = 0;
    std::size_t lineage_ = 0;
    bool keepHistory_ = false;
    std::vector<std::vector<std::byte>> history_;
};

// Builds the whole script.
Builder buildScript(const Script& script, Mode mode, FeatRefs& refs, bool keepHistory = false);

// The features of `script` that built in `b`: the oracle must describe the shape that was built, so the owners of a
// feature that failed (and of blends on its faces) are left out.
Script builtOnly(const Script& script, const Builder& b);

// A reference recorded on a baseline build, with the oracle's description of its element.
struct Ref {
    SubId base;
    naming::TopoRef ref;
    TruthDesc truth;
    ElemSamples picked; // the element's samples on the baseline (for the congruence cross-check)
    std::string names;  // the roles of its names, for diagnosis
};

// Every element of the built shape that the oracle can vouch for and that has a name, as a reference. `skipped`
// counts the others by reason.
std::vector<Ref> recordReferences(const Builder& b, const Owners& owners, const Samples& samples,
                                  std::map<std::string, int>* skipped);

// Totals of judged resolutions.
struct Tally {
    long n = 0;
    std::array<long, 5> state{};         // by Answer
    std::array<long, 7> verdict{};       // by Verdict
    long existing = 0, existingOk = 0;   // references whose element still exists, of them Exact/Split/Merged
    long conflicts = 0;                  // congruent-in-place element exists but the oracle names another answer
    long mutants = 0, mutantsCaught = 0; // non-weak references only
    long weak = 0;                       // references the oracle checks only for membership
    long repairedUsable = 0;             // Repaired answers that claimed to be usable (must stay 0)
    long newTerritory = 0;               // verdicts accepted by the oracle's new-territory rule (oracle.h)
    long unjudged = 0;                   // wrong-looking answers on elements the mesher left without triangles
    void add(Answer a, Verdict v);
    void merge(const Tally& o);
    long wrong() const;
};

// Resolves every reference against the replayed build, judges it (with the new-territory rule of oracle.h, against
// `baseline`, the samples of the build the references were recorded on), and adds it to `tally`. Wrong answers are
// described in `wrongs` (at most `maxWrongs` lines).
void judgeAll(const std::vector<Ref>& refs, const Builder& replay, const Owners& owners, const Samples& samples,
              const Samples& baseline, Tally& tally, std::vector<std::string>& wrongs, std::size_t maxWrongs,
              const std::string& label);

std::string percent(long a, long n);

// For diagnosis: every element of a kind with its names and bounding box, one per line.
std::string dumpElements(const Builder& b, const Samples& samples, SubKind kind);

} // namespace ray::naming_oracle
