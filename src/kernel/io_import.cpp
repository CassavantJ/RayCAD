// STEP and IGES import (task T-005).
//
// Pipeline, all on the import's I/O thread (io_internal.h explains why and how cancel works):
//   1. Read. STEP goes through ReadStream with our own stream buffer (spike S3): byte progress, and a cancel
//      ends the parse within milliseconds. After the parse OCCT builds its model without polling anything
//      (about 30 ms per MiB); the caller has returned by then if it cancelled. IGES has no stream reader in
//      OCCT 8.0.1 and is read with ReadFile (OCCT converts the UTF-8 path itself).
//   2. Transfer into an XCAF document, with names and colours, converted to millimetres.
//      Heal::Reader keeps the reader's built-in shape fixing; None and Full switch it off.
//   3. Flatten: assemblies are walked in document order, each leaf instance gets the product of the locations
//      above it. A leaf part is split into bodies: its solids, then its loose shells and faces. With
//      ImportOptions::sew these are sewn: each sewn shell is a body (a solid where it is closed) and the faces
//      left over are one more body; without sewing they stay together as one body. Free curves and points
//      are skipped with a warning.
//   4. Heal (Heal::Full): one ShapeFix_Shape per body, sequentially, in body order. Sequential on purpose: the
//      kernel has no job system to fan out to (kernel.h gives none), and spike S3 found concurrent OCCT work on
//      one process unsafe for meshing and not yet soak-tested for healing. One unit of work per body keeps the
//      cancel granularity and the progress steps, and the result does not depend on thread count. A part used
//      several times is sewn and healed once.
//   5. Check every body with BRepCheck_Analyzer (an invalid body is kept, with a warning), then place each
//      instance: a part's first use at the identity keeps its shapes, every other use gets its own copy with
//      the transform applied to the geometry, so no two bodies share a TShape (meshing writes into TShapes).
#include "io_occt.h"
#include "shape_impl.h"

#include <BRepBuilderAPI_Copy.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepLib.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <DESTEP_Parameters.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <IGESCAFControl_Reader.hxx>
#include <IGESControl_Writer.hxx>
#include <IGESData_GlobalSection.hxx>
#include <IGESData_IGESModel.hxx>
#include <NCollection_DataMap.hxx>
#include <NCollection_Sequence.hxx>
#include <Quantity_Color.hxx>
#include <Quantity_ColorRGBA.hxx>
#include <STEPCAFControl_Reader.hxx>
#include <STEPControl_Reader.hxx>
#include <STEPControl_Writer.hxx>
#include <ShapeFix_Shape.hxx>
#include <ShapeProcess.hxx>
#include <TCollection_AsciiString.hxx>
#include <TCollection_ExtendedString.hxx>
#include <TCollection_HAsciiString.hxx>
#include <TDF_Label.hxx>
#include <TDF_Tool.hxx>
#include <TDataStd_Name.hxx>
#include <TDocStd_Document.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopTools_ShapeMapHasher.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Solid.hxx>
#include <UnitsMethods_LengthUnit.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_ColorType.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <istream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <sys/types.h>
#endif

namespace ray::kernel {

namespace {

using detail::IoTask;
using detail::TaskIndicator;

enum class Format : std::uint8_t { Step, Iges };

constexpr std::string_view kStageRead = "Reading file";
constexpr std::string_view kStageLoad = "Loading model";
constexpr std::string_view kStageTransfer = "Transferring";
constexpr std::string_view kStageSew = "Sewing";
constexpr std::string_view kStageHeal = "Healing";
constexpr std::string_view kStageCheck = "Checking bodies";
constexpr std::string_view kStagePlace = "Placing bodies";
constexpr std::string_view kStageDone = "Done";

// Where each stage ends, as a fraction of the whole import.
constexpr double kReadEnd = 0.15;     // STEP: bytes parsed
constexpr double kLoadEnd = 0.30;     // model built (no progress inside)
constexpr double kTransferEnd = 0.55; // XCAF transfer
constexpr double kSewEnd = 0.60;
constexpr double kCheckEnd = 0.97; // then placement up to 1.0

constexpr int kMaxAssemblyDepth = 64;

using Rgba = std::array<float, 4>;
// Neutral grey for bodies whose file gives no colour (kernel.h has no "no colour" value).
constexpr Rgba kDefaultColor{0.75f, 0.75f, 0.75f, 1.0f};

using ColorMap = NCollection_DataMap<TopoDS_Shape, Rgba, TopTools_ShapeMapHasher>;

Error failure(ErrorCode code, std::string message, std::string_view where)
{
    return detail::makeError(code, std::move(message), where);
}

std::string trimmed(std::string text)
{
    const auto notSpace = [](unsigned char c) {
        return std::isspace(c) == 0;
    };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), notSpace));
    text.erase(std::find_if(text.rbegin(), text.rend(), notSpace).base(), text.end());
    return text;
}

