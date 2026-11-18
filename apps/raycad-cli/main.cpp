// raycad-cli: headless RayCAD (PLAN.md section 1.1) for tests, golden files and scripting. Everything goes through
// the api: makeHeadlessApp, IApp::open/save, the action registry and describeDocumentJson.
#include <ray/api.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace std::chrono_literals;
using ray::Error;
using ray::ErrorCode;

constexpr const char* kUsage =
    "usage: raycad-cli [--threads N] <command> [arguments]\n"
    "\n"
    "commands:\n"
    "  --version                          print the version\n"
    "  import <in.step|in.iges> -o <out.raycad> [--heal none|reader|full] [--overwrite]\n"
    "                                     import a STEP or IGES file, rebuild and save it as a RayCAD document;\n"
    "                                     prints the state hash and the document description\n"
    "  info <file>                        open a .raycad (or STEP/IGES) file, rebuild, print \"state <hash>\" and\n"
    "                                     the document description (JSON) on the next line\n"
    "  hash <file>                        print only the state hash\n"
    "  export <in> <out.step|.stp|.iges|.igs|.stl> [--chord-tol MM]\n"
    "                                     open a file and export its built bodies\n"
    "  actions [--search TEXT] [--limit N]\n"
    "                                     list the registered actions as JSON\n"
    "  invoke <action-id> [JSON-ARGS] [--open FILE] [--caller user|script|agent] [--yes] [--save]\n"
    "                                     open FILE first (optional), invoke the action and print its result JSON;\n"
    "                                     --yes approves changes to toolpaths or G-code requested by a script or\n"
    "                                     agent caller, --save saves the active document afterwards\n"
    "\n"
    "exit codes: 0 ok, 1 failed, 2 usage error, 3 invalid argument, 4 not found, 5 cancelled or declined,\n"
    "            6 locked, 7 not a valid or supported file\n"
    "Per-user files (recent files) go to $RAYCAD_USER_DATA_DIR when it is set.\n";

constexpr int kExitFailed = 1;
constexpr int kExitUsage = 2;

int exitCodeOf(const Error& e)
{
    switch (e.code) {
    case ErrorCode::InvalidArgument:
        return 3;
    case ErrorCode::NotFound:
        return 4;
    case ErrorCode::Cancelled:
        return 5;
    case ErrorCode::Locked:
        return 6;
    case ErrorCode::FormatError:
    case ErrorCode::FormatTooNew:
        return 7;
    default:
        return kExitFailed;
    }
}

int report(const Error& e)
{
    std::fprintf(stderr, "raycad-cli: error: %s\n", e.message.c_str());
    for (const std::string& line : e.context) {
        std::fprintf(stderr, "  (%s)\n", line.c_str());
    }
    return exitCodeOf(e);
}

int usageError(const std::string& message)
{
    std::fprintf(stderr, "raycad-cli: %s\n\n%s", message.c_str(), kUsage);
    return kExitUsage;
}

void printLine(const std::string& text)
{
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fputc('\n', stdout);
}

// Escapes a string for JSON output (the CLI writes action lists itself).
std::string quoted(const std::string& s)
{
    std::string out = "\"";
    for (const char c : s) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\r':
            out += "\\r";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                out += buf;
            } else {
                out += c;
            }
        }
    }
    return out + "\"";
}

struct Args {
    std::vector<std::string> positional;
    std::string output, heal, search, open, caller = "user";
    double chordTol = 0.01;
    int limit = 0;
    int threads = 0;
    bool overwrite = false, yes = false, save = false;
};

class Session {
public:
    explicit Session(int threads) : app_(ray::api::makeHeadlessApp({}, threads)) {}
    ray::api::HeadlessApp* app() { return app_.get(); }

    // Opens and rebuilds; notes a read-only open on stderr.
    ray::Result<ray::api::OpenDocument*> open(const std::string& path)
    {
        auto od = app_->wait(app_->open(path));
        if (!od) {
            return od;
        }
        if ((*od)->readOnly) {
            std::string who = "another session";
            if ((*od)->lock) {
                who = (*od)->lock->user + " on " + (*od)->lock->host;
            }
            std::fprintf(stderr, "raycad-cli: note: %s is open in %s; opened a read-only copy\n", path.c_str(),
                         who.c_str());
        }
        if (auto st = app_->waitForBuild(**od); !st) {
            return tl::unexpected<Error>(st.error());
        }
        for (const auto& r : (*od)->document->build()->reports) {
            if (r.status == ray::doc::FeatureStatus::Failed || r.status == ray::doc::FeatureStatus::Warning) {
                std::fprintf(stderr, "raycad-cli: warning: feature %s: %s\n", r.id.value.toString().c_str(),
                             r.message.c_str());
            }
        }
        return od;
    }

