// The non-visual part of the direct-edit tools and the reference review (task T-083, model/directedit_logic.h): the
// tools' features, the preview state machine (a stale result never enables OK; a refusal blocks it), drag snapping
// and length text in both units, and the review list's grouping and shop words. No window.
#include "model/directedit_logic.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

using Catch::Approx;
namespace de = ray::ui::directedit;

TEST_CASE("each direct-edit tool adds its feature type", "[directedit][unit]")
{
    for (const de::Tool t : {de::Tool::Delete, de::Tool::PushPull, de::Tool::Resize}) {
        CHECK(de::toolOfKey(de::toolKey(t)) == t);
        CHECK(de::toolOfFeatureType(de::featureType(t)) == t);
        CHECK_FALSE(de::toolTitle(t).isEmpty());
    }
    CHECK(de::featureType(de::Tool::Delete) == QStringLiteral("delete_face"));
    CHECK(de::featureType(de::Tool::PushPull) == QStringLiteral("offset_face"));
    CHECK(de::featureType(de::Tool::Resize) == QStringLiteral("resize_hole"));
    CHECK(de::toolTitle(de::Tool::PushPull) == QStringLiteral("Push/Pull"));
    CHECK(de::toolOfKey(QStringLiteral("fillet")) == de::Tool::None);
    CHECK(de::toolOfFeatureType(QStringLiteral("fillet")) == de::Tool::None);
    CHECK(de::toolKey(de::Tool::None).isEmpty());
}

TEST_CASE("the preview flow: only the newest preview counts, a refusal blocks OK", "[directedit][unit]")
{
    de::Flow f;
    CHECK_FALSE(f.active());
    CHECK_FALSE(f.canCommit());
    f.open();
    CHECK(f.phase() == de::Phase::Picking);
    CHECK(de::phaseKey(f.phase()).isEmpty());
    CHECK_FALSE(f.canCommit());

    // A preview of the current values builds: OK.
    const auto first = f.request();
    CHECK(f.phase() == de::Phase::Building);
    CHECK(f.busy());
    CHECK_FALSE(f.canCommit());
    CHECK(f.result(first, de::Outcome::Ok));
    CHECK(f.phase() == de::Phase::Ready);
    CHECK(f.canCommit());

    // The values change while a preview builds: the older answer is ignored, whatever it says.
    const auto older = f.request();
    const auto newer = f.request();
    CHECK_FALSE(f.result(older, de::Outcome::Ok));
    CHECK(f.phase() == de::Phase::Building);
    CHECK_FALSE(f.canCommit());
    // The kernel's warning still lets the user commit (it is shown first); a refusal does not.
    CHECK(f.result(newer, de::Outcome::Warning));
    CHECK(f.phase() == de::Phase::Warning);
    CHECK(de::phaseKey(f.phase()) == QStringLiteral("warning"));
    CHECK(f.canCommit());
    const auto refused = f.request();
    CHECK(f.result(refused, de::Outcome::Refused));
    CHECK(f.phase() == de::Phase::Refused);
    CHECK(de::phaseKey(f.phase()) == QStringLiteral("failed"));
    CHECK_FALSE(f.canCommit());
    f.commitStarted(); // nothing to commit
    CHECK(f.phase() == de::Phase::Refused);
    // An answer that comes twice changes nothing the second time.
    CHECK_FALSE(f.result(refused, de::Outcome::Ok));
    CHECK(f.phase() == de::Phase::Refused);

    // The document refuses the command before any build.
    const auto again = f.request();
    f.refuse();
    CHECK(f.phase() == de::Phase::Refused);
    CHECK_FALSE(f.result(again, de::Outcome::Ok)); // the build it would have started is not current

    // Incomplete picks or values: nothing to preview, nothing to commit; an answer on its way is dropped.
    const auto lost = f.request();
    f.incomplete();
    CHECK(f.phase() == de::Phase::Picking);
    CHECK_FALSE(f.result(lost, de::Outcome::Ok));

    // Commit: from Ready only; a failed commit is Refused (its message says why); a commit closes.
    const auto ok = f.request();
    REQUIRE(f.result(ok, de::Outcome::Ok));
    f.commitStarted();
    CHECK(f.phase() == de::Phase::Committing);
    CHECK(f.busy());
    CHECK_FALSE(f.canCommit());
    f.commitFailed();
    CHECK(f.phase() == de::Phase::Refused);
    const auto retry = f.request();
    REQUIRE(f.result(retry, de::Outcome::Ok));
    f.commitStarted();
    f.committed();
    CHECK_FALSE(f.active());
    // Closed: nothing reopens it by accident.
    const auto late = f.serial();
    CHECK_FALSE(f.result(late, de::Outcome::Ok));
    f.incomplete();
    f.refuse();
    CHECK(f.phase() == de::Phase::Closed);
}

