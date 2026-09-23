/// \file
/// Bugs that were found, each pinned by the case that showed it. Every case
/// here failed - or crashed - on the code before its fix, and passes on every
/// backend after it. The comment on each says what it caught.

#include <atomic>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "support/harness.h"

namespace {

/// The location of what `source` throws when run, compiled under `origin`.
std::optional<ub::MessageLocation> LocationOfThrow(ub_test::Fixture& fixture, std::string_view source,
                                                   const ub::ScriptOrigin& origin) {
    const auto script = ub::Script::Compile(fixture.context, source, origin);
    REQUIRE(script.has_value());
    ub::TryCatch tryCatch(fixture.iso());
    CHECK_FALSE(script->Run(fixture.context).has_value());
    REQUIRE(tryCatch.HasCaught());
    return tryCatch.Location(fixture.context);
}

}  // namespace

UNIBIND_TEST_CASE(MESSAGE_LOCATION, "regressions: a location never quotes another script of the same name") {
    // The quoted line has to come from the script that raised the error. Two
    // scripts compiled under one name - and every script compiled without an
    // origin shares the default one - are still two scripts. A backend that
    // cannot tell which of them threw quotes neither; quoting the other one is
    // the answer that must not happen.
    ub_test::Fixture fixture;

    const auto thrower = ub::Script::Compile(fixture.context, "var a = 1;\nthrow new Error('first');");
    REQUIRE(thrower.has_value());
    const auto run = [&]() {
        ub::TryCatch tryCatch(fixture.iso());
        CHECK_FALSE(thrower->Run(fixture.context).has_value());
        REQUIRE(tryCatch.HasCaught());
        const auto location = tryCatch.Location(fixture.context);
        REQUIRE(location.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) - REQUIRE above guarantees has_value
        CHECK(location->lineNumber == 2);
        return location->sourceLine;  // NOLINT(bugprone-unchecked-optional-access) - as above
    };

    {
        const auto bystander = ub::Script::Compile(fixture.context, "var b = 2;\nvar c = 3;");
        REQUIRE(bystander.has_value());
        const auto quoted = run();
        CHECK((!quoted || *quoted == "throw new Error('first');"));
    }

    // Once the other script is gone, nothing is ambiguous and the line is
    // quoted on every backend.
    fixture.iso().RequestGarbageCollection();
    CHECK(run() == std::optional<std::string>("throw new Error('first');"));
}

UNIBIND_TEST_CASE(MESSAGE_LOCATION,
                  "regressions: a location quotes the line under every line terminator the language has") {
    // JavaScript ends a line at LF, CR, CRLF, U+2028 and U+2029, and both
    // engines number lines that way - so the quoted line has to be cut the same
    // way, or the number and the text disagree.
    ub_test::Fixture fixture;

    for (const std::string_view terminator : {std::string_view("\n"), std::string_view("\r"), std::string_view("\r\n"),
                                              std::string_view("\xE2\x80\xA8"), std::string_view("\xE2\x80\xA9")}) {
        CAPTURE(terminator.size());
        std::string source = "var a = 1;";
        source += terminator;
        source += "var b = 2;";
        source += terminator;
        source += "null.property;";
        source += terminator;
        source += "var c = 3;";
        const std::string name = "terminators-" + std::to_string(terminator.size()) + "-" +
                                 std::to_string(static_cast<std::uint8_t>(terminator.back())) + ".js";
        const auto location = LocationOfThrow(fixture, source, {.resourceName = name});
        REQUIRE(location.has_value());
        // NOLINTBEGIN(bugprone-unchecked-optional-access) - REQUIRE above guarantees has_value
        CHECK(location->lineNumber == 3);
        CHECK(location->sourceLine == std::optional<std::string>("null.property;"));
        // NOLINTEND(bugprone-unchecked-optional-access)
    }
}

UNIBIND_TEST_CASE2(MESSAGE_LOCATION, CODE_CACHE,
                   "regressions: a location quotes a script that came out of a code cache") {
    // A script made from a blob was never compiled from its text in this
    // process, and its line has to be quoted all the same.
    ub_test::Fixture fixture;

    constexpr std::string_view SOURCE = "var a = 1;\nnull.property;";
    const ub::ScriptOrigin origin{.resourceName = "cached.js"};
    std::vector<std::uint8_t> blob;
    {
        const auto first = ub::Script::Compile(fixture.context, SOURCE, origin);
        REQUIRE(first.has_value());
        const auto made = first->CreateCodeCache();
        REQUIRE(made.has_value());
        blob = *made;  // NOLINT(bugprone-unchecked-optional-access) - REQUIRE above guarantees has_value
    }
    const auto second = ub::Script::CompileWithCache(fixture.context, SOURCE, blob, origin);
    REQUIRE(second.has_value());
    ub::TryCatch tryCatch(fixture.iso());
    CHECK_FALSE(second->Run(fixture.context).has_value());
    REQUIRE(tryCatch.HasCaught());
    const auto location = tryCatch.Location(fixture.context);
    REQUIRE(location.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access) - REQUIRE above guarantees has_value
    CHECK(location->lineNumber == 2);
    CHECK(location->sourceLine == std::optional<std::string>("null.property;"));
    // NOLINTEND(bugprone-unchecked-optional-access)
}

