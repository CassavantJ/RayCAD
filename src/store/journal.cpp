// The recovery journal (store.h IRecoveryJournal, PLAN.md 4.2). Layout and checks: journal.h.
//
// Appending a step costs one small record and one flush, whatever the size of the document: the stateHash() of the
// state after the step (which serialises the whole document) is computed on the journal's own thread and written as
// a separate check record, without a flush of its own (the next step's flush carries it).
#include "journal.h"

#include "file.h"
#include "journal_io.h"
#include "objects.h"

#include <ray/document.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <system_error>
#include <thread>

namespace ray::store {
namespace detail {
namespace {

constexpr char kMagic[8] = {'R', 'A', 'Y', 'C', 'J', 'R', 'N', 'L'};
constexpr std::uint32_t kJournalVersion = 1;
constexpr std::size_t kFrameBytes = 16;
constexpr std::uint64_t kMaxPayload = 1ull << 30;
constexpr std::string_view kSuffix = ".journal";

// Payload kinds. Execute: the command. Undo, Redo: nothing more. Snapshot: the state, its hash first. Object: a
// source object, Zstandard-compressed. Check: the sequence number of an earlier step and the state hash after it.
enum class Tag : std::uint8_t { Execute = 0, Undo = 1, Redo = 2, Snapshot = 3, Object = 4, Check = 5 };

#define RAY_JTRY(expr)                                                                                                 \
    do {                                                                                                               \
        if (auto ray_jtry_status_ = (expr); !ray_jtry_status_) {                                                       \
            return tl::unexpected<Error>(std::move(ray_jtry_status_.error()));                                         \
        }                                                                                                              \
    } while (false)

std::mutex gDirMutex;
std::string gDirOverride;
std::atomic<bool> gChecking{true};

Error internalError(const char* what)
{
    return makeError(ErrorCode::Internal, "Unexpected failure in the recovery data", what);
}

// ---------- Little-endian encoding ----------

class Out {
public:
    void u8(std::uint8_t v) { bytes_.push_back(static_cast<char>(v)); }
    void u32(std::uint32_t v)
    {
        for (int i = 0; i < 4; ++i) {
            bytes_.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
        }
    }
    void u64(std::uint64_t v)
    {
        for (int i = 0; i < 8; ++i) {
            bytes_.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
        }
    }
    void hash(const Hash256& h) { bytes_.append(reinterpret_cast<const char*>(h.bytes.data()), h.bytes.size()); }
    void str(std::string_view s)
    {
        u32(static_cast<std::uint32_t>(s.size()));
        bytes_.append(s);
    }
    std::string take() { return std::move(bytes_); }

private:
    std::string bytes_;
};

class In {
public:
    explicit In(std::string_view bytes) : bytes_(bytes) {}
    bool ok() const { return ok_; }
    bool done() const { return pos_ == bytes_.size(); }
    std::uint8_t u8() { return need(1) ? static_cast<std::uint8_t>(bytes_[pos_++]) : 0; }
    std::uint32_t u32()
    {
        std::uint32_t v = 0;
        if (need(4)) {
            for (int i = 0; i < 4; ++i) {
                v |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes_[pos_++])) << (8 * i);
            }
        }
        return v;
    }
    std::uint64_t u64()
    {
        std::uint64_t v = 0;
        if (need(8)) {
            for (int i = 0; i < 8; ++i) {
                v |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes_[pos_++])) << (8 * i);
            }
        }
        return v;
    }
    Hash256 hash()
    {
        Hash256 h;
        if (need(32)) {
            std::memcpy(h.bytes.data(), bytes_.data() + pos_, 32);
            pos_ += 32;
        }
        return h;
    }
    std::string_view str()
    {
        const std::uint32_t n = u32();
        if (!need(n)) {
            return {};
        }
        const std::string_view out = bytes_.substr(pos_, n);
        pos_ += n;
        return out;
    }

private:
    bool need(std::size_t n)
    {
        if (!ok_ || bytes_.size() - pos_ < n) {
            ok_ = false;
            return false;
        }
        return true;
    }
    std::string_view bytes_;
    std::size_t pos_ = 0;
    bool ok_ = true;
};

std::uint64_t checksum(std::uint64_t generation, std::uint32_t seq, std::string_view payload)
{
    Hasher hasher;
    hasher.i64(static_cast<std::int64_t>(generation));
    hasher.i64(seq);
    hasher.str(payload);
    const Hash256 digest = hasher.finish();
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(digest.bytes[static_cast<std::size_t>(i)]) << (8 * i);
    }
    return v;
}

void appendFrame(std::string& out, std::uint64_t generation, std::uint32_t seq, std::string_view payload)
{
    Out frame;
    frame.u32(static_cast<std::uint32_t>(payload.size()));
    frame.u32(seq);
    frame.u64(checksum(generation, seq, payload));
    out += frame.take();
    out += payload;
}

std::uint64_t randomGeneration()
{
    const Uuid id = Uuid::generate();
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v = (v << 8) | id.bytes[static_cast<std::size_t>(i)];
    }
    return v;
}

// ---------- Header and records ----------

struct Header {
    std::uint64_t generation = 0;
    std::string fileId, filePath, branch;
    std::optional<Hash256> base;
    std::int64_t createdMs = 0;
};

