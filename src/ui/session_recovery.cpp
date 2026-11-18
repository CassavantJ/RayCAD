#include "session_recovery.h"

#include <map>
#include <utility>

namespace ray::ui {
namespace {

QString fromUtf8(const std::string& s)
{
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

class AppRecoverySource final : public IRecoverySource {
public:
    explicit AppRecoverySource(api::IApp& app) : app_(app) {}

    std::vector<PendingRecovery> pending() override
    {
        std::vector<PendingRecovery> out;
        infos_.clear();
        for (api::RecoveryInfo& info : app_.pendingRecoveries()) {
            PendingRecovery p;
            p.id = fromUtf8(info.id);
            p.filePath = fromUtf8(info.filePath);
            p.title = fromUtf8(info.title);
            p.lastWriteMs = info.lastWriteMs;
            p.steps = static_cast<int>(info.steps);
            p.unsaved = info.unsaved;
            p.damagedTail = info.damagedTail;
            infos_[p.id] = std::move(info);
            out.push_back(std::move(p));
        }
        return out;
    }

    JobHandle<api::OpenDocument*> recover(const PendingRecovery& p) override
    {
        const api::RecoveryInfo* info = find(p.id);
        if (info == nullptr)
            return failed();
        return app_.recover(*info);
    }

    Status discard(const PendingRecovery& p) override
    {
        const api::RecoveryInfo* info = find(p.id);
        if (info == nullptr)
            return tl::unexpected<Error>(Error{ErrorCode::NotFound, "There is no such recovery journal", {}});
        Status st = app_.discardRecovery(*info);
        if (st)
            infos_.erase(p.id);
        return st;
    }

private:
    const api::RecoveryInfo* find(const QString& id) const
    {
        auto it = infos_.find(id);
        return it != infos_.end() ? &it->second : nullptr;
    }
    // An unknown id: a job that fails at once with NotFound.
    JobHandle<api::OpenDocument*> failed()
    {
        return app_.jobs().submit<api::OpenDocument*>(
            JobInfo{"Recover", JobPriority::Interactive, false}, [](JobContext&) -> Result<api::OpenDocument*> {
                return tl::unexpected<Error>(Error{ErrorCode::NotFound, "There is no such recovery journal", {}});
            });
    }

    api::IApp& app_;
    std::map<QString, api::RecoveryInfo> infos_; // from the last pending()
};

} // namespace

std::unique_ptr<IRecoverySource> makeAppRecoverySource(api::IApp& app)
{
    return std::make_unique<AppRecoverySource>(app);
}

} // namespace ray::ui
