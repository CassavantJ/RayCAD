// File output of posted programs (task T-047): the file name from a naming pattern, and the atomic write of a program
// into a folder (local or a network share) with an overwrite rule. post.h's writeProgram and OutputRule are built on
// it; the app (src/api, ray_api/cam_post.h) uses the richer pieces below directly. Internal to src/post: not one of
// the contract headers in include/ray/.
//
// Names. A pattern is literal text with fields in braces:
//   {program}     the setup's program number            {program:N}  the same, zero-padded to N digits (1..9)
//   {setup}       the setup's name                      {doc}        the document's name (no folder, no extension)
//   {part}        the part's name (the setup's first part body; writeProgram: the document's name)
//   {comment}     the setup's program comment           {post}       the post's id ("haas-ngc")
//   {machine}     the machine's name                    {ext}        the post's file extension with its dot (".nc")
//   {{ and }}     a literal brace
// A field's value goes in without spaces, since many controls and DNC links refuse a program name with one
// (fieldNameText): whitespace at its ends is dropped and each run of whitespace inside it becomes one '_' ("Setup 1"
// gives "Setup_1"). Literal text of the pattern keeps its spaces: what the user typed stays as typed.
// The post's extension is appended unless the pattern has {ext}. The expanded name is made safe on Windows, Linux
// and macOS at once (sanitizeFileName): only ASCII letters, digits, space and - _ . ( ) + , = # ~ stay, every other
// character becomes '_'; no leading space or dot, no trailing space or dot; a Windows device name (CON, PRN, AUX,
// NUL, COM0-9, LPT0-9, in any case, also with an extension) gets a '_' in front; empty becomes "program"; at most
// kMaxStemBytes. With a stem limit (the machine's maxProgramNameLength when the rule says so) the stem is cut to it
// and a note says so. Names are a pure function of the pattern and the fields (no clock, no locale).
//
// Writing (writeProgramFile). The bytes are written exactly as given (the post's line endings and encoding; nothing
// is converted), to a temporary file in the target folder (".<name>.<hash>-<n>.tmp", created with create-new
// semantics), flushed to the disk (FlushFileBuffers / fsync), then renamed onto the final name: with Replace over an
// existing file (MoveFileEx REPLACE_EXISTING | WRITE_THROUGH, rename(2)), else without replacing anything (MoveFileEx
// without REPLACE_EXISTING, link(2)). So the final name holds the whole old program or the whole new one, never a
// part: a crash, a kill, a cancel, a full disk or a share that goes away leaves at most the temporary file, whose
// ".tmp" name no control loads as a program; every failure the writer sees removes it. The rename is the commit: the
// cancel token is closed just before it (core.h), so a cancel either wins (nothing written) or comes too late.
// Overwrite rules: Refuse (an existing file is an error, ErrorCode::Locked, OutputProblem::Exists), Replace, NewName
// (the first free "<stem>_2", "<stem>_3", ... up to _9999, within the stem limit). "Ask" is the caller's: check with
// targetExists, ask, then write with Replace or NewName.
// Errors carry an OutputProblem (problemOf) and a message that names the file or folder and says what happened and
// that the old file, if any, is unchanged.
#pragma once
#include <ray/post.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ray::post {

// ---------- Names ----------
inline constexpr std::string_view kDefaultFileNamePattern = "{doc}_{setup}";
inline constexpr std::size_t kMaxStemBytes = 180; // well inside every file system's 255, with room for the temp name

struct NameFields {
    int programNumber = 0;
    std::string programComment, setupName, documentName, partName, postId, machineName;
    std::string extension; // PostInfo::fileExtension, with its dot: ".nc"
};
// The fields a post job gives (partName: the document's name; PostJob names no part).
NameFields nameFieldsOf(const PostInfo& info, const PostJob& job);

struct FileName {
    std::string name;               // stem + extension, safe on every OS
    std::string stem, extension;    // extension with its dot, "" when there is none
    std::vector<std::string> notes; // what was changed to make it safe or short enough, for the user
};
// InvalidArgument naming the problem for an unknown field, a bad width or an unbalanced brace.
Result<FileName> fileNameFor(std::string_view pattern, const NameFields& fields, int maxStemLength = 0);
// One stem made safe (see the top of this file); *changed says whether anything was changed.
std::string sanitizeFileName(std::string_view stem, bool* changed = nullptr);
// A field's value as a name takes it: no whitespace at its ends, each inner run of whitespace one '_'.
std::string fieldNameText(std::string_view value);
// True for the temporary names writeProgramFile uses.
bool isTemporaryOutputName(std::string_view name);

// ---------- The file system seam (tests inject fakes: a vanished share, a full disk, a locked file) ----------
class IOutputFile {
public:
    virtual ~IOutputFile() = default;
    virtual Status write(std::string_view bytes) = 0;
    virtual Status flush() = 0; // to the disk
    virtual Status close() = 0;
};
class IOutputFs {
public:
    virtual ~IOutputFs() = default;
    // The folder exists and can be reached: OutputProblem FolderMissing or Unreachable otherwise.
    virtual Status checkFolder(const std::string& dir) = 0;
    virtual Result<bool> exists(const std::string& path) = 0;
    // Create-new: Exists when the name is taken.
    virtual Result<std::unique_ptr<IOutputFile>> createNew(const std::string& path) = 0;
    // Renames `from` onto `to`, replacing it atomically (Locked when `to` is held open or read-only).
    virtual Status replace(const std::string& from, const std::string& to) = 0;
    // Renames `from` to `to` only if `to` does not exist (Exists otherwise), atomically where the file system can.
    virtual Status moveNoReplace(const std::string& from, const std::string& to) = 0;
    virtual Status remove(const std::string& path) = 0;
};
// The real file system (local disks and network shares, UTF-8 paths).
IOutputFs& localOutputFs();

// What went wrong, for the caller's choice of what to offer (Error::context[0] carries it).
enum class OutputProblem : std::uint8_t {
    None,
    Exists,        // the file is there and the rule does not replace it
    Locked,        // the file is open in another program or read-only
    FolderMissing, // the folder does not exist (or is not a folder)
    Unreachable,   // the folder or share cannot be reached, or went away while writing
    DiskFull,
    Denied, // no permission
    Cancelled,
    Other
};
OutputProblem problemOf(const Error& error);
std::string_view problemTag(OutputProblem problem); // "output.exists", ...
Error outputError(ErrorCode code, OutputProblem problem, std::string message, std::string detail = {});

// ---------- Writing ----------
enum class OverwriteMode : std::uint8_t { Refuse, Replace, NewName };
struct OutputTarget {
    std::string directory; // UTF-8; a local folder or a share (\\server\share\..., /mnt/...)
    std::string fileName;  // FileName::name
    OverwriteMode mode = OverwriteMode::Refuse;
    int maxStemLength = 0; // NewName keeps "<stem>_n" within it (0: no limit)
};
struct Written {
    std::string path;     // the final file
    std::string fileName; // its name (NewName may have chosen another)
    bool replaced = false;
};
std::string joinPath(const std::string& dir, const std::string& name);
// Whether the target's file exists now (for "ask"). The folder is checked first.
Result<bool> targetExists(const OutputTarget& target, IOutputFs& fs);
// See the top of this file. `cancel` may be null; it is polled between steps and closed just before the rename.
Result<Written> writeProgramFile(std::string_view bytes, const OutputTarget& target, IOutputFs& fs,
                                 CancelToken* cancel = nullptr);

} // namespace ray::post
