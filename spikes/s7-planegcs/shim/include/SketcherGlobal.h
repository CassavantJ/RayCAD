// RayCAD shim for FreeCAD src/Mod/Sketcher/SketcherGlobal.h.
// PlaneGCS includes it as "../../SketcherGlobal.h"; the build adds shim/include/_up/_up
// to the include path so that relative path lands on this file.
// PlaneGCS is built as a static library, so the export macro is empty.
#pragma once
#ifndef SketcherExport
# define SketcherExport
#endif
