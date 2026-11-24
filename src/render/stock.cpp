// Tiled meshes, the simulation stock's display (task T-073). See stock.h.
#include "stock.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <utility>

namespace ray::render::detail {

namespace {

std::atomic<std::uint64_t> nextSerial{1};

Box3 emptyBox()
{
    constexpr double inf = std::numeric_limits<double>::infinity();
    return Box3{Vec3{inf, inf, inf}, Vec3{-inf, -inf, -inf}};
}

void grow(Box3& box, const Box3& other)
{
    box.min = {std::min(box.min.x, other.min.x), std::min(box.min.y, other.min.y), std::min(box.min.z, other.min.z)};
    box.max = {std::max(box.max.x, other.max.x), std::max(box.max.y, other.max.y), std::max(box.max.z, other.max.z)};
}

// Whether `inner` lies inside `outer`, with a tolerance of a millionth of the outer box's size.
bool inside(const Box3& inner, const Box3& outer)
{
    if (!(outer.min.x <= outer.max.x && outer.min.y <= outer.max.y && outer.min.z <= outer.max.z))
        return false; // an empty box (IStockModel's "nothing changed") holds nothing
    const double dx = outer.max.x - outer.min.x, dy = outer.max.y - outer.min.y, dz = outer.max.z - outer.min.z;
    const double tol = 1e-6 * std::sqrt(dx * dx + dy * dy + dz * dz) + 1e-9;
    return inner.min.x >= outer.min.x - tol && inner.min.y >= outer.min.y - tol && inner.min.z >= outer.min.z - tol &&
           inner.max.x <= outer.max.x + tol && inner.max.y <= outer.max.y + tol && inner.max.z <= outer.max.z + tol;
}

std::shared_ptr<const PieceTable> tableOf(std::vector<std::shared_ptr<const MeshPiece>> pieces)
{
    auto table = std::make_shared<PieceTable>();
    std::sort(pieces.begin(), pieces.end(), [](const auto& a, const auto& b) { return a->face() < b->face(); });
    for (const auto& p : pieces) {
        table->triangles += p->triangles();
        if (!table->bounds)
            table->bounds = p->bounds();
        else
            grow(*table->bounds, p->bounds());
    }
    table->pieces = std::move(pieces);
    return table;
}

// The non-empty ranges of `mesh` by face number (the last of several with one number), and the numbers of its
// empty ranges.
void rangesOf(const std::shared_ptr<const kernel::Mesh>& mesh, std::map<std::uint32_t, std::uint32_t>& ranges,
              std::vector<std::uint32_t>& emptied)
{
    for (std::uint32_t r = 0; r < mesh->faces.size(); ++r) {
        const kernel::FaceRange& f = mesh->faces[r];
        if (f.indexCount >= 3) {
            ranges[f.face.index] = r;
        } else {
            ranges.erase(f.face.index);
            emptied.push_back(f.face.index);
        }
    }
}

} // namespace

// ---------- MeshPiece ----------
MeshPiece::MeshPiece(std::shared_ptr<const kernel::Mesh> source, std::uint32_t range)
    : serial_(nextSerial.fetch_add(1, std::memory_order_relaxed)), source_(std::move(source)), range_(range)
{
    const kernel::FaceRange& f = source_->faces[range];
    face_ = f.face.index;
    triangles_ = f.indexCount / 3;
    const std::uint32_t* idx = source_->indices.data() + f.firstIndex;
    const std::uint32_t count = triangles_ * 3;
    std::uint32_t lo = std::numeric_limits<std::uint32_t>::max(), hi = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        lo = std::min(lo, idx[i]);
        hi = std::max(hi, idx[i]);
    }
    const float* pos = source_->positions.data();
    bounds_ = emptyBox();
    const auto add = [&](std::uint32_t v) {
        const float* p = pos + std::size_t{v} * 3;
        bounds_.min = {std::min(bounds_.min.x, double(p[0])), std::min(bounds_.min.y, double(p[1])),
                       std::min(bounds_.min.z, double(p[2]))};
        bounds_.max = {std::max(bounds_.max.x, double(p[0])), std::max(bounds_.max.y, double(p[1])),
                       std::max(bounds_.max.z, double(p[2]))};
    };
    // Dense: the range uses a block of vertices of its own (what IStockModel and the kernel's tessellation give), which
    // is uploaded as it is. Otherwise the vertices it uses are gathered.
    const std::uint64_t block = count > 0 ? std::uint64_t(hi) - lo + 1 : 0;
    dense_ = block <= 2ull * count + 16;
    if (dense_) {
        firstVertex_ = count > 0 ? lo : 0;
        vertices_ = std::uint32_t(block);
        for (std::uint32_t v = firstVertex_; v < firstVertex_ + vertices_; ++v)
            add(v);
    } else {
        std::vector<std::uint32_t> used(idx, idx + count);
        std::sort(used.begin(), used.end());
        used.erase(std::unique(used.begin(), used.end()), used.end());
        vertices_ = std::uint32_t(used.size());
        for (std::uint32_t v : used)
            add(v);
    }
}