TEST_CASE("drag snapping: 0.1 mm or 0.005 in, a tenth with Shift, never zero", "[directedit][unit]")
{
    CHECK(de::snapStepMm(false, false) == Approx(0.1));
    CHECK(de::snapStepMm(false, true) == Approx(0.01));
    CHECK(de::snapStepMm(true, false) == Approx(0.127));
    CHECK(de::snapStepMm(true, true) == Approx(0.0127));
    CHECK(de::snapLength(1.234, false, false) == Approx(1.2));
    CHECK(de::snapLength(1.26, false, false) == Approx(1.3));
    CHECK(de::snapLength(-2.04, false, false) == Approx(-2.0));
    CHECK(de::snapLength(1.234, false, true) == Approx(1.23));
    CHECK(de::snapLength(25.4 * 0.0512, true, false) == Approx(25.4 * 0.05));
    CHECK(de::snapLength(25.4 * 0.0512, true, true) == Approx(25.4 * 0.0510));
    // A push/pull of 0 is refused: the nearest step away from 0 instead.
    CHECK(de::snapLength(0.03, false, false) == Approx(0.1));
    CHECK(de::snapLength(-0.03, false, false) == Approx(-0.1));
    CHECK(de::snapLength(0.0, false, false) == Approx(0.1));

    CHECK(de::lengthText(1.2, false, false) == QStringLiteral("1.2 mm"));
    CHECK(de::lengthText(3.0, false, false) == QStringLiteral("3 mm"));
    CHECK(de::lengthText(-1.23, false, true) == QStringLiteral("-1.23 mm"));
    CHECK(de::lengthText(25.4 * 0.055, true, false) == QStringLiteral("0.055 in"));
    CHECK(de::lengthText(-25.4 * 0.05, true, false) == QStringLiteral("-0.05 in"));
    CHECK(de::lengthText(25.4 * 0.0505, true, true) == QStringLiteral("0.0505 in"));
    CHECK(de::lengthText(-0.0001, false, false) == QStringLiteral("0 mm"));
    // The text of a snapped value reads back as that value.
    for (const double raw : {0.73, -4.18, 12.06}) {
        for (const bool inch : {false, true}) {
            const double v = de::snapLength(raw, inch, false);
            const QString t = de::lengthText(v, inch, false);
            const double back = t.section(QLatin1Char(' '), 0, 0).toDouble() * (inch ? 25.4 : 1.0);
            CHECK(back == Approx(v).margin(1e-9));
        }
    }
}

