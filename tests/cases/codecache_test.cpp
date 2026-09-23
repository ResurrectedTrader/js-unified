/// \file
/// Compiled-code caching: bytes out of one compile, into the next.
///
/// The interesting half is not the hit, it is the miss. A blob the engine
/// declines is *supposed* to produce an ordinary compile, and an embedder whose
/// blobs have silently stopped being accepted - a new engine build, different
/// flags, source that changed - sees nothing wrong except a slower start.
/// `UsedCodeCache()` is what makes that visible, so every case here asserts it
/// as well as the behaviour.
///
/// Nothing here looks inside a blob. A blob is opaque bytes belonging to one
/// engine build (`unibind/script.h`), so the only portable questions are whether it
/// was accepted and whether the script behaves identically either way.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "support/harness.h"

namespace {

constexpr std::string_view SOURCE = R"(
function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
function make(n) { return { total: fib(n), text: 'n=' + n }; }
make(12).total;
)";

/// Compile, run, and ask the engine for a blob. Empty if it declined to give
/// one, which it is allowed to do.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> BlobFor(const ub::Context& context, std::string_view source) {
    const auto script = ub::Script::Compile(context, source, {.resourceName = "cache-test.js"});
    REQUIRE(script.has_value());
    REQUIRE(script->Run(context).has_value());
    return script->CreateCodeCache();
}

/// What the source evaluates to, so "behaves identically" is a comparison and
/// not a hope.
[[nodiscard]] std::int32_t ResultOf(const ub::Context& context, const ub::Script& script) {
    const auto value = script.Run(context);
    REQUIRE(value.has_value());
    const auto asInt = value->ToInt32(context);
    REQUIRE(asInt.has_value());
    return *asInt;
}

}  // namespace

UNIBIND_TEST_CASE(CODE_CACHE, "code cache: a blob from one compile is consumed by the next") {
    ub_test::Fixture fixture;

    const auto blob = BlobFor(fixture.context, SOURCE);
    if (!blob || blob->empty()) {
        ub_test::ReportSkip("this engine declined to produce a code cache for the test source");
        return;
    }

    const auto second = ub::Script::CompileWithCache(fixture.context, SOURCE, *blob, {.resourceName = "cache-test.js"});
    REQUIRE(second.has_value());
    CHECK(second->UsedCodeCache());
    CHECK(ResultOf(fixture.context, *second) == 144);
}

UNIBIND_TEST_CASE(CODE_CACHE, "code cache: a script compiled without a blob says it used none") {
    ub_test::Fixture fixture;

    const auto plain = ub::Script::Compile(fixture.context, SOURCE, {.resourceName = "cache-test.js"});
    REQUIRE(plain.has_value());
    CHECK_FALSE(plain->UsedCodeCache());
    CHECK(ResultOf(fixture.context, *plain) == 144);
}

UNIBIND_TEST_CASE(CODE_CACHE, "code cache: a damaged blob compiles normally and behaves identically") {
    // The case a real embedder is hurt by, because nothing else about it looks
    // wrong. The script has to come out the same; only `UsedCodeCache()` is
    // allowed to differ.
    ub_test::Fixture fixture;

    auto blob = BlobFor(fixture.context, SOURCE);
    if (!blob || blob->size() < 32) {
        ub_test::ReportSkip("this engine declined to produce a code cache for the test source");
        return;
    }

    SUBCASE("bytes flipped in the middle") {
        auto damaged = *blob;
        for (std::size_t at = damaged.size() / 2; at < (damaged.size() / 2) + 16; ++at) {
            damaged[at] = static_cast<std::uint8_t>(~damaged[at]);
        }
        const auto script =
            ub::Script::CompileWithCache(fixture.context, SOURCE, damaged, {.resourceName = "cache-test.js"});
        REQUIRE(script.has_value());
        CHECK_FALSE(script->UsedCodeCache());
        CHECK(ResultOf(fixture.context, *script) == 144);
    }

    SUBCASE("truncated") {
        const std::vector<std::uint8_t> damaged(blob->begin(),
                                                blob->begin() + static_cast<std::ptrdiff_t>(blob->size() / 3));
        const auto script =
            ub::Script::CompileWithCache(fixture.context, SOURCE, damaged, {.resourceName = "cache-test.js"});
        REQUIRE(script.has_value());
        CHECK_FALSE(script->UsedCodeCache());
        CHECK(ResultOf(fixture.context, *script) == 144);
    }

    SUBCASE("not a blob at all") {
        const std::vector<std::uint8_t> nonsense(256, 0x5A);
        const auto script =
            ub::Script::CompileWithCache(fixture.context, SOURCE, nonsense, {.resourceName = "cache-test.js"});
        REQUIRE(script.has_value());
        CHECK_FALSE(script->UsedCodeCache());
        CHECK(ResultOf(fixture.context, *script) == 144);
    }

    SUBCASE("empty") {
        const std::vector<std::uint8_t> nothing;
        const auto script =
            ub::Script::CompileWithCache(fixture.context, SOURCE, nothing, {.resourceName = "cache-test.js"});
        REQUIRE(script.has_value());
        CHECK_FALSE(script->UsedCodeCache());
        CHECK(ResultOf(fixture.context, *script) == 144);
    }
}

