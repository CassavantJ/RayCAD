// The action registry (<ray/api.h> IActionRegistry): schema-checked, confirmation-gated invocation as jobs.
#include "app_impl.h"
#include "schema.h"
#include "search.h"

#include <map>
#include <memory>
#include <mutex>

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
        // An ordered action waits for the last Ordered action invoked before it (which waited for the one before, and
        // so on). The chain is taken and extended under one lock, so it follows the order of the invoke() calls.
        std::unique_lock<std::mutex> chain(chainMutex_, std::defer_lock);
        JobHandle<ActionResult> before;
        if (order != ActionOrder::Free) {
            chain.lock();
            before = lastOrdered_;
        }
        JobHandle<ActionResult> job =
            app_.runJob<ActionResult>(std::move(info),
                                      [app, entry, id = std::string(id), args = std::string(argsJson), caller,
                                       before](JobContext& ctx) mutable -> Result<ActionResult> {
                                          if (!entry) {
                                              return fail(ErrorCode::NotFound, "There is no action named '" + id + "'");
                                          }
                                          if (before.valid()) {
                                              // Runs it here if it has not started yet, so waiting never needs a free
                                              // worker; its result is its caller's. A cancel of this action is seen
                                              // once it has ended.
                                              (void)before.wait();
                                              before = {};
                                          }
                                          if (auto st = ctx.cancel.check(); !st) {
                                              return tl::unexpected<Error>(st.error());
                                          }
                                          return run(*app, *entry, args, caller, ctx);
                                      });
        if (order == ActionOrder::Ordered) {
            lastOrdered_ = job;
        }
        return job;
    }

private:
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
        const ActionFn& fn = entry.fn;
        return app.bridge().call<ActionResult>(
            [&fn, &canonical, caller, &ctx]() -> Result<ActionResult> { return fn(canonical, caller, ctx); },
            ctx.cancel);
    }

    AppImpl& app_;
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<const Registered>, std::less<>> actions_;
    std::mutex chainMutex_;               // invoke(): the chain of ordered actions (ActionOrder)
    JobHandle<ActionResult> lastOrdered_; // the last Ordered action invoked
};

} // namespace

std::unique_ptr<IActionRegistry> makeActionRegistry(AppImpl& app)
{
    return std::make_unique<ActionRegistry>(app);
}

} // namespace ray::api::detail
