// The stocks that setups whose stock comes from an earlier setup (cam::FromSetupStock) start from (task T-074), asked
// of the CAM engine on the UI thread for whoever resolves such a setup: the CAM tree's setup states and the setup
// panel's preview and WCS (ICamEngine::carriedStock: app jobs, listed with their progress in the status bar, cancelled
// by Esc like any other). The engine keeps the stocks it made by their inputs, so asking again is cheap until an
// earlier setup changes.
//
// chainReady() says when asking will not make the engine generate anything: every operation (not suppressed) of the
// earlier setups the stock comes from is up to date. The CAM tree asks only then, so a status refresh never starts
// toolpath generations of its own; the setup panel, opened by the user on such a setup, asks in any case.
#pragma once

#include <ray/api.h>
#include <ray_api/cam_setup.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace ray::ui::camui {

// Whether setup `setup` of `state` (FromSetupStock) has an earlier chain whose operations are all up to date in
// `statuses` (ICamEngine::status of that state). False for a setup that is not carried or not found.
bool chainReady(const doc::DocState& state, const SetupId& setup, const std::map<OperationId, cam::OpState>& statuses);

class CarriedStockRequest {
public:
    using Done = std::function<void(api::cam_setup::CarriedStocks stocks, std::map<SetupId, Error> errors)>;
    CarriedStockRequest() = default;
    ~CarriedStockRequest();
    CarriedStockRequest(const CarriedStockRequest&) = delete;
    CarriedStockRequest& operator=(const CarriedStockRequest&) = delete;

    // Asks the engine for `setups` of the snapshot; `done` runs on the UI thread once every one has answered (a stock
    // or an error), never after cancel(), a newer start() or the request's end. No setups: `done` at once.
    void start(cam::ICamEngine& engine, const doc::DocStatePtr& state, const doc::BuildResultPtr& build,
               std::vector<SetupId> setups, Done done);
    // Cancels the jobs still running (each is an app job; the engine stops making a stock nobody else waits for).
    void cancel();
    bool busy() const;

private:
    std::vector<JobHandle<std::shared_ptr<const cam::CarriedStock>>> jobs_;
    std::shared_ptr<std::uint64_t> serial_ = std::make_shared<std::uint64_t>(0);
};

} // namespace ray::ui::camui