    void describe(ray::api::OpenDocument& od, bool full)
    {
        const auto state = od.document->state();
        printLine("state " + state->stateHash().hex());
        if (full) {
            const auto build = od.document->build();
            printLine(ray::api::describeDocumentJson(*state, build ? *build : ray::doc::BuildResult{}));
        }
    }

private:
    std::unique_ptr<ray::api::HeadlessApp> app_;
};

int cmdImport(Session& s, const Args& a)
{
    if (a.positional.size() != 2 || a.output.empty()) {
        return usageError("import needs an input file and -o <out.raycad>");
    }
    std::string args = "{\"path\":" + quoted(a.positional[1]);
    if (!a.heal.empty()) {
        args += ",\"heal\":" + quoted(a.heal);
    }
    args += "}";
    // Through the file.import action, so its arguments are checked like any caller's.
    auto imported = s.app()->wait(s.app()->actions().invoke("file.import", args, ray::api::Caller::User));
    if (!imported) {
        return report(imported.error());
    }
    ray::api::OpenDocument* od = s.app()->active();
    if (od == nullptr) {
        return report(Error{ErrorCode::Internal, "The import made no document", {}});
    }
    if (auto st = s.app()->waitForBuild(*od); !st) {
        return report(st.error());
    }
    auto saved = s.app()->wait(s.app()->save(*od, a.output, a.overwrite));
    if (!saved) {
        return report(saved.error());
    }
    s.describe(*od, true);
    return 0;
}

int cmdInfo(Session& s, const Args& a, bool full)
{
    if (a.positional.size() != 2) {
        return usageError(std::string(full ? "info" : "hash") + " needs one file");
    }
    auto od = s.open(a.positional[1]);
    if (!od) {
        return report(od.error());
    }
    s.describe(**od, full);
    return 0;
}

int cmdExport(Session& s, const Args& a)
{
    if (a.positional.size() != 3) {
        return usageError("export needs an input file and an output file");
    }
    auto od = s.open(a.positional[1]);
    if (!od) {
        return report(od.error());
    }
    std::string ext = std::filesystem::path(a.positional[2]).extension().string();
    for (char& c : ext) {
        c = static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
    }
    std::string action;
    std::string args = "{\"path\":" + quoted(a.positional[2]);
    if (ext == ".step" || ext == ".stp") {
        action = "file.exportStep";
    } else if (ext == ".iges" || ext == ".igs") {
        action = "file.exportIges";
    } else if (ext == ".stl") {
        action = "file.exportStl";
        args += ",\"chordTolerance\":" + std::to_string(a.chordTol);
    } else {
        return usageError("export writes .step, .stp, .iges, .igs or .stl files");
    }
    args += "}";
    auto done = s.app()->wait(s.app()->actions().invoke(action, args, ray::api::Caller::User));
    if (!done) {
        return report(done.error());
    }
    printLine(done->resultJson);
    return 0;
}

int cmdActions(Session& s, const Args& a)
{
    const auto list = a.search.empty() ? s.app()->actions().all() : s.app()->actions().search(a.search, a.limit);
    std::string out = "[";
    std::size_t n = 0;
    for (const auto& info : list) {
        if (a.search.empty() && a.limit > 0 && n >= static_cast<std::size_t>(a.limit)) {
            break;
        }
        if (n++ != 0) {
            out += ",";
        }
        out += "{\"argsSchema\":" + (info.argsSchemaJson.empty() ? std::string("{}") : info.argsSchemaJson) +
               ",\"category\":" + quoted(info.category) +
               ",\"changesDocument\":" + (info.changesDocument ? "true" : "false") +
               ",\"changesToolpathsOrGcode\":" + (info.changesToolpathsOrGcode ? "true" : "false") +
               ",\"defaultShortcut\":" + quoted(info.defaultShortcut) + ",\"description\":" + quoted(info.description) +
               ",\"id\":" + quoted(info.id) + ",\"longRunning\":" + (info.longRunning ? "true" : "false") +
               ",\"title\":" + quoted(info.title) + "}";
    }
    out += "]";
    printLine(out);
    return 0;
}