std::string upper(std::string text)
{
    for (char& c : text)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return text;
}

// ---------- XCAF attributes ----------

std::string labelName(const TDF_Label& label)
{
    occ::handle<TDataStd_Name> name;
    if (label.IsNull() || !label.FindAttribute(TDataStd_Name::GetID(), name) || name.IsNull())
        return {};
    const TCollection_AsciiString utf8(name->Get()); // no replacement character: converts to UTF-8
    return trimmed(std::string(utf8.ToCString(), static_cast<std::size_t>(utf8.Length())));
}

std::optional<Rgba> labelColor(const TDF_Label& label)
{
    if (label.IsNull())
        return std::nullopt;
    Quantity_ColorRGBA color;
    if (!XCAFDoc_ColorTool::GetColor(label, XCAFDoc_ColorSurf, color) &&
        !XCAFDoc_ColorTool::GetColor(label, XCAFDoc_ColorGen, color))
        return std::nullopt;
    double r = 0.0, g = 0.0, b = 0.0;
    color.GetRGB().Values(r, g, b, Quantity_TOC_sRGB);
    return Rgba{static_cast<float>(r), static_cast<float>(g), static_cast<float>(b), color.Alpha()};
}

// Colours set on sub-shapes of a part (solids, shells, faces), keyed by the sub-shape.
ColorMap subShapeColors(const TDF_Label& part)
{
    ColorMap colors;
    NCollection_Sequence<TDF_Label> subs;
    if (!XCAFDoc_ShapeTool::GetSubShapes(part, subs))
        return colors;
    for (int i = 1; i <= subs.Length(); ++i) {
        const std::optional<Rgba> color = labelColor(subs.Value(i));
        const TopoDS_Shape shape = XCAFDoc_ShapeTool::GetShape(subs.Value(i));
        if (color.has_value() && !shape.IsNull() && !colors.IsBound(shape))
            colors.Bind(shape, *color);
    }
    return colors;
}

// The colour of the first face of `shape` (canonical order) that `colors` knows.
std::optional<Rgba> firstFaceColor(const TopoDS_Shape& shape, const ColorMap& colors)
{
    if (colors.IsEmpty())
        return std::nullopt;
    detail::ShapeIndex faces;
    TopExp::MapShapes(shape, TopAbs_FACE, faces);
    for (int i = 1; i <= faces.Extent(); ++i)
        if (const Rgba* color = colors.Seek(faces.FindKey(i)))
            return *color;
    return std::nullopt;
}

// False for empty names and for the placeholders that writers put in when nobody named the item ("SOLID",
// "COMPOUND", OCCT's "Open CASCADE STEP translator 8.0 1", ...). Those make bad body names.
bool meaningfulName(const std::string& name)
{
    if (name.empty())
        return false;
    const std::string text = upper(name);
    for (const char* generic : {"SOLID", "SHELL", "OPEN SHELL", "CLOSED SHELL", "COMPOUND", "COMPSOLID", "FACE",
                                "MANIFOLD_SOLID_BREP", "BREP_WITH_VOIDS", "SHAPE", "NONE", "UNKNOWN", "UNNAMED"})
        if (text == generic)
            return false;
    return text.rfind("OPEN CASCADE STEP TRANSLATOR", 0) != 0 && text.rfind("=>[", 0) != 0;
}

// ---------- Assembly walk ----------

struct Leaf {
    TDF_Label part;           // a simple shape (not an assembly)
    TopLoc_Location location; // product of the component locations above it
    // The labels above it from the top down: assembly, component, assembly, component, ...
    std::vector<TDF_Label> ancestors;
};

void collectLeaves(const TDF_Label& label, const TopLoc_Location& location, std::vector<TDF_Label>& ancestors,
                   int depth, std::vector<Leaf>& leaves, bool& tooDeep)
{
    if (depth > kMaxAssemblyDepth) {
        tooDeep = true;
        return;
    }
    if (!XCAFDoc_ShapeTool::IsAssembly(label)) {
        leaves.push_back(Leaf{label, location, ancestors});
        return;
    }
    NCollection_Sequence<TDF_Label> components;
    XCAFDoc_ShapeTool::GetComponents(label, components, false);
    for (int i = 1; i <= components.Length(); ++i) {
        const TDF_Label& component = components.Value(i);
        TDF_Label referred;
        if (!XCAFDoc_ShapeTool::GetReferredShape(component, referred))
            continue;
        ancestors.push_back(label);
        ancestors.push_back(component);
        collectLeaves(referred, location * XCAFDoc_ShapeTool::GetLocation(component), ancestors, depth + 1, leaves,
                      tooDeep);
        ancestors.pop_back();
        ancestors.pop_back();
    }
}

