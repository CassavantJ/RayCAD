// STEP (AP214), IGES and STL export (task T-005).
//
// Every export runs on an I/O thread like the imports (io_internal.h), writes `<path>.partial` and renames it
// over `path` only when complete, so a failed or cancelled export never leaves a broken file under the
// requested name. Input Shapes are immutable and are never modified (PLAN.md section 7 rule 1): every writer gets a
// private copy (BRepBuilderAPI_Copy, geometry included). STEPControl_Writer::Transfer writes into the TShapes it is
// given (measured on OCCT 8.0.1: it sets the Modified flag of faces, so another thread reading the shape races with
// it); IGES is copied for the same reason; STL meshes its copy under the process-wide mesh lock (rule 3). A copy
// costs far less than the transfer and the write that follow it.
#include "io_occt.h"
#include "shape_impl.h"

#include <BRepBuilderAPI_Copy.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <DESTEP_Parameters.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <IGESControl_Writer.hxx>
#include <IGESData_GlobalSection.hxx>
#include <IGESData_IGESModel.hxx>
#include <IMeshTools_Parameters.hxx>
#include <Poly_Triangulation.hxx>
#include <STEPControl_StepModelType.hxx>
#include <STEPControl_Writer.hxx>
#include <StepBasic_Product.hxx>
#include <StepData_StepModel.hxx>
#include <TCollection_AsciiString.hxx>
#include <TCollection_HAsciiString.hxx>
#include <TopExp.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <UnitsMethods_LengthUnit.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ray::kernel {