UNIBIND_TEST_CASE(CODE_CACHE, "code cache: a blob for other source is declined rather than believed") {
    ub_test::Fixture fixture;

    const auto blob = BlobFor(fixture.context, SOURCE);
    if (!blob || blob->empty()) {
        ub_test::ReportSkip("this engine declined to produce a code cache for the test source");
        return;
    }

    constexpr std::string_view OTHER = "function other() { return 'quite different'; } other();";
    const auto script = ub::Script::CompileWithCache(fixture.context, OTHER, *blob, {.resourceName = "cache-test.js"});
    REQUIRE(script.has_value());
    CHECK_FALSE(script->UsedCodeCache());

    const auto value = script->Run(fixture.context);
    REQUIRE(value.has_value());
    // Coerced rather than read as a string, so that a backend which ran the
    // *cached* script instead of the source it was given says what it ran.
    const auto asText = value->ToString(fixture.context);
    REQUIRE(asText.has_value());
    CHECK(asText->Utf8Value() == "quite different");
}

UNIBIND_TEST_CASE(CODE_CACHE, "code cache: a blob is keyed to its origin as well as its source") {
    // The origin is part of what a blob is for: the same text compiled as
    // `a.js` and as `b.js` reports different positions in a diagnostic, so a
    // blob made under one is not a blob for the other. And the question is
    // asked the only way it can be answered honestly - by running the result
    // and looking at what came out, not by believing what the API reported.
    ub_test::Fixture fixture;

    constexpr std::string_view SAYS_WHERE = "(function () { try { null.x; } catch (e) { return e.stack; } })()";

    const auto first = ub::Script::Compile(fixture.context, SAYS_WHERE, {.resourceName = "first.js"});
    REQUIRE(first.has_value());
    REQUIRE(first->Run(fixture.context).has_value());
    const auto blob = first->CreateCodeCache();
    if (!blob || blob->empty()) {
        ub_test::ReportSkip("this engine declined to produce a code cache for the test source");
        return;
    }

    const auto elsewhere =
        ub::Script::CompileWithCache(fixture.context, SAYS_WHERE, *blob, {.resourceName = "second.js"});
    REQUIRE(elsewhere.has_value());
    CHECK_FALSE(elsewhere->UsedCodeCache());

    const auto ran = elsewhere->Run(fixture.context);
    REQUIRE(ran.has_value());
    const auto asText = ran->ToString(fixture.context);
    REQUIRE(asText.has_value());
    // What ran has to be the script that was asked for, not the cached one.
    CHECK(asText->Utf8Value().find("second.js") != std::string::npos);
    CHECK(asText->Utf8Value().find("first.js") == std::string::npos);
}

UNIBIND_TEST_CASE(CODE_CACHE, "code cache: source that does not compile is empty, blob or no blob") {
    ub_test::Fixture fixture;

    ub::TryCatch handler(fixture.iso());
    const std::vector<std::uint8_t> nonsense(64, 0x11);
    CHECK_FALSE(ub::Script::CompileWithCache(fixture.context, "this is not javascript", nonsense).has_value());
    CHECK(handler.HasCaught());
    handler.Reset();
}

UNIBIND_TEST_CASE(CODE_CACHE, "code cache: a cached compile reaches the realm it is run in") {
    // Decision 10 through the cache: a `Script` is not a member of a realm, and
    // a cached one is not either.
    ub_test::Fixture fixture;

    constexpr std::string_view READS_A_GLOBAL = "typeof whereAmI === 'undefined' ? 'nowhere' : whereAmI";
    const auto first = ub::Script::Compile(fixture.context, READS_A_GLOBAL);
    REQUIRE(first.has_value());
    REQUIRE(first->Run(fixture.context).has_value());
    const auto blob = first->CreateCodeCache();
    if (!blob || blob->empty()) {
        ub_test::ReportSkip("this engine declined to produce a code cache for the test source");
        return;
    }

    const auto cached = ub::Script::CompileWithCache(fixture.context, READS_A_GLOBAL, *blob);
    REQUIRE(cached.has_value());

    auto second = ub::Context::New(fixture.iso());
    REQUIRE(second.has_value());
    {
        ub::ContextScope entered(*second);
        REQUIRE(second->GlobalObject()
                    .Set(*second, "whereAmI", *ub::String::New(fixture.iso(), "the second realm"))
                    .value_or(false));
        const auto there = cached->Run(*second);
        REQUIRE(there.has_value());
        CHECK(ub_test::TextOf(*there) == "the second realm");
    }
    const auto here = cached->Run(fixture.context);
    REQUIRE(here.has_value());
    CHECK(ub_test::TextOf(*here) == "nowhere");
}