int cmdInvoke(Session& s, const Args& a)
{
    if (a.positional.size() < 2 || a.positional.size() > 3) {
        return usageError("invoke needs an action id and optionally one JSON argument object");
    }
    ray::api::Caller caller = ray::api::Caller::User;
    if (a.caller == "script") {
        caller = ray::api::Caller::Script;
    } else if (a.caller == "agent") {
        caller = ray::api::Caller::Agent;
    } else if (a.caller != "user") {
        return usageError("--caller must be user, script or agent");
    }
    if (a.yes) {
        s.app()->setConfirmHook([](const auto&, auto, auto) { return true; });
    }
    if (!a.open.empty()) {
        auto od = s.open(a.open);
        if (!od) {
            return report(od.error());
        }
    }
    const std::string args = a.positional.size() == 3 ? a.positional[2] : std::string();
    auto done = s.app()->wait(s.app()->actions().invoke(a.positional[1], args, caller));
    if (!done) {
        return report(done.error());
    }
    for (const auto& w : done->warnings) {
        std::fprintf(stderr, "raycad-cli: warning: %s\n", w.c_str());
    }
    if (a.save) {
        ray::api::OpenDocument* od = s.app()->active();
        if (od == nullptr) {
            return report(Error{ErrorCode::NotFound, "--save: no document is open", {}});
        }
        if (auto st = s.app()->waitForBuild(*od); !st) {
            return report(st.error());
        }
        auto saved = s.app()->wait(s.app()->save(*od));
        if (!saved) {
            return report(saved.error());
        }
    }
    printLine(done->resultJson);
    return 0;
}

int run(std::vector<std::string> argv)
{
    Args a;
    for (std::size_t i = 1; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        const auto value = [&](const char* name) -> const std::string* {
            if (i + 1 >= argv.size()) {
                std::fprintf(stderr, "raycad-cli: %s needs a value\n", name);
                return nullptr;
            }
            return &argv[++i];
        };
        if (arg == "--version" || arg == "-V") {
            std::printf("raycad-cli %s\n", RAY_VERSION_STRING);
            return 0;
        }
        if (arg == "--help" || arg == "-h") {
            std::printf("raycad-cli %s\n%s", RAY_VERSION_STRING, kUsage);
            return 0;
        }
        if (arg == "-o" || arg == "--output" || arg == "--heal" || arg == "--search" || arg == "--open" ||
            arg == "--caller" || arg == "--chord-tol" || arg == "--limit" || arg == "--threads") {
            const std::string* v = value(arg.c_str());
            if (!v) {
                return kExitUsage;
            }
            if (arg == "-o" || arg == "--output") {
                a.output = *v;
            } else if (arg == "--heal") {
                a.heal = *v;
            } else if (arg == "--search") {
                a.search = *v;
            } else if (arg == "--open") {
                a.open = *v;
            } else if (arg == "--caller") {
                a.caller = *v;
            } else {
                char* end = nullptr;
                const double d = std::strtod(v->c_str(), &end);
                if (end == v->c_str() || *end != 0) {
                    return usageError(arg + " needs a number");
                }
                if (arg == "--chord-tol") {
                    a.chordTol = d;
                } else if (arg == "--limit") {
                    a.limit = static_cast<int>(d);
                } else {
                    a.threads = static_cast<int>(d);
                }
            }
        } else if (arg == "--overwrite") {
            a.overwrite = true;
        } else if (arg == "--yes") {
            a.yes = true;
        } else if (arg == "--save") {
            a.save = true;
        } else if (arg.size() > 1 && arg[0] == '-' && arg[1] == '-') {
            return usageError("unknown option " + arg);
        } else {
            a.positional.push_back(arg);
        }
    }
    if (a.positional.empty()) {
        return usageError("no command given");
    }
    const std::string& command = a.positional[0];
    if (command == "version") {
        std::printf("raycad-cli %s\n", RAY_VERSION_STRING);
        return 0;
    }
    if (command != "import" && command != "info" && command != "hash" && command != "export" && command != "actions" &&
        command != "invoke") {
        return usageError("unknown command '" + command + "'");
    }
    Session s(a.threads);
    if (s.app() == nullptr) {
        std::fprintf(stderr, "raycad-cli: error: the application could not start\n");
        return kExitFailed;
    }
    if (command == "import") {
        return cmdImport(s, a);
    }
    if (command == "info") {
        return cmdInfo(s, a, true);
    }
    if (command == "hash") {
        return cmdInfo(s, a, false);
    }
    if (command == "export") {
        return cmdExport(s, a);
    }
    if (command == "actions") {
        return cmdActions(s, a);
    }
    return cmdInvoke(s, a);
}

} // namespace

#ifdef _WIN32
// The arguments as UTF-8 (the narrow argv of main() is in the ANSI code page on Windows).
int wmain(int argc, wchar_t** argv)
{
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) {
        const std::u8string u8 = std::filesystem::path(argv[i]).u8string();
        args.emplace_back(reinterpret_cast<const char*>(u8.data()), u8.size());
    }
    return run(std::move(args));
}
#else
int main(int argc, char** argv)
{
    return run(std::vector<std::string>(argv, argv + argc));
}
#endif
