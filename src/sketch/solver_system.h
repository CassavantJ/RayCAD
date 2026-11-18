// Small helpers shared by the solve and drag code of the PlaneGCS adapter (task T-090). Private to src/sketch.
#pragma once

#include <ray/sketch.h>

// PlaneGCS (third_party/planegcs), unmodified.
#include "GCS.h"

#include <memory>
#include <vector>

namespace ray::sketch::detail {

// A constraint "holds" when |GCS::Constraint::error()| is at most this. It is the test spike S11 used to accept a
// reduced drag step, and the test of a successful solve.
constexpr double kHoldTolerance = 1e-9;

// A new GCS::System with debug output off. Pins Eigen's cache sizes first, and serialises construction (the
// constructor writes Eigen's process-wide thread count).
std::unique_ptr<GCS::System> newSystem();

// The largest |error()| over the objects with tag >= 0 (the sketch's own constraints and the arc rules); NaN counts
// as infinitely large.
double maxConstraintError(const std::vector<GCS::Constraint*>& objects);

SolveStatus statusFromDiagnosis(int dofs, bool redundant);

} // namespace ray::sketch::detail
