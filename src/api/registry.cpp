// The action registry (<ray/api.h> IActionRegistry): schema-checked, confirmation-gated invocation as jobs.
//
// Document queues (ActionOrder): an ordered action of a document is a deferred job (IJobSystem::defer) put on that
// document's lane. One runner job per lane takes the entries in invoke() order and runs each on its own thread
// (JobHandle::runDeferred), so a lane holds at most one worker however many entries wait, and a waiting entry holds
// none: rebuilds and meshing keep the other workers. Cancelling a waiting entry completes it at once (the job system
// claims it); the runner then finds it done and goes on. An After entry (save, close) is handed to a job of its own
// when the runner reaches it, so it starts after the ordered entries before it and the ones after it do not wait.
#include "app_impl.h"
#include "schema.h"
#include "search.h"

#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <utility>

namespace ray::api::detail {
namespace {

struct Registered {
    ActionInfo info;
    ActionFn fn;
    json::Value schema;
};

std::string callerName(Caller caller)
{
    switch (caller) {
    case Caller::User:
        return "the user";
    case Caller::Script:
        return "a script";
    case Caller::Agent:
        break;
    }
    return "an agent";
}

class ActionRegistry final : public IActionRegistry {
public:
    explicit ActionRegistry(AppImpl& app) : app_(app) {}

    Status add(ActionInfo info, ActionFn fn) override
    {
        try {
            if (info.id.empty()) {
                return fail(ErrorCode::InvalidArgument, "An action needs an id");
            }
            if (!fn) {
                return fail(ErrorCode::InvalidArgument, "The action '" + info.id + "' has no function");
            }
            json::Value schema = json::Value::object(
                {{"type", json::Value::string("object")}, {"additionalProperties", json::Value::boolean(false)}});
            if (!info.argsSchemaJson.empty()) {
                auto parsed = json::parse(info.argsSchemaJson);
                if (!parsed || !parsed->isObject()) {
                    return fail(ErrorCode::InvalidArgument,
                                "The arguments schema of '" + info.id + "' is not a JSON object" +
                                    (parsed ? std::string() : ": " + parsed.error().message));
                }
                schema = std::move(*parsed);
            }
            auto entry = std::make_shared<Registered>(Registered{std::move(info), std::move(fn), std::move(schema)});
            std::lock_guard<std::mutex> lock(mutex_);
            if (actions_.count(entry->info.id) != 0) {
                return fail(ErrorCode::InvalidArgument,
                            "An action named '" + entry->info.id + "' is registered already");
            }
            const std::string id = entry->info.id;
            actions_.emplace(id, std::move(entry));
            return {};
        } catch (const std::exception& e) {
            return fail(ErrorCode::Internal, std::string("Cannot register an action: ") + e.what());
        }
    }

    std::vector<ActionInfo> all() const override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<ActionInfo> out;
        out.reserve(actions_.size());
        for (const auto& [id, entry] : actions_) {
            out.push_back(entry->info);
        }
        return out;
    }

    std::optional<ActionInfo> find(std::string_view id) const override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = actions_.find(id);
        if (it == actions_.end()) {
            return std::nullopt;
        }
        return it->second->info;
    }

    std::vector<ActionInfo> search(std::string_view query, int limit) const override
    {
        bool blank = true;
        for (const char c : query) {
            blank = blank && c == ' ';
        }
        std::vector<ActionInfo> everything = all();
        if (blank) {
            if (limit > 0 && everything.size() > static_cast<std::size_t>(limit)) {
                everything.resize(static_cast<std::size_t>(limit));
            }
            return everything;
        }
        return search::rank(query, std::move(everything), limit);
    }

