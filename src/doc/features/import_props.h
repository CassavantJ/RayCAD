// The props of the "import" feature (task T-010), private to src/doc. The JSON layout is documented in
// <ray/document.h> ("The import feature").
#pragma once

#include <ray/document.h>

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ray::doc::features {

inline constexpr std::string_view kImportType = "import";
inline constexpr std::string_view kImportCommand = "document.import";

struct ImportBody {
    BodyId id;
    std::string name;
    std::array<float, 4> color{};
    Hash256 blob; // BLAKE3 of the body's kernel::serialize bytes (== Shape::contentHash())
};

struct ImportProps {
    std::string sourceFile;
    ImportFormat format = ImportFormat::Step;
    Hash256 fileHash;
    kernel::ImportOptions options;
    Body::Role role = Body::Role::Part;
    std::optional<UnitSystem> sourceUnits;
    std::optional<std::vector<ImportBody>> bodies;
    std::vector<std::string> warnings;
};

// Reads import props. Errors are InvalidArgument, "<where>: '<field>' <what>". `strict` rejects unknown keys
// (the document.import command); the rebuild ignores them, so a file from a newer version still builds.
Result<ImportProps> parseImportProps(std::string_view propsJson, bool strict, std::string_view where);
// The canonical JSON of the props (every option written out).
std::string writeImportProps(const ImportProps& props);
// The canonical arguments of document.import: {"id", "index"?, "name", "props"}.
std::string writeImportCommandArgs(const FeatureId& feature, const std::string& name, const ImportProps& props,
                                   std::optional<std::size_t> index);

// 64 lower-case hex digits.
std::optional<Hash256> parseHashHex(std::string_view text);
// From the extension, any case: .step .stp .p21 are STEP, .iges .igs are IGES.
std::optional<ImportFormat> formatFromPath(std::string_view path);
std::string_view formatName(ImportFormat format);
// The last component of a path written with '/' or '\'.
std::string_view fileNameOf(std::string_view path);

} // namespace ray::doc::features
