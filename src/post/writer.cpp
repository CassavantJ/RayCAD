// G-code output state behind the script's `out` object (see writer.h).
#include "writer.h"

#include "format.h"

#include <algorithm>
#include <charconv>

namespace ray::post {
namespace {

bool commentCharAllowed(char c)
{
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
        return true;
    }
    switch (c) {
    case ' ':
    case '.':
    case ',':
    case '-':
    case '+':
    case '/':
    case '*':
    case '=':
    case ':':
    case '#':
    case '_':
        return true;
    default:
        return false;
    }
}

void appendInt(std::string& out, std::int64_t value)
{
    std::array<char, 24> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    out.append(buffer.data(), result.ptr);
}

int axisIndex(char letter)
{
    return letter >= 'A' && letter <= 'Z' ? letter - 'A' : -1;
}

} // namespace

std::string sanitizeComment(std::string_view text, std::string_view dialect)
{
    const bool keepLowerCase = dialect == "haas-ngc" || dialect == "haas-classic";
    std::string out;
    out.reserve(text.size());
    bool pendingSpace = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        const auto byte = static_cast<unsigned char>(c);
        if (byte >= 0x80) {
            // One space per UTF-8 character: skip its continuation bytes.
            while (i + 1 < text.size() && (static_cast<unsigned char>(text[i + 1]) & 0xC0) == 0x80) {
                ++i;
            }
            c = ' ';
        } else if (!commentCharAllowed(c)) {
            c = ' ';
        }
        if (c == ' ') {
            pendingSpace = !out.empty();
            continue;
        }
        if (pendingSpace) {
            out += ' ';
            pendingSpace = false;
        }
        if (!keepLowerCase && c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
        out += c;
    }
    return out;
}

Writer::Writer(WriterOptions options) : options_(std::move(options))
{
    Format f;
    f.axisDecimals = options_.inch ? 4 : 3;
    f.feedDecimals = options_.inch ? 2 : 1;
    setFormat(f);
    grid_ = options_.inch ? fmt::kInchGrid : fmt::kMicroGrid;
    out_.text.reserve(std::min(options_.reserveBytes, options_.maxOutputBytes));
    out_.lineMap.reserve(options_.reserveLines);
}

bool Writer::emit(std::string_view line)
{
    const std::size_t eol = options_.crlf ? 2 : 1;
    if (outputLimitHit_ || out_.text.size() + line.size() + eol > options_.maxOutputBytes) {
        outputLimitHit_ = true;
        return false;
    }
    out_.text.append(line);
    out_.text.append(options_.crlf ? "\r\n" : "\n");
    if (inMove_) {
        out_.lineMap.push_back(GcodeLineMap{lines_, section_, move_});
    }
    ++lines_;
    return true;
}

bool Writer::block(std::span<const std::string_view> words)
{
    line_.clear();
    for (const std::string_view w : words) {
        if (w.empty()) {
            continue;
        }
        if (!line_.empty()) {
            line_ += ' ';
        }
        line_.append(w);
    }
    if (line_.empty()) {
        return true;
    }
    const bool numbered = sequence_.enabled && (!sequence_.onlyOnToolChange || toolChangePending_);
    toolChangePending_ = false;
    if (numbered) {
        if (options_.maxBlockNumber > 0 && nextNumber_ > options_.maxBlockNumber) {
            nextNumber_ = sequence_.start;
        }
        std::string n = "N";
        appendInt(n, nextNumber_);
        n += ' ';
        line_.insert(0, n);
        nextNumber_ += sequence_.increment;
    }
    return emit(line_);
}

bool Writer::comment(std::string_view text)
{
    std::string clean = sanitizeComment(text, options_.dialect);
    if (format_.commentMaxLength > 0 && clean.size() > format_.commentMaxLength) {
        clean.resize(format_.commentMaxLength); // ASCII only after sanitising: a byte is a character
        while (!clean.empty() && clean.back() == ' ') {
            clean.pop_back();
        }
    }
    if (clean.empty()) {
        return true;
    }
    line_.clear();
    line_ += '(';
    line_ += clean;
    line_ += ')';
    return emit(line_);
}

bool Writer::raw(std::string_view text)
{
    std::size_t begin = 0;
    for (;;) {
        const std::size_t end = text.find('\n', begin);
        std::string_view part =
            text.substr(begin, end == std::string_view::npos ? std::string_view::npos : end - begin);
        if (end != std::string_view::npos && !part.empty() && part.back() == '\r') {
            part.remove_suffix(1);
        }
        if (!emit(part)) {
            return false;
        }
        if (end == std::string_view::npos) {
            return true;
        }
        begin = end + 1;
    }
}

bool Writer::modal(std::string_view group, std::string_view word)
{
    for (auto& [name, last] : modal_) {
        if (name == group) {
            if (last == word) {
                return false;
            }
            last.assign(word);
            return true;
        }
    }
    modal_.emplace_back(std::string(group), std::string(word));
    return true;
}