// ---------- Parts ----------

struct PartBody {
    TopoDS_Shape shape;            // after sewing and healing
    std::optional<Rgba> ownColor;  // colour of the solid itself (sub-shape label)
    std::optional<Rgba> faceColor; // colour of its first coloured face
    int faceCount = 0;
    bool valid = true;
};

struct Part {
    std::string name;
    std::optional<Rgba> color;
    std::vector<PartBody> bodies;
    int skippedCurves = 0; // free edges and vertices: wireframe is not imported
};

int countFaces(const TopoDS_Shape& shape)
{
    detail::ShapeIndex faces;
    TopExp::MapShapes(shape, TopAbs_FACE, faces);
    return faces.Extent();
}

// A closed shell as a solid with its faces pointing out; the shell itself if that fails.
TopoDS_Shape solidFromShell(const TopoDS_Shell& shell)
{
    if (!BRep_Tool::IsClosed(shell))
        return shell;
    BRepBuilderAPI_MakeSolid maker(shell);
    if (!maker.IsDone())
        return shell;
    TopoDS_Solid solid = maker.Solid();
    if (!BRepLib::OrientClosedSolid(solid))
        return shell;
    return solid;
}

// Splits a part's shape into bodies. Sewing reports into [from, to] of the task. Returns false if stopped.
bool splitPart(const TopoDS_Shape& shape, const ImportOptions& options, const ColorMap& colors, IoTask& task,
               double from, double to, Part& part)
{
    detail::ShapeIndex solids;
    TopExp::MapShapes(shape, TopAbs_SOLID, solids);
    for (int i = 1; i <= solids.Extent(); ++i) {
        const TopoDS_Shape& solid = solids.FindKey(i);
        PartBody body;
        body.shape = solid;
        if (const Rgba* color = colors.Seek(solid))
            body.ownColor = *color;
        body.faceColor = firstFaceColor(solid, colors);
        part.bodies.push_back(std::move(body));
    }

    detail::ShapeIndex looseShells, looseFaces;
    for (TopExp_Explorer it(shape, TopAbs_SHELL, TopAbs_SOLID); it.More(); it.Next())
        looseShells.Add(it.Current());
    for (TopExp_Explorer it(shape, TopAbs_FACE, TopAbs_SHELL); it.More(); it.Next())
        looseFaces.Add(it.Current());
    detail::ShapeIndex freeCurves;
    for (TopExp_Explorer it(shape, TopAbs_EDGE, TopAbs_FACE); it.More(); it.Next())
        freeCurves.Add(it.Current());
    for (TopExp_Explorer it(shape, TopAbs_VERTEX, TopAbs_EDGE); it.More(); it.Next())
        freeCurves.Add(it.Current());
    part.skippedCurves = freeCurves.Extent();

    if (looseShells.IsEmpty() && looseFaces.IsEmpty())
        return true;

    std::vector<TopoDS_Shape> shells;
    TopoDS_Compound leftoverFaces;
    BRep_Builder builder;
    builder.MakeCompound(leftoverFaces);
    bool anyLeftover = false;
    ColorMap pieceColors; // colours of the loose faces, keyed by the face after sewing

    if (options.sew) {
        BRepBuilderAPI_Sewing sewing(options.sewTolerance);
        for (int i = 1; i <= looseShells.Extent(); ++i)
            sewing.Add(looseShells.FindKey(i));
        for (int i = 1; i <= looseFaces.Extent(); ++i)
            sewing.Add(looseFaces.FindKey(i));
        occ::handle<TaskIndicator> indicator = new TaskIndicator(task, from, to, kStageSew);
        sewing.Perform(indicator->Start());
        if (task.stopRequested())
            return false;
        const TopoDS_Shape sewn = sewing.SewedShape();
        if (sewn.IsNull())
            return true;
        if (!colors.IsEmpty()) {
            detail::ShapeIndex originalFaces;
            for (int i = 1; i <= looseShells.Extent(); ++i)
                TopExp::MapShapes(looseShells.FindKey(i), TopAbs_FACE, originalFaces);
            for (int i = 1; i <= looseFaces.Extent(); ++i)
                originalFaces.Add(looseFaces.FindKey(i));
            for (int i = 1; i <= originalFaces.Extent(); ++i) {
                const TopoDS_Shape& face = originalFaces.FindKey(i);
                const Rgba* color = colors.Seek(face);
                if (color == nullptr)
                    continue;
                const TopoDS_Shape after = sewing.IsModified(face) ? sewing.Modified(face) : face;
                if (!after.IsNull() && !pieceColors.IsBound(after))
                    pieceColors.Bind(after, *color);
            }
        }
        detail::ShapeIndex sewnShells;
        TopExp::MapShapes(sewn, TopAbs_SHELL, sewnShells);
        for (int i = 1; i <= sewnShells.Extent(); ++i)
            shells.push_back(solidFromShell(TopoDS::Shell(sewnShells.FindKey(i))));
        for (TopExp_Explorer it(sewn, TopAbs_FACE, TopAbs_SHELL); it.More(); it.Next()) {
            builder.Add(leftoverFaces, it.Current());
            anyLeftover = true;
        }
    } else {
        // Without sewing, the loose shells and faces of a part stay together as one body of surfaces.
        for (int i = 1; i <= looseShells.Extent(); ++i) {
            builder.Add(leftoverFaces, looseShells.FindKey(i));
            anyLeftover = true;
        }
        for (int i = 1; i <= looseFaces.Extent(); ++i) {
            builder.Add(leftoverFaces, looseFaces.FindKey(i));
            anyLeftover = true;
        }
        pieceColors = colors;
    }

    for (const TopoDS_Shape& shell : shells) {
        PartBody body;
        body.shape = shell;
        body.faceColor = firstFaceColor(shell, pieceColors);
        part.bodies.push_back(std::move(body));
    }
    if (anyLeftover) {
        PartBody body;
        body.shape = leftoverFaces;
        body.faceColor = firstFaceColor(leftoverFaces, pieceColors);
        part.bodies.push_back(std::move(body));
    }
    return true;
}

