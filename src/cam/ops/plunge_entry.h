// Private to src/cam (added after M3): the straight plunge that pocket2d, slot and adaptive2d fall back to when no
// helix, ramp or pre-drilled entry fits, and the warning it gives.
//
// THE RULE (the same in all three, and the invariant checker's, tests/unit/cam/invariants/checker.cpp): a plunge is
// allowed only when the tool cuts at its centre, Tool::cutsAtCentre() (its library entry's "centreCutting", else the
// type's default, libraries.h: flat, bull nose and ball end mills yes); it is the last kind tried, after every entry
// the operation tried before (so a toolpath that generated before comes out the same, byte for byte); and an operation
// whose user restricted the entry to pre-drilled holes never plunges. The plunge itself is linking's (link.h,
// EntryKind::Plunge): a feed descent through cut area as far as it reaches, then straight down at the plunge feed
// (Operation cutting feedPlunge; 0: the cut feed) from the operation's entry clearance above the material (or the feed
// plane, when that is lower) to the pass's depth, at the pass's start, where the operation keeps the whole tool inside
// the area it cuts. Each depth plunges its own step.
#pragma once

#include "../link.h"

#include <ray/libraries.h>

#include <optional>
#include <string>
#include <string_view>

namespace ray::cam::ops {

// Whether an operation may fall back to a straight plunge with this tool (THE RULE).
bool mayPlunge(const lib::Tool& tool);

// The warning for the straight plunges linking made (Linked::entered == Entered::Plunge), nullopt when there are none:
// "Plunges straight down at 3 places (X13.5 Y13.5 to Z12, Z9 and Z6): no helix or ramp fits there. Check that the tool
// cuts at its centre and can plunge 3 mm at a time." Places are listed by XY in the order cut (at most kPlungeListed,
// then how many more), with the depths at each. `more` is appended after a space when not empty.
inline constexpr std::size_t kPlungeListed = 4;
std::optional<std::string> plungeWarning(const link::Linked& linked, std::string_view more = {});

} // namespace ray::cam::ops