bool Writer::axis(char letter, std::int64_t n, std::string& word)
{
    const int index = axisIndex(letter);
    if (index < 0) {
        return false;
    }
    const auto slot = static_cast<std::size_t>(index);
    const std::int64_t q = fmt::roundGrid(n, coordinate_.decimals, grid_);
    if (axisSet_[slot] && axisLast_[slot] == q) {
        return false;
    }
    axisSet_[slot] = true;
    axisLast_[slot] = q;
    word.clear();
    word += letter;
    fmt::appendFixed(word, q, coordinate_.decimals, coordinate_.trimTrailingZeros, coordinate_.forceDecimalPoint,
                     coordinate_.leadingZero);
    applyTrailing(word, coordinate_);
    return true;
}

bool Writer::feed(std::int64_t n, std::string& word, int decimals)
{
    NumberFormat f = feed_;
    if (decimals >= 0) {
        f.decimals = std::min(decimals, fmt::kMaxDecimals);
    }
    const std::int64_t q = fmt::roundGrid(n, f.decimals, grid_);
    if (feedSet_ && feedLast_ == q && feedLastDecimals_ == f.decimals) {
        return false;
    }
    feedSet_ = true;
    feedLast_ = q;
    feedLastDecimals_ = f.decimals;
    word.clear();
    word += 'F';
    fmt::appendFixed(word, q, f.decimals, f.trimTrailingZeros, f.forceDecimalPoint, f.leadingZero);
    applyTrailing(word, f);
    return true;
}

void Writer::word(std::string_view prefix, std::int64_t n, const NumberFormat& format, std::string& word,
                  bool trailing) const
{
    word.assign(prefix);
    fmt::appendGrid(word, n, format, grid_);
    if (trailing) {
        applyTrailing(word, format);
    }
}

void Writer::applyTrailing(std::string& word, const NumberFormat& f) const
{
    // Style "one": a number that ends in its decimal point gets one zero ("X12." -> "X12.0"). "all" is written with
    // trimTrailingZeros off (setFormat), and "trim" leaves the word as it is.
    if (format_.trailing == Trailing::One && f.decimals > 0 && !word.empty() && word.back() == '.') {
        word += '0';
    }
}

void Writer::setFormat(const Format& f)
{
    format_ = f;
    format_.axisDecimals = std::clamp(f.axisDecimals, 0, fmt::kMaxDecimals);
    format_.feedDecimals = std::clamp(f.feedDecimals, 0, fmt::kMaxDecimals);
    const bool trim = format_.trailing != Trailing::All;
    coordinate_ = NumberFormat{format_.axisDecimals, trim, true, true, 1.0};
    feed_ = NumberFormat{format_.feedDecimals, trim, true, true, 1.0};
    axisSet_.fill(false);
    feedSet_ = false;
}

void Writer::resetAll()
{
    axisSet_.fill(false);
    feedSet_ = false;
    modal_.clear();
}

void Writer::reset(std::string_view name)
{
    if (name == "feed") {
        feedSet_ = false;
    } else if (name == "axes") {
        axisSet_.fill(false);
    } else if (name.size() == 1 && axisIndex(name[0]) >= 0) {
        axisSet_[static_cast<std::size_t>(axisIndex(name[0]))] = false;
    } else {
        std::erase_if(modal_, [&](const auto& entry) { return entry.first == name; });
    }
}

void Writer::setSequenceStart(std::int64_t start)
{
    sequence_.start = start;
    nextNumber_ = start;
}

bool Writer::warn(std::string_view message)
{
    if (out_.warnings.size() >= options_.maxWarnings) {
        return false;
    }
    if (message.size() <= options_.maxWarningBytes) {
        out_.warnings.emplace_back(message);
        return true;
    }
    std::size_t cut = options_.maxWarningBytes;
    while (cut > 0 && (static_cast<unsigned char>(message[cut]) & 0xC0) == 0x80) {
        --cut; // do not split a UTF-8 character
    }
    std::string shortened(message.substr(0, cut));
    shortened += "...";
    out_.warnings.push_back(std::move(shortened));
    return true;
}

void Writer::enterMove(std::uint32_t section, std::uint32_t move)
{
    inMove_ = true;
    section_ = section;
    move_ = move;
}

// ---------- PostOutput ----------
Hash256 PostOutput::hash() const
{
    Hasher h;
    h.str(text);
    h.i64(static_cast<std::int64_t>(lineMap.size()));
    for (const GcodeLineMap& entry : lineMap) {
        h.i64(entry.line).i64(entry.sectionIndex).i64(entry.moveIndex);
    }
    h.i64(static_cast<std::int64_t>(warnings.size()));
    for (const std::string& w : warnings) {
        h.str(w);
    }
    return h.finish();
}

} // namespace ray::post
