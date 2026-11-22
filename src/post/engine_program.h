// The handler calls of a post run, in program order (engine_program.cpp). Internal to src/post.
#pragma once
#include "engine_host.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ray::post::detail {

// Output units: what the script sees. Lengths arrive as integer nanometres and leave as one division, so the host
// can snap the script's numbers back to the same integers (fmt::snapToGrid).
struct Units {
    bool inch = false;
    double len(Nm nm) const { return static_cast<double>(nm) / (inch ? 25.4e6 : 1.0e6); }
    double lenD(double nm) const { return nm / (inch ? 25.4e6 : 1.0e6); }
    double mm(double millimetres) const { return len(toNm(millimetres)); } // library values in mm: snapped first
    double feed(std::int32_t micronPerMin) const
    {
        return static_cast<double>(micronPerMin) / (inch ? 25400.0 : 1000.0);
    }
};

// Everything the C++ side worked out before the script runs.
struct ProgramPlan {
    Units units;
    std::vector<std::pair<std::string, PropertyValue>> properties; // resolved, in declaration order
    std::optional<std::string> date;
    std::vector<bool> toolChanged;   // per section
    std::vector<SectionStats> stats; // per section
    std::size_t totalMoves = 0;
    std::vector<std::string> warnings; // host warnings, put first in PostOutput::warnings
};

Result<PostOutput> runProgram(Session& s, const Declaration& decl, const PostJob& job, JobContext& jobCtx,
                              const ProgramPlan& plan);

} // namespace ray::post::detail
