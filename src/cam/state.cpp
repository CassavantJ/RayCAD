// CAM state (cam.h, task T-032): the operation registry, lookups, and the input hash of an operation.
//
// The input hash is what decides staleness: an operation is UpToDate exactly when the engine holds a toolpath made
// for its current input hash. cam.h (operationInputHash) lists what goes into it. Everything is fed to one Hasher in a
// fixed order, every variable-length part preceded by its length or tag, so two different inputs never feed the same
// byte sequence. Geometry is hashed by what it is (BREP content hash of whole bodies; surface, curve and point data
// of referenced elements), never by ids, SubIds or pointer values, so an unchanged model hashes the same in every
// rebuild, after undo and redo, and after a reload.
#include "state.h"

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

void put(Hasher& h, const Uuid& id)
{
    h.bytes(std::as_bytes(std::span<const std::uint8_t>(id.bytes.data(), id.bytes.size())));
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

void putTool(Hasher& h, const Embedded<lib::Tool>& e)
{
    const lib::Tool& t = e.item;
    h.str("tool");
    h.str(t.name).str(t.vendor).str(t.productId);
    h.i64(static_cast<std::int64_t>(t.type)).i64(t.flutes);
    h.f64(t.diameter).f64(t.shankDiameter).f64(t.fluteLength).f64(t.shoulderLength).f64(t.overallLength);
    h.f64(t.cornerRadius).f64(t.tipAngleDeg).f64(t.tipDiameter).f64(t.threadPitch);
    h.str(t.threadSpec).str(t.bodyMaterial).i64(t.defaultToolNumber);
    h.i64(static_cast<std::int64_t>(t.presets.size()));
    for (const lib::ToolPreset& p : t.presets) {
        put(h, p.material.value);
        h.str(p.opClass);
        h.f64(p.surfaceSpeedMMin)
            .f64(p.chipLoadMm)
            .f64(p.maxRadialFraction)
            .f64(p.maxAxialFraction)
            .f64(p.rampAngleDeg);
    }
    h.str(e.extraJson);
}

void putHolder(Hasher& h, const Embedded<lib::Holder>* e)
{
    h.str("holder");
    if (e == nullptr) {
        h.i64(0);
        return;
    }
    const lib::Holder& holder = e->item;
    h.i64(1);
    h.str(holder.name).str(holder.vendor).f64(holder.gaugeLength);
    h.i64(static_cast<std::int64_t>(holder.segments.size()));
    for (const lib::HolderSegment& s : holder.segments) {
        h.f64(s.height).f64(s.lowerDiameter).f64(s.upperDiameter);
    }
    h.str(e->extraJson);
}

void putMachine(Hasher& h, const Embedded<lib::MachineDef>& e)
{
    const lib::MachineDef& m = e.item;
    h.str("machine");
    h.str(m.name).str(m.vendor).str(m.model).i64(static_cast<std::int64_t>(m.kind));
    h.i64(static_cast<std::int64_t>(m.axes.size()));
    for (const lib::AxisDef& a : m.axes) {
        h.i64(a.letter).f64(a.minTravel).f64(a.maxTravel).f64(a.rapidMmMin).f64(a.maxFeedMmMin).f64(a.accelMmS2);
    }
    h.f64(m.maxRpm).f64(m.minRpm).f64(m.spindlePowerKw).f64(m.spindleTorqueNm);
    h.i64(m.toolCapacity).f64(m.toolChangeSec);
    h.str(m.controlDialect).str(m.defaultPost);
    putFlag(h, m.rigidTapping);
    putFlag(h, m.throughSpindleCoolant);
    putFlag(h, m.hasProbe);
    putFlag(h, m.hasToolSetter);
    h.i64(m.maxBlockNumber).i64(m.maxProgramNameLength);
    h.i64(static_cast<std::int64_t>(m.postPropertyOverrides.size()));
    for (const auto& [key, value] : m.postPropertyOverrides) {
        h.str(key).str(value);
    }
    h.str(e.extraJson);
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

Result<Hash256> operationHash(const CamSnapshot& snapshot, const naming::IResolver& resolver, const OperationId& id,
                              int depth);

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
    } else if (const auto* from = std::get_if<FromSetupStock>(&setup.stock)) {
        // The stock left by an earlier setup: its own stock and setup, and every operation that cuts it.
        std::size_t previousIndex = 0;
        const Setup* previous = detail::findSetup(cam, from->previous, &previousIndex);
        if (previous == nullptr) {
            return failure(ErrorCode::NotFound, "The stock of setup " + label(setup) + " comes from setup " +
                                                    from->previous.value.toString() + ", which is not in the document");
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
                e.message =
                    "The stock of setup " + label(setup) + " depends on " + label(op->second) + ": " + e.message;
                return tl::unexpected(std::move(e));
            }
            put(h, *opHash);
        }
    }
    // WCS.
    h.str("wcs");
    h.i64(static_cast<std::int64_t>(setup.wcs.originMode));
    h.i64(setup.wcs.originMode == Wcs::OriginMode::StockCorner ? setup.wcs.stockCornerIndex : 0);
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
    h.str("raycad.cam.operation-input.v1");
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

} // namespace ray::cam