const PackedPiece& MeshPiece::packed(const std::optional<Box3>& outerBox) const
{
    std::lock_guard lock(mutex_);
    if (!source_)
        return packed_;
    const kernel::Mesh& m = *source_;
    const kernel::FaceRange& f = m.faces[range_];
    const std::uint32_t* idx = m.indices.data() + f.firstIndex;
    const std::uint32_t count = triangles_ * 3;
    std::vector<std::uint32_t> order; // dense: empty (vertex v is firstVertex_ + v); else the source vertices used
    if (!dense_) {
        order.assign(idx, idx + count);
        std::sort(order.begin(), order.end());
        order.erase(std::unique(order.begin(), order.end()), order.end());
    }
    packed_.vertices.resize(std::size_t{vertices_} * kPieceVertexBytes);
    std::byte* out = packed_.vertices.data();
    const std::uint32_t id = face_ + 1;
    for (std::uint32_t v = 0; v < vertices_; ++v, out += kPieceVertexBytes) {
        const std::size_t src = std::size_t{dense_ ? firstVertex_ + v : order[v]} * 3;
        std::memcpy(out, &m.positions[src], 12);
        std::memcpy(out + 12, &m.normals[src], 12);
        std::memcpy(out + 24, &id, 4);
    }
    // The three parts (PackedPiece): inside, in an edge plane on the outer box, in an edge plane inside the stock.
    const float planes[2][2] = {{float(bounds_.min.x), float(bounds_.max.x)},
                                {float(bounds_.min.y), float(bounds_.max.y)}};
    double tol = 1e-6;
    if (outerBox) {
        const double dx = outerBox->max.x - outerBox->min.x, dy = outerBox->max.y - outerBox->min.y;
        tol = std::max(1e-6, 1e-6 * std::sqrt(dx * dx + dy * dy));
    }
    const auto onOuterBox = [&](int axis, double plane) {
        if (!outerBox)
            return false;
        const double lo = axis == 0 ? outerBox->min.x : outerBox->min.y;
        const double hi = axis == 0 ? outerBox->max.x : outerBox->max.y;
        return std::abs(plane - lo) <= tol || std::abs(plane - hi) <= tol;
    };
    const auto partOf = [&](const std::uint32_t* tri) {
        const float* p[3] = {&m.positions[std::size_t{tri[0]} * 3], &m.positions[std::size_t{tri[1]} * 3],
                             &m.positions[std::size_t{tri[2]} * 3]};
        for (int axis = 0; axis < 2; ++axis) {
            for (int side = 0; side < 2; ++side) {
                const float plane = planes[axis][side];
                if (p[0][axis] == plane && p[1][axis] == plane && p[2][axis] == plane)
                    return onOuterBox(axis, double(plane)) ? 1 : 2;
            }
        }
        return 0;
    };
    const auto local = [&](std::uint32_t v) {
        return dense_ ? v - firstVertex_
                      : std::uint32_t(std::lower_bound(order.begin(), order.end(), v) - order.begin());
    };
    std::vector<std::uint32_t> parts[3];
    parts[0].reserve(count);
    for (std::uint32_t t = 0; t < count; t += 3)
        parts[partOf(idx + t)].insert(parts[partOf(idx + t)].end(),
                                      {local(idx[t]), local(idx[t + 1]), local(idx[t + 2])});
    packed_.indices.clear();
    packed_.indices.reserve(count);
    for (int k = 0; k < 3; ++k) {
        packed_.partIndices[k] = std::uint32_t(parts[k].size());
        packed_.indices.insert(packed_.indices.end(), parts[k].begin(), parts[k].end());
    }
    // The third part's vertices carry the mark in the face id's high byte (face numbers stay below 2^24), for
    // mesh.vert's push.
    for (const std::uint32_t v : parts[2])
        packed_.vertices[std::size_t{v} * kPieceVertexBytes + 27] = std::byte{1};
    source_.reset(); // the source mesh is freed once every piece of it is packed
    return packed_;
}

// ---------- tables ----------
std::shared_ptr<const PieceTable> piecesOf(const std::shared_ptr<const kernel::Mesh>& mesh)
{
    return patchPieces(nullptr, mesh, std::nullopt);
}

std::shared_ptr<const PieceTable> patchPieces(const std::shared_ptr<const PieceTable>& old,
                                              const std::shared_ptr<const kernel::Mesh>& mesh,
                                              const std::optional<Box3>& dirty)
{
    std::map<std::uint32_t, std::uint32_t> ranges; // face number -> range in mesh
    std::vector<std::uint32_t> emptied;
    if (mesh)
        rangesOf(mesh, ranges, emptied);
    std::sort(emptied.begin(), emptied.end());
    std::vector<std::shared_ptr<const MeshPiece>> pieces;
    if (old) {
        pieces.reserve(old->pieces.size() + ranges.size());
        for (const auto& p : old->pieces) {
            const std::uint32_t face = p->face();
            if (ranges.count(face) > 0 || std::binary_search(emptied.begin(), emptied.end(), face))
                continue; // replaced or emptied
            if (dirty && inside(p->bounds(), *dirty))
                continue; // inside the dirty box and not brought again: gone
            pieces.push_back(p);
        }
    }
    for (const auto& [face, range] : ranges)
        pieces.push_back(std::make_shared<const MeshPiece>(mesh, range));
    return tableOf(std::move(pieces));
}

