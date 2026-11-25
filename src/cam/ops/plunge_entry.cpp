// The last-resort straight plunge of pocket2d, slot and adaptive2d (plunge_entry.h).
#include "plunge_entry.h"
#include "slot_common.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace ray::cam::ops {

bool mayPlunge(const lib::Tool& tool)
{
    return tool.cutsAtCentre();
}

std::optional<std::string> plungeWarning(const link::Linked& linked, std::string_view more)
{
    struct Place {
        Nm x = 0, y = 0;
        std::vector<Nm> depths;
    };
    std::vector<Place> places;
    std::size_t count = 0;
    Nm deepest = 0;
    const std::size_t n = std::min(linked.entered.size(), linked.passMove.size());
    for (std::size_t i = 0; i < n; ++i) {
        if (linked.entered[i] != link::Entered::Plunge) {
            continue;
        }
        // The plunge is the last move before the pass (its lead-in); linking emits it as one Plunge move.
        std::size_t at = std::min(linked.passMove[i], linked.moves.size());
        while (at > 0 && linked.moves[at - 1].role != MoveRole::Plunge) {
            --at;
        }
        if (at == 0) {
            continue;
        }
        const Move& m = linked.moves[at - 1];
        const Nm top = at >= 2 ? linked.moves[at - 2].z : linked.start.z;
        ++count;
        deepest = std::max(deepest, top - m.z);
        auto it = std::find_if(places.begin(), places.end(), [&](const Place& p) { return p.x == m.x && p.y == m.y; });
        if (it == places.end()) {
            places.push_back(Place{m.x, m.y, {}});
            it = places.end() - 1;
        }
        it->depths.push_back(m.z);
    }
    if (count == 0) {
        return std::nullopt;
    }
    std::string where;
    for (std::size_t k = 0; k < places.size() && k < kPlungeListed; ++k) {
        const Place& p = places[k];
        where += (k == 0 ? "" : "; ") + mill::atXY(geom2d::Pt{p.x, p.y}) + " to ";
        for (std::size_t j = 0; j < p.depths.size(); ++j) {
            where += (j == 0                     ? ""
                      : j + 1 == p.depths.size() ? " and "
                                                 : ", ") +
                     std::string("Z") + mill::mmText(p.depths[j]);
        }
    }
    if (places.size() > kPlungeListed) {
        where += "; and " + std::to_string(places.size() - kPlungeListed) + " more";
    }
    std::string text = "Plunges straight down at " + std::to_string(count) + (count == 1 ? " place (" : " places (") +
                       where +
                       "): no helix or ramp fits there. Check that the tool cuts at its centre and can plunge " +
                       mill::mmText(deepest) + " mm at a time.";
    if (!more.empty()) {
        text += " ";
        text += more;
    }
    return text;
}

} // namespace ray::cam::ops
