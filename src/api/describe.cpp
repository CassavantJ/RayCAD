// describeDocumentJson and describeCamJson (<ray/api.h>): read-only summaries for the CLI, scripts and agents.
#include "json.h"

#include <ray/api.h>

#include <exception>
#include <set>

namespace ray::api {
namespace {

using json::Value;

const char* unitsName(UnitSystem u)
{
    return u == UnitSystem::Inch ? "inch" : "metric";
}

const char* kindName(doc::Parameter::Kind k)
{
    switch (k) {
    case doc::Parameter::Kind::Length:
        return "length";
    case doc::Parameter::Kind::Angle:
        return "angle";
    case doc::Parameter::Kind::Count:
        return "count";
    case doc::Parameter::Kind::Unitless:
        break;
    }
    return "unitless";
}

const char* roleName(doc::Body::Role r)
{
    switch (r) {
    case doc::Body::Role::Part:
        return "part";
    case doc::Body::Role::Fixture:
        return "fixture";
    case doc::Body::Role::StockModel:
        break;
    }
    return "stock_model";
}

const char* statusName(doc::FeatureStatus s)
{
    switch (s) {
    case doc::FeatureStatus::Ok:
        return "ok";
    case doc::FeatureStatus::Warning:
        return "warning";
    case doc::FeatureStatus::Failed:
        return "failed";
    case doc::FeatureStatus::Suppressed:
        return "suppressed";
    case doc::FeatureStatus::NotBuilt:
        break;
    }
    return "notBuilt";
}

Value builtBody(const doc::BuiltBody& built)
{
    Value out = Value::object();
    const bool has = !built.shape.isNull();
    out.set("built", Value::boolean(has));
    if (has) {
        out.set("contentHash", Value::string(built.shape.contentHash().hex()));
        out.set("solids", Value::integer(built.shape.count(kernel::SubKind::Solid)));
        out.set("faces", Value::integer(built.shape.count(kernel::SubKind::Face)));
        out.set("edges", Value::integer(built.shape.count(kernel::SubKind::Edge)));
        out.set("vertices", Value::integer(built.shape.count(kernel::SubKind::Vertex)));
    }
    return out;
}

Value colorOf(const std::array<float, 4>& c)
{
    Value out = Value::array();
    for (const float f : c) {
        out.push(Value::number(static_cast<double>(f)));
    }
    return out;
}

} // namespace

std::string describeDocumentJson(const doc::DocState& state, const doc::BuildResult& build)
{
    try {
        Value root = Value::object();
        root.set("stateHash", Value::string(state.stateHash().hex()));

        Value settings = Value::object();
        settings.set("units", Value::string(unitsName(state.settings.units)));
        settings.set("decimals", Value::integer(state.settings.decimals));
        root.set("settings", std::move(settings));

        auto values = doc::evaluateParameters(state.parameters);
        Value params = Value::array();
        for (const doc::Parameter& p : state.parameters) {
            Value item = Value::object();
            item.set("id", Value::string(p.id.value.toString()));
            item.set("name", Value::string(p.name));
            item.set("expression", Value::string(p.expression));
            item.set("kind", Value::string(kindName(p.kind)));
            if (values) {
                if (auto it = values->find(p.name); it != values->end()) {
                    item.set("value", Value::number(it->second));
                }
            }
            params.push(std::move(item));
        }
        root.set("parameters", std::move(params));
        if (!values) {
            root.set("parameterError", Value::string(values.error().message));
        }

        Value features = Value::array();
        for (const doc::Feature& f : state.features) {
            Value item = Value::object();
            item.set("id", Value::string(f.id.value.toString()));
            item.set("type", Value::string(f.type));
            item.set("name", Value::string(f.name));
            item.set("suppressed", Value::boolean(f.suppressed));
            const doc::FeatureReport* report = nullptr;
            for (const doc::FeatureReport& r : build.reports) {
                if (r.id == f.id) {
                    report = &r;
                    break;
                }
            }
            if (report) {
                item.set("status", Value::string(statusName(report->status)));
                if (!report->message.empty()) {
                    item.set("message", Value::string(report->message));
                }
            } else {
                item.set("status", Value::string("pending"));
            }
            features.push(std::move(item));
        }
        root.set("features", std::move(features));

        Value bodies = Value::array();
        std::set<BodyId> listed;
        for (const doc::Body& b : state.bodies) {
            Value item = Value::object();
            item.set("id", Value::string(b.id.value.toString()));
            item.set("name", Value::string(b.name));
            item.set("role", Value::string(roleName(b.role)));
            item.set("visible", Value::boolean(b.visible));
            item.set("color", colorOf(b.color));
            if (auto it = build.bodies.find(b.id); it != build.bodies.end()) {
                const Value built = builtBody(it->second);
                for (const auto& [k, v] : built.asObject()) {
                    item.set(k, v);
                }
            } else {
                item.set("built", Value::boolean(false));
            }
            listed.insert(b.id);
            bodies.push(std::move(item));
        }
        // Bodies the build made that the state does not list (import features without a "bodies" list).
        for (const auto& [id, built] : build.bodies) {
            if (listed.count(id) != 0) {
                continue;
            }
            Value item = builtBody(built);
            item.set("id", Value::string(id.value.toString()));
            item.set("inState", Value::boolean(false));
            bodies.push(std::move(item));
        }
        root.set("bodies", std::move(bodies));
        return json::canonical(root);
    } catch (const std::exception& e) {
        return json::canonical(Value::object({{"error", Value::string(e.what())}}));
    }
}

std::string describeCamJson(const doc::DocState&, const std::map<OperationId, cam::OpState>& ops)
{
    try {
        Value list = Value::array();
        for (const auto& [id, op] : ops) {
            Value item = Value::object();
            item.set("id", Value::string(id.value.toString()));
            const char* status = "upToDate";
            switch (op.status) {
            case cam::OpStatus::UpToDate:
                break;
            case cam::OpStatus::Stale:
                status = "stale";
                break;
            case cam::OpStatus::Generating:
                status = "generating";
                break;
            case cam::OpStatus::Failed:
                status = "failed";
                break;
            case cam::OpStatus::ReferenceBroken:
                status = "referenceBroken";
                break;
            case cam::OpStatus::Suppressed:
                status = "suppressed";
                break;
            }
            item.set("status", Value::string(status));
            if (!op.message.empty()) {
                item.set("message", Value::string(op.message));
            }
            item.set("hasToolpath", Value::boolean(op.toolpath != nullptr));
            list.push(std::move(item));
        }
        return json::canonical(Value::object({{"operations", std::move(list)}}));
    } catch (const std::exception& e) {
        return json::canonical(Value::object({{"error", Value::string(e.what())}}));
    }
}

} // namespace ray::api
