#include "post_logic.h"

#include <algorithm>
#include <tuple>

namespace ray::ui::camui {

namespace {

bool upperLetter(char c)
{
    return c >= 'A' && c <= 'Z';
}
bool letter(char c)
{
    return upperLetter(c) || (c >= 'a' && c <= 'z');
}
bool digit(char c)
{
    return c >= '0' && c <= '9';
}
char upper(char c)
{
    return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
}

GToken wordKind(char letterUpper)
{
    switch (letterUpper) {
    case 'N':
        return GToken::Sequence;
    case 'O':
        return GToken::Program;
    case 'G':
        return GToken::GWord;
    case 'M':
        return GToken::MWord;
    case 'X':
    case 'Y':
    case 'Z':
    case 'A':
    case 'B':
    case 'C':
    case 'U':
    case 'V':
    case 'W':
    case 'I':
    case 'J':
    case 'K':
    case 'R':
        return GToken::Axis;
    case 'F':
    case 'S':
        return GToken::FeedSpeed;
    default:
        return GToken::Word;
    }
}

} // namespace

const char* tokenName(GToken kind)
{
    switch (kind) {
    case GToken::Text:
        return "text";
    case GToken::Comment:
        return "comment";
    case GToken::Sequence:
        return "sequence";
    case GToken::Program:
        return "program";
    case GToken::GWord:
        return "g";
    case GToken::MWord:
        return "m";
    case GToken::Axis:
        return "axis";
    case GToken::FeedSpeed:
        return "feed";
    case GToken::Word:
        return "word";
    }
    return "text";
}

std::vector<GSpan> tokenize(std::string_view line)
{
    std::vector<GSpan> out;
    const auto push = [&](std::size_t start, std::size_t length, GToken kind) {
        if (length == 0)
            return;
        // Adjacent plain text joins up.
        if (kind == GToken::Text && !out.empty() && out.back().kind == GToken::Text &&
            out.back().start + out.back().length == start) {
            out.back().length += static_cast<std::uint32_t>(length);
            return;
        }
        out.push_back(GSpan{static_cast<std::uint32_t>(start), static_cast<std::uint32_t>(length), kind});
    };
    std::size_t i = 0;
    while (i < line.size()) {
        const char c = line[i];
        if (c == '(') {
            // To the matching ')' (comments do not nest on the controls the posts write for), or the line's end.
            std::size_t end = line.find(')', i + 1);
            end = end == std::string_view::npos ? line.size() : end + 1;
            push(i, end - i, GToken::Comment);
            i = end;
            continue;
        }
        if (c == ';') {
            push(i, line.size() - i, GToken::Comment);
            break;
        }
        if (letter(c)) {
            // A word: the letter and a number ("G1", "X-12.5", "F.5", "N10"); a letter without one is text.
            std::size_t j = i + 1;
            if (j < line.size() && (line[j] == '-' || line[j] == '+'))
                ++j;
            const std::size_t digitsFrom = j;
            while (j < line.size() && (digit(line[j]) || line[j] == '.'))
                ++j;
            const bool number = j > digitsFrom && std::any_of(line.begin() + static_cast<std::ptrdiff_t>(digitsFrom),
                                                              line.begin() + static_cast<std::ptrdiff_t>(j), digit);
            // Words written by the posts are upper case, except the letters of Haas program names; a lower-case
            // letter followed by a number is still a word.
            if (number) {
                push(i, j - i, wordKind(upper(c)));
                i = j;
                continue;
            }
            push(i, 1, GToken::Text);
            ++i;
            continue;
        }
        push(i, 1, GToken::Text);
        ++i;
    }
    return out;
}

// ---------- Index ----------
std::string_view GcodeIndex::line(std::size_t i) const
{
    if (!text || i >= lineStart.size())
        return {};
    const std::size_t from = lineStart[i];
    std::size_t to = i + 1 < lineStart.size() ? lineStart[i + 1] : text->size();
    std::string_view s(text->data() + from, to - from);
    if (!s.empty() && s.back() == '\n')
        s.remove_suffix(1);
    if (!s.empty() && s.back() == '\r')
        s.remove_suffix(1);
    return s;
}

std::string_view GcodeIndex::lines(std::size_t first, std::size_t last) const
{
    if (!text || first >= lineStart.size() || last < first)
        return {};
    last = std::min(last, lineStart.size() - 1);
    const std::size_t from = lineStart[first];
    const std::size_t to = last + 1 < lineStart.size() ? lineStart[last + 1] : text->size();
    return std::string_view(text->data() + from, to - from);
}

std::optional<MoveRef> GcodeIndex::moveOf(std::size_t line) const
{
    if (line >= lineEntry.size() || lineEntry[line] < 0)
        return std::nullopt;
    const post::GcodeLineMap& e = map[static_cast<std::size_t>(lineEntry[line])];
    return MoveRef{e.sectionIndex, e.moveIndex};
}

std::optional<std::pair<std::uint32_t, std::uint32_t>> GcodeIndex::linesOf(MoveRef move) const
{
    const auto key = [&](std::uint32_t i) {
        return std::pair(map[i].sectionIndex, map[i].moveIndex);
    };
    const std::pair<std::uint32_t, std::uint32_t> want(move.section, move.move);
    auto it =
        std::lower_bound(byMove.begin(), byMove.end(), want,
                         [&](std::uint32_t i, const std::pair<std::uint32_t, std::uint32_t>& k) { return key(i) < k; });
    if (it == byMove.end() || key(*it) != want)
        return std::nullopt;
    const std::uint32_t first = map[*it].line;
    std::uint32_t last = first;
    for (; it != byMove.end() && key(*it) == want; ++it)
        last = std::max(last, map[*it].line);
    return std::pair(first, last);
}

std::shared_ptr<const GcodeIndex> buildIndex(std::shared_ptr<const std::string> text,
                                             std::vector<post::GcodeLineMap> lineMap)
{
    auto index = std::make_shared<GcodeIndex>();
    index->text = text ? std::move(text) : std::make_shared<const std::string>();
    const std::string& t = *index->text;
    if (!t.empty()) {
        index->lineStart.reserve(t.size() / 24 + 1);
        index->lineStart.push_back(0);
        for (std::size_t i = 0; i < t.size(); ++i) {
            if (t[i] == '\n' && i + 1 < t.size())
                index->lineStart.push_back(static_cast<std::uint32_t>(i + 1));
        }
    }
    for (std::size_t i = 0; i < index->lineStart.size(); ++i)
        index->longestLine = std::max(index->longestLine, static_cast<std::uint32_t>(index->line(i).size()));
    index->lineEntry.assign(index->lineStart.size(), -1);
    index->map = std::move(lineMap);
    for (std::size_t i = 0; i < index->map.size(); ++i) {
        const std::uint32_t line = index->map[i].line;
        if (line < index->lineEntry.size() && index->lineEntry[line] < 0)
            index->lineEntry[line] = static_cast<std::int32_t>(i);
    }
    index->byMove.resize(index->map.size());
    for (std::size_t i = 0; i < index->byMove.size(); ++i)
        index->byMove[i] = static_cast<std::uint32_t>(i);
    const auto& m = index->map;
    std::sort(index->byMove.begin(), index->byMove.end(), [&](std::uint32_t a, std::uint32_t b) {
        return std::tie(m[a].sectionIndex, m[a].moveIndex, m[a].line) <
               std::tie(m[b].sectionIndex, m[b].moveIndex, m[b].line);
    });
    return index;
}

// ---------- Find ----------
std::vector<FindHit> findAll(const GcodeIndex& index, std::string_view needle, std::size_t limit)
{
    std::vector<FindHit> out;
    if (needle.empty())
        return out;
    std::string want(needle);
    std::transform(want.begin(), want.end(), want.begin(), upper);
    for (std::size_t l = 0; l < index.lineCount() && out.size() < limit; ++l) {
        const std::string_view line = index.line(l);
        if (line.size() < want.size())
            continue;
        for (std::size_t at = 0; at + want.size() <= line.size() && out.size() < limit; ++at) {
            bool same = true;
            for (std::size_t k = 0; k < want.size() && same; ++k)
                same = upper(line[at + k]) == want[k];
            if (same) {
                out.push_back(FindHit{static_cast<std::uint32_t>(l), static_cast<std::uint32_t>(at)});
                at += want.size() - 1;
            }
        }
    }
    return out;
}

// ---------- Messages ----------
MessagePlace attachTo(std::string_view message)
{
    MessagePlace place;
    // "<name>.post.js:<line>:" at the start: the script line of a script error.
    if (const std::size_t js = message.find(".post.js:"); js != std::string_view::npos && js < 200) {
        std::size_t p = js + 9;
        int line = 0;
        bool any = false;
        while (p < message.size() && digit(message[p])) {
            line = line * 10 + (message[p] - '0');
            any = true;
            ++p;
        }
        if (any && p < message.size() && message[p] == ':') {
            place.scriptLine = line;
            place.script = std::string(message.substr(0, js + 8));
            place.textStart = p + 1 < message.size() && message[p + 1] == ' ' ? p + 2 : p + 1;
        }
    }
    // "section <n>" anywhere: the posts name the operation they refuse or warn about so.
    for (std::size_t at = message.find("section "); at != std::string_view::npos;
         at = message.find("section ", at + 8)) {
        std::size_t p = at + 8;
        std::uint32_t n = 0;
        bool any = false;
        while (p < message.size() && digit(message[p])) {
            n = n * 10 + static_cast<std::uint32_t>(message[p] - '0');
            any = true;
            ++p;
        }
        if (any) {
            place.section = n;
            break;
        }
    }
    return place;
}

} // namespace ray::ui::camui