std::string encodeHeader(const Header& h)
{
    Out out;
    out.u32(kJournalVersion);
    out.u64(h.generation);
    out.str(h.fileId);
    out.str(h.filePath);
    out.str(h.branch);
    out.u8(h.base ? 1 : 0);
    out.hash(h.base.value_or(Hash256{}));
    out.u64(static_cast<std::uint64_t>(h.createdMs));
    return out.take();
}

std::optional<Header> decodeHeader(std::string_view payload)
{
    In in(payload);
    Header h;
    if (in.u32() != kJournalVersion) {
        return std::nullopt; // another journal format: never replayed
    }
    h.generation = in.u64();
    h.fileId = std::string(in.str());
    h.filePath = std::string(in.str());
    h.branch = std::string(in.str());
    const std::uint8_t hasBase = in.u8();
    const Hash256 base = in.hash();
    h.createdMs = static_cast<std::int64_t>(in.u64());
    if (!in.ok() || !in.done() || hasBase > 1 || h.fileId.empty() || h.branch.empty()) {
        return std::nullopt;
    }
    if (hasBase) {
        h.base = base;
    }
    return h;
}

std::string encodeStep(Tag tag, std::string_view command = {})
{
    Out out;
    out.u8(static_cast<std::uint8_t>(tag));
    if (tag == Tag::Execute) {
        out.str(command);
    }
    return out.take();
}

std::string encodeCheck(std::uint32_t stepSeq, const Hash256& hash)
{
    Out out;
    out.u8(static_cast<std::uint8_t>(Tag::Check));
    out.u32(stepSeq);
    out.hash(hash);
    return out.take();
}

// A snapshot record, and the state's hash (stateHash() is the hash of the manifest, so it costs nothing extra).
struct Snapshot {
    std::string payload;
    Hash256 hash;
};
Snapshot encodeSnapshot(const doc::DocState& state)
{
    const doc::StateObjects objects = doc::serializeState(state);
    Snapshot out;
    out.hash = hashObject(std::as_bytes(std::span<const char>(objects.manifest.data(), objects.manifest.size())));
    Out payload;
    payload.u8(static_cast<std::uint8_t>(Tag::Snapshot));
    payload.hash(out.hash);
    payload.str(objects.manifest);
    payload.u32(static_cast<std::uint32_t>(objects.features.size()));
    for (const auto& [hash, bytes] : objects.features) {
        payload.hash(hash);
        payload.str(bytes);
    }
    out.payload = payload.take();
    return out;
}

Result<std::string> encodeObject(const Hash256& hash, std::span<const std::byte> raw)
{
    auto packed = compressObject(raw);
    if (!packed) {
        return tl::unexpected<Error>(packed.error());
    }
    Out out;
    out.u8(static_cast<std::uint8_t>(Tag::Object));
    out.hash(hash);
    out.u64(static_cast<std::uint64_t>(raw.size()));
    out.str(std::string_view(reinterpret_cast<const char*>(packed->data()), packed->size()));
    return out.take();
}

struct Parsed {
    Header header;
    std::vector<JournalRecord> records;
    std::vector<std::uint32_t> recordSeqs; // the sequence number of each record, ascending
    std::vector<std::pair<Hash256, std::vector<std::byte>>> objects;
    std::vector<std::uint64_t> ends; // offset after each intact record, the header first
    std::uint32_t nextSeq = 1;
    bool damagedTail = false;
};

// Decodes one record's payload into `out`; false if it does not decode completely (then it counts as damaged).
bool decodeRecord(std::string_view payload, std::uint32_t seq, Parsed& out, bool loadObjects)
{
    In in(payload);
    const auto tag = static_cast<Tag>(in.u8());
    if (!in.ok()) {
        return false;
    }
    JournalRecord record;
    switch (tag) {
    case Tag::Execute: {
        const std::string_view json = in.str();
        if (!in.ok() || !in.done()) {
            return false;
        }
        auto command = doc::commandFromJson(json);
        if (!command) {
            return false;
        }
        record.kind = JournalRecord::Kind::Execute;
        record.command = std::move(*command);
        break;
    }
    case Tag::Undo:
    case Tag::Redo:
        if (!in.done()) {
            return false;
        }
        record.kind = tag == Tag::Undo ? JournalRecord::Kind::Undo : JournalRecord::Kind::Redo;
        break;
    case Tag::Snapshot: {
        const Hash256 after = in.hash();
        const std::string_view manifest = in.str();
        const std::uint32_t count = in.u32();
        std::map<Hash256, std::string_view> features;
        for (std::uint32_t i = 0; i < count && in.ok(); ++i) {
            const Hash256 hash = in.hash();
            features[hash] = in.str();
        }
        if (!in.ok() || !in.done()) {
            return false;
        }
        auto state = doc::deserializeState(manifest, [&features](const Hash256& hash) -> Result<std::string> {
            const auto found = features.find(hash);
            if (found == features.end()) {
                return fail(ErrorCode::NotFound, "A feature of a recovered snapshot is missing", hash.hex());
            }
            return std::string(found->second);
        });
        if (!state || state->stateHash() != after) {
            return false;
        }
        record.kind = JournalRecord::Kind::Snapshot;
        record.snapshot = std::make_shared<const doc::DocState>(std::move(*state));
        record.stateAfter = after;
        break;
    }
    case Tag::Object: {
        const Hash256 hash = in.hash();
        const std::uint64_t rawSize = in.u64();
        const std::string_view packed = in.str();
        if (!in.ok() || !in.done() || rawSize > kMaxObjectBytes) {
            return false;
        }
        if (loadObjects) {
            auto raw = decodeObject(
                std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(packed.data()), packed.size()),
                static_cast<std::int64_t>(rawSize), hash);
            if (!raw) {
                return false;
            }
            out.objects.emplace_back(hash, std::move(*raw));
        }
        return true;
    }
    case Tag::Check: {
        const std::uint32_t stepSeq = in.u32();
        const Hash256 hash = in.hash();
        if (!in.ok() || !in.done()) {
            return false;
        }
        const auto at = std::lower_bound(out.recordSeqs.begin(), out.recordSeqs.end(), stepSeq);
        if (at == out.recordSeqs.end() || *at != stepSeq) {
            return false; // names no step before it
        }
        JournalRecord& step = out.records[static_cast<std::size_t>(at - out.recordSeqs.begin())];
        if (step.stateAfter && *step.stateAfter != hash) {
            return false;
        }
        step.stateAfter = hash;
        return true;
    }
    default:
        return false;
    }
    out.records.push_back(std::move(record));
    out.recordSeqs.push_back(seq);
    return true;
}