    JobHandle<ActionResult> invoke(std::string_view id, std::string_view argsJson, Caller caller) override
    {
        std::shared_ptr<const Registered> entry;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (auto it = actions_.find(id); it != actions_.end()) {
                entry = it->second;
            }
        }
        JobInfo info;
        info.title = entry ? entry->info.title : std::string(id);
        info.priority = entry && entry->info.longRunning ? JobPriority::Normal : JobPriority::Interactive;
        info.userVisible = entry && entry->info.longRunning;
        AppImpl* app = &app_;
        const ActionOrder order = entry ? app_.orderOf(entry->info.id) : ActionOrder::Free;
        std::string args(argsJson);
        // Ordered actions go on their document's lane, which is fixed now (with "document" filled in when the
        // arguments leave it to the active document).
        const std::string lane = order != ActionOrder::Free ? laneOf(*entry, args) : std::string();
        std::function<Result<ActionResult>(JobContext&)> work = [app, entry, id = std::string(id),
                                                                 args = std::move(args),
                                                                 caller](JobContext& ctx) -> Result<ActionResult> {
            if (!entry) {
                return fail(ErrorCode::NotFound, "There is no action named '" + id + "'");
            }
            if (auto st = ctx.cancel.check(); !st) {
                return tl::unexpected<Error>(st.error());
            }
            return run(*app, *entry, args, caller, ctx);
        };
        if (order == ActionOrder::Free) {
            return app_.runJob<ActionResult>(std::move(info), std::move(work));
        }
        JobHandle<ActionResult> job = app_.deferJob<ActionResult>(std::move(info), std::move(work));
        enqueue(lane, job, order);
        return job;
    }

