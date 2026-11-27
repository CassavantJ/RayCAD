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
//  - Edit (task T-081b): a direct edit of the faces of its target owners (oracle.h EditSpec). While authoring, or
//    while replaying an edit that was not in the baseline (no picks recorded for it), the faces of every target are
//    picked by the oracle (each must be described by its target alone) and a TopoRef is made of each; while replaying
//    a recorded edit, each TopoRef is resolved and judged like a blend pick, and the edit runs only if every answer
//    is Exact (a Split or Merged face would make the edit move or remove more than its targets, which the oracle's
//    model of the edit does not describe). The kernel call (deleteFaces, offsetFaces, resizeHole, unifySameDomain)
//    has a time limit (RAYCAD_NAMING_EDIT_LIMIT_S, default 60 s, 600 s without NDEBUG); then measureEdit fills in what
//    it did from the shapes, and a result the oracle cannot describe fails the feature (the body stays as it was). The
//    result is named by propagate() with the kernel's history, the body's map and shape as the one input.
// A failing feature is skipped and recorded in Build::failed; later features continue on the body.
#pragma once
#include "oracle.h"

#include <ray/naming.h>

#include <functional>
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
    PickRecord record; // record.truth.ok false: the oracle cannot vouch for the pick, and it is not judged
};
using FeatRefs = std::map<std::string, std::vector<Pick>>;

// One reference judged on an edited build: the verdict on the resolver's answer, the oracle's own cross-checks,
// and the mutation test of the oracle (mutants of the oracle's expected answer pushed through the same judge).
struct RefResult {
    Answer answer = Answer::Broken;
    Judgement judgement;
    std::vector<std::uint32_t> expected; // E
    bool merged = false;
    std::string noAnchor;
    std::string crossing;  // E was told by the sliding-crossing rule (oracle.h Expectation::crossing), and how
    long congruent = -1;   // the one element congruent in place with the pick, or -1
    int conflict = 0;      // the one element congruent in place is not E, nothing moved, and: 1 the answer was judged
                           // correct, 2 it was not (Broken, or judged wrong); 3: the same after a move
    std::string selfCheck; // non-empty: the oracle's own expected answer was not judged correct (why)
    std::string signature; // the resolver's answer, answerSignature (matched against the reviewed lists)
    bool ambiguous = false; // an element of the answer has a near twin in the shape (signatureAmbiguous): never reviewed
    struct Mutant {
        std::string cls;
        bool caught = false;
        bool byReviewOnly = false; // accepted by the new-territory rule: caught only because mutants are never on
                                   // the reviewed list
        std::string detail;
    };
    std::vector<Mutant> mutants;
};
// `prefix` names the reference class in the mutant classes ("weak: ", "pick: ").
RefResult judgeReference(const PickRecord& pick, const Owners& now, const Samples& samples,
                         const naming::Resolved& resolved, const std::string& prefix);

struct PickOutcome {
    std::string feature;
    std::size_t target = 0;
    std::uint32_t edge = 0; // the picked edge on the shape it was picked on
    naming::Resolved resolved;
    RefResult result;
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
    // Replay: picks the oracle could not vouch for when they were made (resolved and used, not judged).
    int unjudgedPicks() const { return unjudgedPicks_; }
    std::size_t lineageEvents() const { return lineage_; }
    // The features that built, in order, as built: a direct edit with what measureEdit found it did. The oracle's
    // owners of the built shape are buildOwners(builtScript()) (for a script without direct edits the same as
    // buildOwners(builtOnly(script, builder))).
    const Script& builtScript() const { return builtFeatures_; }
    // Direct edits that failed because the kernel call ran past its time limit (included in failed()).
    int editTimeouts() const { return editTimeouts_; }
    // What became of every direct edit applied, as "<role>: <outcome>" -> count; outcomes: "applied", "kernel
    // refused" (KernelFailure or InvalidArgument), "time limit", "not modelled (<why>)" (measureEdit), "pick not
    // Exact", "no target face", "other" (sampling or naming failed).
    const std::map<std::string, int>& editCounts() const { return editCounts_; }
    // Every map the build produced, serialised, in order (for determinism checks).
    const std::vector<std::vector<std::byte>>& history() const { return history_; }
    void keepHistory(bool on) { keepHistory_ = on; }

private:
    bool sweep(const Feature& f);
    bool blend(const Feature& f);
    bool edit(Feature& f); // fills f.edit's measured fields
    Script builtFeatures_;
    int editTimeouts_ = 0;
    std::map<std::string, int> editCounts_;
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
    int unjudgedPicks_ = 0;
    std::size_t lineage_ = 0;
    bool keepHistory_ = false;
    std::vector<std::vector<std::byte>> history_;
};

