// File output of posted programs (output.h): names from patterns, the overwrite rules and the atomic write. The file
// system itself is output_fs.cpp. post.h's writeProgram is at the end.
#include "output.h"

#include <algorithm>
#include <array>
#include <charconv>

namespace ray::post {

namespace {

constexpr std::size_t kChunkBytes = std::size_t(256) << 10;
constexpr int kMaxNewNameNumber = 9999;
constexpr int kMaxTempAttempts = 64;

bool asciiAlnum(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

bool keptChar(char c)
{
    if (asciiAlnum(c)) {
        return true;
    }
    switch (c) {
    case ' ':
    case '-':
    case '_':
    case '.':
    case '(':
    case ')':
    case '+':
    case ',':
    case '=':
    case '#':
    case '~':
        return true;
    default:
        return false;
    }
}

char upper(char c)
{
    return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
}

// CON, PRN, AUX, NUL, COM1-9, LPT1-9 (Windows refuses them as file names, with any extension).
bool deviceName(std::string_view stem)
{
    const std::size_t dot = stem.find('.');
    std::string base(stem.substr(0, dot));
    while (!base.empty() && base.back() == ' ') {
        base.pop_back();
    }
    std::transform(base.begin(), base.end(), base.begin(), upper);
    static constexpr std::array<std::string_view, 4> plain{"CON", "PRN", "AUX", "NUL"};
    if (std::find(plain.begin(), plain.end(), base) != plain.end()) {
        return true;
    }
    return base.size() == 4 && (base.compare(0, 3, "COM") == 0 || base.compare(0, 3, "LPT") == 0) && base[3] >= '0' &&
           base[3] <= '9';
}

std::string numberText(int n, int width)
{
    std::string digits = std::to_string(n < 0 ? -static_cast<long long>(n) : static_cast<long long>(n));
    if (static_cast<int>(digits.size()) < width) {
        digits.insert(0, static_cast<std::size_t>(width) - digits.size(), '0');
    }
    return n < 0 ? "-" + digits : digits;
}

Error invalid(std::string message)
{
    return Error{ErrorCode::InvalidArgument, std::move(message), {}};
}

std::string hex8(const Hash256& h)
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    for (int i = 0; i < 4; ++i) {
        out.push_back(kHex[h.bytes[static_cast<std::size_t>(i)] >> 4]);
        out.push_back(kHex[h.bytes[static_cast<std::size_t>(i)] & 15]);
    }
    return out;
}

// The stem with "_n" appended, cut so that the whole stays within `limit` characters (0: no limit).
std::string numbered(const std::string& stem, int n, int limit)
{
    const std::string suffix = "_" + std::to_string(n);
    std::string base = stem;
    if (limit > 0 && base.size() + suffix.size() > static_cast<std::size_t>(limit)) {
        const std::size_t keep =
            static_cast<std::size_t>(limit) > suffix.size() ? static_cast<std::size_t>(limit) - suffix.size() : 0;
        base.resize(std::min(base.size(), keep));
        while (!base.empty() && (base.back() == ' ' || base.back() == '.')) {
            base.pop_back();
        }
    }
    return base + suffix;
}

// The file name split into stem and extension at the last dot (a name with no dot, or only a leading one, has none).
std::pair<std::string, std::string> splitName(const std::string& name)
{
    const std::size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot == 0) {
        return {name, std::string()};
    }
    return {name.substr(0, dot), name.substr(dot)};
}

Status cancelledStatus()
{
    return tl::unexpected(outputError(ErrorCode::Cancelled, OutputProblem::Cancelled,
                                      "Posting was cancelled; no file was written"));
}

} // namespace

// ---------- Problems ----------
std::string_view problemTag(OutputProblem problem)
{
    switch (problem) {
    case OutputProblem::None:
        return "output.none";
    case OutputProblem::Exists:
        return "output.exists";
    case OutputProblem::Locked:
        return "output.locked";
    case OutputProblem::FolderMissing:
        return "output.folderMissing";
    case OutputProblem::Unreachable:
        return "output.unreachable";
    case OutputProblem::DiskFull:
        return "output.diskFull";
    case OutputProblem::Denied:
        return "output.denied";
    case OutputProblem::Cancelled:
        return "output.cancelled";
    case OutputProblem::Other:
        return "output.other";
    }
    return "output.other";
}

