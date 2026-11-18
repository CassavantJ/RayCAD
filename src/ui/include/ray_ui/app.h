// Entry point of the desktop application. apps/raycad calls this and nothing else, so that only src/ui and
// src/render name Qt (PLAN.md 0.4 rule 1). Not one of the contract headers in include/ray/.
#pragma once

namespace ray::ui {

// Runs the Qt Quick application and returns the process exit code.
//   --smoke    open the main window, wait for its first rendered frame, then exit with 0.
//              Exit code 2: the QML failed to load. Exit code 3: no frame within 20 seconds.
//   --version  print the version and exit.
int runApp(int argc, char** argv);

} // namespace ray::ui
