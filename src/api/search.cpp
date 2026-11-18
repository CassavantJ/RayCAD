#include "search.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

namespace ray::api::search {
namespace {

char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string lowered(std::string_view s)
{
    std::string out(s);
    for (char& c : out) {
        c = lower(c);
    }
    return out;
}

bool isSeparator(char c)
{
    return c == '.' || c == ' ' || c == '-' || c == '_' || c == '/' || c == ':';
}

// Positions where a word starts in the original (not lowered) text.
std::vector<bool> wordStarts(std::string_view original)
{
    std::vector<bool> starts(original.size(), false);
    for (std::size_t i = 0; i < original.size(); ++i) {
        if (isSeparator(original[i])) {
            continue;
        }
        const bool afterSeparator = i == 0 || isSeparator(original[i - 1]);
        const bool caseChange =
            i > 0 && original[i] >= 'A' && original[i] <= 'Z' && original[i - 1] >= 'a' && original[i - 1] <= 'z';
        starts[i] = afterSeparator || caseChange;
    }
    return starts;
}

int lengthPenalty(std::size_t extra)
{
    return static_cast<int>(std::min<std::size_t>(extra, 99));
}

// 0 = no match. Bands: exact 1000, prefix 800, word prefix 600, substring 400, subsequence 100..399.
int fieldScore(const std::string& q, std::string_view original)
{
    if (q.empty() || original.empty()) {
        return 0;
    }
    const std::string t = lowered(original);
    if (q.size() > t.size()) {
        return 0;
    }
    const std::size_t extra = t.size() - q.size();
    if (t == q) {
        return 1000;
    }
    if (t.compare(0, q.size(), q) == 0) {
        return 800 - lengthPenalty(extra);
    }
    const std::vector<bool> starts = wordStarts(original);
    for (std::size_t i = 1; i + q.size() <= t.size(); ++i) {
        if (starts[i] && t.compare(i, q.size(), q) == 0) {
            return 600 - lengthPenalty(extra) - static_cast<int>(std::min<std::size_t>(i, 50));
        }
    }
    if (const std::size_t at = t.find(q); at != std::string::npos) {
        return 400 - lengthPenalty(extra) - static_cast<int>(std::min<std::size_t>(at, 50));
    }
    // Subsequence: greedy, preferring a word start for each query character when one is ahead.
    int bonus = 0;
    std::size_t pos = 0;
    std::size_t gaps = 0;
    std::size_t last = std::string::npos;
    for (const char c : q) {
        std::size_t found = std::string::npos;
        for (std::size_t i = pos; i < t.size(); ++i) {
            if (t[i] == c && starts[i]) {
                found = i;
                break;
            }
        }
        // A word start is only worth jumping to when no adjacent continuation exists.
        if (last != std::string::npos && last + 1 < t.size() && t[last + 1] == c) {
            found = last + 1;
        }
        if (found == std::string::npos) {
            found = t.find(c, pos);
        }
        if (found == std::string::npos) {
            return 0;
        }
        if (starts[found]) {
            bonus += 8;
        }
        if (last != std::string::npos && found == last + 1) {
            bonus += 5;
        } else if (last != std::string::npos) {
            gaps += found - last - 1;
        }
        last = found;
        pos = found + 1;
    }
    const int value = 200 + bonus - static_cast<int>(std::min<std::size_t>(gaps, 100)) - lengthPenalty(extra);
    return std::clamp(value, 100, 399);
}

std::string_view lastSegment(std::string_view id)
{
    const std::size_t dot = id.rfind('.');
    return dot == std::string_view::npos ? id : id.substr(dot + 1);
}

// Titles may end in an ellipsis ("Save As..."); it is not part of the name.
std::string_view trimTitle(std::string_view title)
{
    while (!title.empty() && (title.back() == '.' || title.back() == ' ')) {
        title.remove_suffix(1);
    }
    constexpr std::string_view kEllipsis = "\xE2\x80\xA6";
    if (title.size() >= kEllipsis.size() && title.substr(title.size() - kEllipsis.size()) == kEllipsis) {
        title.remove_suffix(kEllipsis.size());
    }
    return title;
}

} // namespace

int score(std::string_view query, const ActionInfo& action)
{
    // Trim and lower the query; inner spaces are kept (titles have spaces).
    std::size_t b = 0;
    std::size_t e = query.size();
    while (b < e && query[b] == ' ') {
        ++b;
    }
    while (e > b && query[e - 1] == ' ') {
        --e;
    }
    const std::string q = lowered(query.substr(b, e - b));
    if (q.empty()) {
        return 1;
    }
    // Weights in tenths.
    int best = 0;
    best = std::max(best, fieldScore(q, lastSegment(action.id)) * 10);
    best = std::max(best, fieldScore(q, trimTitle(action.title)) * 10);
    best = std::max(best, fieldScore(q, action.id) * 9);
    best = std::max(best, fieldScore(q, action.category) * 5);
    // Descriptions are long: only a substring or word match counts there.
    const int description = fieldScore(q, action.description);
    if (description >= 400) {
        best = std::max(best, description * 3);
    }
    return best;
}

std::vector<ActionInfo> rank(std::string_view query, std::vector<ActionInfo> actions, int limit)
{
    std::vector<std::pair<int, ActionInfo>> scored;
    scored.reserve(actions.size());
    for (ActionInfo& action : actions) {
        const int s = score(query, action);
        if (s > 0) {
            scored.emplace_back(s, std::move(action));
        }
    }
    std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) {
            return a.first > b.first;
        }
        if (a.second.id.size() != b.second.id.size()) {
            return a.second.id.size() < b.second.id.size();
        }
        return a.second.id < b.second.id;
    });
    std::vector<ActionInfo> out;
    for (auto& [s, action] : scored) {
        if (limit > 0 && static_cast<int>(out.size()) >= limit) {
            break;
        }
        out.push_back(std::move(action));
    }
    return out;
}

} // namespace ray::api::search