// ---------------------------------------------------------------------------
// Eager compilation: a blob that covers the whole file
//
// Both engines compile a function's body on its first call, so a blob made
// straight after an ordinary compile covers the top level and nothing inside a
// function. `CompileOptions::EagerCompile` is how an embedder that keeps blobs
// gets one that covers everything. What a blob holds is opaque, so the one
// portable way to see the difference is its size: more compiled code is more
// bytes, on both engines, by a wide margin for source that is mostly function
// bodies.
// ---------------------------------------------------------------------------

namespace {

/// Mostly function bodies, none of which the top level calls.
[[nodiscard]] std::string ManyFunctions() {
    std::string source;
    for (int i = 0; i < 40; ++i) {
        const std::string n = std::to_string(i);
        source.append("function f").append(n).append("(a) { var total = a * ").append(n);
        source.append("; for (var k = 0; k < a; ++k) { total += k % ").append(std::to_string(i + 2));
        source.append("; } return function () { return total + ").append(n).append("; }; }\n");
    }
    source += "40;\n";
    return source;
}

/// The blob a compile with these options gives before anything runs. Empty if
/// the engine declined to make one.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> BlobBeforeRunning(const ub::Context& context,
                                                                         std::string_view source,
                                                                         ub::CompileOptions options) {
    const auto script = ub::Script::Compile(context, source, {.resourceName = "eager.js"}, options);
    REQUIRE(script.has_value());
    return script->CreateCodeCache();
}

}  // namespace

UNIBIND_TEST_CASE(EAGER_COMPILE, "code cache: an eager compile's blob covers functions that never ran") {
    ub_test::Fixture fixture;
    const std::string source = ManyFunctions();

    const auto lazy = BlobBeforeRunning(fixture.context, source, ub::CompileOptions::NoCompileOptions);
    // The same source, again, in the same isolate: an engine that answers a
    // repeat compile from its own cache must not answer this one with the lazy
    // result it kept.
    const auto eager = BlobBeforeRunning(fixture.context, source, ub::CompileOptions::EagerCompile);
    if (!lazy || !eager) {
        ub_test::ReportSkip("this engine declined to produce a code cache for the test source");
        return;
    }
    MESSAGE("lazy blob ", lazy->size(), " bytes, eager blob ", eager->size(), " bytes");
    CHECK(eager->size() > lazy->size() + (lazy->size() / 2));

    // And the eager compile is an ordinary script.
    const auto script =
        ub::Script::Compile(fixture.context, source, {.resourceName = "eager.js"}, ub::CompileOptions::EagerCompile);
    REQUIRE(script.has_value());
    CHECK_FALSE(script->UsedCodeCache());
    CHECK(ResultOf(fixture.context, *script) == 40);
    CHECK(ub_test::EvalInt(fixture.context, "f3(4)()") == (3 * 4) + (0 + 1 + 2 + 3) + 3);
}

UNIBIND_TEST_CASE(EAGER_COMPILE, "code cache: an eager blob is consumed by an ordinary compile in another isolate") {
    const std::string source = ManyFunctions();

    std::optional<std::vector<std::uint8_t>> blob;
    {
        ub_test::Fixture writer;
        blob = BlobBeforeRunning(writer.context, source, ub::CompileOptions::EagerCompile);
    }
    if (!blob) {
        ub_test::ReportSkip("this engine declined to produce a code cache for the test source");
        return;
    }

    ub_test::Fixture reader;
    const auto script = ub::Script::CompileWithCache(reader.context, source, *blob, {.resourceName = "eager.js"});
    REQUIRE(script.has_value());
    CHECK(script->UsedCodeCache());
    CHECK(ResultOf(reader.context, *script) == 40);
    CHECK(ub_test::EvalInt(reader.context, "f5(2)()") == (5 * 2) + (0 + 1) + 5);
}

UNIBIND_TEST_CASE(EAGER_COMPILE, "code cache: asking for eager with a refused blob still compiles eagerly") {
    // The case the option is for: an embedder whose kept blob went stale, and
    // which is about to make a fresh one. A fallback to a lazy compile would
    // make the fresh blob cover nothing, which is the silent failure.
    ub_test::Fixture fixture;
    const std::string source = ManyFunctions();

    const auto lazy = BlobBeforeRunning(fixture.context, source, ub::CompileOptions::NoCompileOptions);
    const auto stale = BlobBeforeRunning(fixture.context, "1 + 1;", ub::CompileOptions::NoCompileOptions);
    if (!lazy || !stale) {
        ub_test::ReportSkip("this engine declined to produce a code cache for the test source");
        return;
    }

    const auto script = ub::Script::CompileWithCache(fixture.context, source, *stale, {.resourceName = "eager.js"},
                                                     ub::CompileOptions::EagerCompile);
    REQUIRE(script.has_value());
    CHECK_FALSE(script->UsedCodeCache());
    const auto fresh = script->CreateCodeCache();
    REQUIRE(fresh.has_value());
    MESSAGE("lazy blob ", lazy->size(), " bytes, blob after a refused-then-eager compile ", fresh->size(), " bytes");
    CHECK(fresh->size() > lazy->size() + (lazy->size() / 2));
    CHECK(ResultOf(fixture.context, *script) == 40);
}