// Builds the whole script.
Builder buildScript(const Script& script, Mode mode, FeatRefs& refs, bool keepHistory = false);

// The features of `script` that built in `b`: the oracle must describe the shape that was built, so the owners of a
// feature that failed (and of blends on its faces) are left out.
Script builtOnly(const Script& script, const Builder& b);

// A reference recorded on a baseline build, with what the oracle keeps of it.
struct Ref {
    naming::TopoRef ref;
    PickRecord record;
    std::string names; // the roles of its names, for diagnosis
};

// Every element of the built shape that the oracle can vouch for and that has a name, as a reference. `skipped`
// counts the others by reason.
std::vector<Ref> recordReferences(const Builder& b, std::shared_ptr<const Owners> owners,
                                  std::shared_ptr<const Samples> samples, std::map<std::string, int>* skipped);

// Totals of judged resolutions.
struct Tally {
    long n = 0;
    std::array<long, 5> state{};             // by Answer
    std::array<long, kVerdicts> verdict{};   // by Verdict
    long existing = 0, existingOk = 0;       // references whose element still exists, of them Exact/Split/Merged
    long weak = 0;                           // references without separators (judged by position, oracle.h)
    long picks = 0;                          // blend picks judged (included in n)
    long picksUnjudged = 0;                  // blend picks the oracle could not vouch for (not in n)
    long repairedUsable = 0;                 // Repaired answers that claimed to be usable (must stay 0)
    long noAnchor = 0;                       // references whose continuation the oracle cannot tell (any answer)
    long crossings = 0, crossingsAtSeam = 0; // references whose E the sliding-crossing rule told (any answer)
    long accepted = 0, acceptedReviewed = 0; // new-territory acceptances, and of them those on the reviewed list
    long acceptedNewGround = 0;         // flagged answers accepted by the new-ground rule (oracle.cpp newGroundRule)
    long acceptedNewGroundReviewed = 0; // of those, on the reviewed list (reviewed_territory.txt)
    long conflicts = 0;                 // the one congruent-in-place element is not E, nothing moved, and the
                                        // answer was judged correct (gated: oracle and resolver may share an error)
    long conflictsReviewed = 0;
    long unjudgedReviewed = 0;  // not-judged answers on the reviewed list (reviewed_unjudged.txt)
    long conflictsOther = 0;    // the same with a Broken or wrong answer (counted: no answer was accepted)
    long congruentGhosts = 0;   // the same after a move (another element took the place: counted only)
    long injectivity = 0;       // elements answered Exact for two or more distinct references
    long twins = 0;             // Exact on one of coincident vertices (oracle.h Expectation::twins)
    long selfCheckFailures = 0; // the oracle's own expected answer not judged correct
    std::map<std::string, std::array<long, 3>> mutants; // class: made, caught, caught only by the review gate
    void add(Answer a, Verdict v);
    void merge(const Tally& o);
    long wrong() const;
    long unjudged() const { return verdict[static_cast<std::size_t>(Verdict::Unjudged)]; }
    long mutantsMade() const;
    long mutantsCaught() const;
};

struct JudgeLog {
    std::vector<std::string> wrongs; // wrong answers and failed cross-checks (at most maxWrongs)
    std::vector<std::string> notes;  // every new-territory acceptance and every conflict, reviewed or not
    std::vector<std::string> trace;  // one line per judged reference, when RAYCAD_NAMING_VERDICTS is set
    std::size_t maxWrongs = 20;
};

