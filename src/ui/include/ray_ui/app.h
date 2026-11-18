// Entry point of the desktop application. apps/raycad calls this and nothing else, so that only src/ui and
// src/render name Qt (PLAN.md 0.4 rule 1). Not one of the contract headers in include/ray/.
#pragma once

namespace ray::ui {

// Runs the Qt Quick application and returns the process exit code.
//   --theme <name>      system (default: follow the OS light/dark setting), dark, light, or a theme id
//   --glass on|off      glass or flat panels (same layout)
//   --gallery           show the controls gallery instead of the shell
//   --size WxH          window size in logical pixels
//   --screenshot <png>  wait for the first frame, let it settle, save the window as PNG, exit 0
//   --smoke             open the main window, wait for its first rendered frame, then exit with 0
//   --bench-start       print the time from OS process creation to the first frame swapped, then exit 0
//                       (also accepted as --bench-startup, the name PLAN.md section 9 uses)
//   --version           print the version and exit
// Exit codes: 1 bad option or unknown theme, 2 the QML failed to load, 3 no frame within 20 seconds,
// 4 the screenshot could not be saved.
int runApp(int argc, char** argv);

} // namespace ray::ui