// nullopt if the journal has no intact header (not a journal, another format, or torn while it was being written).
std::optional<Parsed> parseJournal(std::string_view bytes, bool loadObjects)
{
    if (bytes.size() < sizeof kMagic || std::memcmp(bytes.data(), kMagic, sizeof kMagic) != 0) {
        return std::nullopt;
    }
    Parsed out;
    std::uint64_t pos = sizeof kMagic;
    for (std::uint32_t seq = 0;; ++seq) {
        if (bytes.size() - pos < kFrameBytes) {
            out.damagedTail = pos != bytes.size();
            break;
        }
        In frame(bytes.substr(static_cast<std::size_t>(pos), kFrameBytes));
        const std::uint32_t length = frame.u32();
        const std::uint32_t storedSeq = frame.u32();
        const std::uint64_t check = frame.u64();
        if (length > kMaxPayload || bytes.size() - pos - kFrameBytes < length || storedSeq != seq) {
            out.damagedTail = true;
            break;
        }
        const std::string_view payload = bytes.substr(static_cast<std::size_t>(pos + kFrameBytes), length);
        const std::uint64_t generation = seq == 0 ? 0 : out.header.generation;
        if (checksum(generation, seq, payload) != check) {
            out.damagedTail = true;
            break;
        }
        if (seq == 0) {
            auto header = decodeHeader(payload);
            if (!header) {
                return std::nullopt;
            }
            out.header = std::move(*header);
        } else if (!decodeRecord(payload, seq, out, loadObjects)) {
            out.damagedTail = true;
            break;
        }
        pos += kFrameBytes + length;
        out.ends.push_back(pos);
        out.nextSeq = seq + 1;
    }
    if (out.ends.empty()) {
        return std::nullopt;
    }
    return out;
}

std::string newJournal(const Header& header)
{
    std::string content(kMagic, sizeof kMagic);
    appendFrame(content, 0, 0, encodeHeader(header));
    return content;
}

doc::DocStatePtr emptyState()
{
    return std::make_shared<const doc::DocState>();
}

// The undo and redo depth a document replayed from the base would have after the records so far.
struct Depth {
    std::size_t undo = 0, redo = 0;
    void apply(JournalRecord::Kind kind)
    {
        switch (kind) {
        case JournalRecord::Kind::Execute:
            ++undo;
            redo = 0;
            break;
        case JournalRecord::Kind::Undo:
            undo -= undo > 0 ? 1 : 0;
            ++redo;
            break;
        case JournalRecord::Kind::Redo:
            redo -= redo > 0 ? 1 : 0;
            ++undo;
            break;
        case JournalRecord::Kind::Snapshot:
            undo = redo = 0;
            break;
        }
    }
};

// ---------- The open journal ----------

class JournalImpl final : public IRecoveryJournal {
public:
    JournalImpl(std::unique_ptr<jio::ExclusiveFile> file, Header header, std::uint64_t end, std::uint32_t nextSeq,
                doc::DocStatePtr latest, std::optional<Hash256> latestHash, Depth depth)
        : file_(std::move(file)), header_(std::move(header)), end_(end), nextSeq_(nextSeq), latest_(std::move(latest)),
          latestHash_(latestHash), depth_(depth)
    {
        checker_ = std::thread([this] { checkLoop(); });
    }

    ~JournalImpl() override
    {
        {
            std::lock_guard<std::mutex> guard(mutex_);
            stop_ = true;
        }
        wake_.notify_all();
        if (checker_.joinable()) {
            checker_.join();
        }
    }