namespace {

using detail::IoTask;
using detail::TaskIndicator;

constexpr std::string_view kStageTransfer = "Converting";
constexpr std::string_view kStageMesh = "Triangulating";
constexpr std::string_view kStageWrite = "Writing file";
constexpr std::string_view kStageDone = "Done";

Error failure(ErrorCode code, std::string message, std::string_view where)
{
    return detail::makeError(code, std::move(message), where);
}

Error cancelled(std::string_view where)
{
    return failure(ErrorCode::Cancelled, "Cancelled.", where);
}

// Checks the common arguments; returns copies of the shapes for the I/O thread.
Result<std::vector<Shape>> checkShapes(std::span<const Shape> shapes, std::string_view where)
{
    if (shapes.empty())
        return tl::unexpected(failure(ErrorCode::InvalidArgument, "There is nothing to export.", where));
    for (const Shape& shape : shapes)
        if (shape.isNull())
            return tl::unexpected(failure(ErrorCode::KernelFailure, "The shape is empty.", where));
    return std::vector<Shape>(shapes.begin(), shapes.end());
}

// File names in IGES and STEP headers are plain ASCII.
std::string asciiName(const std::filesystem::path& path)
{
    std::string name = detail::utf8Of(path.filename());
    for (char& c : name)
        if (static_cast<unsigned char>(c) >= 0x80 || c == '\'' || c == ',' || c == ';' || c < 0x20)
            c = '_';
    return name;
}

// ---------- STEP ----------

Status writeStep(IoTask& task, const std::filesystem::path& path, const std::vector<Shape>& shapes,
                 std::string_view where)
{
    STEPControl_Writer writer;
    DESTEP_Parameters params; // per call, never Interface_Static (PLAN.md section 7 rule 5)
    params.WriteSchema = DESTEP_Parameters::WriteMode_StepSchema_AP214IS;
    params.WriteUnit = UnitsMethods_LengthUnit_Millimeter;
    params.WriteAssembly = DESTEP_Parameters::WriteMode_Assembly_Off;
    constexpr double kTransferEnd = 0.8;
    for (std::size_t i = 0; i < shapes.size(); ++i) {
        const double from = kTransferEnd * static_cast<double>(i) / shapes.size();
        const double to = kTransferEnd * static_cast<double>(i + 1) / shapes.size();
        occ::handle<TaskIndicator> indicator = new TaskIndicator(task, from, to, kStageTransfer);
        BRepBuilderAPI_Copy copy(detail::occt(shapes[i]), /*copyGeom=*/true, /*copyMesh=*/false);
        if (!copy.IsDone())
            return tl::unexpected(failure(ErrorCode::KernelFailure,
                                          "Body " + std::to_string(i + 1) + " could not be prepared for STEP.", where));
        const IFSelect_ReturnStatus status =
            writer.Transfer(copy.Shape(), STEPControl_AsIs, params, true, indicator->Start());
        if (task.stopRequested())
            return tl::unexpected(cancelled(where));
        if (status != IFSelect_RetDone)
            return tl::unexpected(failure(ErrorCode::KernelFailure,
                                          "Body " + std::to_string(i + 1) + " could not be converted to STEP.", where));
    }
    // One PRODUCT per Transfer (assembly mode off), in transfer order. Name them "Body 1", "Body 2", ...
    // here: OCCT's WriteProductName gets a process-wide counter appended, which would make the file depend
    // on what was exported before.
    const occ::handle<StepData_StepModel> model = writer.Model();
    int productIndex = 0;
    for (int e = 1; e <= model->NbEntities(); ++e) {
        const occ::handle<StepBasic_Product> product = occ::handle<StepBasic_Product>::DownCast(model->Value(e));
        if (product.IsNull())
            continue;
        const std::string name = "Body " + std::to_string(++productIndex);
        product->SetId(new TCollection_HAsciiString(name.c_str()));
        product->SetName(new TCollection_HAsciiString(name.c_str()));
    }
    detail::OutputFile out(path);
    if (auto opened = out.open(where); !opened)
        return opened;
    task.report(kTransferEnd, kStageWrite);
    const IFSelect_ReturnStatus written = writer.WriteStream(out.stream());
    if (task.stopRequested())
        return tl::unexpected(cancelled(where));
    if (written != IFSelect_RetDone)
        return tl::unexpected(failure(ErrorCode::IoError, "Writing the file failed: " + detail::utf8Of(path), where));
    if (auto committed = out.commit(task, where); !committed)
        return committed;
    task.report(1.0, kStageDone);
    return std::monostate{};
}

// ---------- IGES ----------

Status writeIges(IoTask& task, const std::filesystem::path& path, const std::vector<Shape>& shapes,
                 std::string_view where)
{
    IGESControl_Writer writer("MM", 1); // millimetres; 1 = B-rep mode (solids as MSBO entities)
    constexpr double kTransferEnd = 0.8;
    for (std::size_t i = 0; i < shapes.size(); ++i) {
        const double from = kTransferEnd * static_cast<double>(i) / shapes.size();
        const double to = kTransferEnd * static_cast<double>(i + 1) / shapes.size();
        occ::handle<TaskIndicator> indicator = new TaskIndicator(task, from, to, kStageTransfer);
        BRepBuilderAPI_Copy copy(detail::occt(shapes[i]), /*copyGeom=*/true, /*copyMesh=*/false);
        if (!copy.IsDone())
            return tl::unexpected(failure(ErrorCode::KernelFailure,
                                          "Body " + std::to_string(i + 1) + " could not be prepared for IGES.", where));
        const bool added = writer.AddShape(copy.Shape(), indicator->Start());
        if (task.stopRequested())
            return tl::unexpected(cancelled(where));
        if (!added)
            return tl::unexpected(failure(ErrorCode::KernelFailure,
                                          "Body " + std::to_string(i + 1) + " could not be converted to IGES.", where));
    }
    writer.ComputeModel();
    // The global section names the file and the sender; never the user or the machine.
    const occ::handle<IGESData_IGESModel> model = writer.Model();
    IGESData_GlobalSection global = model->GlobalSection();
    global.SetFileName(new TCollection_HAsciiString(asciiName(path).c_str()));
    global.SetSendName(new TCollection_HAsciiString("RayCAD"));
    global.SetAuthorName(new TCollection_HAsciiString(""));
    global.SetCompanyName(new TCollection_HAsciiString(""));
    model->SetGlobalSection(global);

    detail::OutputFile out(path);
    if (auto opened = out.open(where); !opened)
        return opened;
    task.report(kTransferEnd, kStageWrite);
    const bool written = writer.Write(out.stream());
    if (task.stopRequested())
        return tl::unexpected(cancelled(where));
    if (!written)
        return tl::unexpected(failure(ErrorCode::IoError, "Writing the file failed: " + detail::utf8Of(path), where));
    if (auto committed = out.commit(task, where); !committed)
        return committed;
    task.report(1.0, kStageDone);
    return std::monostate{};
}

// ---------- STL ----------

void putU32(std::vector<char>& out, std::uint32_t value)
{
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<char>((value >> (8 * i)) & 0xFFu));
}