TEST_CASE("the review list: grouped by owner, features first, in shop words", "[directedit][unit]")
{
    std::vector<de::ReviewItem> items;
    const auto add = [&](de::OwnerKind owner, const char* id, const char* name, std::size_t order, std::size_t index,
                         std::size_t of, de::RefState state, de::ElementKind kind, std::size_t elements) {
        de::ReviewItem i;
        i.owner = owner;
        i.ownerId = QString::fromLatin1(id);
        i.ownerName = QString::fromLatin1(name);
        i.setupName = owner == de::OwnerKind::Feature ? QString() : QStringLiteral("Setup 1");
        i.ownerOrder = order;
        i.index = index;
        i.of = of;
        i.state = state;
        i.kind = kind;
        i.elements = elements;
        i.path = owner == de::OwnerKind::Setup ? QStringLiteral("wcs/zFace") : QStringLiteral("faces/%1").arg(index);
        items.push_back(i);
    };
    // Listed out of order on purpose: an operation, then a feature's second and first references, a setup, a feature.
    add(de::OwnerKind::Operation, "op1", "Drill 1", 10, 0, 2, de::RefState::Broken, de::ElementKind::Face, 0);
    add(de::OwnerKind::Feature, "f5", "Fillet 1", 5, 1, 3, de::RefState::Split, de::ElementKind::Edge, 2);
    add(de::OwnerKind::Feature, "f5", "Fillet 1", 5, 0, 3, de::RefState::Merged, de::ElementKind::Edge, 1);
    add(de::OwnerKind::Setup, "s1", "Setup 1", 9, 0, 1, de::RefState::Repaired, de::ElementKind::Face, 1);
    add(de::OwnerKind::Feature, "f2", "Delete Faces 1", 2, 0, 1, de::RefState::Broken, de::ElementKind::Face, 0);
    const std::vector<de::ReviewGroup> groups = de::groupItems(items);
    REQUIRE(groups.size() == 4);
    CHECK(groups[0].title == QStringLiteral("Delete Faces 1"));
    CHECK(groups[1].title == QStringLiteral("Fillet 1"));
    CHECK(groups[1].items == std::vector<std::size_t>{2, 1}); // in reference order
    CHECK(groups[2].title == QStringLiteral("Setup 1: work offset"));
    CHECK(groups[3].title == QStringLiteral("Drill 1 in Setup 1"));
    CHECK(groups[3].owner == de::OwnerKind::Operation);

    // Shop words.
    CHECK(de::stateWords(items[1]) == QStringLiteral("This reference now points at two edges"));
    CHECK(de::stateWords(items[2]) == QStringLiteral("This edge is now part of a longer edge"));
    CHECK(de::stateWords(items[4]) == QStringLiteral("This face no longer exists"));
    CHECK(de::stateWords(items[3]) == QStringLiteral("This face changed; a likely match was found"));
    de::ReviewItem merged = items[4];
    merged.state = de::RefState::Merged;
    CHECK(de::stateWords(merged) == QStringLiteral("This face is now part of a larger face"));
    CHECK(de::stateLabel(de::RefState::Broken) == QStringLiteral("Missing"));
    CHECK(de::stateLabel(de::RefState::Repaired) == QStringLiteral("Check match"));
    CHECK(de::stateLabel(de::RefState::Exact).isEmpty());
    CHECK(de::stateKey(de::RefState::Split) == QStringLiteral("split"));
    // What it means for the owner.
    CHECK(de::consequence(items[4]) == QStringLiteral("The feature does not build until the face is picked again."));
    CHECK(de::consequence(items[1]) == QStringLiteral("The feature uses both pieces."));
    de::ReviewItem three = items[1];
    three.elements = 3;
    CHECK(de::consequence(three) == QStringLiteral("The feature uses all three pieces."));
    CHECK(de::consequence(items[0]) == QStringLiteral("The toolpath cannot be posted until the face is picked again."));
    CHECK(de::consequence(items[3]).contains(QStringLiteral("confirm the match")));
    // Which reference.
    CHECK(de::placeWords(items[1]) == QStringLiteral("Edge 2 of 3"));
    CHECK(de::placeWords(items[4]) == QStringLiteral("The face"));
    CHECK(de::placeWords(items[0]) == QStringLiteral("Face 1 of 2 it machines"));
    CHECK(de::placeWords(items[3]) == QStringLiteral("Z face of the work offset"));
    CHECK(de::countWord(12) == QStringLiteral("twelve"));
    CHECK(de::countWord(13) == QStringLiteral("13"));
    CHECK(de::elementWord(de::ElementKind::Vertex, 2) == QStringLiteral("points"));
    // An empty list has no groups.
    CHECK(de::groupItems({}).empty());
}
