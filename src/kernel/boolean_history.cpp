// History normalisation and result validation shared by the modifying operations (task T-094). See
// boolean_internal.h.
#include "boolean_internal.h"

#include <BRepBuilderAPI_Copy.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Compound.hxx>

#include <algorithm>
#include <cmath>
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

Result<double> validateSolids(const TopoDS_Shape& shape, std::string_view what, std::string_view where)
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
    if (!BRepCheck_Analyzer(shape).IsValid())
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