    Status append(const doc::Command& command, const doc::DocStatePtr& after) override
    {
        try {
            if (!after) {
                return fail(ErrorCode::InvalidArgument, "No state given to the recovery journal");
            }
            auto json = doc::commandToJson(command);
            if (!json) {
                return tl::unexpected<Error>(json.error());
            }
            std::lock_guard<std::mutex> guard(mutex_);
            std::uint32_t seq = 0;
            RAY_JTRY(writeRecord(encodeStep(Tag::Execute, *json), true, &seq));
            depth_.apply(JournalRecord::Kind::Execute);
            stepped(seq, after, std::nullopt);
            return {};
        } catch (const std::exception& e) {
            return tl::unexpected<Error>(internalError(e.what()));
        }
    }

    Status appendUndo(const doc::DocStatePtr& after) override { return appendStep(true, after); }
    Status appendRedo(const doc::DocStatePtr& after) override { return appendStep(false, after); }

    Status appendObject(const Hash256& hash, std::span<const std::byte> bytes) override
    {
        try {
            if (hashObject(bytes) != hash) {
                return fail(ErrorCode::InvalidArgument, "An object does not match its hash", hash.hex());
            }
            auto payload = encodeObject(hash, bytes);
            if (!payload) {
                return tl::unexpected<Error>(payload.error());
            }
            std::lock_guard<std::mutex> guard(mutex_);
            return writeRecord(*payload, true, nullptr);
        } catch (const std::exception& e) {
            return tl::unexpected<Error>(internalError(e.what()));
        }
    }

    Status markSaved(const Commit& saved) override
    {
        try {
            std::lock_guard<std::mutex> guard(mutex_);
            RAY_JTRY(usable());
            if (!latestHash_) {
                latestHash_ = latest_->stateHash();
            }
            Header header = header_;
            header.generation = randomGeneration();
            header.base = saved.id;
            std::string content = newJournal(header);
            std::uint32_t seq = 1;
            std::uint32_t snapshotSeq = 0;
            if (*latestHash_ != saved.root) {
                // Steps were appended while the save ran: keep their result, and every object the journal holds.
                auto bytes = file_->readAll();
                if (!bytes) {
                    return failWrite(bytes.error());
                }
                if (auto parsed = parseJournal(*bytes, true)) {
                    for (const auto& [hash, raw] : parsed->objects) {
                        auto payload = encodeObject(hash, raw);
                        if (!payload) {
                            return tl::unexpected<Error>(payload.error());
                        }
                        appendFrame(content, header.generation, seq++, *payload);
                    }
                }
                snapshotSeq = seq;
                appendFrame(content, header.generation, seq++, encodeSnapshot(*latest_).payload);
            }
            // In place: a crash part way leaves either the old header and records (then the head is not the base
            // and a later recover() accepts the journal only if the head's state is one it reached), or the new
            // header followed by records of the old generation, which fail their checksum and are ignored.
            if (auto st = file_->writeAt(0, content); !st) {
                return failWrite(st.error());
            }
            if (auto st = file_->truncate(content.size()); !st) {
                return failWrite(st.error());
            }
            if (auto st = file_->flush(); !st) {
                return failWrite(st.error());
            }
            header_ = std::move(header);
            end_ = content.size();
            nextSeq_ = seq;
            depth_ = Depth{};
            pending_.reset(); // a check of the old generation's step has nothing to refer to any more
            latestSeq_ = snapshotSeq;
            return {};
        } catch (const std::exception& e) {
            return tl::unexpected<Error>(internalError(e.what()));
        }
    }

    Status discard() override
    {
        try {
            std::lock_guard<std::mutex> guard(mutex_);
            if (!file_) {
                return fail(ErrorCode::InvalidArgument, "The recovery journal is closed already");
            }
            pending_.reset();
            auto file = std::move(file_);
            return file->removeAndClose();
        } catch (const std::exception& e) {
            return tl::unexpected<Error>(internalError(e.what()));
        }
    }

private:
    struct PendingCheck {
        std::uint32_t seq = 0;
        doc::DocStatePtr state;
    };

    Status usable() const
    {
        if (!file_) {
            return fail(ErrorCode::InvalidArgument, "The recovery journal is closed");
        }
        if (broken_) {
            return fail(ErrorCode::IoError, "The recovery journal could not be written and is no longer kept");
        }
        return {};
    }

    tl::unexpected<Error> failWrite(Error error)
    {
        broken_ = true;
        return tl::unexpected<Error>(std::move(error));
    }

    // After step record `seq`: the state it led to, and its hash if known; otherwise the checker computes it.
    // Caller holds mutex_.
    void stepped(std::uint32_t seq, const doc::DocStatePtr& state, std::optional<Hash256> hash)
    {
        latest_ = state;
        latestSeq_ = seq;
        latestHash_ = hash;
        if (hash || !gChecking.load()) {
            pending_.reset();
        } else {
            pending_ = PendingCheck{seq, state}; // an older unchecked step stays unchecked: checking is best effort
            wake_.notify_one();
        }
    }

