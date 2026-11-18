// Fuzzy action search, private to src/api. A pure function of its inputs: integer scores, no locale, no hashing,
// ties broken by id, so the ranking is identical on every run and platform.
#pragma once

#include <ray/api.h>

#include <string_view>
#include <vector>

namespace ray::api::search {

// Score of `query` against one action; 0 means no match. Matching ignores ASCII case.
//  - against the id's last segment ("save" of "file.save"), the title and the full id: an exact match scores
//    highest, then a prefix, then a prefix of a word inside it (words split at '.', ' ', '-', '_' and lower-to-upper
//    case changes), then a substring, then a subsequence (characters in order, rewarding word starts and runs);
//  - shorter targets win among equal kinds of match, so "sav" ranks "file.save" above "file.saveAs";
//  - category and description count with a lower weight.
int score(std::string_view query, const ActionInfo& action);

// Matching actions, best first; ties by id. limit <= 0 means all.
std::vector<ActionInfo> rank(std::string_view query, std::vector<ActionInfo> actions, int limit);

} // namespace ray::api::search
