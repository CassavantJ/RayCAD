#include "sim_carried.h"

#include <algorithm>
#include <set>
#include <utility>
#include <variant>

namespace ray::ui::camui {

namespace {

const cam::Setup* findSetup(const doc::DocState& state, const SetupId& id)
{
    if (!state.cam)
        return nullptr;
    const auto it = std::find_if(state.cam->setups.begin(), state.cam->setups.end(),
                                 [&](const cam::Setup& s) { return s.id == id; });
    return it == state.cam->setups.end() ? nullptr : &*it;
}

} // namespace

bool chainReady(const doc::DocState& state, const SetupId& setup, const std::map<OperationId, cam::OpState>& statuses)
{
    const cam::Setup* s = findSetup(state, setup);
    std::set<SetupId> seen;
    while (s != nullptr) {
        const auto* from = std::get_if<cam::FromSetupStock>(&s->stock);
        if (from == nullptr)
            return seen.size() > 0; // reached a setup with stock of its own
        if (!seen.insert(s->id).second)
            return false; // a loop: never ready
        const cam::Setup* previous = findSetup(state, from->previous);
        if (previous == nullptr)
            return false;
        for (const OperationId& id : previous->operations) {
            const auto op = state.cam->operations.find(id);
            if (op == state.cam->operations.end() || op->second.suppressed)
                continue;
            const auto st = statuses.find(id);
            if (st == statuses.end() || st->second.status != cam::OpStatus::UpToDate || !st->second.toolpath)
                return false;
        }
        s = previous;
    }
    return false;
}

CarriedStockRequest::~CarriedStockRequest()
{
    cancel();
}

void CarriedStockRequest::cancel()
{
    ++*serial_;
    for (auto& job : jobs_)
        (void)job.cancel();
    jobs_.clear();
}

bool CarriedStockRequest::busy() const
{
    return std::any_of(jobs_.begin(), jobs_.end(), [](const auto& j) { return !j.done(); });
}

void CarriedStockRequest::start(cam::ICamEngine& engine, const doc::DocStatePtr& state,
                                const doc::BuildResultPtr& build, std::vector<SetupId> setups, Done done)
{
    cancel();
    const std::uint64_t serial = *serial_;
    if (setups.empty()) {
        if (done)
            done({}, {});
        return;
    }
    struct Pending {
        api::cam_setup::CarriedStocks stocks;
        std::map<SetupId, Error> errors;
        std::size_t left = 0;
        Done done;
    };
    auto pending = std::make_shared<Pending>();
    pending->left = setups.size();
    pending->done = std::move(done);
    std::shared_ptr<std::uint64_t> current = serial_;
    for (const SetupId& id : setups) {
        JobHandle<std::shared_ptr<const cam::CarriedStock>> job = engine.carriedStock(state, build, id);
        if (!job.valid()) {
            pending->errors[id] = Error{ErrorCode::InvalidArgument, "The CAM engine cannot simulate a setup", {}};
            --pending->left;
            continue;
        }
        jobs_.push_back(job);
        job.then([pending, current, serial, id](Result<std::shared_ptr<const cam::CarriedStock>> r) {
            if (*current != serial)
                return; // cancelled or superseded
            if (r && *r)
                pending->stocks[id] = *r;
            else
                pending->errors[id] = r ? Error{ErrorCode::Internal, "No stock", {}} : r.error();
            if (--pending->left == 0 && pending->done) {
                Done d = std::move(pending->done);
                d(std::move(pending->stocks), std::move(pending->errors));
            }
        });
    }
    if (pending->left == 0 && pending->done) {
        Done d = std::move(pending->done);
        d(std::move(pending->stocks), std::move(pending->errors));
    }
}

} // namespace ray::ui::camui
