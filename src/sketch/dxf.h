// DXF exchange for sketches (task T-091): the full results behind sketch::importDxf and sketch::exportDxf. Private
// to src/sketch. Written from Autodesk's public DXF reference (group codes, entity layouts, the arbitrary axis
// algorithm); no DXF library is used.
//
// READING. ASCII DXF of any version that keeps the R12 layout of sections and group codes: R12 (AC1009) to 2018
// (AC1032) are the versions tested, older files (AC1003 to AC1006) and files with only an ENTITIES section are read
// the same way, and a newer $ACADVER is read with a warning. Binary DXF is refused (FormatError).
//   Geometry imported exactly:
//     LINE, POINT, CIRCLE, ARC;
//     LWPOLYLINE and 2D POLYLINE (VERTEX ... SEQEND), with bulges as arcs; 3D POLYLINE as lines;
//     SPLINE (control points): degree 1 as lines; clamped uniform knots as one spline; a periodic uniform spline as
//       one closed spline; any other knot vector as a chain of Bezier splines (exact, one sketch spline per span);
//       a rational spline whose spans are circular arcs (how many writers store arcs) as arcs;
//     ELLIPSE with axis ratio 1 as a circle or arc;
//     INSERT (and MINSERT arrays) by exploding the block, recursively, with any rotation, mirror and uniform scale;
//       a non-uniform scale keeps lines, points and splines and leaves out arcs and circles (with a warning).
//   Left out with a warning (the sketch model cannot hold them exactly, or they are not geometry): a true ellipse, a
//   rational spline that is not made of circular arcs, a spline given only by fit points, polyface and polygon
//   meshes, entities in paper space, entities not parallel to the XY plane (extrusion other than +Z or -Z), external
//   references, and every other entity type (TEXT, MTEXT, DIMENSION, HATCH, LEADER, SOLID, 3DFACE, IMAGE, ...).
//   Z coordinates are dropped (a warning says so when any were non-zero).
//   Units: $INSUNITS when it names a unit; otherwise $MEASUREMENT (0 inch, 1 millimetre); otherwise the caller's
//   assumption. Everything is converted to millimetres.
//   Layers: the sketch model has none. Entities on a layer named CONSTRUCTION (any case) become construction
//   geometry; the layer of every imported entity is listed in DxfImport::layers.
//   End points closer than kDxfMergeTolerance become one sketch point.
//   Limits (DxfLimits) bound memory and time: file size, entities produced, block nesting, block expansion and
//   spline size. A file beyond a limit fails with FormatError, or, for block expansion and spline size, loses the
//   excess with a warning. Malformed input never crashes: a damaged entity is left out with a warning, a damaged file
//   structure (a group code that is not a number, a file cut off inside a section) fails with FormatError.
//
// WRITING. R2000 (AC1015) ASCII with the tables, blocks and objects a full R2000 reader expects. Points that no curve
// uses become POINT, lines LINE, arcs ARC, circles CIRCLE, splines SPLINE (open: clamped uniform knots; closed:
// periodic uniform), and a slot its two LINEs and two ARCs. Construction geometry goes on layer CONSTRUCTION.
// Coordinates are written in the shortest form that reads back to the same double, in mm or inch.
#pragma once

#include <ray/sketch.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ray::sketch {

// mm: imported end points closer than this become one sketch point.
inline constexpr double kDxfMergeTolerance = 1e-9;
// Layer that marks construction geometry, on import (any case) and on export.
inline constexpr const char* kDxfConstructionLayer = "CONSTRUCTION";

struct DxfLimits {
    std::size_t maxFileBytes = std::size_t{256} << 20; // FormatError above
    std::size_t maxEntities = 2'000'000;               // sketch entities produced; FormatError above
    int maxBlockDepth = 16;                            // nested INSERTs deeper are left out with a warning
    std::size_t maxBlockExpansion = 1'000'000;         // DXF entities produced through INSERTs, in total
    std::size_t maxSplinePoles = 5'000;                // a larger SPLINE is left out with a warning
    double maxCoordinate = 1e9;                        // mm: an entity reaching further is left out
};

struct DxfImport {
    Sketch sketch;                     // canonical (entities sorted by id); ids derived from the file's bytes
    std::string version;               // $ACADVER, empty when the file has no header
    int insunits = -1;                 // $INSUNITS, -1 when absent
    int measurement = -1;              // $MEASUREMENT, -1 when absent
    double mmPerUnit = 1;              // the scale applied
    std::string unitSource;            // "$INSUNITS", "$MEASUREMENT" or "assumed"
    std::vector<std::string> warnings; // what was left out or changed, one line each
    std::vector<std::pair<SketchEntId, std::string>> layers; // the DXF layer of each imported curve and point
    // Imported DXF entities by type (after exploding blocks), e.g. {"LINE", 12}; and those left out.
    std::vector<std::pair<std::string, std::size_t>> imported;
    std::vector<std::pair<std::string, std::size_t>> ignored;
};

// Reads DXF text held in memory.
Result<DxfImport> readDxf(std::string_view bytes, UnitSystem assumeIfUnitless, const DxfLimits& limits = {});
// Reads a DXF file (UTF-8 path).
Result<DxfImport> importDxfFile(const std::string& utf8Path, UnitSystem assumeIfUnitless, const DxfLimits& limits = {});
// The DXF text exportDxf writes.
Result<std::string> writeDxf(const Sketch&, UnitSystem);

} // namespace ray::sketch