void putF32(std::vector<char>& out, double value)
{
    const float f = static_cast<float>(value);
    std::uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof bits);
    putU32(out, bits);
}

// The process-wide mesh lock, polling the task's stop request while another mesh call holds it.
bool lockMesh(std::unique_lock<std::mutex>& lock, IoTask& task)
{
    while (!lock.try_lock()) {
        if (task.stopRequested())
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

// Called with the process-wide mesh lock held and the pool's team set (exportStl takes both outside guarded()).
Status writeStl(IoTask& task, const std::filesystem::path& path, const std::vector<Shape>& shapes, double chordTol,
                int team, std::string_view where)
{
    constexpr double kMeshEnd = 0.85;
    constexpr double kAngle = 0.349065850398865915; // 20 degrees, the Low-tier angle (PLAN.md 6.2)
    std::vector<char> triangles;                    // 50 bytes per triangle, little-endian
    std::uint64_t count = 0;
    int facesWithoutTriangles = 0;
    {
        for (std::size_t i = 0; i < shapes.size(); ++i) {
            // A private copy: meshing writes triangulations into the faces, and Shapes are immutable.
            BRepBuilderAPI_Copy copier(detail::occt(shapes[i]), true, false);
            const TopoDS_Shape copy = copier.Shape();
            IMeshTools_Parameters params;
            // As tessellate(): Watson named explicitly (CSF_MeshAlgo must not switch to Delabella), InParallel within
            // the team that keeps the faces in flight inside the memory budget (mesh.cpp MEMORY CAP).
            params.MeshAlgo = IMeshTools_MeshAlgoType_Watson;
            params.Deflection = chordTol;
            params.Angle = kAngle;
            params.Relative = false;
            params.InParallel = team >= 2; // bit-identical to single-threaded in spike S3
            const double from = kMeshEnd * static_cast<double>(i) / shapes.size();
            const double to = kMeshEnd * static_cast<double>(i + 1) / shapes.size();
            occ::handle<TaskIndicator> indicator = new TaskIndicator(task, from, to, kStageMesh);
            BRepMesh_IncrementalMesh mesher(copy, params, indicator->Start());
            if (task.stopRequested())
                return tl::unexpected(cancelled(where));

            detail::ShapeIndex faces;
            TopExp::MapShapes(copy, TopAbs_FACE, faces);
            for (int f = 1; f <= faces.Extent(); ++f) {
                const TopoDS_Face& face = TopoDS::Face(faces.FindKey(f));
                TopLoc_Location location;
                const occ::handle<Poly_Triangulation> mesh = BRep_Tool::Triangulation(face, location);
                if (mesh.IsNull() || mesh->NbTriangles() == 0) {
                    ++facesWithoutTriangles;
                    continue;
                }
                const gp_Trsf trsf = location.Transformation();
                const bool reversed = face.Orientation() == TopAbs_REVERSED;
                for (int t = 1; t <= mesh->NbTriangles(); ++t) {
                    int n1 = 0, n2 = 0, n3 = 0;
                    mesh->Triangle(t).Get(n1, n2, n3);
                    if (reversed)
                        std::swap(n2, n3);
                    const gp_Pnt p1 = mesh->Node(n1).Transformed(trsf);
                    const gp_Pnt p2 = mesh->Node(n2).Transformed(trsf);
                    const gp_Pnt p3 = mesh->Node(n3).Transformed(trsf);
                    gp_Vec normal = gp_Vec(p1, p2).Crossed(gp_Vec(p1, p3));
                    const double length = normal.Magnitude();
                    if (length > 0.0)
                        normal /= length;
                    else
                        normal = gp_Vec(0.0, 0.0, 0.0);
                    putF32(triangles, normal.X());
                    putF32(triangles, normal.Y());
                    putF32(triangles, normal.Z());
                    for (const gp_Pnt* p : {&p1, &p2, &p3}) {
                        putF32(triangles, p->X());
                        putF32(triangles, p->Y());
                        putF32(triangles, p->Z());
                    }
                    triangles.push_back(0);
                    triangles.push_back(0);
                    ++count;
                }
            }
        }
    }
    if (facesWithoutTriangles > 0)
        return tl::unexpected(failure(ErrorCode::KernelFailure,
                                      std::to_string(facesWithoutTriangles) +
                                          " faces could not be triangulated, so no STL file was written.",
                                      where));
    if (count > std::numeric_limits<std::uint32_t>::max())
        return tl::unexpected(failure(ErrorCode::InvalidArgument, "Too many triangles for an STL file.", where));

    detail::OutputFile out(path);
    if (auto opened = out.open(where); !opened)
        return opened;
    task.report(kMeshEnd, kStageWrite);
    std::vector<char> header(80, ' ');
    constexpr std::string_view kTitle = "RayCAD binary STL, millimetres"; // must not start with "solid"
    std::copy(kTitle.begin(), kTitle.end(), header.begin());
    putU32(header, static_cast<std::uint32_t>(count));
    out.stream().write(header.data(), static_cast<std::streamsize>(header.size()));
    out.stream().write(triangles.data(), static_cast<std::streamsize>(triangles.size()));
    if (task.stopRequested())
        return tl::unexpected(cancelled(where));
    if (auto committed = out.commit(task, where); !committed)
        return committed;
    task.report(1.0, kStageDone);
    return std::monostate{};
}

// `meshChordTol` > 0: the writer meshes, so the process-wide mesh lock and the pool's team (detail::MeshTeamScope) are
// taken first, OUTSIDE guarded(), where a structured exception cannot skip their release; the writer gets the team.
template <class Writer>
Status exportWith(const std::string& path, std::span<const Shape> shapes, JobContext& ctx, std::string_view where,
                  Writer writer, double meshChordTol = 0.0)
{
    Result<std::vector<Shape>> copies = checkShapes(shapes, where);
    if (!copies)
        return tl::unexpected(std::move(copies.error()));
    Result<std::filesystem::path> fsPath = detail::pathFromUtf8(path, where);
    if (!fsPath)
        return tl::unexpected(std::move(fsPath.error()));
    return detail::runAbandonable<std::monostate>(
        ctx, where,
        [file = std::move(*fsPath), owned = std::move(*copies), at = std::string(where), writer,
         meshChordTol](IoTask& task) -> Status {
            std::unique_lock<std::mutex> lock(detail::processMeshMutex(), std::defer_lock);
            std::optional<detail::MeshTeamScope> team;
            if (meshChordTol > 0.0) {
                if (!lockMesh(lock, task))
                    return tl::unexpected(cancelled(at));
                team.emplace(meshChordTol);
            }
            return detail::guarded(at, [&]() -> Status {
                task.report(0.0, kStageTransfer);
                detail::initializeDataExchange();
                return writer(task, file, owned, team ? team->team() : 1, at);
            });
        });
}

} // namespace

Status exportStep(const std::string& path, std::span<const Shape> shapes, JobContext& ctx)
{
    return exportWith(path, shapes, ctx, "kernel::exportStep",
                      [](IoTask& task, const std::filesystem::path& file, const std::vector<Shape>& owned, int,
                         std::string_view at) { return writeStep(task, file, owned, at); });
}

Status exportIges(const std::string& path, std::span<const Shape> shapes, JobContext& ctx)
{
    return exportWith(path, shapes, ctx, "kernel::exportIges",
                      [](IoTask& task, const std::filesystem::path& file, const std::vector<Shape>& owned, int,
                         std::string_view at) { return writeIges(task, file, owned, at); });
}

Status exportStl(const std::string& path, std::span<const Shape> shapes, double chordTol, JobContext& ctx)
{
    constexpr std::string_view where = "kernel::exportStl";
    if (!(std::isfinite(chordTol) && chordTol > 0.0))
        return tl::unexpected(
            failure(ErrorCode::InvalidArgument, "The chord tolerance must be a positive number.", where));
    return exportWith(
        path, shapes, ctx, where,
        [chordTol](IoTask& task, const std::filesystem::path& file, const std::vector<Shape>& owned, int team,
                   std::string_view at) { return writeStl(task, file, owned, chordTol, team, at); },
        chordTol);
}

} // namespace ray::kernel