private:
    struct Queued {
        JobHandle<ActionResult> job; // deferred: runs when the lane's runner gets to it
        ActionOrder order = ActionOrder::Ordered;
    };
    // One document's queue. `runner` is the job draining it, valid while `running`.
    struct Lane {
        std::deque<Queued> waiting;
        bool running = false;
        JobHandle<std::monostate> runner;
    };

    // The document an ordered action is queued for: its "document" argument, else the active document now (then
    // written into the arguments, so the action runs on the document it was queued for). Arguments that are not a
    // JSON object, or a "document" that is not a string, are left for run() to refuse.
    std::string laneOf(const Registered& entry, std::string& args) const
    {
        bool blank = true;
        for (const char c : args) {
            blank = blank && (c == ' ' || c == '\t' || c == '\n' || c == '\r');
        }
        Result<json::Value> parsed = blank ? Result<json::Value>(json::Value::object()) : json::parse(args);
        if (!parsed || !parsed->isObject()) {
            return {};
        }
        if (const json::Value* named = parsed->find("document")) {
            if (!named->isString()) {
                return {};
            }
            if (!named->asString().empty()) {
                return named->asString();
            }
        }
        std::string active = app_.activeDocumentId();
        const json::Value* properties = entry.schema.find("properties");
        if (!active.empty() && properties != nullptr && properties->find("document") != nullptr) {
            parsed->set("document", json::Value::string(active));
            args = json::write(*parsed);
        }
        return active;
    }

    // Appends `job` to the lane and starts the lane's runner if none is running.
    void enqueue(const std::string& lane, const JobHandle<ActionResult>& job, ActionOrder order)
    {
        std::lock_guard<std::mutex> lock(lanesMutex_);
        Lane& l = lanes_[lane];
        l.waiting.push_back(Queued{job, order});
        // A runner cancelled before it ran (only the app's shutdown cancels it) never cleared `running`.
        if (l.running && !l.runner.done()) {
            return;
        }
        l.running = true;
        l.runner = app_.runJob<std::monostate>(JobInfo{"Document commands", JobPriority::Interactive, false},
                                               [this, lane](JobContext&) -> Result<std::monostate> {
                                                   drain(lane);
                                                   return std::monostate{};
                                               });
    }

    // The runner: the lane's entries one at a time, in order, on this worker, until the lane is empty. It ignores its
    // own cancellation: an entry's cancel is the entry's, and at shutdown every entry is cancelled and ends at once.
    void drain(const std::string& lane)
    {
        for (;;) {
            Queued next;
            {
                std::lock_guard<std::mutex> lock(lanesMutex_);
                auto it = lanes_.find(lane);
                if (it == lanes_.end()) {
                    return;
                }
                if (it->second.waiting.empty()) {
                    lanes_.erase(it);
                    return;
                }
                next = std::move(it->second.waiting.front());
                it->second.waiting.pop_front();
            }
            if (next.job.done()) {
                continue; // cancelled while it waited
            }
            if (next.order == ActionOrder::After) {
                // Started now, after the ordered entries before it; the ones after it do not wait for it.
                JobHandle<ActionResult> job = next.job;
                (void)app_.jobs().submit<std::monostate>(JobInfo{"Document command", JobPriority::Normal, false},
                                                         [job](JobContext&) mutable -> Result<std::monostate> {
                                                             job.runDeferred();
                                                             return std::monostate{};
                                                         });
                continue;
            }
            next.job.runDeferred();
        }
    }
    static Result<ActionResult> run(AppImpl& app, const Registered& entry, const std::string& argsJson, Caller caller,
                                    JobContext& ctx)
    {
        const ActionInfo& info = entry.info;
        bool blank = true;
        for (const char c : argsJson) {
            blank = blank && (c == ' ' || c == '\t' || c == '\n' || c == '\r');
        }
        json::Value args = json::Value::object();
        if (!blank) {
            auto parsed = json::parse(argsJson);
            if (!parsed) {
                return fail(ErrorCode::InvalidArgument,
                            info.id + ": the arguments are not valid JSON (" + parsed.error().message + ")");
            }
            args = std::move(*parsed);
        }
        if (args.isObject()) {
            schema::fillDefaults(entry.schema, args);
        }
        if (auto st = schema::validate(entry.schema, args); !st) {
            Error e = st.error();
            if (e.code == ErrorCode::InvalidArgument) {
                e.message = info.id + ": invalid arguments: " + e.message;
            }
            return tl::unexpected<Error>(std::move(e));
        }
        const std::string canonical = json::canonical(args);

        // Automation may not change toolpaths or G-code without the user's say-so (PLAN.md 0.4, differentiator 4).
        if (caller != Caller::User && info.changesToolpathsOrGcode) {
            const std::string summary = info.title + " requested by " + callerName(caller) + " with " + canonical;
            ConfirmHook hook = app.confirmHook();
            auto approved = app.bridge().call<bool>(
                [hook, &info, &canonical, &summary]() -> Result<bool> {
                    return hook ? hook(info, canonical, summary) : false;
                },
                ctx.cancel);
            if (!approved) {
                return tl::unexpected<Error>(approved.error());
            }
            if (!*approved) {
                return fail(ErrorCode::Cancelled,
                            info.id + ": the user did not confirm this change, so nothing was done");
            }
        }
        if (auto st = ctx.cancel.check(); !st) {
            return tl::unexpected<Error>(st.error());
        }
        if (info.longRunning || app.runsOnWorker(info.id)) {
            return entry.fn(canonical, caller, ctx);
        }
        // An action on the UI thread runs to its end once started (undo, redo, close, new): its start is the job's
        // point of no return, so a cancel either stops it before it changes anything or is too late.
        const ActionFn& fn = entry.fn;
        return app.bridge().call<ActionResult>(
            [&app, &fn, &canonical, caller, &ctx]() -> Result<ActionResult> {
                if (!app.pointOfNoReturn(ctx.cancel)) {
                    return fail(ErrorCode::Cancelled, "Cancelled");
                }
                return fn(canonical, caller, ctx);
            },
            ctx.cancel);
    }

    AppImpl& app_;
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<const Registered>, std::less<>> actions_;
    std::mutex lanesMutex_;
    std::map<std::string, Lane> lanes_; // the document queues (ActionOrder), by document id
};

} // namespace

std::unique_ptr<IActionRegistry> makeActionRegistry(AppImpl& app)
{
    return std::make_unique<ActionRegistry>(app);
}

} // namespace ray::api::detail