OutputProblem problemOf(const Error& error)
{
    if (error.context.empty()) {
        return error.code == ErrorCode::Cancelled ? OutputProblem::Cancelled : OutputProblem::Other;
    }
    for (int i = 0; i <= static_cast<int>(OutputProblem::Other); ++i) {
        const auto p = static_cast<OutputProblem>(i);
        if (error.context.front() == problemTag(p)) {
            return p;
        }
    }
    return error.code == ErrorCode::Cancelled ? OutputProblem::Cancelled : OutputProblem::Other;
}

Error outputError(ErrorCode code, OutputProblem problem, std::string message, std::string detail)
{
    Error e{code, std::move(message), {std::string(problemTag(problem))}};
    if (!detail.empty()) {
        e.context.push_back(std::move(detail));
    }
    return e;
}

// ---------- Names ----------
NameFields nameFieldsOf(const PostInfo& info, const PostJob& job)
{
    NameFields f;
    if (job.setup != nullptr) {
        f.programNumber = job.setup->programNumber;
        f.programComment = job.setup->programComment;
        f.setupName = job.setup->name;
    }
    f.documentName = job.documentName;
    f.partName = job.documentName;
    f.postId = info.id;
    f.machineName = job.machine != nullptr ? job.machine->name : std::string();
    f.extension = info.fileExtension;
    return f;
}

std::string sanitizeFileName(std::string_view stem, bool* changed)
{
    std::string out;
    out.reserve(stem.size());
    bool touched = false;
    for (std::size_t i = 0; i < stem.size(); ++i) {
        const auto c = static_cast<unsigned char>(stem[i]);
        if (c >= 0x80) {
            // One '_' per UTF-8 character (its continuation bytes are skipped), not per byte.
            out.push_back('_');
            touched = true;
            while (i + 1 < stem.size() && (static_cast<unsigned char>(stem[i + 1]) & 0xC0) == 0x80) {
                ++i;
            }
            continue;
        }
        if (keptChar(static_cast<char>(c))) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('_');
            touched = true;
        }
    }
    // No leading spaces or dots (a dot file is hidden on Linux and macOS), no trailing ones (Windows drops them).
    const std::size_t first = out.find_first_not_of(" .");
    if (first == std::string::npos) {
        touched = touched || !out.empty();
        out.clear();
    } else if (first > 0) {
        out.erase(0, first);
        touched = true;
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) {
        out.pop_back();
        touched = true;
    }
    if (out.size() > kMaxStemBytes) {
        out.resize(kMaxStemBytes);
        while (!out.empty() && (out.back() == ' ' || out.back() == '.')) {
            out.pop_back();
        }
        touched = true;
    }
    if (out.empty()) {
        out = "program";
        touched = true;
    }
    if (deviceName(out)) {
        out.insert(0, 1, '_');
        touched = true;
    }
    if (changed != nullptr) {
        *changed = touched;
    }
    return out;
}

bool isTemporaryOutputName(std::string_view name)
{
    return name.size() > 6 && name.front() == '.' && name.substr(name.size() - 4) == ".tmp";
}

