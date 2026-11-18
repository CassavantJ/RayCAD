// Start-up clock for --bench-start (PLAN.md section 9): milliseconds since the OS created this process, so the
// loader, DLL loading and static initialisers are included.
#pragma once

namespace ray::ui {

// Milliseconds since process creation. Where the OS start time cannot be read, the first call counts as 0.
double msSinceProcessStart();

} // namespace ray::ui
