// RayCAD interface contract: post-processing
// Module: src/post   Depends on: core, cam, lib, QuickJS-NG (private)
// Status: PLAN DRAFT. The script-side API is docs/interfaces/post_api.d.ts.
//
// A post is ONE plain-text JavaScript file. It is a pure function of (program, machine, properties):
// the host gives it no clock, no random numbers, no file system and no locale. The host stamps the
// date into the header only if the `includeDate` property is on (off in golden tests).
// All number formatting is done by host functions so output is identical on every platform.
#pragma once
#include "core.h"
#include "cam.h"
#include "libraries.h"
#include <map>

namespace ray::post {

struct PostProperty {
    std::string key, title, description, group;
    enum class Type { Bool, Int, Number, Enum, String } type;
    std::string defaultValue;
    std::vector<std::string> enumValues;
};
struct PostInfo {
    std::string id;                 // file stem
    std::string title, vendor, dialect, fileExtension;   // ".nc", ".eia", ".tap"
    int apiVersion;                 // host refuses scripts needing a newer API
    std::vector<PostProperty> properties;
    struct Caps { bool arcsIJK, arcsR, helical, cannedCycles, rigidTap, cutterComp, toolLengthProbe; int maxArcDeg; } caps;
};

struct PostSection {                // one operation as the post sees it
    const cam::Operation* op;
    const cam::Toolpath* toolpath;
    const lib::Tool* tool;
    const lib::Holder* holder;
};
struct PostJob {
    const cam::Setup* setup;
    std::vector<PostSection> sections;
    const lib::MachineDef* machine;
    UnitSystem outputUnits;
    std::map<std::string, std::string> properties;    // user overrides; unset keys take defaults
    std::string documentName, userName;
    std::optional<std::string> fixedDate;             // ISO date; tests leave empty
};

struct GcodeLineMap { std::uint32_t line; std::uint32_t sectionIndex; std::uint32_t moveIndex; };
struct PostOutput {
    std::string text;                        // line endings per property (CRLF default)
    std::vector<GcodeLineMap> lineMap;       // G-code line -> move; drives timeline "jump to line" and highlighting
    std::vector<std::string> warnings;
    Hash256 hash() const;
};

class IPostEngine {
public:
    virtual ~IPostEngine() = default;
    virtual Result<PostInfo> inspect(const std::string& scriptPath) = 0;        // runs only the declaration block
    // Refuses (ErrorCode::PostFailed) if any section's toolpath is missing, or if the caller flags it stale.
    // Limits: 256 MB heap, 60 s CPU per job; exceeding either aborts with a script error and line number.
    virtual Result<PostOutput> run(const std::string& scriptPath, const PostJob&, JobContext&) = 0;
};
std::unique_ptr<IPostEngine> makeQuickJsPostEngine();

// Host-side helpers exposed to scripts (documented in post_api.d.ts) and reused by tests.
struct NumberFormat { int decimals; bool trimTrailingZeros; bool forceDecimalPoint; bool leadingZero; double scale; };
std::string formatNumber(double value, const NumberFormat&);       // round-half-even on the decimal string; "-0" never emitted
// Fits lines to arcs is NOT done in posts; arcs come from the IR. Posts may only LINEARISE arcs the control cannot do.

// ---------- Send to machine (v1 basic) ----------
struct OutputRule { std::string directory; std::string fileNamePattern; bool overwrite; };   // pattern: "O{program:04}", "{doc}_{setup}"
Result<std::string> writeProgram(const PostOutput&, const PostInfo&, const PostJob&, const OutputRule&);   // returns final path; atomic write

// ---------- Setup sheet ----------
struct SetupSheetInput { const PostJob* job; cam::CycleTime time; std::vector<std::byte> fixtureImagePng; Box3 stock; };
Result<std::string> renderSetupSheetHtml(const SetupSheetInput&);   // self-contained HTML; PDF is produced by the UI layer from this HTML

} // namespace ray::post
