// RayCAD interface contract: application API (the one API everything goes through)
// Module: src/api   Depends on: core, doc, cam, lib, sim, post, store
// Status: PLAN DRAFT.
//
// The UI, the command palette, keyboard shortcuts, scripts (v2) and the MCP server (v2) all call this
// and nothing else. Built-in features and operations are registered through the same registries a
// script would use, so the API is never second-class (differentiator 7).
#pragma once
#include "core.h"
#include "document.h"
#include "cam.h"
#include "libraries.h"
#include "sim.h"
#include "post.h"
#include "store.h"

namespace ray::api {

// ---------- Actions: anything a user can invoke ----------
// Document-changing actions wrap a doc::Command. Others (view, file, simulate, post) are app actions.
struct ActionInfo {
    std::string id;                 // "cam.operation.add", "view.fit", "file.save"
    std::string title, category, description;
    std::string argsSchemaJson;     // JSON Schema; empty args allowed when every property has a default
    std::string defaultShortcut;    // "Ctrl+S"; remappable
    bool changesDocument;
    bool changesToolpathsOrGcode;   // automation callers must obtain user confirmation first
    bool longRunning;               // returns a job; shows progress; Esc cancels
};
enum class Caller { User, Script, Agent };
struct ActionResult { std::string resultJson; std::vector<std::string> warnings; };

class IActionRegistry {
public:
    virtual ~IActionRegistry() = default;
    virtual void add(ActionInfo, std::function<Result<ActionResult>(std::string_view argsJson, Caller, JobContext&)>) = 0;
    virtual std::vector<ActionInfo> all() const = 0;                             // feeds the command palette and MCP tool list
    virtual std::vector<ActionInfo> search(std::string_view query, int limit) const = 0;   // fuzzy; deterministic ranking
    // Validates args against the schema, enforces the confirmation rule for Caller::Script/Agent, runs it.
    virtual JobHandle<ActionResult> invoke(std::string_view id, std::string_view argsJson, Caller) = 0;
};

// Confirmation hook: for Caller != User and changesToolpathsOrGcode, the app shows what will change
// (a diff of operations and parameters) and waits. Headless test runs install an auto-approve hook.
using ConfirmHook = std::function<bool(const ActionInfo&, std::string_view argsJson, std::string_view humanSummary)>;

// ---------- Application session ----------
struct OpenDocument {
    std::string path;
    std::unique_ptr<store::IDocumentFile> file;
    std::unique_ptr<doc::IDocument> document;
    std::unique_ptr<store::IRecoveryJournal> journal;
};

class IApp {
public:
    virtual ~IApp() = default;
    virtual IActionRegistry& actions() = 0;
    virtual doc::FeatureRegistry& featureTypes() = 0;
    virtual cam::OperationRegistry& operationTypes() = 0;
    virtual lib::LibraryStack& libraries() = 0;
    virtual IJobSystem& jobs() = 0;
    virtual cam::ICamEngine& cam() = 0;
    virtual post::IPostEngine& posts() = 0;
    virtual OpenDocument* active() = 0;
    virtual Result<OpenDocument*> newDocument(std::string_view templateId) = 0;
    virtual Result<OpenDocument*> open(const std::string& path) = 0;        // .raycad, or STEP/IGES/DXF => new doc with an import feature
    virtual Status save(OpenDocument&, std::optional<std::string> saveAsPath = {}) = 0;
    virtual void setConfirmHook(ConfirmHook) = 0;
    struct RecentFile { std::string path; std::int64_t openedMs; std::vector<std::byte> thumbnailPng; };
    virtual std::vector<RecentFile> recentFiles() const = 0;
};

// Headless entry point used by tests, golden-file tools and (v2) the MCP server: no Qt, no window.
std::unique_ptr<IApp> makeHeadlessApp(std::vector<std::string> libraryRoots, int workerThreads);

// ---------- Queries (read-only, safe from any thread on a snapshot) ----------
std::string describeDocumentJson(const doc::DocState&, const doc::BuildResult&);     // feature tree, bodies, params
std::string describeCamJson(const doc::DocState&, const std::map<OperationId, cam::OpState>&);

} // namespace ray::api
