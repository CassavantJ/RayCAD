// In-memory content-addressed objects: ObjectPool and the object chain (<ray/document.h>).
#include <ray/document.h>

#include <exception>
#include <map>
#include <mutex>
#include <new>
#include <utility>

namespace ray::doc {
namespace {

tl::unexpected<Error> notFound(const Hash256& hash)
{
    return tl::unexpected(Error{ErrorCode::NotFound, "The object " + hash.hex() + " is not stored", {}});
}

class Pool final : public ObjectPool {
public:
    Result<Hash256> put(std::span<const std::byte> bytes) override
    {
        try {
            Hasher hasher;
            hasher.bytes(bytes);
            const Hash256 hash = hasher.finish();
            std::lock_guard<std::mutex> lock(mutex_);
            if (objects_.count(hash) == 0) {
                objects_.emplace(hash, std::make_shared<const std::vector<std::byte>>(bytes.begin(), bytes.end()));
            }
            return hash;
        } catch (...) {
            return tl::unexpected(Error{ErrorCode::Internal, "Out of memory while storing an object", {}});
        }
    }

    bool has(const Hash256& hash) const override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return objects_.count(hash) != 0;
    }

    std::vector<Hash256> hashes() const override
    {
        std::vector<Hash256> out;
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& entry : objects_) {
                out.push_back(entry.first);
            }
        } catch (...) {
            out.clear();
        }
        return out;
    }

    Result<std::vector<std::byte>> object(const Hash256& hash) const override
    {
        try {
            std::shared_ptr<const std::vector<std::byte>> bytes;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                const auto it = objects_.find(hash);
                if (it == objects_.end()) {
                    return notFound(hash);
                }
                bytes = it->second;
            }
            return *bytes; // copied outside the lock
        } catch (...) {
            return tl::unexpected(Error{ErrorCode::Internal, "Out of memory while reading an object", {}});
        }
    }

private:
    mutable std::mutex mutex_;
    std::map<Hash256, std::shared_ptr<const std::vector<std::byte>>> objects_;
};

class Chain final : public IObjectSource {
public:
    explicit Chain(std::vector<std::shared_ptr<const IObjectSource>> sources) : sources_(std::move(sources)) {}

    Result<std::vector<std::byte>> object(const Hash256& hash) const override
    {
        for (const auto& source : sources_) {
            if (!source) {
                continue;
            }
            auto found = source->object(hash);
            if (found || found.error().code != ErrorCode::NotFound) {
                return found;
            }
        }
        return notFound(hash);
    }

private:
    std::vector<std::shared_ptr<const IObjectSource>> sources_;
};

} // namespace

std::shared_ptr<ObjectPool> makeObjectPool()
{
    try {
        return std::make_shared<Pool>();
    } catch (...) {
        return nullptr;
    }
}

std::shared_ptr<IObjectSource> makeObjectChain(std::vector<std::shared_ptr<const IObjectSource>> sources)
{
    try {
        return std::make_shared<Chain>(std::move(sources));
    } catch (...) {
        return nullptr;
    }
}

} // namespace ray::doc
