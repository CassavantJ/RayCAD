// History checks shared by the boolean and blend tests (tasks T-094, T-095). No OCCT header here (PLAN.md 0.4 rule 1).
#pragma once
#include "kernel_test_util.h"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <thread>
#include <vector>

namespace ray::kernel::test {

// The normalisation guarantees of boolean_internal.h: every input sub-shape has exactly one Modified or Deleted
// entry, results are sorted and inside the result, and entries come in canonical order.
inline void checkHistory(const OpResult& op, std::initializer_list<const Shape*> inputs)
{
    const Shape& result = op.shape;
    std::vector<std::array<std::vector<int>, 4>> primary(inputs.size());
    int input = 0;
    for (const Shape* in : inputs) {
        for (int k = 0; k < 4; ++k)
            primary[input][k].assign(in->count(static_cast<SubKind>(k)), 0);
        ++input;
    }
    const HistoryEntry* previous = nullptr;
    for (const HistoryEntry& e : op.history.entries) {
        REQUIRE(e.inputIndex >= 0);
        REQUIRE(e.inputIndex < static_cast<int>(inputs.size()));
        const auto kind = static_cast<std::size_t>(e.source.kind);
        REQUIRE(e.source.index < primary[e.inputIndex][kind].size());
        if (e.relation != HistoryEntry::Relation::Generated)
            ++primary[e.inputIndex][kind][e.source.index];
        if (e.relation == HistoryEntry::Relation::Deleted)
            CHECK(e.results.empty());
        else
            CHECK_FALSE(e.results.empty());
        CHECK(std::is_sorted(e.results.begin(), e.results.end()));
        CHECK(std::adjacent_find(e.results.begin(), e.results.end()) == e.results.end());
        for (const SubId& id : e.results)
            CHECK(id.index < result.count(id.kind));
        if (e.relation == HistoryEntry::Relation::Modified)
            for (const SubId& id : e.results)
                CHECK(id.kind == e.source.kind);
        if (previous != nullptr) {
            const bool ordered = previous->inputIndex < e.inputIndex ||
                                 (previous->inputIndex == e.inputIndex && previous->source <= e.source);
            CHECK(ordered);
        }
        previous = &e;
    }
    for (const auto& perInput : primary)
        for (const auto& perKind : perInput)
            for (const int count : perKind)
                CHECK(count == 1);
}

// Result faces that no Modified or Generated entry lists.
inline std::vector<SubId> orphans(const OpResult& op)
{
    std::vector<char> covered(op.shape.count(SubKind::Face), 0);
    for (const HistoryEntry& e : op.history.entries)
        if (e.relation != HistoryEntry::Relation::Deleted)
            for (const SubId& id : e.results)
                if (id.kind == SubKind::Face)
                    covered[id.index] = 1;
    std::vector<SubId> out;
    for (std::uint32_t i = 0; i < covered.size(); ++i)
        if (covered[i] == 0)
            out.push_back(SubId{SubKind::Face, i});
    return out;
}

// The results of the entry with this relation for `source` of input `inputIndex`, or empty.
inline std::vector<SubId> resultsOf(const OpResult& op, HistoryEntry::Relation relation, int inputIndex, SubId source)
{
    for (const HistoryEntry& e : op.history.entries)
        if (e.inputIndex == inputIndex && e.source == source && e.relation == relation)
            return e.results;
    return {};
}

// Result faces each listed input face went to (Modified), or empty.
inline std::vector<SubId> modifiedTo(const OpResult& op, int inputIndex, SubId source)
{
    return resultsOf(op, HistoryEntry::Relation::Modified, inputIndex, source);
}

// Hash and history of an operation as bytes, for determinism comparisons.
inline void appendHash(std::vector<unsigned char>& out, const Shape& s)
{
    const Hash256 h = s.contentHash();
    const auto* bytes = reinterpret_cast<const unsigned char*>(&h);
    out.insert(out.end(), bytes, bytes + sizeof(h));
}
inline void appendHistory(std::vector<unsigned char>& out, const ShapeHistory& history)
{
    for (const HistoryEntry& e : history.entries) {
        out.push_back(static_cast<unsigned char>(e.relation));
        out.push_back(static_cast<unsigned char>(e.inputIndex));
        out.push_back(static_cast<unsigned char>(e.source.kind));
        appendBits(out, static_cast<double>(e.source.index));
        for (const SubId& r : e.results)
            appendBits(out, static_cast<double>(r.index) + 0.25 * static_cast<double>(r.kind));
    }
}

template <class F>
void runOnThreads(int count, F body)
{
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(count));
    for (int t = 0; t < count; ++t)
        threads.emplace_back([&body, t] { body(t); });
    for (std::thread& thread : threads)
        thread.join();
}

} // namespace ray::kernel::test
