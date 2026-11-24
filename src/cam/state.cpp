// CAM state (cam.h, task T-032): the operation registry, lookups, and the input hash of an operation.
//
// The input hash is what decides staleness: an operation is UpToDate exactly when the engine holds a toolpath made
// for its current input hash. cam.h (operationInputHash) lists what goes into it. Everything is fed to one Hasher in a
// fixed order, every variable-length part preceded by its length or tag, so two different inputs never feed the same
// byte sequence. Geometry is hashed by what it is (BREP content hash of whole bodies; surface, curve and point data
// of referenced elements), never by ids, SubIds or pointer values, so an unchanged model hashes the same in every
// rebuild, after undo and redo, and after a reload.
#include "state.h"

#include "stock.h"

#include <algorithm>
#include <exception>
#include <new>
#include <set>
#include <utility>

namespace ray::cam {

// ---------- OperationRegistry ----------

void OperationRegistry::add(std::unique_ptr<IOperationType> type)
{
    try {
        if (!type) {
            ray::log(LogLevel::Error, "cam", "OperationRegistry::add: a null operation type was ignored");
            return;
        }
        std::string name(type->typeName());
        if (name.empty()) {
            ray::log(LogLevel::Error, "cam", "OperationRegistry::add: an operation type without a name was ignored");
            return;
        }
        if (types_.count(name) != 0) {
            ray::log(LogLevel::Error, "cam",
                     "OperationRegistry::add: the operation type '" + name +
                         "' is registered already; the first one is kept");
            return;
        }
        types_.emplace(std::move(name), std::move(type));
    } catch (...) {
        ray::log(LogLevel::Error, "cam", "OperationRegistry::add: out of memory");
    }
}

const IOperationType* OperationRegistry::find(std::string_view typeName) const
{
    const auto it = types_.find(typeName);
    return it == types_.end() ? nullptr : it->second.get();
}

std::vector<std::string_view> OperationRegistry::types() const
{
    std::vector<std::string_view> out;
    try {
        for (const auto& entry : types_) {
            out.push_back(entry.first);
        }
    } catch (...) {
        out.clear();
    }
    return out;
}

// ---------- Lookups ----------

namespace detail {

const Setup* setupOf(const CamState& state, const OperationId& op, std::size_t* setupIndex)
{
    for (std::size_t i = 0; i < state.setups.size(); ++i) {
        const auto& ops = state.setups[i].operations;
        if (std::find(ops.begin(), ops.end(), op) != ops.end()) {
            if (setupIndex != nullptr) {
                *setupIndex = i;
            }
            return &state.setups[i];
        }
    }
    return nullptr;
}

const Setup* findSetup(const CamState& state, const SetupId& id, std::size_t* setupIndex)
{
    for (std::size_t i = 0; i < state.setups.size(); ++i) {
        if (state.setups[i].id == id) {
            if (setupIndex != nullptr) {
                *setupIndex = i;
            }
            return &state.setups[i];
        }
    }
    return nullptr;
}

const naming::ResolverBody* bodyOf(const CamSnapshot& snapshot, const BodyId& id)
{
    const auto it = std::lower_bound(snapshot.bodies.begin(), snapshot.bodies.end(), id,
                                     [](const naming::ResolverBody& b, const BodyId& want) { return b.id < want; });
    if (it != snapshot.bodies.end() && it->id == id) {
        return &*it;
    }
    // The view promises sorted bodies; a caller that made one by hand may not have sorted it.
    for (const naming::ResolverBody& b : snapshot.bodies) {
        if (b.id == id) {
            return &b;
        }
    }
    return nullptr;
}

std::string label(const Operation& op)
{
    return op.name.empty() ? "operation " + op.id.value.toString() : "'" + op.name + "'";
}

std::string label(const Setup& setup)
{
    return setup.name.empty() ? "setup " + setup.id.value.toString() : "'" + setup.name + "'";
}

namespace {

tl::unexpected<Error> failure(ErrorCode code, std::string message)
{
    return tl::unexpected(Error{code, std::move(message), {}});
}

const char* stateWord(naming::ResolveState state)
{
    switch (state) {
    case naming::ResolveState::Exact:
        return "exact";
    case naming::ResolveState::Split:
        return "split";
    case naming::ResolveState::Merged:
        return "merged";
    case naming::ResolveState::Repaired:
        return "repaired";
    case naming::ResolveState::Broken:
        return "broken";
    }
    return "broken";
}

} // namespace

Result<naming::Resolved> resolveForCam(const naming::IResolver& resolver, const CamSnapshot& snapshot,
                                       const naming::TopoRef& ref, const std::string& what)
{
    if (bodyOf(snapshot, ref.body) == nullptr) {
        return failure(ErrorCode::ReferenceBroken,
                       what + " is on body " + ref.body.value.toString() + ", which is not built");
    }
    naming::Resolved resolved = resolver.resolve(ref);
    if (resolved.postableWithoutReview() && !resolved.elements.empty()) {
        return resolved;
    }
    std::string why;
    switch (resolved.state) {
    case naming::ResolveState::Split:
        why = " was split by a model edit; pick the geometry again";
        break;
    case naming::ResolveState::Merged:
        why = " was merged with other geometry by a model edit; pick the geometry again";
        break;
    case naming::ResolveState::Repaired:
        why = " was repaired and needs review; confirm or pick it again";
        break;
    default:
        why = " cannot be found in the model";
        break;
    }
    std::string message = what + why;
    if (!resolved.reason.empty()) {
        message += " (" + resolved.reason + ")";
    } else {
        message += std::string(" (") + stateWord(resolved.state) + ")";
    }
    return failure(ErrorCode::ReferenceBroken, std::move(message));
}

Result<GenerateItems> itemsOf(const CamState& state, const Setup& setup, const Operation& op)
{
    GenerateItems items;
    if (const auto t = state.tools.find(op.tool.tool); t != state.tools.end()) {
        items.tool = &t->second.item;
    } else {
        return failure(ErrorCode::NotFound,
                       "The tool of " + label(op) + " (" + op.tool.tool.value.toString() + ") is not in the document");
    }
    if (op.tool.holder.value != Uuid{}) {
        if (const auto h = state.holders.find(op.tool.holder); h != state.holders.end()) {
            items.holder = &h->second.item;
        } else {
            return failure(ErrorCode::NotFound, "The holder of " + label(op) + " (" + op.tool.holder.value.toString() +
                                                    ") is not in the document");
        }
    }
    if (setup.machine.value == Uuid{}) {
        return failure(ErrorCode::NotFound, "Setup " + label(setup) + " has no machine");
    }
    if (const auto m = state.machines.find(setup.machine); m != state.machines.end()) {
        items.machine = &m->second.item;
    } else {
        return failure(ErrorCode::NotFound, "The machine of setup " + label(setup) + " (" +
                                                setup.machine.value.toString() + ") is not in the document");
    }
    return items;
}

} // namespace detail

// ---------- The input hash ----------

namespace {

using detail::label;

tl::unexpected<Error> failure(ErrorCode code, std::string message)
{
    return tl::unexpected(Error{code, std::move(message), {}});
}

void put(Hasher& h, const Hash256& hash)
{
    h.bytes(std::as_bytes(std::span<const std::uint8_t>(hash.bytes.data(), hash.bytes.size())));
}
void put(Hasher& h, const Vec3& v)
{
    h.f64(v.x).f64(v.y).f64(v.z);
}
void putFlag(Hasher& h, bool b)
{
    h.i64(b ? 1 : 0);
}

// The geometry of one element, independent of its SubId: the same element in another build of the same model hashes
// the same. A face adds its boundary curves (sorted by their own hash, not by index), so a face whose outline changed
// with the same area and centroid still hashes differently. A kernel query that fails hashes its error code.
Hash256 elementHash(const kernel::Shape& shape, kernel::SubId sub);

Hash256 curveHash(const kernel::Shape& shape, kernel::SubId edge)
{
    Hasher h;
    h.str("edge");
    auto c = kernel::curveInfo(shape, edge);
    if (!c) {
        h.i64(-1).i64(static_cast<std::int64_t>(c.error().code));
        return h.finish();
    }
    h.i64(static_cast<std::int64_t>(c->type));
    put(h, c->start);
    put(h, c->end);
    put(h, c->center);
    put(h, c->axis);
    h.f64(c->radius).f64(c->length);
    putFlag(h, c->closed);
    return h.finish();
}

Hash256 elementHash(const kernel::Shape& shape, kernel::SubId sub)
{
    Hasher h;
    switch (sub.kind) {
    case kernel::SubKind::Face: {
        h.str("face");
        auto s = kernel::surfaceInfo(shape, sub);
        if (!s) {
            h.i64(-1).i64(static_cast<std::int64_t>(s.error().code));
            break;
        }
        h.i64(static_cast<std::int64_t>(s->type));
        put(h, s->origin);
        put(h, s->axis);
        h.f64(s->radius).f64(s->semiAngle).f64(s->area);
        put(h, s->centroid);
        put(h, s->bounds.min);
        put(h, s->bounds.max);
        putFlag(h, s->reversed);
        std::vector<Hash256> edges;
        for (const kernel::SubId& e : kernel::adjacent(shape, sub, kernel::SubKind::Edge)) {
            edges.push_back(curveHash(shape, e));
        }
        std::sort(edges.begin(), edges.end());
        h.i64(static_cast<std::int64_t>(edges.size()));
        for (const Hash256& e : edges) {
            put(h, e);
        }
        break;
    }
    case kernel::SubKind::Edge:
        return curveHash(shape, sub);
    case kernel::SubKind::Vertex: {
        h.str("vertex");
        auto p = kernel::vertexPoint(shape, sub);
        if (!p) {
            h.i64(-1).i64(static_cast<std::int64_t>(p.error().code));
        } else {
            put(h, *p);
        }
        break;
    }
    case kernel::SubKind::Solid:
        h.str("solid");
        h.i64(static_cast<std::int64_t>(sub.index));
        break;
    }
    return h.finish();
}

// A reference as it resolves now: its state and the geometry of each element, in element order (the resolver lists
// them in a fixed order). `partIndex` is the index of the reference's body among the setup's part bodies (-1: none).
Status putReference(Hasher& h, const CamSnapshot& snapshot, const naming::IResolver& resolver,
                    const naming::TopoRef& ref, const std::string& what, std::int64_t partIndex)
{
    auto resolved = detail::resolveForCam(resolver, snapshot, ref, what);
    if (!resolved) {
        return tl::unexpected(resolved.error());
    }
    const naming::ResolverBody* body = detail::bodyOf(snapshot, ref.body);
    h.str("ref");
    h.i64(partIndex);
    h.i64(static_cast<std::int64_t>(ref.kind));
    h.i64(static_cast<std::int64_t>(resolved->state));
    h.i64(static_cast<std::int64_t>(resolved->elements.size()));
    for (const kernel::SubId& e : resolved->elements) {
        put(h, elementHash(body->shape, e));
    }
    return {};
}

// The tool and holder by their geometry, the machine by the fields a generator reads (cam.h generatorMachine), so a
// rename, a comment or another material's cutting data does not make an operation stale.
void putTool(Hasher& h, const Embedded<lib::Tool>& e)
{
    h.str("tool");
    put(h, e.item.geometryHash());
}

void putHolder(Hasher& h, const Embedded<lib::Holder>* e)
{
    h.str("holder");
    if (e == nullptr) {
        h.i64(0);
        return;
    }
    h.i64(1);
    put(h, e->item.geometryHash());
}

void putMachine(Hasher& h, const Embedded<lib::MachineDef>& e)
{
    // The canonical JSON of the generator's view: whatever that view keeps is hashed, and nothing else.
    h.str("machine");
    h.str(lib::toJson(generatorMachine(e.item)));
}

Status putBody(Hasher& h, const CamSnapshot& snapshot, const BodyId& id, const std::string& what)
{
    const naming::ResolverBody* body = detail::bodyOf(snapshot, id);
    if (body == nullptr || body->shape.isNull()) {
        return failure(ErrorCode::ReferenceBroken, what + " (body " + id.value.toString() + ") is not built");
    }
    put(h, body->shape.contentHash());
    return {};
}

// The threads of the hole features named on the faces of part body `id` (T-035): the holes' threads are not modelled,
// so a thread edit changes no shape and only this makes the operations on the body stale. Every "hole.*" name of
// every face counts (a superset of what hole recognition reads: the first such name on a bore face), in feature id
// order. Nothing is read when the document has no tapped hole.
void putHoleThreads(Hasher& h, const CamSnapshot& snapshot, const BodyId& id)
{
    h.str("threads");
    const naming::ResolverBody* body = detail::bodyOf(snapshot, id);
    if (snapshot.holeThreads.empty() || body == nullptr) {
        h.i64(0);
        return;
    }
    std::set<FeatureId> origins;
    const std::uint32_t faces = body->shape.count(kernel::SubKind::Face);
    for (std::uint32_t f = 0; f < faces; ++f) {
        for (const Hash256& name : body->map.namesOf(kernel::SubId{kernel::SubKind::Face, f})) {
            const naming::TopoName* record = body->map.record(name);
            if (record != nullptr && record->role.rfind("hole.", 0) == 0 &&
                snapshot.holeThreads.count(record->origin) != 0) {
                origins.insert(record->origin);
            }
        }
    }
    h.i64(static_cast<std::int64_t>(origins.size()));
    for (const FeatureId& origin : origins) {
        const HoleThread& t = snapshot.holeThreads.at(origin);
        h.str(origin.value.toString()).str(t.spec).f64(t.pitch).f64(t.nominalDiameter).f64(t.depth);
    }
}

Result<Hash256> operationHash(const CamSnapshot& snapshot, const naming::IResolver& resolver, const OperationId& id,
                              int depth);
Status putSetup(Hasher& h, const CamSnapshot& snapshot, const naming::IResolver& resolver, const Setup& setup,
                std::size_t setupIndex, int depth);

// The stock left by the earlier setup a FromSetupStock setup names (cam.h operationInputHash): that setup as its
// operations consume it, every operation of it that is not suppressed, in order, and the carrying rules (T-075:
// kCarriedStockCellMm and kCarriedRulesVersion, so a change of the cell or of the slicing makes the later setup stale).
// This is CarriedStock::key's content too (carriedStockKey).
Status putCarried(Hasher& h, const CamSnapshot& snapshot, const naming::IResolver& resolver, const Setup& setup,
                  std::size_t setupIndex, int depth)
{
    const CamState& cam = *snapshot.cam;
    const FromSetupStock& from = std::get<FromSetupStock>(setup.stock);
    std::size_t previousIndex = 0;
    const Setup* previous = detail::findSetup(cam, from.previous, &previousIndex);
    if (previous == nullptr) {
        return failure(ErrorCode::NotFound, "The stock of setup " + label(setup) + " comes from setup " +
                                                from.previous.value.toString() + ", which is not in the document");
    }
    if (previousIndex >= setupIndex || depth > static_cast<int>(cam.setups.size())) {
        return failure(ErrorCode::InvalidArgument, "The stock of setup " + label(setup) +
                                                       " must come from an "
                                                       "earlier setup");
    }
    RAY_CAM_TRY(putSetup(h, snapshot, resolver, *previous, previousIndex, depth + 1));
    for (const OperationId& opId : previous->operations) {
        const auto op = cam.operations.find(opId);
        if (op == cam.operations.end() || op->second.suppressed) {
            continue;
        }
        auto opHash = operationHash(snapshot, resolver, opId, depth + 1);
        if (!opHash) {
            Error e = opHash.error();
            e.message = "The stock of setup " + label(setup) + " depends on " + label(op->second) + ": " + e.message;
            return tl::unexpected(std::move(e));
        }
        put(h, *opHash);
    }
    h.str("carried");
    h.i64(detail::kCarriedRulesVersion);
    h.f64(kCarriedStockCellMm);
    return {};
}

// Everything of a setup that the operations in it consume: part and fixture bodies, stock, WCS and machine.
Status putSetup(Hasher& h, const CamSnapshot& snapshot, const naming::IResolver& resolver, const Setup& setup,
                std::size_t setupIndex, int depth)
{
    const CamState& cam = *snapshot.cam;
    h.str("setup");
    h.i64(static_cast<std::int64_t>(setup.partBodies.size()));
    for (std::size_t i = 0; i < setup.partBodies.size(); ++i) {
        RAY_CAM_TRY(putBody(h, snapshot, setup.partBodies[i],
                            "Part body " + std::to_string(i + 1) + " of setup " + label(setup)));
        putHoleThreads(h, snapshot, setup.partBodies[i]);
    }
    h.i64(static_cast<std::int64_t>(setup.fixtureBodies.size()));
    for (std::size_t i = 0; i < setup.fixtureBodies.size(); ++i) {
        RAY_CAM_TRY(putBody(h, snapshot, setup.fixtureBodies[i],
                            "Fixture body " + std::to_string(i + 1) + " of setup " + label(setup)));
    }
    // Stock.
    h.str("stock");
    h.i64(static_cast<std::int64_t>(setup.stock.index()));
    if (const auto* box = std::get_if<BoxStock>(&setup.stock)) {
        h.f64(box->xNeg).f64(box->xPos).f64(box->yNeg).f64(box->yPos).f64(box->zNeg).f64(box->zPos);
    } else if (const auto* cyl = std::get_if<CylinderStock>(&setup.stock)) {
        h.f64(cyl->diameter).f64(cyl->length).i64(cyl->axis).f64(cyl->endOffset);
    } else if (const auto* solid = std::get_if<SolidStock>(&setup.stock)) {
        RAY_CAM_TRY(putBody(h, snapshot, solid->body, "The stock body of setup " + label(setup)));
    } else if (std::holds_alternative<FromSetupStock>(setup.stock)) {
        RAY_CAM_TRY(putCarried(h, snapshot, resolver, setup, setupIndex, depth));
    }
    // WCS.
    h.str("wcs");
    h.i64(static_cast<std::int64_t>(setup.wcs.originMode));
    h.i64(setup.wcs.originMode == Wcs::OriginMode::StockCorner ? setup.wcs.stockCornerIndex : 0);
    // The flips (T-044), only when one is set: a setup without them keeps the hash it had before they existed.
    if (setup.wcs.flipX || setup.wcs.flipZ) {
        h.str("flip");
        h.i64(setup.wcs.flipX ? 1 : 0);
        h.i64(setup.wcs.flipZ ? 1 : 0);
    }
    const std::pair<const std::optional<naming::TopoRef>*, const char*> refs[] = {
        {&setup.wcs.zFace, "The Z face"}, {&setup.wcs.xEdge, "The X edge"}, {&setup.wcs.originVertex, "The origin"}};
    for (const auto& [ref, what] : refs) {
        if (!*ref) {
            h.i64(0);
            continue;
        }
        h.i64(1);
        RAY_CAM_TRY(putReference(h, snapshot, resolver, **ref, std::string(what) + " of setup " + label(setup), -1));
    }
    // Machine.
    if (setup.machine.value == Uuid{}) {
        return failure(ErrorCode::NotFound, "Setup " + label(setup) + " has no machine");
    }
    const auto machine = cam.machines.find(setup.machine);
    if (machine == cam.machines.end()) {
        return failure(ErrorCode::NotFound, "The machine of setup " + label(setup) + " (" +
                                                setup.machine.value.toString() + ") is not in the document");
    }
    putMachine(h, machine->second);
    return {};
}

Result<Hash256> operationHash(const CamSnapshot& snapshot, const naming::IResolver& resolver, const OperationId& id,
                              int depth)
{
    if (!snapshot.cam) {
        return failure(ErrorCode::NotFound, "The document has no CAM state");
    }
    const CamState& cam = *snapshot.cam;
    const auto found = cam.operations.find(id);
    if (found == cam.operations.end()) {
        return failure(ErrorCode::NotFound, "Operation " + id.value.toString() + " is not in the document");
    }
    const Operation& op = found->second;
    std::size_t setupIndex = 0;
    const Setup* setup = detail::setupOf(cam, id, &setupIndex);
    if (setup == nullptr) {
        return failure(ErrorCode::NotFound, "Operation " + label(op) + " is in no setup");
    }

    Hasher h;
    h.str("raycad.cam.operation-input.v3"); // v3 (T-035): the hole threads of each part body
    h.str(op.type);
    h.str(op.paramsJson);
    // Tool: what it is, how far it sticks out, and what holds it (tool number and offset registers are G-code only).
    const auto tool = cam.tools.find(op.tool.tool);
    if (tool == cam.tools.end()) {
        return failure(ErrorCode::NotFound,
                       "The tool of " + label(op) + " (" + op.tool.tool.value.toString() + ") is not in the document");
    }
    putTool(h, tool->second);
    h.f64(op.tool.stickOut);
    const Embedded<lib::Holder>* holder = nullptr;
    if (op.tool.holder.value != Uuid{}) {
        const auto found_holder = cam.holders.find(op.tool.holder);
        if (found_holder == cam.holders.end()) {
            return failure(ErrorCode::NotFound, "The holder of " + label(op) + " (" + op.tool.holder.value.toString() +
                                                    ") is not in the document");
        }
        holder = &found_holder->second;
    }
    putHolder(h, holder);
    // Cutting data and heights.
    const CuttingData& c = op.cutting;
    h.str("cutting");
    h.f64(c.spindleRpm);
    putFlag(h, c.clockwise);
    h.f64(c.feedCut).f64(c.feedPlunge).f64(c.feedRamp).f64(c.feedLeadIn).f64(c.feedLeadOut);
    h.i64(static_cast<std::int64_t>(c.coolant));
    const Heights& z = op.heights;
    h.str("heights");
    h.f64(z.clearance).f64(z.retract).f64(z.feedPlane).f64(z.top).f64(z.bottom);
    // Geometry, as it resolves now.
    h.str("geometry");
    h.i64(static_cast<std::int64_t>(op.geometry.size()));
    for (std::size_t i = 0; i < op.geometry.size(); ++i) {
        const naming::TopoRef& ref = op.geometry[i];
        const auto part = std::find(setup->partBodies.begin(), setup->partBodies.end(), ref.body);
        const std::string what = "Geometry " + std::to_string(i + 1) + " of " + label(op);
        if (part == setup->partBodies.end()) {
            return failure(ErrorCode::ReferenceBroken, what + " is not on a part body of setup " + label(*setup));
        }
        RAY_CAM_TRY(putReference(h, snapshot, resolver, ref, what, part - setup->partBodies.begin()));
    }
    // The setup it runs in.
    RAY_CAM_TRY(putSetup(h, snapshot, resolver, *setup, setupIndex, depth));
    return h.finish();
}

} // namespace

Result<Hash256> operationInputHash(const CamSnapshot& snapshot, const naming::IResolver& resolver,
                                   const OperationId& op)
{
    try {
        return operationHash(snapshot, resolver, op, 0);
    } catch (const std::bad_alloc&) {
        return failure(ErrorCode::Internal, "Out of memory while hashing an operation");
    } catch (...) {
        return failure(ErrorCode::Internal, "Unexpected failure while hashing an operation");
    }
}

namespace detail {

Result<Hash256> carriedStockKey(const CamSnapshot& snapshot, const naming::IResolver& resolver, const Setup& setup)
{
    try {
        if (!snapshot.cam) {
            return failure(ErrorCode::NotFound, "The document has no CAM state");
        }
        if (!std::holds_alternative<FromSetupStock>(setup.stock)) {
            return failure(ErrorCode::InvalidArgument,
                           "The stock of setup " + label(setup) + " is not carried from an earlier setup");
        }
        std::size_t setupIndex = 0;
        if (findSetup(*snapshot.cam, setup.id, &setupIndex) == nullptr) {
            return failure(ErrorCode::NotFound, "Setup " + label(setup) + " is not in the document");
        }
        Hasher h;
        h.str("raycad.cam.carried-stock.v1");
        RAY_CAM_TRY(putCarried(h, snapshot, resolver, setup, setupIndex, 0));
        return h.finish();
    } catch (const std::bad_alloc&) {
        return failure(ErrorCode::Internal, "Out of memory while hashing a setup's stock");
    }
}

} // namespace detail

// ---------- What a generator receives ----------

lib::Tool generatorTool(const lib::Tool& t)
{
    lib::Tool out;
    out.type = t.type;
    out.flutes = t.flutes;
    out.diameter = t.diameter;
    out.shankDiameter = t.shankDiameter;
    out.fluteLength = t.fluteLength;
    out.shoulderLength = t.shoulderLength;
    out.overallLength = t.overallLength;
    out.cornerRadius = t.cornerRadius;
    out.tipAngleDeg = t.tipAngleDeg;
    out.tipDiameter = t.tipDiameter;
    out.threadPitch = t.threadPitch;
    out.neckDiameter = t.neckDiameter; // in geometryHash, so in the input hash (after M3)
    out.neckLength = t.neckLength;
    out.centreCutting = t.centreCutting; // geometryHash covers cutsAtCentre() where it is not the type's default
    return out;
}

lib::Holder generatorHolder(const lib::Holder& h)
{
    lib::Holder out;
    out.gaugeLength = h.gaugeLength;
    out.segments = h.segments;
    return out;
}

lib::MachineDef generatorMachine(const lib::MachineDef& m)
{
    lib::MachineDef out;
    out.kind = m.kind;
    out.axes = m.axes;
    out.maxRpm = m.maxRpm;
    out.minRpm = m.minRpm;
    out.rigidTapping = m.rigidTapping;
    return out;
}

namespace detail {

Result<GeneratorItems> generatorItemsOf(const CamState& state, const Setup& setup, const Operation& op)
{
    auto items = itemsOf(state, setup, op);
    if (!items) {
        return tl::unexpected(items.error());
    }
    GeneratorItems out;
    out.tool = generatorTool(*items->tool);
    if (items->holder != nullptr) {
        out.holder = generatorHolder(*items->holder);
    }
    out.machine = generatorMachine(*items->machine);
    return out;
}

} // namespace detail

// ---------- Embedded items as JSON ----------

namespace {
template <class T, class Read>
Result<Embedded<T>> embeddedFrom(std::string_view text, Read read)
{
    auto item = read(text);
    if (!item) {
        return tl::unexpected(item.error());
    }
    return Embedded<T>{std::move(*item)};
}
} // namespace

std::string embeddedJson(const Embedded<lib::Tool>& e)
{
    return lib::toJson(e.item);
}
std::string embeddedJson(const Embedded<lib::Holder>& e)
{
    return lib::toJson(e.item);
}
std::string embeddedJson(const Embedded<lib::MachineDef>& e)
{
    return lib::toJson(e.item);
}
std::string embeddedJson(const Embedded<lib::Material>& e)
{
    return lib::toJson(e.item);
}
Result<Embedded<lib::Tool>> embeddedToolFromJson(std::string_view text)
{
    return embeddedFrom<lib::Tool>(text, &lib::toolFromJson);
}
Result<Embedded<lib::Holder>> embeddedHolderFromJson(std::string_view text)
{
    return embeddedFrom<lib::Holder>(text, &lib::holderFromJson);
}
Result<Embedded<lib::MachineDef>> embeddedMachineFromJson(std::string_view text)
{
    return embeddedFrom<lib::MachineDef>(text, &lib::machineFromJson);
}
Result<Embedded<lib::Material>> embeddedMaterialFromJson(std::string_view text)
{
    return embeddedFrom<lib::Material>(text, &lib::materialFromJson);
}

} // namespace ray::cam
