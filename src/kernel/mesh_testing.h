// Private to src/kernel and its tests (task T-006). Not a public interface. Includes no OCCT header, so
// tests/unit/kernel can use it (PLAN.md 0.4 rule 1). Hooks into mesh.cpp for the tessellation tests.
#pragma once
#include <ray/kernel.h>

namespace ray::kernel::testing {

// Switches OCCT's InParallel for every later mesh call in the process (default: on). Tests use it to
// compare a parallel mesh with a single-threaded one. Takes effect at the next mesh call; never call it
// while another thread may be meshing and expecting a particular mode.
void setMeshInParallel(bool on) noexcept;
bool meshInParallel() noexcept;

// How many faces a mesh call at this chord tolerance lets OCCT mesh at the same time (the size of the OCCT
// thread team inside the mesh lock): at least 1, at most OCCT's pool size. See mesh.cpp.
int meshConcurrency(double chordTol);

// The number of threads (the caller included) the last mesh call could run on. 0 before the first mesh call.
int lastMeshTeam() noexcept;

// The current size of OCCT's default thread pool (the caller included). Fixed from its first use: the kernel never
// resizes it (mesh.cpp THE DEFAULT POOL).
int meshPoolThreads() noexcept;

// How many of the pool's threads the last mesh call held back to keep to its team (pool size - team).
int lastMeshHeldThreads() noexcept;

// Creates OCCT's default pool with `threads` threads if nothing in the process has used it yet, to run a test as on a
// machine with that many processors; returns the pool's size (the earlier size if it already existed; 0 on failure).
// Never resizes it.
int createMeshPool(int threads) noexcept;

// While one lives, the mesh calls this thread makes use a team of at most `cap` threads (0: no cap), whatever their
// tolerance: a team smaller than the pool on any machine (on 4 processors every tolerance from 0.2 to 0.01 mm gets
// the whole pool). Guards may nest.
class MeshTeamCap {
public:
    explicit MeshTeamCap(int cap) noexcept;
    ~MeshTeamCap();
    MeshTeamCap(const MeshTeamCap&) = delete;
    MeshTeamCap& operator=(const MeshTeamCap&) = delete;

private:
    int previous_;
};

// Under the mesh lock and the team for `chordTol`, runs `items` tasks the way BRepMesh does (OSD_Parallel::For) and
// returns how many distinct threads took part: at most the team. -1 on failure.
int threadsUnderMeshTeam(double chordTol, int items);

// True if any face of the shape holds a triangulation or any edge a polygon: shows that meshing did not
// write into the caller's shape.
bool hasMeshData(const Shape& shape);

} // namespace ray::kernel::testing