// ---------- Units ----------

bool isImperialLengthName(const std::string& name)
{
    const std::string text = upper(name);
    for (const char* word : {"INCH", "FOOT", "FEET", "YARD", "MILE"})
        if (text.find(word) != std::string::npos)
            return true;
    return false;
}

UnitSystem stepUnits(STEPControl_Reader& reader, std::vector<std::string>& warnings)
{
    NCollection_Sequence<TCollection_AsciiString> lengths, angles, solidAngles;
    reader.FileUnits(lengths, angles, solidAngles);
    if (lengths.IsEmpty())
        return UnitSystem::Metric;
    const std::string first(lengths.First().ToCString());
    for (int i = 2; i <= lengths.Length(); ++i) {
        if (upper(lengths.Value(i).ToCString()) != upper(first)) {
            warnings.push_back("The file uses more than one length unit; it is reported as " + trimmed(first) +
                               ". Geometry is converted to millimetres either way.");
            break;
        }
    }
    return isImperialLengthName(first) ? UnitSystem::Inch : UnitSystem::Metric;
}

UnitSystem igesUnits(const occ::handle<IGESData_IGESModel>& model)
{
    if (model.IsNull())
        return UnitSystem::Metric;
    const IGESData_GlobalSection& global = model->GlobalSection();
    switch (global.UnitFlag()) {
    case 1:  // inch
    case 4:  // foot
    case 5:  // mile
    case 8:  // mil
    case 11: // microinch
        return UnitSystem::Inch;
    case 3: {
        const occ::handle<TCollection_HAsciiString> name = global.UnitName();
        if (!name.IsNull()) {
            const std::string text = upper(name->ToCString());
            if (text.find("IN") != std::string::npos || text.find("FT") != std::string::npos ||
                isImperialLengthName(text))
                return UnitSystem::Inch;
        }
        return UnitSystem::Metric;
    }
    default:
        return UnitSystem::Metric;
    }
}

// ---------- Complete-file checks ----------

bool stepTailComplete(std::string_view tail)
{
    return upper(std::string(tail)).find("END-ISO-10303-21") != std::string::npos;
}

// An IGES file ends with its Terminate section: one line with 'T' in column 73.
bool igesTailComplete(std::string_view tail)
{
    std::size_t end = tail.size();
    while (end > 0 && (tail[end - 1] == '\n' || tail[end - 1] == '\r' || tail[end - 1] == ' ' || tail[end - 1] == '\0'))
        --end;
    if (end == 0)
        return false;
    std::size_t begin = end;
    while (begin > 0 && tail[begin - 1] != '\n' && tail[begin - 1] != '\r')
        --begin;
    const std::string_view line = tail.substr(begin, end - begin);
    return line.size() >= 73 && line[72] == 'T';
}