    Status appendStep(bool undo, const doc::DocStatePtr& after)
    {
        try {
            if (!after) {
                return fail(ErrorCode::InvalidArgument, "No state given to the recovery journal");
            }
            std::unique_lock<std::mutex> guard(mutex_);
            std::uint32_t seq = 0;
            if (undo ? depth_.undo > 0 : depth_.redo > 0) {
                RAY_JTRY(writeRecord(encodeStep(undo ? Tag::Undo : Tag::Redo), true, &seq));
                depth_.apply(undo ? JournalRecord::Kind::Undo : JournalRecord::Kind::Redo);
                stepped(seq, after, std::nullopt);
            } else {
                // Into history from before the base: a replayed document could not repeat it, so record the state.
                const Snapshot snapshot = encodeSnapshot(*after);
                RAY_JTRY(writeRecord(snapshot.payload, true, &seq));
                depth_.apply(JournalRecord::Kind::Snapshot);
                stepped(seq, after, snapshot.hash);
            }
            return {};
        } catch (const std::exception& e) {
            return tl::unexpected<Error>(internalError(e.what()));
        }
    }

    // One record at the end. With `flush` it is on the disk before this returns. A failed write is cut off again;
    // if even that fails, the journal takes no more records, so it never holds a record after a gap.
    Status writeRecord(std::string_view payload, bool flush, std::uint32_t* seqOut)
    {
        RAY_JTRY(usable());
        if (payload.size() > kMaxPayload) {
            return fail(ErrorCode::InvalidArgument, "A step is too large for the recovery journal");
        }
        std::string record;
        record.reserve(kFrameBytes + payload.size());
        appendFrame(record, header_.generation, nextSeq_, payload);
        auto st = file_->writeAt(end_, record);
        if (st && flush) {
            st = file_->flush();
        }
        if (!st) {
            if (!file_->truncate(end_) || !file_->flush()) {
                broken_ = true;
            }
            return st;
        }
        if (seqOut) {
            *seqOut = nextSeq_;
        }
        end_ += record.size();
        ++nextSeq_;
        return {};
    }