Result<FileName> fileNameFor(std::string_view pattern, const NameFields& fields, int maxStemLength)
{
    std::string raw;
    bool wroteExt = false;
    for (std::size_t i = 0; i < pattern.size();) {
        const char c = pattern[i];
        if (c == '{' && i + 1 < pattern.size() && pattern[i + 1] == '{') {
            raw.push_back('{');
            i += 2;
            continue;
        }
        if (c == '}' && i + 1 < pattern.size() && pattern[i + 1] == '}') {
            raw.push_back('}');
            i += 2;
            continue;
        }
        if (c == '}') {
            return tl::unexpected(invalid("The file name pattern has a '}' without its '{' (write }} for a brace)"));
        }
        if (c != '{') {
            raw.push_back(c);
            ++i;
            continue;
        }
        const std::size_t close = pattern.find('}', i + 1);
        if (close == std::string_view::npos) {
            return tl::unexpected(invalid("The file name pattern has a '{' without its '}' (write {{ for a brace)"));
        }
        const std::string_view field = pattern.substr(i + 1, close - i - 1);
        i = close + 1;
        std::string_view key = field;
        int width = 0;
        if (const std::size_t colon = field.find(':'); colon != std::string_view::npos) {
            key = field.substr(0, colon);
            const std::string_view w = field.substr(colon + 1);
            const auto r = std::from_chars(w.data(), w.data() + w.size(), width);
            if (key != "program" || r.ec != std::errc() || r.ptr != w.data() + w.size() || width < 1 || width > 9) {
                return tl::unexpected(invalid("The file name pattern's field {" + std::string(field) +
                                              "} is not valid: only {program:N} takes a width, N from 1 to 9"));
            }
        }
        if (key == "program") {
            raw += numberText(fields.programNumber, width);
        } else if (key == "setup") {
            raw += fields.setupName;
        } else if (key == "doc") {
            raw += fields.documentName;
        } else if (key == "part") {
            raw += fields.partName;
        } else if (key == "comment") {
            raw += fields.programComment;
        } else if (key == "post") {
            raw += fields.postId;
        } else if (key == "machine") {
            raw += fields.machineName;
        } else if (key == "ext") {
            raw += fields.extension;
            wroteExt = true;
        } else {
            return tl::unexpected(invalid("The file name pattern names an unknown field {" + std::string(field) +
                                          "}; the fields are {program}, {program:N}, {setup}, {doc}, {part}, "
                                          "{comment}, {post}, {machine} and {ext}"));
        }
    }
    FileName out;
    // The extension as the post gives it, made safe the same way (".nc" stays ".nc").
    std::string ext;
    if (!wroteExt && !fields.extension.empty()) {
        bool extChanged = false;
        const std::string body =
            sanitizeFileName(fields.extension.front() == '.' ? fields.extension.substr(1) : fields.extension,
                             &extChanged);
        ext = "." + body;
    }
    std::string stem = raw;
    if (wroteExt) {
        // The pattern wrote the extension: the name is made safe as a whole, the stem is up to its last dot.
        auto [s, e] = splitName(raw);
        stem = s;
        ext = e;
        bool extChanged = false;
        if (!ext.empty()) {
            ext = "." + sanitizeFileName(ext.substr(1), &extChanged);
        }
    }
    bool changed = false;
    stem = sanitizeFileName(stem, &changed);
    if (changed) {
        out.notes.push_back("The file name was made safe for Windows, Linux and macOS: characters other than "
                            "letters, digits, space and - _ . ( ) + , = # ~ became '_'");
    }
    if (maxStemLength > 0 && stem.size() > static_cast<std::size_t>(maxStemLength)) {
        stem.resize(static_cast<std::size_t>(maxStemLength));
        while (!stem.empty() && (stem.back() == ' ' || stem.back() == '.')) {
            stem.pop_back();
        }
        if (stem.empty()) {
            stem = "P";
        }
        if (deviceName(stem)) {
            stem = "_" + stem.substr(0, stem.size() > 1 ? stem.size() - 1 : stem.size());
        }
        out.notes.push_back("The file name was cut to " + std::to_string(maxStemLength) +
                            " characters, the machine's longest program name");
    }
    out.stem = stem;
    out.extension = ext;
    out.name = stem + ext;
    return out;
}

// ---------- Writing ----------
std::string joinPath(const std::string& dir, const std::string& name)
{
    if (dir.empty()) {
        return name;
    }
    const char last = dir.back();
    if (last == '/' || last == '\\') {
        return dir + name;
    }
#ifdef _WIN32
    return dir + "\\" + name;
#else
    return dir + "/" + name;
#endif
}

Result<bool> targetExists(const OutputTarget& target, IOutputFs& fs)
{
    if (auto ok = fs.checkFolder(target.directory); !ok) {
        return tl::unexpected(ok.error());
    }
    return fs.exists(joinPath(target.directory, target.fileName));
}