std::string readTail(const std::filesystem::path& path, std::size_t bytes, bool& ioError)
{
    ioError = false;
    std::FILE* file = detail::openFile(path, false);
    if (file == nullptr) {
        ioError = true;
        return {};
    }
    std::string tail;
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (!ec) {
        const std::uintmax_t start = size > bytes ? size - bytes : 0;
#if defined(_WIN32)
        const bool sought = _fseeki64(file, static_cast<long long>(start), SEEK_SET) == 0;
#else
        const bool sought = fseeko(file, static_cast<off_t>(start), SEEK_SET) == 0;
#endif
        if (sought) {
            tail.resize(static_cast<std::size_t>(size - start));
            tail.resize(std::fread(tail.data(), 1, tail.size(), file));
        }
    }
    ioError = static_cast<bool>(ec) || std::ferror(file) != 0;
    std::fclose(file);
    return tail;
}

// ---------- The import ----------

ShapeProcess::OperationsFlags noShapeProcessing()
{
    return ShapeProcess::OperationsFlags();
}

struct Transferred {
    occ::handle<TDocStd_Document> doc;
    UnitSystem units = UnitSystem::Metric;
};

occ::handle<TDocStd_Document> newDocument()
{
    occ::handle<TDocStd_Document> doc = new TDocStd_Document("BinXCAF");
    XCAFDoc_DocumentTool::Set(doc->Main(), false);
    // Bring every length into millimetres (ray::kernel's unit), whatever the file uses.
    XCAFDoc_DocumentTool::SetLengthUnit(doc, 1.0, UnitsMethods_LengthUnit_Millimeter);
    return doc;
}

Result<Transferred> transferStep(IoTask& task, const std::filesystem::path& path, const ImportOptions& options,
                                 std::vector<std::string>& warnings, std::string_view where)
{
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec)
        return tl::unexpected(failure(ErrorCode::IoError, "The file cannot be read: " + detail::utf8Of(path), where));
    std::FILE* file = detail::openFile(path, false);
    if (file == nullptr)
        return tl::unexpected(failure(ErrorCode::IoError, "The file cannot be opened: " + detail::utf8Of(path), where));
    detail::InputFileBuffer buffer(file, size, task, 0.0, kReadEnd, kStageRead, kStageLoad);
    std::istream stream(&buffer);

    Transferred out;
    out.doc = newDocument();
    {
        STEPCAFControl_Reader reader;
        reader.SetColorMode(true);
        reader.SetNameMode(true);
        reader.SetLayerMode(false);
        reader.SetPropsMode(false);
        reader.SetMetaMode(false);
        reader.SetProductMetaMode(false);
        reader.SetSHUOMode(false);
        reader.SetGDTMode(false);
        reader.SetMatMode(false);
        reader.SetViewMode(false);
        DESTEP_Parameters params; // per call, never Interface_Static (PLAN.md section 7 rule 5)
        params.ReadColor = true;
        params.ReadName = true;
        params.ReadLayer = false;
        params.ReadProps = false;
        params.ReadMetadata = false;
        params.ReadProductMetadata = false;
        task.report(0.0, kStageRead);
        const IFSelect_ReturnStatus status = reader.ChangeReader().ReadStream("model.step", params, stream);
        if (buffer.stopped() || task.stopRequested())
            return tl::unexpected(failure(ErrorCode::Cancelled, "Cancelled.", where));
        if (buffer.readError())
            return tl::unexpected(
                failure(ErrorCode::IoError, "Reading the file failed: " + detail::utf8Of(path), where));
        if (size == 0)
            return tl::unexpected(failure(ErrorCode::FormatError, "The file is empty.", where));
        if (status != IFSelect_RetDone || !buffer.reachedEnd())
            return tl::unexpected(failure(ErrorCode::FormatError, "This is not a STEP file, or it is damaged.", where));
        if (!stepTailComplete(buffer.tail()))
            return tl::unexpected(failure(
                ErrorCode::FormatError, "The STEP file is incomplete: it does not end with END-ISO-10303-21.", where));
        task.report(kLoadEnd, kStageTransfer);

        out.units = stepUnits(reader.ChangeReader(), warnings);
        if (options.heal != Heal::Reader) {
            // After the read, on both readers: reading a model resets the transfer settings, and the XCAF
            // reader keeps its own copy of the flags.
            reader.SetShapeProcessFlags(noShapeProcessing());
            reader.ChangeReader().SetShapeProcessFlags(noShapeProcessing());
        }
        occ::handle<TaskIndicator> indicator = new TaskIndicator(task, kLoadEnd, kTransferEnd, kStageTransfer);
        const bool transferred = reader.Transfer(out.doc, indicator->Start());
        if (task.stopRequested())
            return tl::unexpected(failure(ErrorCode::Cancelled, "Cancelled.", where));
        if (!transferred)
            return tl::unexpected(
                failure(ErrorCode::FormatError, "No geometry could be read from the STEP file.", where));
    } // the reader and its model are released here (not cancellable: about 4 ms per MiB, spike S3)
    return out;
}

