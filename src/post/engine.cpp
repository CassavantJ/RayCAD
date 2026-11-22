// IPostEngine on QuickJS-NG (engine.h): job checks, property resolution, and the post thread that owns each run's
// runtime from creation to destruction (PLAN.md section 7).
#include "engine.h"

#include "engine_host.h"
#include "engine_program.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <new>

namespace ray::post {

using detail::Declaration;
using detail::DeclaredProperty;
using detail::PropertyValue;
using detail::Session;

namespace {

constexpr std::uintmax_t kMaxScriptBytes = std::uintmax_t(8) << 20;

Error error(ErrorCode code, std::string message)
{
    return Error{code, std::move(message), {}};
}

Result<std::string> readScript(const std::string& path)
{
    const std::filesystem::path p(std::u8string(reinterpret_cast<const char8_t*>(path.data()), path.size()));
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(p, ec);
    if (ec) {
        return tl::unexpected(error(ErrorCode::NotFound, "cannot read the post script " + path));
    }
    if (size > kMaxScriptBytes) {
        return tl::unexpected(error(ErrorCode::ScriptError, "the post script " + path + " is larger than 8 MB"));
    }
    std::ifstream in(p, std::ios::binary);
    std::string text(static_cast<std::size_t>(size), '\0');
    if (!in || !in.read(text.data(), static_cast<std::streamsize>(text.size()))) {
        return tl::unexpected(error(ErrorCode::IoError, "cannot read the post script " + path));
    }
    return text;
}

std::string scriptNameOf(const std::string& path)
{
    const std::filesystem::path p(std::u8string(reinterpret_cast<const char8_t*>(path.data()), path.size()));
    const std::u8string name = p.filename().u8string();
    return std::string(reinterpret_cast<const char*>(name.data()), name.size());
}

Status validateJob(const PostJob& job)
{
    if (!job.setup) {
        return tl::unexpected(error(ErrorCode::PostFailed, "the post job has no setup"));
    }
    if (!job.machine) {
        return tl::unexpected(error(ErrorCode::PostFailed, "the post job has no machine"));
    }
    std::size_t total = 0;
    for (std::size_t i = 0; i < job.sections.size(); ++i) {
        const PostSection& s = job.sections[i];
        const std::string label = "section " + std::to_string(i);
        if (!s.op) {
            return tl::unexpected(error(ErrorCode::PostFailed, label + " has no operation"));
        }
        if (!s.toolpath) {
            return tl::unexpected(error(ErrorCode::PostFailed, label + " (" + s.op->name + ") has no toolpath"));
        }
        if (!s.tool) {
            return tl::unexpected(error(ErrorCode::PostFailed, label + " (" + s.op->name + ") has no tool"));
        }
        total += s.toolpath->moves.size();
    }
    if (total > std::numeric_limits<std::uint32_t>::max()) {
        return tl::unexpected(error(ErrorCode::PostFailed, "the program has more moves than a post run can take"));
    }
    return std::monostate{};
}

std::size_t totalMoves(const PostJob& job)
{
    std::size_t total = 0;
    for (const PostSection& s : job.sections) {
        total += s.toolpath ? s.toolpath->moves.size() : 0;
    }
    return total;
}

bool wholeNumber(double d)
{
    return std::floor(d) == d && std::fabs(d) <= 9007199254740992.0;
}

// An override string for a declared property, read as its type.
Result<PropertyValue> parseOverride(Session& s, const DeclaredProperty& p, const std::string& text,
                                    const std::string& origin)
{
    auto bad = [&](const std::string& why) -> Result<PropertyValue> {
        return tl::unexpected(error(ErrorCode::InvalidArgument,
                                    "post property '" + p.info.key + "' (" + origin + "): \"" + text + "\" " + why));
    };
    switch (p.info.type) {
    case PostProperty::Type::Bool:
        if (text == "true" || text == "1") {
            return PropertyValue(true);
        }
        if (text == "false" || text == "0") {
            return PropertyValue(false);
        }
        return bad("is not true or false");
    case PostProperty::Type::Int:
    case PostProperty::Type::Number: {
        std::optional<double> d;
        if (p.info.type == PostProperty::Type::Int) {
            std::int64_t v = 0;
            const auto r = std::from_chars(text.data(), text.data() + text.size(), v);
            if (r.ec == std::errc() && r.ptr == text.data() + text.size() && wholeNumber(static_cast<double>(v))) {
                d = static_cast<double>(v);
            } else {
                return bad("is not a whole number");
            }
        } else {
            d = s.parseNumber(text);
            if (!d) {
                return bad("is not a number");
            }
        }
        if ((p.min && *d < *p.min) || (p.max && *d > *p.max)) {
            return bad("is outside the property's min and max");
        }
        return PropertyValue(*d);
    }
    case PostProperty::Type::Enum:
        if (std::find(p.info.enumValues.begin(), p.info.enumValues.end(), text) == p.info.enumValues.end()) {
            return bad("is not one of the property's values");
        }
        return PropertyValue(text);
    case PostProperty::Type::String:
        return PropertyValue(text);
    }
    return bad("has an unknown type");
}

// Declared default < machine override (MachineDef::postPropertyOverrides) < job override (PostJob::properties).
Status resolveProperties(Session& s, const Declaration& d, const PostJob& job, detail::ProgramPlan& plan)
{
    for (const DeclaredProperty& p : d.properties) {
        PropertyValue value = p.defaultValue;
        if (const auto it = job.machine->postPropertyOverrides.find(p.info.key);
            it != job.machine->postPropertyOverrides.end()) {
            auto v = parseOverride(s, p, it->second, "machine " + job.machine->name);
            if (!v) {
                return tl::unexpected(v.error());
            }
            value = std::move(*v);
        }
        if (const auto it = job.properties.find(p.info.key); it != job.properties.end()) {
            auto v = parseOverride(s, p, it->second, "setup");
            if (!v) {
                return tl::unexpected(v.error());
            }
            value = std::move(*v);
        }
        plan.properties.emplace_back(p.info.key, std::move(value));
    }
    // Overrides the post does not declare: the setup's are reported (std::map: in key order); the machine's are not,
    // as a machine carries overrides for every post it may be used with.
    for (const auto& [key, text] : job.properties) {
        const bool known = std::any_of(d.properties.begin(), d.properties.end(),
                                       [&](const DeclaredProperty& p) { return p.info.key == key; });
        if (!known) {
            plan.warnings.push_back("post property '" + key + "' is not declared by the post and was ignored");
        }
    }
    return std::monostate{};
}

const PropertyValue* findProperty(const detail::ProgramPlan& plan, std::string_view key)
{
    for (const auto& [k, v] : plan.properties) {
        if (k == key) {
            return &v;
        }
    }
    return nullptr;
}

PostInfo infoOf(const Declaration& d, const std::string& scriptName)
{
    PostInfo info;
    info.id = postIdFromPath(scriptName);
    info.title = d.title;
    info.vendor = d.vendor;
    info.dialect = d.dialect;
    info.fileExtension = d.fileExtension;
    info.apiVersion = d.apiVersion;
    for (const DeclaredProperty& p : d.properties) {
        info.properties.push_back(p.info);
    }
    const bool anyCycle = std::any_of(d.motion.cycles.begin(), d.motion.cycles.end(), [](bool b) { return b; });
    info.caps = PostInfo::Caps{d.motion.arcs == ArcOutput::IJK,
                               d.motion.arcs == ArcOutput::R,
                               d.motion.helical,
                               anyCycle,
                               d.rigidTap,
                               d.motion.cutterComp,
                               d.toolLengthProbe,
                               static_cast<int>(d.motion.maxArcDegrees)};
    return info;
}

double rapidRate(const lib::MachineDef& m)
{
    double rate = 0;
    for (const lib::AxisDef& a : m.axes) {
        if (a.rapidMmMin > 0 && (rate == 0 || a.rapidMmMin < rate)) {
            rate = a.rapidMmMin;
        }
    }
    return rate;
}

Error threadFailure()
{
    return error(ErrorCode::Internal, "the post thread could not be started");
}

} // namespace

Result<PostInfo> QuickJsPostEngine::inspect(const std::string& scriptPath)
{
    auto source = readScript(scriptPath);
    if (!source) {
        return tl::unexpected(source.error());
    }
    return inspectSource(*source, scriptNameOf(scriptPath));
}

Result<PostOutput> QuickJsPostEngine::run(const std::string& scriptPath, const PostJob& job, JobContext& ctx)
{
    if (auto ok = validateJob(job); !ok) {
        return tl::unexpected(ok.error());
    }
    auto source = readScript(scriptPath);
    if (!source) {
        return tl::unexpected(source.error());
    }
    return runSource(*source, scriptNameOf(scriptPath), job, ctx);
}

Result<PostInfo> QuickJsPostEngine::inspectSource(std::string_view source, std::string_view scriptName)
{
    diagnostics_ = {};
    Result<PostInfo> result = tl::unexpected(threadFailure());
    const std::string name(scriptName);
    const bool started = runOnPostThread(limits_.threadStackBytes, [&] {
        const double cpu0 = threadCpuSeconds();
        try {
            Session s(name, limits_, limits_.inspectCpuSeconds, nullptr);
            s.setReport(&diagnostics_);
            result = [&]() -> Result<PostInfo> {
                if (auto loaded = s.load(source, diagnostics_.bytecodeBytes); !loaded) {
                    return tl::unexpected(loaded.error());
                }
                auto d = s.readDeclaration();
                if (!d) {
                    return tl::unexpected(d.error());
                }
                return infoOf(*d, name);
            }();
        } catch (const std::bad_alloc&) {
            result = tl::unexpected(error(ErrorCode::Internal, name + ": out of memory in the post host"));
        }
        diagnostics_.cpuSeconds = threadCpuSeconds() - cpu0;
    });
    if (!started) {
        return tl::unexpected(threadFailure());
    }
    return result;
}

Result<PostOutput> QuickJsPostEngine::runSource(std::string_view source, std::string_view scriptName,
                                                const PostJob& job, JobContext& ctx)
{
    diagnostics_ = {};
    if (auto ok = validateJob(job); !ok) {
        return tl::unexpected(ok.error());
    }
    if (ctx.cancel.cancelled()) {
        return tl::unexpected(error(ErrorCode::Cancelled, "posting was cancelled"));
    }
    const std::string name(scriptName);
    const std::size_t moves = totalMoves(job);
    const double cpuLimit = limits_.cpuSecondsBase + limits_.cpuSecondsPerMove * static_cast<double>(moves);
    const std::size_t outputLimit = limits_.outputBytesBase + limits_.outputBytesPerMove * moves;

    Result<PostOutput> result = tl::unexpected(threadFailure());
    const bool started = runOnPostThread(limits_.threadStackBytes, [&] {
        const double cpu0 = threadCpuSeconds();
        try {
            Session s(name, limits_, cpuLimit, &ctx.cancel);
            s.setReport(&diagnostics_);
            result = [&]() -> Result<PostOutput> {
                if (auto loaded = s.load(source, diagnostics_.bytecodeBytes); !loaded) {
                    return tl::unexpected(loaded.error());
                }
                auto d = s.readDeclaration();
                if (!d) {
                    return tl::unexpected(d.error());
                }
                detail::ProgramPlan plan;
                plan.units.inch = job.outputUnits == UnitSystem::Inch;
                plan.totalMoves = moves;
                if (auto resolved = resolveProperties(s, *d, job, plan); !resolved) {
                    return tl::unexpected(resolved.error());
                }
                const PropertyValue* lineEnding = findProperty(plan, detail::kPropLineEnding);
                const bool crlf = !lineEnding || std::get<std::string>(*lineEnding) != "lf";
                const PropertyValue* includeDate = findProperty(plan, detail::kPropIncludeDate);
                if (includeDate && std::get<bool>(*includeDate)) {
                    if (job.fixedDate) {
                        plan.date = job.fixedDate;
                    } else {
                        plan.warnings.push_back("includeDate is on but the post job carries no date "
                                                "(PostJob::fixedDate); the program has no date");
                    }
                }
                const double rapid = rapidRate(*job.machine);
                for (std::size_t i = 0; i < job.sections.size(); ++i) {
                    const PostSection& ps = job.sections[i];
                    plan.toolChanged.push_back(i == 0 ||
                                               job.sections[i - 1].op->tool.toolNumber != ps.op->tool.toolNumber);
                    plan.stats.push_back(sectionStats(*ps.toolpath, rapid));
                }
                WriterOptions options;
                options.inch = plan.units.inch;
                options.dialect = d->dialect;
                options.crlf = crlf;
                options.maxOutputBytes = outputLimit;
                options.maxWarnings = limits_.maxWarnings;
                options.maxBlockNumber = job.machine->maxBlockNumber;
                s.startOutput(std::move(options));
                return detail::runProgram(s, *d, job, ctx, plan);
            }();
        } catch (const std::bad_alloc&) {
            result = tl::unexpected(error(ErrorCode::Internal, name + ": out of memory in the post host"));
        }
        diagnostics_.cpuSeconds = threadCpuSeconds() - cpu0;
        diagnostics_.movesPosted = result ? moves : 0;
    });
    if (!started) {
        return tl::unexpected(threadFailure());
    }
    return result;
}

std::unique_ptr<IPostEngine> makeQuickJsPostEngine()
{
    return std::make_unique<QuickJsPostEngine>();
}

} // namespace ray::post
