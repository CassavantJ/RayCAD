// The thread table of the hole feature (task T-097), private to src/doc, its tests and the standards.threads.list
// action (src/api/builtin_actions.cpp).
//
// The table is resources/standards/threads.json, written by tools/gen_thread_table.py (the formula, the rounding rule
// and the public sources are in the file and in resources/standards/README.md). It is compiled into the program
// (src/doc/CMakeLists.txt), as src/ui compiles in the themes, so a build tree and an installed program find it alike.
// Lengths are mm; unified sizes also carry their threads per inch.
#pragma once

#include <ray/core.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ray::doc::features {

struct TapDrill {
    std::string size;    // "5.0 mm", "#7", "F", "5/16 in"
    double diameter = 0; // mm
};

struct ThreadSize {
    std::string designation;       // "M6x1", "1/4-20 UNC", "#10-32 UNF"
    std::string nominal;           // "M6", "1/4", "#10"
    double majorDiameter = 0;      // mm
    double pitch = 0;              // mm
    std::optional<double> tpi;     // unified sizes: threads per inch
    double basicMinorDiameter = 0; // mm
    TapDrill tapDrill;
};

struct ThreadSeries {
    std::string id;       // "iso_metric_coarse", "iso_metric_fine", "unc", "unf"
    std::string name;     // "ISO metric coarse"
    std::string system;   // "metric" | "inch"
    std::string standard; // what the series follows, in words
    std::vector<ThreadSize> sizes;
};

struct ThreadTable {
    std::vector<ThreadSeries> series;
};

// The shipped file's text.
std::string_view threadTableText();
// Reads a table in the form of threads.json; FormatError naming what is wrong.
Result<ThreadTable> parseThreadTable(std::string_view json);
// The shipped table, read once (thread-safe). Fails only if the compiled-in file is damaged.
Result<std::shared_ptr<const ThreadTable>> threadTable();

const ThreadSeries* findSeries(const ThreadTable& table, std::string_view series);
// A size by designation, ignoring case, spaces, a leading '#', the series suffix ("UNC") and "×" for "x"; a nominal
// size ("M6", "1/4") is accepted when the series has one size of it. Null if none.
const ThreadSize* findThread(const ThreadTable& table, std::string_view series, std::string_view designation);

// The result of the standards.threads.list action: canonical JSON
//   {"series": [{"id", "name", "system", "standard", "sizes": [<size as in threads.json>, ...]}, ...],
//    "method": {...}, "sources": [...]}
// with one series when `series` names one (NotFound if it names none).
Result<std::string> threadListJson(std::string_view series);

} // namespace ray::doc::features
