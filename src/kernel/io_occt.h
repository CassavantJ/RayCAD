// Private to src/kernel (task T-005): OCCT-side helpers shared by io_import.cpp and io_export.cpp.
// Includes OCCT headers; nothing outside src/kernel may include it (PLAN.md 0.4 rule 1).
#pragma once
#include "io_internal.h"

#include <Message_ProgressIndicator.hxx>
#include <Message_ProgressScope.hxx>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <streambuf>
#include <string>
#include <string_view>
#include <vector>

namespace ray::kernel::detail {

// Brings up OCCT's data-exchange toolkits once per process, on one thread: their first use loads the DLLs
// (delay-loaded on Windows) and registers controllers and static tables, which OCCT does not promise to be
// safe from several threads at once. Every import and export calls it first, on its I/O thread (about 40 ms,
// once). May throw (call inside guarded()).
void initializeDataExchange();

// Maps an OCCT progress range onto [from, to] of an IoTask and answers OCCT's cancel polls from it.
class TaskIndicator final : public Message_ProgressIndicator {
public:
    TaskIndicator(IoTask& task, double from, double to, std::string_view stage)
        : task_(task), from_(from), to_(to), stage_(stage)
    {
    }
    bool UserBreak() override { return task_.stopRequested(); }
    void Show(const Message_ProgressScope&, const bool) override
    {
        task_.report(from_ + (to_ - from_) * GetPosition(), stage_);
    }

private:
    IoTask& task_;
    double from_, to_;
    std::string_view stage_;
};

// Reads a file for OCCT's ReadStream. Reports the bytes read as progress in [from, to] of the task, ends the
// stream early (end-of-file) once the task is asked to stop, and keeps the last bytes of the file so the
// caller can tell a complete file from a truncated one.
class InputFileBuffer final : public std::streambuf {
public:
    InputFileBuffer(std::FILE* file, std::uint64_t size, IoTask& task, double from, double to, std::string_view stage,
                    std::string_view nextStage)
        : file_(file), size_(size), task_(task), from_(from), to_(to), stage_(stage), nextStage_(nextStage),
          buffer_(kChunk)
    {
    }
    ~InputFileBuffer() override
    {
        if (file_ != nullptr)
            std::fclose(file_);
    }
    InputFileBuffer(const InputFileBuffer&) = delete;
    InputFileBuffer& operator=(const InputFileBuffer&) = delete;

    bool stopped() const noexcept { return stopped_; }
    bool readError() const noexcept { return readError_; }
    bool reachedEnd() const noexcept { return reachedEnd_; }
    // The last bytes of the file (up to kTail), valid once reachedEnd().
    std::string_view tail() const noexcept { return std::string_view(tail_.data(), tailSize_); }

    static constexpr std::size_t kTail = 1024;

protected:
    int_type underflow() override
    {
        if (gptr() < egptr())
            return traits_type::to_int_type(*gptr());
        if (file_ == nullptr || reachedEnd_ || stopped_)
            return traits_type::eof();
        if (task_.stopRequested()) {
            stopped_ = true;
            return traits_type::eof();
        }
        const std::size_t count = std::fread(buffer_.data(), 1, buffer_.size(), file_);
        if (count == 0) {
            readError_ = std::ferror(file_) != 0;
            reachedEnd_ = true;
            task_.report(to_, nextStage_);
            return traits_type::eof();
        }
        consumed_ += count;
        keepTail(count);
        const double share = size_ > 0 ? static_cast<double>(consumed_) / static_cast<double>(size_) : 1.0;
        task_.report(from_ + (to_ - from_) * std::min(share, 1.0), stage_);
        setg(buffer_.data(), buffer_.data(), buffer_.data() + count);
        return traits_type::to_int_type(buffer_[0]);
    }

private:
    static constexpr std::size_t kChunk = 256 * 1024;

    void keepTail(std::size_t count)
    {
        if (count >= kTail) {
            std::copy(buffer_.data() + (count - kTail), buffer_.data() + count, tail_.data());
            tailSize_ = kTail;
            return;
        }
        const std::size_t keep = std::min(tailSize_, kTail - count);
        std::copy(tail_.data() + (tailSize_ - keep), tail_.data() + tailSize_, tail_.data());
        std::copy(buffer_.data(), buffer_.data() + count, tail_.data() + keep);
        tailSize_ = keep + count;
    }

    std::FILE* file_;
    std::uint64_t size_;
    std::uint64_t consumed_ = 0;
    IoTask& task_;
    double from_, to_;
    std::string_view stage_, nextStage_;
    std::vector<char> buffer_;
    std::array<char, kTail> tail_{};
    std::size_t tailSize_ = 0;
    bool stopped_ = false;
    bool readError_ = false;
    bool reachedEnd_ = false;
};

// A file written next to its final name (`<name>.partial`) and moved over it only once complete, so a failed
// or cancelled export never leaves a half-written file under the requested name.
class OutputFile {
public:
    explicit OutputFile(std::filesystem::path target) : target_(std::move(target))
    {
        temp_ = target_;
        temp_ += ".partial";
    }
    ~OutputFile()
    {
        if (stream_.is_open())
            stream_.close();
        if (!committed_) {
            std::error_code ignored;
            std::filesystem::remove(temp_, ignored);
        }
    }
    OutputFile(const OutputFile&) = delete;
    OutputFile& operator=(const OutputFile&) = delete;

    Status open(std::string_view where)
    {
        stream_.open(temp_, std::ios::binary | std::ios::trunc);
        if (!stream_.is_open())
            return tl::unexpected(
                Error{ErrorCode::IoError, "The file cannot be written: " + utf8Of(target_), {std::string(where)}});
        return std::monostate{};
    }
    std::ostream& stream() { return stream_; }
    // Closes the temporary file and renames it to the target.
    Status commit(std::string_view where)
    {
        stream_.flush();
        const bool good = stream_.good();
        stream_.close();
        if (!good || stream_.fail())
            return tl::unexpected(Error{ErrorCode::IoError,
                                        "Writing the file failed (is the disk full?): " + utf8Of(target_),
                                        {std::string(where)}});
        std::error_code ec;
        std::filesystem::rename(temp_, target_, ec);
        if (ec)
            return tl::unexpected(Error{ErrorCode::IoError,
                                        "The file cannot be replaced: " + utf8Of(target_) + " (" + ec.message() + ")",
                                        {std::string(where)}});
        committed_ = true;
        return std::monostate{};
    }

private:
    std::filesystem::path target_;
    std::filesystem::path temp_;
    std::ofstream stream_;
    bool committed_ = false;
};

} // namespace ray::kernel::detail
