// The output side of a post run: G-code text, line map, modal state, sequence numbers and warnings. It is the C++
// behind the script's `out` object (post_api.d.ts, Writer) and knows nothing about JavaScript, so it is tested on
// its own. Internal to src/post.
#pragma once
#include <ray/post.h>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ray::post {

struct WriterOptions {
    bool inch = false;   // output units: inch (4 coordinate decimals) or millimetres (3)
    std::string dialect; // PostDefinition.dialect; picks the comment rules
    bool crlf = true;    // line ending of every line, the last one included
    std::size_t maxOutputBytes = std::size_t(1) << 20;
    std::size_t maxWarnings = 1000;
    std::size_t maxWarningBytes = 1000; // a longer warning is cut (UTF-8 safe) and marked with "..."
    std::size_t reserveBytes = 0;       // text capacity to reserve up front (an estimate; growth copies are slow)
    std::size_t reserveLines = 0;       // line map entries to reserve up front
    std::int64_t maxBlockNumber = 0;    // MachineDef::maxBlockNumber; 0 = no limit. Past it, N numbers wrap to start.
};

// The comment text the dialect accepts, without the parentheses. Every dialect: ASCII letters, digits, space and
// . , - + / * = : # _ ; anything else (parentheses, control characters, ';', '%', each non-ASCII character)
// becomes a space; runs of spaces collapse and the ends are trimmed. Upper case, except for the Haas dialects
// ("haas-ngc", "haas-classic"), whose controls show lower case.
std::string sanitizeComment(std::string_view text, std::string_view dialect);

class Writer {
public:
    explicit Writer(WriterOptions options);

    // ---------- Lines. Each returns false, and writes nothing, once the output limit would be exceeded. ----------
    // Joins the non-empty words with one space; nothing is written when all are empty. With sequence numbers on,
    // the line starts with "N<n>".
    bool block(std::span<const std::string_view> words);
    // "(" + sanitizeComment(text) + ")"; nothing when the sanitised text is empty.
    bool comment(std::string_view text);
    // The text as given, one line per "\n"-separated part ("\r" before a "\n" is dropped).
    bool raw(std::string_view text);

    // ---------- Modal state ----------
    // True (write the word) when `word` differs from the last word of `group`, or the group was reset.
    bool modal(std::string_view group, std::string_view word);
    // Every number below is a value on grid() (fmt::snapToGrid): the script's double snapped back to integers, one
    // nanometre per step in mm and in inch output.
    std::int64_t grid() const { return grid_; }
    // Axis words with the coordinate format of the output units: mm "X12.5" (3 decimals), inch "X0.4921" (4).
    // True, with `word` set, when the value rounded to the format differs from the last one written for that letter
    // (A to Z, upper case).
    bool axis(char letter, std::int64_t n, std::string& word);
    // Feed word, mm/min with 1 decimal or inch/min with 2; true with `word` set when it changed.
    bool feed(std::int64_t n, std::string& word);
    // prefix + the number, always.
    void word(std::string_view prefix, std::int64_t n, const NumberFormat& format, std::string& word) const;
    // Forgets every modal group, every axis and the feed, so that the next word of each is written.
    void resetAll();
    // Forgets one thing: "feed", "axes" (every axis), a single upper-case letter A to Z (that axis), or a modal
    // group name.
    void reset(std::string_view name);

    const NumberFormat& coordinateFormat() const { return coordinate_; }
    const NumberFormat& feedFormat() const { return feed_; }

    // ---------- Sequence numbers (post_api.d.ts, Writer.sequence) ----------
    struct Sequence {
        bool enabled = false;
        std::int64_t start = 10;
        std::int64_t increment = 10;
        bool onlyOnToolChange = false; // number only the first block of a section that changes the tool
    };
    const Sequence& sequence() const { return sequence_; }
    void setSequenceEnabled(bool on) { sequence_.enabled = on; }
    void setSequenceStart(std::int64_t start); // also restarts the count
    void setSequenceIncrement(std::int64_t increment) { sequence_.increment = increment; }
    void setSequenceOnlyOnToolChange(bool on) { sequence_.onlyOnToolChange = on; }
    // Set by the engine before onSectionStart of a section with toolChanged; the next block takes the number.
    void setToolChangeBlockPending(bool pending) { toolChangePending_ = pending; }

    // ---------- Warnings ----------
    // False, and nothing stored, once maxWarnings warnings are stored.
    bool warn(std::string_view message);

    // ---------- Line map ----------
    // While a move handler runs, every line written is mapped to (section, move): 0-based line index into text,
    // section index into PostJob::sections, move index into that section's Toolpath::moves. The engine also enters
    // (section, first move) around onSectionStart and (section, last move) around onSectionEnd.
    void enterMove(std::uint32_t section, std::uint32_t move);
    void leaveMove() { inMove_ = false; }

    bool outputLimitHit() const { return outputLimitHit_; }
    std::size_t outputBytes() const { return out_.text.size(); }
    std::size_t outputLimit() const { return options_.maxOutputBytes; }
    std::uint32_t lineCount() const { return lines_; }
    std::size_t warningCount() const { return out_.warnings.size(); }

    PostOutput take() { return std::move(out_); }

private:
    bool emit(std::string_view line);

    WriterOptions options_;
    NumberFormat coordinate_{};
    NumberFormat feed_{};
    std::int64_t grid_ = 0;
    PostOutput out_;
    std::uint32_t lines_ = 0;
    bool outputLimitHit_ = false;
    bool inMove_ = false;
    std::uint32_t section_ = 0;
    std::uint32_t move_ = 0;

    std::array<std::int64_t, 26> axisLast_{};
    std::array<bool, 26> axisSet_{};
    std::int64_t feedLast_ = 0;
    bool feedSet_ = false;
    std::vector<std::pair<std::string, std::string>> modal_; // few groups: a linear search beats a map

    Sequence sequence_;
    std::int64_t nextNumber_ = 10;
    bool toolChangePending_ = false;
    std::string line_; // scratch
};

} // namespace ray::post
