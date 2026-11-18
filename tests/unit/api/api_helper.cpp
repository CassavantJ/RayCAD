// The process the recovery tests kill (task T-017 follow-up): it changes a document through the app's actions, writes
// what it reached to a file, then waits to be killed, so its recovery journal is left behind as after a crash.
//
//   ray_api_helper edit <userDataDir> <doc.raycad> <step> <out>   open the document, param.set twice, undo, redo,
//                                                                 another param.set, import the STEP file into it
//   ray_api_helper new <userDataDir> <step> <out>                 open the STEP file (a never-saved document), then
//                                                                 one param.set
// <out> gets "<state hash>\n<describeDocumentJson>\n" (written beside it, then renamed) once the rebuild finished.
#include <ray/api.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using ray::api::Caller;

std::filesystem::path fsPath(const std::string& utf8)
{
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string jsonString(const std::string& s)
{
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    return out + "\"";
}

int failWith(const std::string& what, const ray::Error& e)
{
    std::fprintf(stderr, "ray_api_helper: %s: %s\n", what.c_str(), e.message.c_str());
    return 2;
}

int run(const std::vector<std::string>& args)
{
    if (args.size() < 2) {
        std::fprintf(stderr, "ray_api_helper: no mode\n");
        return 2;
    }
    const std::string& mode = args[1];
    const bool edit = mode == "edit";
    if ((edit && args.size() != 6) || (!edit && (mode != "new" || args.size() != 5))) {
        std::fprintf(stderr, "ray_api_helper: bad arguments\n");
        return 2;
    }
    ray::api::AppOptions options;
    options.userDataDir = args[2];
    options.workerThreads = 2;
    auto app = ray::api::makeHeadlessApp(options);
    if (!app) {
        return 2;
    }
    const std::string step = edit ? args[4] : args[3];
    const std::string out = edit ? args[5] : args[4];
    auto& actions = app->actions();
    const auto invoke = [&](const char* id, const std::string& json) -> bool {
        auto r = app->wait(actions.invoke(id, json, Caller::User));
        if (!r) {
            failWith(id, r.error());
        }
        return r.has_value();
    };
    if (edit) {
        auto od = app->wait(app->open(args[3]));
        if (!od) {
            return failWith("open", od.error());
        }
        const std::string p = R"({"id":"00000000-0000-4000-8000-0000000000c1",)";
        if (!invoke("param.set", p + R"("name":"len","expression":"2 in","kind":"length"})") ||
            !invoke("param.set", p + R"("expression":"3 in"})") || !invoke("edit.undo", "") ||
            !invoke("edit.redo", "") ||
            !invoke("param.set",
                    R"({"id":"00000000-0000-4000-8000-0000000000c2","name":"n","expression":"4","kind":"count"})") ||
            !invoke("file.import", "{\"path\":" + jsonString(step) + "}")) {
            return 2;
        }
    } else {
        auto od = app->wait(app->open(step));
        if (!od) {
            return failWith("open", od.error());
        }
        if (!invoke(
                "param.set",
                R"({"id":"00000000-0000-4000-8000-0000000000c3","name":"w","expression":"5 mm","kind":"length"})")) {
            return 2;
        }
    }
    ray::api::OpenDocument* od = app->active();
    if (auto st = app->waitForBuild(*od); !st) {
        return failWith("rebuild", st.error());
    }
    const auto state = od->document->state();
    const auto build = od->document->build();
    {
        std::ofstream file(fsPath(out + ".tmp"), std::ios::binary | std::ios::trunc);
        file << state->stateHash().hex() << "\n" << ray::api::describeDocumentJson(*state, *build) << "\n";
    }
    std::error_code ec;
    std::filesystem::rename(fsPath(out + ".tmp"), fsPath(out), ec);
    // Wait to be killed; keep serving the UI queue meanwhile.
    for (;;) {
        app->runUntil([] { return false; }, 1000ms);
    }
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
{
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) {
        const std::u8string u8 = std::filesystem::path(argv[i]).u8string();
        args.emplace_back(reinterpret_cast<const char*>(u8.data()), u8.size());
    }
    return run(args);
}
#else
int main(int argc, char** argv)
{
    return run(std::vector<std::string>(argv, argv + argc));
}
#endif