Result<Written> writeProgramFile(std::string_view bytes, const OutputTarget& target, IOutputFs& fs,
                                 CancelToken* cancel)
{
    const auto isCancelled = [&] { return cancel != nullptr && cancel->cancelled(); };
    if (target.fileName.empty() || target.fileName.find_first_of("/\\") != std::string::npos) {
        return tl::unexpected(invalid("The file name '" + target.fileName + "' is not a plain file name"));
    }
    if (isCancelled()) {
        return tl::unexpected(cancelledStatus().error());
    }
    if (auto ok = fs.checkFolder(target.directory); !ok) {
        return tl::unexpected(ok.error());
    }

    // The final name: as given, or with Refuse the existing file is an error before anything is written. NewName
    // looks for a free name now and again at the rename (another program may take it meanwhile).
    std::string name = target.fileName;
    auto [stem, ext] = splitName(name);
    bool replaced = false;
    const auto taken = [&](const std::string& n) { return fs.exists(joinPath(target.directory, n)); };
    if (target.mode != OverwriteMode::Replace) {
        auto there = taken(name);
        if (!there) {
            return tl::unexpected(there.error());
        }
        if (*there && target.mode == OverwriteMode::Refuse) {
            return tl::unexpected(outputError(ErrorCode::Locked, OutputProblem::Exists,
                                              name + " already exists in " + target.directory +
                                                  "; it was not replaced (the overwrite rule refuses)"));
        }
        if (*there) {
            bool found = false;
            for (int n = 2; n <= kMaxNewNameNumber && !found; ++n) {
                const std::string candidate = numbered(stem, n, target.maxStemLength) + ext;
                auto t = taken(candidate);
                if (!t) {
                    return tl::unexpected(t.error());
                }
                if (!*t) {
                    name = candidate;
                    found = true;
                }
            }
            if (!found) {
                return tl::unexpected(outputError(ErrorCode::Locked, OutputProblem::Exists,
                                                  "No free numbered name for " + target.fileName + " in " +
                                                      target.directory));
            }
        }
    }

    // The temporary file, beside the target so the rename never crosses a volume.
    Hasher h;
    h.str(name).i64(static_cast<std::int64_t>(bytes.size()));
    const std::string tag = hex8(h.finish());
    std::unique_ptr<IOutputFile> file;
    std::string temp;
    for (int attempt = 0; attempt < kMaxTempAttempts && !file; ++attempt) {
        temp = joinPath(target.directory, "." + name + "." + tag + "-" + std::to_string(attempt) + ".tmp");
        auto made = fs.createNew(temp);
        if (made) {
            file = std::move(*made);
        } else if (problemOf(made.error()) != OutputProblem::Exists) {
            return tl::unexpected(made.error());
        }
    }
    if (!file) {
        return tl::unexpected(outputError(ErrorCode::IoError, OutputProblem::Other,
                                          "Could not make a temporary file in " + target.directory));
    }
    // From here every failure removes the temporary file (best effort: a share that went away keeps it).
    const auto abandon = [&](Error e) -> Result<Written> {
        (void)file->close();
        (void)fs.remove(temp);
        return tl::unexpected(std::move(e));
    };
    for (std::size_t at = 0; at < bytes.size(); at += kChunkBytes) {
        if (isCancelled()) {
            return abandon(cancelledStatus().error());
        }
        if (auto ok = file->write(bytes.substr(at, kChunkBytes)); !ok) {
            return abandon(ok.error());
        }
    }
    if (auto ok = file->flush(); !ok) {
        return abandon(ok.error());
    }
    if (auto ok = file->close(); !ok) {
        (void)fs.remove(temp);
        return tl::unexpected(ok.error());
    }
    // The point of no return.
    if (cancel != nullptr && !cancel->close()) {
        (void)fs.remove(temp);
        return tl::unexpected(cancelledStatus().error());
    }
    const std::string finalPath = joinPath(target.directory, name);
    if (target.mode == OverwriteMode::Replace) {
        auto there = fs.exists(finalPath);
        replaced = there && *there;
        if (auto ok = fs.replace(temp, finalPath); !ok) {
            (void)fs.remove(temp);
            return tl::unexpected(ok.error());
        }
    } else {
        Status moved = fs.moveNoReplace(temp, finalPath);
        // NewName: someone took the name since it was chosen; the next free one.
        for (int n = 2; !moved && target.mode == OverwriteMode::NewName && problemOf(moved.error()) == OutputProblem::Exists &&
                        n <= kMaxNewNameNumber;
             ++n) {
            name = numbered(stem, n, target.maxStemLength) + ext;
            moved = fs.moveNoReplace(temp, joinPath(target.directory, name));
        }
        if (!moved) {
            (void)fs.remove(temp);
            if (problemOf(moved.error()) == OutputProblem::Exists) {
                return tl::unexpected(outputError(ErrorCode::Locked, OutputProblem::Exists,
                                                  name + " appeared in " + target.directory +
                                                      " while the program was written; it was not replaced"));
            }
            return tl::unexpected(moved.error());
        }
    }
    return Written{joinPath(target.directory, name), name, replaced};
}

// ---------- post.h ----------
Result<std::string> writeProgram(const PostOutput& output, const PostInfo& info, const PostJob& job,
                                 const OutputRule& rule)
{
    const std::string_view pattern =
        rule.fileNamePattern.empty() ? kDefaultFileNamePattern : std::string_view(rule.fileNamePattern);
    auto name = fileNameFor(pattern, nameFieldsOf(info, job));
    if (!name) {
        return tl::unexpected(name.error());
    }
    OutputTarget target;
    target.directory = rule.directory;
    target.fileName = name->name;
    target.mode = rule.overwrite ? OverwriteMode::Replace : OverwriteMode::Refuse;
    auto written = writeProgramFile(output.text, target, localOutputFs());
    if (!written) {
        return tl::unexpected(written.error());
    }
    return written->path;
}

} // namespace ray::post