    // The checker thread: hashes the newest unchecked step's state outside the lock and records it.
    void checkLoop()
    {
        try {
            std::unique_lock<std::mutex> guard(mutex_);
            for (;;) {
                wake_.wait(guard, [this] { return stop_ || pending_.has_value(); });
                if (stop_) {
                    return;
                }
                const PendingCheck check = std::move(*pending_);
                pending_.reset();
                const std::uint64_t generation = header_.generation;
                guard.unlock();
                const Hash256 hash = check.state->stateHash();
                guard.lock();
                if (stop_) {
                    return;
                }
                if (generation != header_.generation || !usable()) {
                    continue; // saved (or discarded) meanwhile
                }
                if (writeRecord(encodeCheck(check.seq, hash), false, nullptr) && check.seq == latestSeq_) {
                    latestHash_ = hash;
                }
            }
        } catch (...) {
            // Checking is best effort; the steps themselves are recorded already.
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::unique_ptr<jio::ExclusiveFile> file_;
    Header header_;
    std::uint64_t end_ = 0;
    std::uint32_t nextSeq_ = 1;
    doc::DocStatePtr latest_;           // state() after the last step
    std::uint32_t latestSeq_ = 0;       // its record, 0 for none (the base)
    std::optional<Hash256> latestHash_; // its hash, once known
    std::optional<PendingCheck> pending_;
    Depth depth_;
    bool broken_ = false;
    bool stop_ = false;
    std::thread checker_; // last: started after everything above is ready
};

// ---------- Helpers of the static members ----------

Result<std::string> fileIdChecked(const IDocumentFile& file)
{
    std::string id = fileIdOf(file);
    if (id.empty()) {
        return fail(ErrorCode::FormatError, "The document has no file id", "meta file_id");
    }
    for (const char c : id) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == '-';
        if (!ok) {
            return fail(ErrorCode::FormatError, "The document's file id is not valid", id);
        }
    }
    return id;
}

Result<std::optional<Commit>> headOf(const IDocumentFile& file, std::string_view branch)
{
    auto log = file.log(branch, 1);
    if (!log) {
        return tl::unexpected<Error>(log.error());
    }
    if (log->empty()) {
        return std::optional<Commit>();
    }
    return std::optional<Commit>(log->front());
}

std::optional<IRecoveryJournal::Pending> readPending(const std::string& path)
{
    auto file = jio::ExclusiveFile::open(path, false);
    if (!file) {
        return std::nullopt; // in use by a running session, or gone
    }
    auto bytes = (*file)->readAll();
    if (!bytes) {
        return std::nullopt;
    }
    auto parsed = parseJournal(*bytes, true);
    if (!parsed || parsed->records.empty()) {
        return std::nullopt;
    }
    IRecoveryJournal::Pending pending;
    pending.journalPath = path;
    pending.filePath = parsed->header.filePath;
    pending.fileId = parsed->header.fileId;
    pending.branch = parsed->header.branch;
    pending.baseCommit = parsed->header.base;
    pending.records = std::move(parsed->records);
    pending.objects = std::move(parsed->objects);
    pending.damagedTail = parsed->damagedTail;
    pending.lastWriteMs = jio::modifiedMs(path);
    return pending;
}

Status ensureFolder(const std::string& dir)
{
    std::error_code ec;
    std::filesystem::create_directories(toFsPath(dir), ec);
    if (ec) {
        return fail(ErrorCode::IoError, "Cannot create the folder for recovery data", dir + ": " + ec.message());
    }
    return {};
}

// Appends `record` (and its check, if it has a hash) to a journal being written from scratch.
void appendRecord(std::string& content, std::uint64_t generation, std::uint32_t& seq, const JournalRecord& record)
{
    const std::uint32_t stepSeq = seq;
    switch (record.kind) {
    case JournalRecord::Kind::Execute: {
        auto json = doc::commandToJson(record.command);
        appendFrame(content, generation, seq++, encodeStep(Tag::Execute, json ? *json : std::string()));
        break;
    }
    case JournalRecord::Kind::Undo:
        appendFrame(content, generation, seq++, encodeStep(Tag::Undo));
        break;
    case JournalRecord::Kind::Redo:
        appendFrame(content, generation, seq++, encodeStep(Tag::Redo));
        break;
    case JournalRecord::Kind::Snapshot:
        appendFrame(content, generation, seq++, encodeSnapshot(*record.snapshot).payload);
        return;
    }
    if (record.stateAfter) {
        appendFrame(content, generation, seq++, encodeCheck(stepSeq, *record.stateAfter));
    }
}

// Replays records[0, count) onto a document made from `base`; `applied` counts the records that replayed and
// `stopped` says why it ended early. With `watch`, `reached` becomes true once state() had that hash.
std::unique_ptr<doc::Document> replay(const IRecoveryJournal::DocumentFactory& makeDocument,
                                      const doc::DocStatePtr& base, const std::vector<JournalRecord>& records,
                                      std::size_t count, std::size_t& applied, std::optional<Error>& stopped,
                                      const std::optional<Hash256>& watch, bool& reached)
{
    applied = 0;
    std::unique_ptr<doc::Document> document = makeDocument(base);
    if (!document) {
        stopped = makeError(ErrorCode::Internal, "Cannot create the recovered document");
        return nullptr;
    }
    for (std::size_t i = 0; i < count; ++i) {
        const JournalRecord& record = records[i];
        Status step;
        switch (record.kind) {
        case JournalRecord::Kind::Execute:
            if (auto result = document->execute(record.command); !result) {
                step = tl::unexpected<Error>(result.error());
            }
            break;
        case JournalRecord::Kind::Undo:
            step = document->undo();
            break;
        case JournalRecord::Kind::Redo:
            step = document->redo();
            break;
        case JournalRecord::Kind::Snapshot:
            if (record.snapshot) {
                auto next = makeDocument(record.snapshot);
                if (next) {
                    document = std::move(next);
                } else {
                    step = fail(ErrorCode::Internal, "Cannot create the recovered document");
                }
            } else {
                step = fail(ErrorCode::FormatError, "A recovered snapshot is empty");
            }
            break;
        }
        // A step that failed changed nothing; one that succeeded may have given the wrong state.
        const bool mayHaveChanged = step.has_value();
        if (step && record.stateAfter && document->state()->stateHash() != *record.stateAfter) {
            step = fail(ErrorCode::FormatError, "Replaying a change gave a different result than before the crash",
                        "record " + std::to_string(i + 1));
        } else if (!step) {
            step.error().context.push_back("record " + std::to_string(i + 1));
        }
        if (!step) {
            stopped = step.error();
            if (mayHaveChanged) {
                // state() is something it never was before the crash: replay the good prefix into a new document,
                // so neither state() nor its undo stack holds the bad step.
                std::size_t again = 0;
                std::optional<Error> ignored;
                bool unused = false;
                return replay(makeDocument, base, records, applied, again, ignored, std::nullopt, unused);
            }
            return document;
        }
        ++applied;
        if (watch && !reached) {
            reached = (record.stateAfter ? *record.stateAfter : document->state()->stateHash()) == *watch;
        }
    }
    return document;
}

} // namespace

void setJournalChecking(bool on) noexcept
{
    gChecking = on;
}

void setRecoveryDir(const std::string& utf8Dir)
{
    std::lock_guard<std::mutex> guard(gDirMutex);
    gDirOverride = utf8Dir;
}

std::string recoveryDir()
{
    {
        std::lock_guard<std::mutex> guard(gDirMutex);
        if (!gDirOverride.empty()) {
            return gDirOverride;
        }
    }
    return jio::defaultRecoveryDir();
}

std::string journalPathFor(const std::string& fileId)
{
    return fromFsPath(toFsPath(recoveryDir()) / toFsPath(fileId + std::string(kSuffix)));
}

std::vector<std::uint64_t> journalRecordEnds(const std::string& journalPath)
{
    try {
        auto file = jio::ExclusiveFile::open(journalPath, false);
        if (!file) {
            return {};
        }
        auto bytes = (*file)->readAll();
        if (!bytes) {
            return {};
        }
        auto parsed = parseJournal(*bytes, false);
        return parsed ? parsed->ends : std::vector<std::uint64_t>();
    } catch (...) {
        return {};
    }
}

} // namespace detail

using detail::fail;

