// The spike S4 part and its edit scenarios (spikes/s4-naming/script.cpp and main.cpp), promoted to a permanent
// test (task T-081a). A 160 x 100 x 30 mm block built from 40 features (extrudes, cuts, holes, fillets, chamfers);
// every dimension is a named parameter so that a scenario can change it.
#pragma once
#include "oracle.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace ray::naming_oracle::s4 {

using Params = std::map<std::string, double>;
Params baseParams();
Script makeScript(const Params& params);

struct Scenario {
    std::string id, desc;
    std::function<void(Params&)> p; // parameter edit, or empty
    std::function<void(Script&)> s; // script edit, or empty
};
std::vector<Scenario> scenarios();

// The 18 scenarios of the PLAN.md 3.3 acceptance set.
bool inPlanSet(const std::string& id);

} // namespace ray::naming_oracle::s4
