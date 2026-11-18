// Helpers shared by the command, undo and preview tests (task T-009).
#pragma once

#include "document_impl.h"
#include "test_dispatcher.h"

#include <ray/document.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace ray::doc::test {

// A fixed, valid, lower-case UUID per (kind, n), e.g. uuid('f', 3) = "f0000000-0000-4000-8000-000000000003".
inline std::string uuid(char kind, std::uint64_t n)
{
    static const char* const hex = "0123456789abcdef";
    std::string tail(12, '0');
    for (int i = 11; i >= 0; --i) {
        tail[static_cast<std::size_t>(i)] = hex[n % 16];
        n /= 16;
    }
    return std::string(1, kind) + "0000000-0000-4000-8000-" + tail;
}

inline Command cmd(std::string name, std::string args, std::string label = {})
{
    return Command{std::move(name), std::move(args), std::move(label)};
}

inline std::string hashOf(const DocStatePtr& state)
{
    REQUIRE(state != nullptr);
    return state->stateHash().hex();
}

// A document on the built-in commands whose notifications run when the test drains `ui`.
struct DocFixture {
    ray::test::TestDispatcher ui;
    std::unique_ptr<Document> doc = makeDocument(ui);

    DocStatePtr run(const Command& command)
    {
        auto result = doc->execute(command);
        if (!result) {
            FAIL_CHECK(command.name << " " << command.argsJson << " failed: " << result.error().message);
            return nullptr;
        }
        return *result;
    }

    Error fails(const Command& command)
    {
        auto result = doc->execute(command);
        if (result) {
            FAIL_CHECK(command.name << " " << command.argsJson << " should have failed");
            return Error{ErrorCode::Internal, "did not fail", {}};
        }
        return result.error();
    }
};

inline std::string featureAddArgs(std::uint64_t n, const std::string& extra = {})
{
    return R"({"id":")" + uuid('f', n) + R"(","type":"extrude","name":"Extrude )" + std::to_string(n) +
           R"(","props":{"distance":")" + std::to_string(n) + R"( mm"})" + extra + "}";
}

} // namespace ray::doc::test