Result<std::unique_ptr<IRecoveryJournal>> IRecoveryJournal::start(const IDocumentFile& file,
                                                                  const std::string& filePath, doc::DocStatePtr base,
                                                                  std::string_view branch)
{
    try {
        if (!detail::snapshotPath(file).empty()) {
            return fail(ErrorCode::InvalidArgument, "A document opened read-only has no recovery journal");
        }
        if (branch.empty()) {
            return fail(ErrorCode::InvalidArgument, "No branch given");
        }
        auto fileId = detail::fileIdChecked(file);
        if (!fileId) {
            return tl::unexpected<Error>(fileId.error());
        }
        auto head = detail::headOf(file, branch);
        if (!head) {
            return tl::unexpected<Error>(head.error());
        }
        if (!base) {
            base = detail::emptyState();
        }
        const Hash256 baseHash = base->stateHash();
        if (*head && (*head)->root != baseHash) {
            return fail(ErrorCode::InvalidArgument, "The recovery journal must start from the saved version",
                        "state " + baseHash.hex() + " is not the head " + (*head)->id.hex());
        }
        RAY_JTRY(detail::ensureFolder(detail::recoveryDir()));
        const std::string path = detail::journalPathFor(*fileId);
        auto opened = detail::jio::ExclusiveFile::open(path, true);
        if (!opened) {
            if (opened.error().code == ErrorCode::Locked) {
                return fail(ErrorCode::Locked, "This document is already being edited in another RayCAD window", path);
            }
            return tl::unexpected<Error>(opened.error());
        }
        auto old = (*opened)->readAll();
        if (!old) {
            return tl::unexpected<Error>(old.error());
        }
        if (auto parsed = detail::parseJournal(*old, false); parsed && !parsed->records.empty()) {
            return fail(ErrorCode::InvalidArgument,
                        "This document has unsaved changes from an earlier session; recover or discard them first",
                        path);
        }
        detail::Header header;
        header.generation = detail::randomGeneration();
        header.fileId = *fileId;
        header.filePath = filePath;
        header.branch = std::string(branch);
        if (*head) {
            header.base = (*head)->id;
        }
        header.createdMs = detail::unixNowMs();
        std::string content = detail::newJournal(header);
        std::uint32_t seq = 1;
        if (!*head && baseHash != detail::emptyState()->stateHash()) {
            // Nothing saved yet: the starting state itself is the first step.
            detail::appendFrame(content, header.generation, seq++, detail::encodeSnapshot(*base).payload);
        }
        RAY_JTRY((*opened)->writeAt(0, content));
        RAY_JTRY((*opened)->truncate(content.size()));
        RAY_JTRY((*opened)->flush());
        RAY_JTRY(detail::jio::syncFolderOf(path));
        return std::unique_ptr<IRecoveryJournal>(new detail::JournalImpl(
            std::move(*opened), std::move(header), content.size(), seq, std::move(base), baseHash, detail::Depth{}));
    } catch (const std::exception& e) {
        return tl::unexpected<Error>(detail::internalError(e.what()));
    }
}

std::vector<IRecoveryJournal::Pending> IRecoveryJournal::findPending()
{
    std::vector<Pending> out;
    try {
        std::error_code ec;
        const auto dir = detail::toFsPath(detail::recoveryDir());
        std::vector<std::string> paths;
        for (auto it = std::filesystem::directory_iterator(dir, ec); !ec && it != std::filesystem::directory_iterator();
             it.increment(ec)) {
            const std::string name = detail::fromFsPath(it->path().filename());
            if (name.size() > detail::kSuffix.size() &&
                name.compare(name.size() - detail::kSuffix.size(), detail::kSuffix.size(), detail::kSuffix) == 0 &&
                it->is_regular_file(ec)) {
                paths.push_back(detail::fromFsPath(it->path()));
            }
        }
        std::sort(paths.begin(), paths.end());
        for (const std::string& path : paths) {
            if (auto pending = detail::readPending(path)) {
                out.push_back(std::move(*pending));
            }
        }
        std::stable_sort(out.begin(), out.end(),
                         [](const Pending& a, const Pending& b) { return a.lastWriteMs > b.lastWriteMs; });
    } catch (...) {
        out.clear();
    }
    return out;
}

std::optional<IRecoveryJournal::Pending> IRecoveryJournal::findPendingFor(const IDocumentFile& file)
{
    try {
        auto fileId = detail::fileIdChecked(file);
        if (!fileId) {
            return std::nullopt;
        }
        return detail::readPending(detail::journalPathFor(*fileId));
    } catch (...) {
        return std::nullopt;
    }
}

Status IRecoveryJournal::discard(const Pending& pending)
{
    try {
        auto file = detail::jio::ExclusiveFile::open(pending.journalPath, false);
        if (!file) {
            if (file.error().code == ErrorCode::NotFound) {
                return {};
            }
            return tl::unexpected<Error>(file.error());
        }
        return (*file)->removeAndClose();
    } catch (const std::exception& e) {
        return tl::unexpected<Error>(detail::internalError(e.what()));
    }
}

