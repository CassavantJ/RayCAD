// History normalisation and result validation shared by the modifying operations (task T-094). See
// boolean_internal.h.
#include "boolean_internal.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepCheck_Shell.hxx>
#include <BRepCheck_Solid.hxx>
#include <BRepCheck_Status.hxx>
#include <BRepGProp.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Solid.hxx>
#include <gp_Pln.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <utility>

namespace ray::kernel::detail {

namespace {

void sortUnique(std::vector<SubId>& ids)
{
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
}

} // namespace

Error cancelledError(std::string_view where)
{
    return makeError(ErrorCode::Cancelled, "Cancelled.", where);
}

Error kernelFailure(std::string message, std::string_view where)
{
    return makeError(ErrorCode::KernelFailure, std::move(message), where);
}

ShapeHistory buildHistory(BRepBuilderAPI_MakeShape& maker, std::span<const Shape* const> inputs, const Shape& result)
{
    ShapeHistory history;
    for (std::size_t input = 0; input < inputs.size(); ++input) {
        const Shape* shape = inputs[input];
        if (shape == nullptr || shape->impl == nullptr)
            continue;
        for (std::size_t slot = 0; slot < kSubKindCount; ++slot) {
            const auto kind = static_cast<SubKind>(slot);
            const ShapeIndex& map = shape->impl->subs[slot];
            for (int j = 1; j <= map.Extent(); ++j) {
                const TopoDS_Shape& sub = map.FindKey(j);
                const SubId source{kind, static_cast<std::uint32_t>(j - 1)};
                std::vector<SubId> modified;
                for (const TopoDS_Shape& piece : maker.Modified(sub))
                    if (const auto id = subIdOf(result, piece); id && id->kind == kind)
                        modified.push_back(*id);
                sortUnique(modified);
                if (modified.empty())
                    if (const auto self = subIdOf(result, sub))
                        modified.push_back(*self);
                const int inputIndex = static_cast<int>(input);
                if (!modified.empty())
                    history.entries.push_back(
                        {HistoryEntry::Relation::Modified, inputIndex, source, std::move(modified)});
                else
                    history.entries.push_back({HistoryEntry::Relation::Deleted, inputIndex, source, {}});
                std::vector<SubId> generated;
                for (const TopoDS_Shape& made : maker.Generated(sub))
                    if (const auto id = subIdOf(result, made))
                        generated.push_back(*id);
                sortUnique(generated);
                if (!generated.empty())
                    history.entries.push_back(
                        {HistoryEntry::Relation::Generated, inputIndex, source, std::move(generated)});
            }
        }
    }
    return history;
}

Result<PrivateCopy> privateCopy(const Shape& input, bool copyGeometry, std::string_view where)
{
    PrivateCopy out;
    BRepBuilderAPI_Copy copier(occt(input), copyGeometry, /*copyMesh=*/false);
    if (!copier.IsDone() || copier.Shape().IsNull())
        return tl::unexpected(kernelFailure("The body could not be copied for the geometry kernel.", where));
    out.shape = wrap(copier.Shape());
    for (std::size_t slot = 0; slot < kSubKindCount; ++slot) {
        const auto kind = static_cast<SubKind>(slot);
        const ShapeIndex& map = input.impl->subs[slot];
        std::vector<std::uint32_t>& table = out.toInput[slot];
        table.assign(out.shape.count(kind), kNoInputIndex);
        if (table.size() != static_cast<std::size_t>(map.Extent()))
            return tl::unexpected(kernelFailure("Copying the body changed its topology.", where));
        for (int j = 1; j <= map.Extent(); ++j) {
            const auto id = subIdOf(out.shape, copier.ModifiedShape(map.FindKey(j)));
            if (!id || id->kind != kind || table[id->index] != kNoInputIndex)
                return tl::unexpected(kernelFailure("Copying the body changed its topology.", where));
            table[id->index] = static_cast<std::uint32_t>(j - 1);
            out.identity = out.identity && id->index == static_cast<std::uint32_t>(j - 1);
        }
    }
    return out;
}

std::optional<SubId> inputSubId(const PrivateCopy& copy, SubId id)
{
    if (!validKind(id.kind))
        return std::nullopt;
    const std::vector<std::uint32_t>& table = copy.toInput[kindSlot(id.kind)];
    if (id.index >= table.size() || table[id.index] == kNoInputIndex)
        return std::nullopt;
    return SubId{id.kind, table[id.index]};
}

Status remapSources(ShapeHistory& history, int input, const PrivateCopy& copy, std::string_view where)
{
    if (copy.identity)
        return std::monostate{};
    for (HistoryEntry& entry : history.entries) {
        if (entry.inputIndex != input)
            continue;
        const auto source = inputSubId(copy, entry.source);
        if (!source)
            return tl::unexpected(kernelFailure("The history names an element the body does not have.", where));
        entry.source = *source;
    }
    std::stable_sort(history.entries.begin(), history.entries.end(), [](const HistoryEntry& a, const HistoryEntry& b) {
        if (a.inputIndex != b.inputIndex)
            return a.inputIndex < b.inputIndex;
        return a.source < b.source;
    });
    return std::monostate{};
}

std::vector<SubId> orphanFaces(const ShapeHistory& history, const Shape& result)
{
    const std::uint32_t faces = result.count(SubKind::Face);
    std::vector<char> covered(faces, 0);
    for (const HistoryEntry& entry : history.entries) {
        if (entry.relation == HistoryEntry::Relation::Deleted)
            continue;
        for (const SubId& id : entry.results)
            if (id.kind == SubKind::Face && id.index < faces)
                covered[id.index] = 1;
    }
    std::vector<SubId> orphans;
    for (std::uint32_t i = 0; i < faces; ++i)
        if (covered[i] == 0)
            orphans.push_back(SubId{SubKind::Face, i});
    return orphans;
}

std::string orphanWarning(std::span<const SubId> orphans)
{
    if (orphans.empty())
        return {};
    std::string text = std::to_string(orphans.size()) +
                       (orphans.size() == 1 ? " face of the result has" : " faces of the result have") +
                       " no recorded origin (face";
    text += orphans.size() == 1 ? " " : "s ";
    for (std::size_t i = 0; i < orphans.size(); ++i) {
        if (i > 0)
            text += ", ";
        text += std::to_string(orphans[i].index);
    }
    text += "); references to them may need to be picked again after an edit.";
    return text;
}

std::vector<TopoDS_Shape> solidsOf(const TopoDS_Shape& shape)
{
    std::vector<TopoDS_Shape> solids;
    if (shape.IsNull())
        return solids;
    for (TopExp_Explorer it(shape, TopAbs_SOLID); it.More(); it.Next())
        solids.push_back(it.Current());
    return solids;
}

double solidVolume(const TopoDS_Shape& shape)
{
    double total = 0.0;
    for (const TopoDS_Shape& solid : solidsOf(shape)) {
        GProp_GProps props;
        BRepGProp::VolumeProperties(solid, props);
        total += props.Mass();
    }
    return total;
}

std::vector<SubId> sliverFaces(const Shape& shape, const std::vector<bool>& involved)
{
    std::vector<bool> sliver;
    if (shape.isNull())
        return {};
    const ShapeIndex& faces = shape.impl->subs[kindSlot(SubKind::Face)];
    sliver.assign(static_cast<std::size_t>(faces.Extent()), false);
    const auto counts = [&](std::size_t index) {
        return involved.empty() || (index < involved.size() && involved[index]);
    };
    struct PlanarFace {
        std::size_t index;
        gp_Pnt origin;
        gp_Dir normal; // outward: the plane's normal, reversed for a reversed face
        std::optional<Bnd_Box> box;
    };
    std::vector<PlanarFace> planar;
    for (int i = 1; i <= faces.Extent(); ++i) {
        const TopoDS_Face face = TopoDS::Face(faces.FindKey(i));
        const auto index = static_cast<std::size_t>(i - 1);
        // Area and perimeter only of the faces asked about (integrating every face of a large body costs as much as
        // its volume).
        if (counts(index)) {
            GProp_GProps area;
            BRepGProp::SurfaceProperties(face, area);
            GProp_GProps length;
            BRepGProp::LinearProperties(face, length);
            if (length.Mass() > 0.0 && 2.0 * area.Mass() / length.Mass() < kSliverWidth)
                sliver[index] = true;
        }
        const BRepAdaptor_Surface surface(face, false);
        if (surface.GetType() != GeomAbs_Plane)
            continue;
        const gp_Pln plane = surface.Plane();
        PlanarFace p{index, plane.Location(), plane.Axis().Direction(), std::nullopt};
        if (face.Orientation() == TopAbs_REVERSED)
            p.normal.Reverse();
        planar.push_back(std::move(p));
    }
    // Boxes only for the faces of a candidate pair.
    const auto boxOf = [&](PlanarFace& p) -> const Bnd_Box& {
        if (!p.box) {
            p.box.emplace();
            BRepBndLib::Add(faces.FindKey(static_cast<int>(p.index) + 1), *p.box, false);
            p.box->Enlarge(kSliverWidth);
        }
        return *p.box;
    };
    // Parallel within about 1.4e-6 rad: over a 100 mm face that is 1.4e-4 mm, the order of kSliverWidth.
    constexpr double kOpposite = -1.0 + 1e-12;
    for (std::size_t a = 0; a < planar.size(); ++a)
        for (std::size_t b = a + 1; b < planar.size(); ++b) {
            PlanarFace& A = planar[a];
            PlanarFace& B = planar[b];
            if ((!counts(A.index) && !counts(B.index)) || A.normal.Dot(B.normal) > kOpposite)
                continue;
            const double apart = std::abs(gp_Vec(A.origin, B.origin).Dot(gp_Vec(A.normal)));
            if (apart < kSliverWidth && !boxOf(A).IsOut(boxOf(B)))
                sliver[A.index] = sliver[B.index] = true;
        }
    std::vector<SubId> out;
    for (std::size_t i = 0; i < sliver.size(); ++i)
        if (sliver[i])
            out.push_back(SubId{SubKind::Face, static_cast<std::uint32_t>(i)});
    return out;
}

namespace {

// BRepCheck restricted to what can have changed (boolean_internal.h INCREMENTAL CHECK): the faces of `shape` that are
// not faces of `unchangedFrom`, with their sub-shapes, and every shell and solid. False on a fault; an OCCT exception
// counts as one, as BRepCheck_Analyzer records it as a failed status.
bool changedPartsValid(const TopoDS_Shape& shape, const std::vector<TopoDS_Shape>& solids,
                       std::span<const TopoDS_Shape> unchangedFrom)
{
    ShapeIndex before;
    for (const TopoDS_Shape& input : unchangedFrom)
        TopExp::MapShapes(input, TopAbs_FACE, before);
    BRep_Builder builder;
    TopoDS_Compound changed;
    builder.MakeCompound(changed);
    bool anyChanged = false;
    ShapeIndex seen;
    for (TopExp_Explorer face(shape, TopAbs_FACE); face.More(); face.Next()) {
        if (before.Contains(face.Current()) || seen.Contains(face.Current()))
            continue;
        seen.Add(face.Current());
        builder.Add(changed, face.Current());
        anyChanged = true;
    }
    if (anyChanged && !BRepCheck_Analyzer(changed).IsValid())
        return false;
    try {
        for (const TopoDS_Shape& solid : solids) {
            const occ::handle<BRepCheck_Solid> solidCheck = new BRepCheck_Solid(TopoDS::Solid(solid));
            if (solidCheck->Status().First() != BRepCheck_NoError)
                return false;
            for (TopExp_Explorer shell(solid, TopAbs_SHELL); shell.More(); shell.Next()) {
                const occ::handle<BRepCheck_Shell> shellCheck = new BRepCheck_Shell(TopoDS::Shell(shell.Current()));
                if (shellCheck->Status().First() != BRepCheck_NoError)
                    return false;
                shellCheck->InContext(solid);
                for (NCollection_List<BRepCheck_Status>::Iterator status(shellCheck->StatusOnShape(solid));
                     status.More(); status.Next())
                    if (status.Value() != BRepCheck_NoError)
                        return false;
            }
        }
    } catch (const Standard_Failure&) {
        return false;
    }
    return true;
}

} // namespace

std::atomic<std::uint64_t>& validationDisagreementCount() noexcept
{
    static std::atomic<std::uint64_t> count{0};
    return count;
}

double volumeOf(const Shape& shape)
{
    if (shape.isNull())
        return 0.0;
    const double known = shape.impl->volume.load(std::memory_order_relaxed);
    if (!std::isnan(known))
        return known;
    const double measured = solidVolume(shape.impl->shape);
    shape.impl->volume.store(measured, std::memory_order_relaxed);
    return measured;
}

void markValidated(const Shape& shape, double volume) noexcept
{
    if (shape.isNull())
        return;
    shape.impl->validated.store(true, std::memory_order_relaxed);
    if (!std::isnan(volume))
        shape.impl->volume.store(volume, std::memory_order_relaxed);
}

bool allValidated(std::span<const Shape* const> shapes) noexcept
{
    return std::all_of(shapes.begin(), shapes.end(), [](const Shape* s) {
        return s != nullptr && !s->isNull() && s->impl->validated.load(std::memory_order_relaxed);
    });
}

Result<double> validateSolids(const TopoDS_Shape& shape, std::string_view what, std::string_view where,
                              std::span<const TopoDS_Shape> unchangedFrom)
{
    const std::string subject(what);
    if (shape.IsNull())
        return tl::unexpected(kernelFailure(subject + " produced nothing.", where));
    const std::vector<TopoDS_Shape> solids = solidsOf(shape);
    if (solids.empty())
        return tl::unexpected(kernelFailure(subject + " produced no solid.", where));

    ShapeIndex allFaces;
    TopExp::MapShapes(shape, TopAbs_FACE, allFaces);
    ShapeIndex solidFaces;
    for (const TopoDS_Shape& solid : solids)
        TopExp::MapShapes(solid, TopAbs_FACE, solidFaces);
    if (allFaces.Extent() != solidFaces.Extent())
        return tl::unexpected(kernelFailure(subject + " left faces that do not belong to a solid.", where));

    // Before BRepCheck, which crashes on some shapes it should only reject (shape_check.cpp).
    if (const auto fault = structuralFault(shape))
        return tl::unexpected(kernelFailure(subject + " produced an invalid solid (" + *fault + ").", where));
    const bool valid =
        unchangedFrom.empty() ? BRepCheck_Analyzer(shape).IsValid() : changedPartsValid(shape, solids, unchangedFrom);
    if (!unchangedFrom.empty() && std::getenv("RAYCAD_KERNEL_VALIDATE_BOTH") != nullptr &&
        BRepCheck_Analyzer(shape).IsValid() != valid) {
        validationDisagreementCount().fetch_add(1, std::memory_order_relaxed);
        std::fprintf(stderr, "[kernel] %s: the incremental BRepCheck says %s, the full one the opposite\n",
                     subject.c_str(), valid ? "valid" : "invalid");
    }
    if (!valid)
        return tl::unexpected(kernelFailure(subject + " produced an invalid solid (BRepCheck).", where));

    double total = 0.0;
    for (const TopoDS_Shape& solid : solids) {
        for (TopExp_Explorer shell(solid, TopAbs_SHELL); shell.More(); shell.Next())
            if (!BRep_Tool::IsClosed(shell.Current()))
                return tl::unexpected(kernelFailure(subject + " produced a solid that is not closed.", where));
        GProp_GProps props;
        BRepGProp::VolumeProperties(solid, props);
        const double volume = props.Mass();
        if (!std::isfinite(volume) || !(volume > 0.0))
            return tl::unexpected(kernelFailure(subject + " produced a solid without volume.", where));
        total += volume;
    }
    return total;
}

TopoDS_Shape normaliseSolids(const TopoDS_Shape& shape)
{
    const std::vector<TopoDS_Shape> solids = solidsOf(shape);
    if (solids.empty())
        return {};
    if (solids.size() == 1)
        return solids.front();
    BRep_Builder builder;
    TopoDS_Compound compound;
    builder.MakeCompound(compound);
    for (const TopoDS_Shape& solid : solids)
        builder.Add(compound, solid);
    return compound;
}

} // namespace ray::kernel::detail