// ---------- RangeAllocator ----------
RangeAllocator::RangeAllocator(std::uint64_t capacity) : capacity_(capacity)
{
    if (capacity_ > 0)
        free_.emplace(0, capacity_);
}

std::optional<std::uint64_t> RangeAllocator::allocate(std::uint64_t size)
{
    if (size == 0)
        return std::nullopt;
    for (auto it = free_.begin(); it != free_.end(); ++it) {
        if (it->second < size)
            continue;
        const std::uint64_t offset = it->first;
        const std::uint64_t rest = it->second - size;
        free_.erase(it);
        if (rest > 0)
            free_.emplace(offset + size, rest);
        used_ += size;
        return offset;
    }
    return std::nullopt;
}

void RangeAllocator::free(std::uint64_t offset, std::uint64_t size)
{
    if (size == 0)
        return;
    used_ -= std::min(used_, size);
    std::uint64_t start = offset, end = offset + size;
    auto next = free_.lower_bound(offset);
    if (next != free_.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second == start) {
            start = prev->first;
            free_.erase(prev);
        }
    }
    if (next != free_.end() && next->first == end) {
        end = next->first + next->second;
        free_.erase(next);
    }
    free_.emplace(start, end - start);
}

std::uint64_t RangeAllocator::largestFree() const
{
    std::uint64_t best = 0;
    for (const auto& [offset, size] : free_)
        best = std::max(best, size);
    return best;
}

// ---------- stock look ----------
StockMarkSlot stockMarkSlot(Token t)
{
    switch (t) {
    case Token::SimGouge:
        return StockMarkSlot::Gouge;
    case Token::SimLeftover:
        return StockMarkSlot::Leftover;
    default:
        return StockMarkSlot::Collision;
    }
}

Token stockMarkToken(StockMarkSlot s)
{
    switch (s) {
    case StockMarkSlot::Gouge:
        return Token::SimGouge;
    case StockMarkSlot::Leftover:
        return Token::SimLeftover;
    default:
        return Token::SimCollision;
    }
}

double stockMaxPushMm(const std::optional<Box3>& bounds)
{
    if (!bounds)
        return 0;
    const double dx = bounds->max.x - bounds->min.x, dy = bounds->max.y - bounds->min.y;
    const double dz = bounds->max.z - bounds->min.z;
    const double diag = std::sqrt(dx * dx + dy * dy + dz * dz);
    return std::isfinite(diag) ? 0.1 * diag : 0.0;
}

StockUniforms stockUniforms(const StockShading& shading, const std::optional<Box3>& bounds)
{
    StockUniforms u;
    u.stock[0] = 1.0f;
    u.stock[3] = 1.0f;
    u.uncutMax[3] = float(stockMaxPushMm(bounds));
    // Tolerance of "lies on the uncut box": well above float rounding of the stored and interpolated positions, far
    // below anything a cut leaves (S6: material thinner than 1e-4 mm is dropped, cells are 0.05 mm or more).
    const std::optional<Box3>& ref = shading.uncut ? shading.uncut : bounds;
    double diag = 0;
    if (ref) {
        const double dx = ref->max.x - ref->min.x, dy = ref->max.y - ref->min.y, dz = ref->max.z - ref->min.z;
        diag = std::isfinite(dx + dy + dz) ? std::sqrt(dx * dx + dy * dy + dz * dz) : 0.0;
    }
    u.stock[2] = float(std::max(1e-3, 1e-5 * diag));
    if (ref) {
        // The stock's outer box (mesh.vert: its sides get no extra push); a colour only with an uncut box.
        const Box3& b = *ref;
        u.uncutMin[0] = float(b.min.x), u.uncutMin[1] = float(b.min.y), u.uncutMin[2] = float(b.min.z);
        u.uncutMax[0] = float(b.max.x), u.uncutMax[1] = float(b.max.y), u.uncutMax[2] = float(b.max.z);
        u.uncutMin[3] = shading.uncut ? 1.0f : 0.0f;
    }
    int n = 0;
    for (const StockMark& m : shading.marks) {
        if (n == kMaxStockMarks)
            break;
        u.markMin[n][0] = float(m.box.min.x), u.markMin[n][1] = float(m.box.min.y),
        u.markMin[n][2] = float(m.box.min.z);
        u.markMax[n][0] = float(m.box.max.x), u.markMax[n][1] = float(m.box.max.y),
        u.markMax[n][2] = float(m.box.max.z);
        u.markMin[n][3] = float(int(stockMarkSlot(m.colour)));
        ++n;
    }
    u.stock[1] = float(n);
    return u;
}

} // namespace ray::render::detail