UNIBIND_TEST_CASE(MESSAGE_LOCATION, "regressions: a column offset moves the first line's columns and no other's") {
    // `ScriptOrigin::columnOffset` says the source began that far into its
    // first line - an inline script in a page, a template's expression. Every
    // column on that line moves by it, and no column on any later line does.
    ub_test::Fixture fixture;

    const auto firstPlain = LocationOfThrow(fixture, "null.property;", {.resourceName = "col.js"});
    const auto firstMoved = LocationOfThrow(fixture, "null.property;", {.resourceName = "col.js", .columnOffset = 10});
    const auto secondPlain = LocationOfThrow(fixture, "var a = 1;\nnull.property;", {.resourceName = "col.js"});
    const auto secondMoved =
        LocationOfThrow(fixture, "var a = 1;\nnull.property;", {.resourceName = "col.js", .columnOffset = 10});
    REQUIRE(firstPlain.has_value());
    REQUIRE(firstMoved.has_value());
    REQUIRE(secondPlain.has_value());
    REQUIRE(secondMoved.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access) - REQUIRE above guarantees has_value
    CHECK(firstMoved->columnNumber == firstPlain->columnNumber + 10);
    CHECK(secondMoved->columnNumber == secondPlain->columnNumber);
    // NOLINTEND(bugprone-unchecked-optional-access)
}

namespace {

std::atomic<int> g_delayedRuns{0};

void CountDelayedRun(ub::Isolate& /*isolate*/, ub::CallbackData /*data*/) {
    g_delayedRuns.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

UNIBIND_TEST_CASE(DELAYED_JOBS, "regressions: a delay too long for the clock is a long delay, not none") {
    // The delay is a double and the steady clock counts integer nanoseconds,
    // which run out a little under three hundred years from now. A delay past
    // that - infinity included - must still be a floor nothing reaches, not an
    // overflow that lands in the past and runs at the next pump.
    ub_test::Fixture fixture;
    g_delayedRuns = 0;

    auto& isolate = fixture.iso();
    isolate.PostDelayedJob(&CountDelayedRun, {}, std::numeric_limits<double>::infinity());
    isolate.PostDelayedJob(&CountDelayedRun, {}, 1e12);
    isolate.PostDelayedJob(&CountDelayedRun, {}, std::numeric_limits<double>::max());
    isolate.PostDelayedJob(&CountDelayedRun, {}, 9.3e9);
    isolate.PumpJobs();
    CHECK(g_delayedRuns.load() == 0);
}

UNIBIND_TEST_CASE(OBJECTS, "regressions: defining a property an object refuses is false, not a throw") {
    // V8's `DefineOwnProperty` answers the way `Reflect.defineProperty` does:
    // a definition the object refuses - it is frozen, or the property is not
    // configurable - is `false`, with nothing thrown. Empty would mean script
    // threw, and nothing did.
    ub_test::Fixture fixture;

    const auto frozen = ub_test::Eval(fixture.context, "Object.freeze({ kept: 1 })").To<ub::Object>();
    REQUIRE(frozen.has_value());

    ub::TryCatch tryCatch(fixture.iso());
    // NOLINTBEGIN(bugprone-unchecked-optional-access) - REQUIRE above guarantees has_value
    const auto added = frozen->DefineOwnProperty(fixture.context, ub_test::Str(fixture.iso(), "added"),
                                                 ub::Integer::New(fixture.iso(), 2));
    CHECK(added == std::optional<bool>(false));
    CHECK_FALSE(tryCatch.HasCaught());
    tryCatch.Reset();

    const auto changed = frozen->DefineOwnProperty(fixture.context, ub_test::Str(fixture.iso(), "kept"),
                                                   ub::Integer::New(fixture.iso(), 3));
    CHECK(changed == std::optional<bool>(false));
    CHECK_FALSE(tryCatch.HasCaught());
    // NOLINTEND(bugprone-unchecked-optional-access)
}