Result<IRecoveryJournal::Recovered> IRecoveryJournal::recover(const Pending& pending, IDocumentFile& file,
                                                              const DocumentFactory& makeDocument)
{
    try {
        if (!makeDocument) {
            return fail(ErrorCode::InvalidArgument, "No way to make the recovered document");
        }
        if (!detail::snapshotPath(file).empty()) {
            return fail(ErrorCode::InvalidArgument, "Recovery needs the document opened for writing");
        }
        auto fileId = detail::fileIdChecked(file);
        if (!fileId) {
            return tl::unexpected<Error>(fileId.error());
        }
        if (*fileId != pending.fileId) {
            return fail(ErrorCode::InvalidArgument, "This recovery data belongs to another document",
                        "file id " + *fileId + ", journal " + pending.fileId);
        }
        auto head = detail::headOf(file, pending.branch);
        if (!head) {
            return tl::unexpected<Error>(head.error());
        }
        const std::optional<Commit>& h = *head;
        const bool atBase = h ? (pending.baseCommit && h->id == *pending.baseCommit) : !pending.baseCommit;
        const bool childOfBase =
            h && (pending.baseCommit ? !h->parents.empty() && h->parents.front() == *pending.baseCommit
                                     : h->parents.empty());
        const auto older = [&]() {
            return fail(ErrorCode::InvalidArgument,
                        "This recovery data is older than the document's latest save and cannot be applied",
                        "head " + (h ? h->id.hex() : std::string("none")) + ", journal base " +
                            (pending.baseCommit ? pending.baseCommit->hex() : std::string("none")));
        };
        if (!atBase && !childOfBase) {
            return older();
        }
        doc::DocStatePtr base = detail::emptyState();
        if (pending.baseCommit) {
            auto loaded = file.loadCommit(*pending.baseCommit);
            if (!loaded) {
                if (loaded.error().code == ErrorCode::NotFound) {
                    return fail(ErrorCode::InvalidArgument,
                                "The version this recovery data continues is no longer in the document",
                                pending.baseCommit->hex());
                }
                return tl::unexpected<Error>(loaded.error());
            }
            base = std::move(*loaded);
        }

        // Not at the base: the save that followed it landed but markSaved did not, if its state is one the journal
        // reached; then the journal continues from that head.
        const std::optional<Hash256> watch = atBase ? std::nullopt : std::optional<Hash256>(h->root);
        bool reached = watch && base->stateHash() == *watch;
        Recovered out;
        out.document = detail::replay(makeDocument, base, pending.records, pending.records.size(), out.applied,
                                      out.stopped, watch, reached);
        if (!out.document) {
            return tl::unexpected<Error>(out.stopped.value_or(detail::internalError("replay")));
        }
        if (!atBase && !reached) {
            return older();
        }
        const std::optional<Commit> continued = atBase ? std::nullopt : h;
        const doc::DocStatePtr finalState = out.document->state();
        const Hash256 finalHash = finalState->stateHash();
        out.modified = !h || finalHash != h->root;
        if (!out.modified) {
            out.document->markClean();
        }

        // The journal continues: rewritten with the replayed records only (or, when it continues from a later head,
        // the state as one snapshot), beside the old one and then moved over it, so a crash now loses nothing.
        detail::Header header;
        header.generation = detail::randomGeneration();
        header.fileId = pending.fileId;
        header.filePath = pending.filePath;
        header.branch = pending.branch;
        header.base = continued ? std::optional<Hash256>(continued->id) : pending.baseCommit;
        header.createdMs = detail::unixNowMs();
        std::string content = detail::newJournal(header);
        std::uint32_t seq = 1;
        for (const auto& [hash, raw] : pending.objects) {
            auto payload = detail::encodeObject(hash, raw);
            if (!payload) {
                return tl::unexpected<Error>(payload.error());
            }
            detail::appendFrame(content, header.generation, seq++, *payload);
        }
        detail::Depth depth;
        if (continued) {
            if (finalHash != continued->root) {
                detail::appendFrame(content, header.generation, seq++, detail::encodeSnapshot(*finalState).payload);
            }
        } else {
            for (std::size_t i = 0; i < out.applied; ++i) {
                detail::appendRecord(content, header.generation, seq, pending.records[i]);
                depth.apply(pending.records[i].kind);
            }
        }
        const std::string path = pending.journalPath;
        {
            // Not in use by anyone (a running session would hold it).
            auto probe = detail::jio::ExclusiveFile::open(path, true);
            if (!probe) {
                return tl::unexpected<Error>(probe.error());
            }
        }
        const std::string temp = path + ".rewrite-" + detail::randomName();
        {
            auto fresh = detail::jio::ExclusiveFile::open(temp, true);
            if (!fresh) {
                return tl::unexpected<Error>(fresh.error());
            }
            auto st = (*fresh)->truncate(0);
            if (st) {
                st = (*fresh)->writeAt(0, content);
            }
            if (st) {
                st = (*fresh)->flush();
            }
            if (!st) {
                (void)(*fresh)->removeAndClose();
                return tl::unexpected<Error>(st.error());
            }
        }
        if (auto st = detail::jio::replaceFile(temp, path); !st) {
            detail::removeQuietly(temp);
            return tl::unexpected<Error>(st.error());
        }
        auto journal = detail::jio::ExclusiveFile::open(path, false);
        if (!journal) {
            return tl::unexpected<Error>(journal.error());
        }
        out.journal = std::unique_ptr<IRecoveryJournal>(new detail::JournalImpl(
            std::move(*journal), std::move(header), content.size(), seq, finalState, finalHash, depth));
        return out;
    } catch (const std::exception& e) {
        return tl::unexpected<Error>(detail::internalError(e.what()));
    }
}

} // namespace ray::store