// The reviewed lists (tests/naming/oracle/reviewed_territory.txt, reviewed_conflicts.txt, reviewed_unjudged.txt):
// one line per reviewed ANSWER, "<key> => <answer signature> => <one-line justification>". Keys are
// "<run key>|<kind> <baseline index>" for references and "<run key>|pick <feature>/<target> edge <index>" for blend
// picks. A key may be listed more than once (one entry per reviewed answer: another platform's, or the answer
// without placements). An answer counts as reviewed only when its signature matches an entry's (sameAnswer): a later
// answer to a listed reference that is another element is NOT reviewed (T-081b round 5, item 1: the list used to
// match the key alone, so a regression to a wrong element on a listed reference passed).
struct ReviewedEntry {
    std::string answer; // the reviewed answer's signature (answerSignature); empty: none recorded, never matches
    std::string why;
};
using ReviewedList = std::multimap<std::string, ReviewedEntry>;
const ReviewedList& reviewedTerritory();
const ReviewedList& reviewedConflicts();
// tests/naming/oracle/reviewed_unjudged.txt: answers the oracle could not judge (no anchor), each looked at.
const ReviewedList& reviewedUnjudged();
// The gate on answers the oracle could not judge, for every block: each must be on the reviewed list by its answer
// (T-081b round 6: it used to apply to a holdout block only once the list had an entry with the block's prefix).
inline bool unjudgedGateOk(const Tally& t) { return t.unjudged() == t.unjudgedReviewed; }
// The entry of `list` for reference `id` whose reviewed answer matches `signature`, or null.
const ReviewedEntry* reviewedAnswer(const ReviewedList& list, const std::string& id, const std::string& signature);
// " [reviewed: <why>]", or " [NOT REVIEWED; answer <signature>]" with the listed answers when the key is listed for
// other answers only.
std::string reviewNote(const ReviewedList& list, const std::string& id, const std::string& signature);

// An answer's geometric signature, platform-tolerant (element numbers differ between platforms; the geometry of one
// answer does not): its state, then per element its kind, centroid (by the samples' weights; an edge without samples:
// its curve's midpoint) and area or length, and for an edge its end points (sorted), e.g.
// "Exact edge(1.3573 1.1583 0.4967 3.3246 1.3573 1.1583 -1.1656 1.3573 1.1583 2.1590)"; elements in order of kind and
// centroid (T-081b round 6: edges got their end points and the kernel's length; an edge without samples used to read
// edge(0 0 0 0) and match every other).
std::string answerSignature(Answer answer, const std::vector<SubId>& elements, const Samples& samples);
std::string answerSignature(const naming::Resolved& resolved, const Samples& samples);
// Do two signatures describe the same answer: the same state, and element for element the same kind, centroids
// within 0.02 mm, areas or lengths within 1 % (+0.001)? An empty or unreadable signature matches nothing.
bool sameAnswer(const std::string& a, const std::string& b);
// Has an element of the answer a near twin in its shape: another element of its kind whose signature sameAnswer cannot
// tell from its own (ElemSamples::twin)? Such an answer is never counted as reviewed: an entry could not tell it from
// its twin (T-081b round 6, the reviewer's collision scan: vertex pairs 5..19 um apart).
bool signatureAmbiguous(const std::vector<SubId>& elements, const Samples& samples);

// Resolves every reference against the replayed build, judges it with the full judge (oracle.h), adds it and the
// replay's blend picks to `tally`, and runs the cross-checks (congruence, injectivity, mutants). `key` names the run
// in the reviewed lists and in the log.
void judgeAll(const std::vector<Ref>& refs, const Builder& replay, const Owners& owners, const Samples& samples,
              Tally& tally, JudgeLog& log, const std::string& key);

// While set (on the calling thread), judgeAll calls this for every reference it judges: for tests that look at one
// reference of a randomised sequence (random_test.cpp [oracle-gate]). Pass an empty function to clear it.
using Inspector = std::function<void(const std::string& id, const Ref& ref, const naming::Resolved& resolved,
                                     const RefResult& result, const Owners& now, const Samples& samples)>;
void setInspector(Inspector inspector);

// Prints the totals of a tally, every bucket, in one block.
void printTally(const char* title, const Tally& t);

std::string percent(long a, long n);

// For diagnosis: every element of a kind with its names and bounding box, one per line.
std::string dumpElements(const Builder& b, const Samples& samples, SubKind kind);

} // namespace ray::naming_oracle