std::mutex& igesReadMutex()
{
    static std::mutex mutex;
    return mutex;
}

Result<Transferred> transferIges(IoTask& task, const std::filesystem::path& path, const ImportOptions& options,
                                 std::string_view where)
{
    bool tailError = false;
    const std::string tail = readTail(path, detail::InputFileBuffer::kTail, tailError);
    if (tailError)
        return tl::unexpected(failure(ErrorCode::IoError, "Reading the file failed: " + detail::utf8Of(path), where));
    if (tail.empty())
        return tl::unexpected(failure(ErrorCode::FormatError, "The file is empty.", where));
    if (!igesTailComplete(tail))
        return tl::unexpected(failure(ErrorCode::FormatError,
                                      "This is not an IGES file, or it is incomplete (no Terminate section).", where));

    Transferred out;
    out.doc = newDocument();
    {
        IGESCAFControl_Reader reader;
        reader.SetColorMode(true);
        reader.SetNameMode(true);
        reader.SetLayerMode(false);
        if (options.heal != Heal::Reader)
            reader.SetShapeProcessFlags(noShapeProcessing()); // IGESCAFControl_Reader is the IGES reader itself
        task.report(0.0, kStageRead);
        // OCCT 8.0.1 reads IGES only from a named file (IGESSelect_WorkLibrary has no stream reader); it opens
        // the file through OSD_OpenFile, which takes UTF-8.
        const std::string utf8 = detail::utf8Of(path);
        // OCCT's IGES parser (igesread.c) keeps its state in C globals and has no lock of its own, unlike the
        // STEP parser: two concurrent reads crashed with an access violation (about 1 run in 20 of the
        // concurrency test). One IGES read at a time in the process; waiting here keeps polling cancel.
        std::unique_lock<std::mutex> igesLock(igesReadMutex(), std::defer_lock);
        while (!igesLock.try_lock()) {
            if (task.stopRequested())
                return tl::unexpected(failure(ErrorCode::Cancelled, "Cancelled.", where));
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const IFSelect_ReturnStatus status = reader.ReadFile(utf8.c_str());
        igesLock.unlock();
        if (task.stopRequested())
            return tl::unexpected(failure(ErrorCode::Cancelled, "Cancelled.", where));
        if (status != IFSelect_RetDone)
            return tl::unexpected(
                failure(ErrorCode::FormatError, "This is not an IGES file, or it is damaged.", where));
        task.report(kLoadEnd, kStageTransfer);

        out.units = igesUnits(reader.IGESModel());
        occ::handle<TaskIndicator> indicator = new TaskIndicator(task, kLoadEnd, kTransferEnd, kStageTransfer);
        const bool transferred = reader.Transfer(out.doc, indicator->Start());
        if (task.stopRequested())
            return tl::unexpected(failure(ErrorCode::Cancelled, "Cancelled.", where));
        if (!transferred)
            return tl::unexpected(
                failure(ErrorCode::FormatError, "No geometry could be read from the IGES file.", where));
    }
    return out;
}

// Puts one instance of a part body into world coordinates. `firstUse` keeps the shapes when nothing moves.
TopoDS_Shape place(const TopoDS_Shape& shape, const TopLoc_Location& location, bool firstUse)
{
    if (location.IsIdentity()) {
        if (firstUse)
            return shape;
        BRepBuilderAPI_Copy copy(shape, true, false);
        return copy.Shape();
    }
    BRepBuilderAPI_Transform transform(shape, location.Transformation(), true);
    return transform.Shape();
}

Result<ImportResult> runImport(Format format, IoTask& task, const std::filesystem::path& path,
                               const ImportOptions& options, std::string_view where)
{
    task.report(0.0, kStageRead);
    detail::initializeDataExchange();
    if (task.stopRequested())
        return tl::unexpected(failure(ErrorCode::Cancelled, "Cancelled.", where));
    ImportResult result;
    Result<Transferred> transferred = format == Format::Step ? transferStep(task, path, options, result.warnings, where)
                                                             : transferIges(task, path, options, where);
    if (!transferred)
        return tl::unexpected(std::move(transferred.error()));
    result.sourceUnits = transferred->units;
    const occ::handle<TDocStd_Document>& doc = transferred->doc;
    const occ::handle<XCAFDoc_ShapeTool> shapeTool = XCAFDoc_DocumentTool::ShapeTool(doc->Main());

    // 3. Flatten the assembly tree.
    std::vector<Leaf> leaves;
    {
        NCollection_Sequence<TDF_Label> roots;
        shapeTool->GetFreeShapes(roots);
        std::vector<TDF_Label> ancestors;
        bool tooDeep = false;
        for (int i = 1; i <= roots.Length(); ++i)
            collectLeaves(roots.Value(i), TopLoc_Location(), ancestors, 0, leaves, tooDeep);
        if (tooDeep)
            result.warnings.push_back("The assembly is nested more than 64 levels deep; deeper levels were skipped.");
    }

    // Parts in first-use order; a part used several times is split, sewn and healed once.
    std::map<std::string, std::size_t> partIndex;
    std::vector<Part> parts;
    std::vector<std::size_t> leafPart(leaves.size());
    std::vector<TDF_Label> partLabels;
    for (std::size_t i = 0; i < leaves.size(); ++i) {
        TCollection_AsciiString entry;
        TDF_Tool::Entry(leaves[i].part, entry);
        const auto [it, added] = partIndex.emplace(std::string(entry.ToCString()), parts.size());
        if (added) {
            parts.emplace_back();
            partLabels.push_back(leaves[i].part);
        }
        leafPart[i] = it->second;
    }
    for (std::size_t p = 0; p < parts.size(); ++p) {
        if (task.stopRequested())
            return tl::unexpected(failure(ErrorCode::Cancelled, "Cancelled.", where));
        Part& part = parts[p];
        part.name = labelName(partLabels[p]);
        part.color = labelColor(partLabels[p]);
        const TopoDS_Shape shape = XCAFDoc_ShapeTool::GetShape(partLabels[p]);
        if (shape.IsNull())
            continue;
        const ColorMap colors = subShapeColors(partLabels[p]);
        const double from = kTransferEnd + (kSewEnd - kTransferEnd) * static_cast<double>(p) / parts.size();
        const double to = kTransferEnd + (kSewEnd - kTransferEnd) * static_cast<double>(p + 1) / parts.size();
        if (!splitPart(shape, options, colors, task, from, to, part))
            return tl::unexpected(failure(ErrorCode::Cancelled, "Cancelled.", where));
        if (part.skippedCurves > 0) {
            const std::string label = meaningfulName(part.name) ? "'" + part.name + "'" : std::string("a part");
            result.warnings.push_back(std::to_string(part.skippedCurves) + " free curves and points in " + label +
                                      " were not imported (only solids, shells and faces are).");
        }
    }
    task.report(kSewEnd, options.heal == Heal::Full ? kStageHeal : kStageCheck);

    // 4. Heal, one body at a time, weighted by face count for progress. 5. Check.
    double totalFaces = 0.0;
    for (Part& part : parts)
        for (PartBody& body : part.bodies) {
            body.faceCount = countFaces(body.shape);
            totalFaces += std::max(body.faceCount, 1);
        }
    double doneFaces = 0.0;
    for (Part& part : parts) {
        for (std::size_t b = 0; b < part.bodies.size(); ++b) {
            PartBody& body = part.bodies[b];
            if (task.stopRequested())
                return tl::unexpected(failure(ErrorCode::Cancelled, "Cancelled.", where));
            // Each body owns a slice of [kSewEnd, kCheckEnd] by face count: healing in its first 80%, the
            // check in the rest. One stage name for the loop, so the name does not flip once per body.
            const double weight = std::max(body.faceCount, 1);
            const double from = kSewEnd + (kCheckEnd - kSewEnd) * doneFaces / totalFaces;
            const double to = kSewEnd + (kCheckEnd - kSewEnd) * (doneFaces + weight) / totalFaces;
            const double healed = from + 0.8 * (to - from);
            const std::string_view stage = options.heal == Heal::Full ? kStageHeal : kStageCheck;
            if (options.heal == Heal::Full) {
                ShapeFix_Shape fix;
                fix.Init(body.shape);
                occ::handle<TaskIndicator> indicator = new TaskIndicator(task, from, healed, kStageHeal);
                fix.Perform(indicator->Start());
                if (task.stopRequested())
                    return tl::unexpected(failure(ErrorCode::Cancelled, "Cancelled.", where));
                const TopoDS_Shape fixedShape = fix.Shape();
                if (!fixedShape.IsNull())
                    body.shape = fixedShape;
            }
            task.report(healed, stage);
            body.valid = BRepCheck_Analyzer(body.shape).IsValid();
            doneFaces += weight;
            task.report(to, stage);
        }
    }

    // Place every instance, in leaf order.
    std::unordered_set<const void*> used; // TShapes already handed out (lookup only, never iterated)
    for (std::size_t i = 0; i < leaves.size(); ++i) {
        if (task.stopRequested())
            return tl::unexpected(failure(ErrorCode::Cancelled, "Cancelled.", where));
        const Leaf& leaf = leaves[i];
        const Part& part = parts[leafPart[i]];
        // The part's own name, else the nearest meaningful name above it (component, then assembly).
        std::string baseName = meaningfulName(part.name) ? part.name : std::string();
        std::optional<Rgba> instanceColor;
        for (auto it = leaf.ancestors.rbegin(); it != leaf.ancestors.rend(); ++it) {
            if (baseName.empty()) {
                std::string name = labelName(*it);
                if (meaningfulName(name))
                    baseName = std::move(name);
            }
            if (!instanceColor.has_value())
                instanceColor = labelColor(*it);
        }
        for (std::size_t b = 0; b < part.bodies.size(); ++b) {
            const PartBody& body = part.bodies[b];
            if (body.shape.IsNull() || body.faceCount == 0)
                continue;
            const bool firstUse = used.insert(body.shape.TShape().get()).second;
            const TopoDS_Shape placed = place(body.shape, leaf.location, firstUse);
            if (placed.IsNull())
                return tl::unexpected(failure(ErrorCode::KernelFailure, "A body could not be placed.", where));
            ImportedBody imported;
            imported.shape = detail::wrap(placed);
            if (baseName.empty())
                imported.name = "Body " + std::to_string(result.bodies.size() + 1);
            else if (part.bodies.size() > 1)
                imported.name = baseName + " (" + std::to_string(b + 1) + ")";
            else
                imported.name = baseName;
            imported.color = body.ownColor.value_or(
                instanceColor.value_or(part.color.value_or(body.faceColor.value_or(kDefaultColor))));
            if (!body.valid)
                result.warnings.push_back("Body '" + imported.name +
                                          "' is not valid geometry after import; operations on it may fail.");
            result.bodies.push_back(std::move(imported));
        }
        task.report(kCheckEnd + (1.0 - kCheckEnd) * static_cast<double>(i + 1) / leaves.size(), kStagePlace);
    }
    if (result.bodies.empty())
        return tl::unexpected(failure(ErrorCode::FormatError,
                                      "The file contains no solids, shells or faces that can be imported.", where));
    task.report(1.0, kStageDone);
    return result;
}

Result<ImportResult> importFile(Format format, const std::string& path, const ImportOptions& options, JobContext& ctx,
                                std::string_view where)
{
    if (options.heal != Heal::None && options.heal != Heal::Reader && options.heal != Heal::Full)
        return tl::unexpected(failure(ErrorCode::InvalidArgument, "Unknown healing mode.", where));
    if (options.sew && !(std::isfinite(options.sewTolerance) && options.sewTolerance > 0.0))
        return tl::unexpected(
            failure(ErrorCode::InvalidArgument, "The sewing tolerance must be a positive number.", where));
    Result<std::filesystem::path> fsPath = detail::pathFromUtf8(path, where);
    if (!fsPath)
        return tl::unexpected(std::move(fsPath.error()));
    if (auto readable = detail::checkReadable(*fsPath, where); !readable)
        return tl::unexpected(std::move(readable.error()));
    return detail::runAbandonable<ImportResult>(
        ctx, where, [format, file = std::move(*fsPath), options, at = std::string(where)](IoTask& task) {
            return detail::guarded(
                at, [&]() -> Result<ImportResult> { return runImport(format, task, file, options, at); });
        });
}

} // namespace

void detail::initializeDataExchange()
{
    static std::once_flag once;
    std::call_once(once, [] {
        {
            STEPCAFControl_Reader reader;
        }
        {
            IGESCAFControl_Reader reader;
        }
        {
            STEPControl_Writer writer;
        }
        {
            IGESControl_Writer writer("MM", 1);
        }
        {
            const occ::handle<TDocStd_Document> doc = newDocument();
            (void)XCAFDoc_DocumentTool::ShapeTool(doc->Main());
            (void)XCAFDoc_DocumentTool::ColorTool(doc->Main());
        }
        {
            ShapeFix_Shape fix;
        }
        {
            BRepBuilderAPI_Sewing sewing;
        }
        {
            BRepMesh_IncrementalMesh mesher;
        }
    });
}

Result<ImportResult> importStep(const std::string& path, const ImportOptions& options, JobContext& ctx)
{
    return importFile(Format::Step, path, options, ctx, "kernel::importStep");
}

Result<ImportResult> importIges(const std::string& path, const ImportOptions& options, JobContext& ctx)
{
    return importFile(Format::Iges, path, options, ctx, "kernel::importIges");
}

} // namespace ray::kernel
