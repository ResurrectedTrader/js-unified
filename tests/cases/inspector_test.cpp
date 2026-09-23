/// \file
/// The inspector: Chrome DevTools, over the protocol, where the engine has one.
///
/// Two kinds of case, and the split is `unibind/inspector.h`'s own. Whether an
/// inspector exists is a question every backend answers - `Supported()` - and
/// the first case holds every backend to answering it consistently. What an
/// inspector *does* is a question only one that has one can answer, so the
/// rest ask `Supported()` first and report a skip when it says no. They never
/// ask which backend they are on: the suite does not name engines, and a third
/// backend with an inspector should pass these unchanged.
///
/// The protocol is asserted only as far as it has to be - an id came back, a
/// value is in it, a notification of a given method arrived - by looking for
/// text in what the client was sent. Everything else about a message is the
/// engine's.

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "support/harness.h"

namespace {

/// A client that keeps what it is sent, and runs a pause by dispatching what
/// the case queued for it.
struct Client final : ub::InspectorClient {
    Client() = default;

    void SendProtocolMessage(std::string_view message) override { messages.emplace_back(message); }

    void RunMessageLoopOnPause() override {
        ++pauses;
        quit = false;
        for (const std::string& message : onPause) {
            if (quit) {
                break;
            }
            session->DispatchProtocolMessage(message);
        }
        // Nothing left to feed and still paused: a real loop would wait on its
        // socket, and a case that got here would wait for ever. Resuming from
        // outside the protocol ends it the other way.
        if (!quit) {
            session->Resume();
        }
    }

    void QuitMessageLoopOnPause() override {
        ++quits;
        quit = true;
    }

    std::optional<std::string> ResourceNameToUrl(std::string_view resourceName) override {
        if (!urlPrefix) {
            return std::nullopt;
        }
        return *urlPrefix + std::string(resourceName);
    }

    /// Whether any message sent so far contains `needle`.
    [[nodiscard]] bool Saw(std::string_view needle) const {
        for (const std::string& message : messages) {
            if (message.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    /// The response to request `id`, or empty if there was none.
    [[nodiscard]] std::string ResponseTo(int id) const {
        const std::string marker = "\"id\":" + std::to_string(id) + ",";
        for (const std::string& message : messages) {
            if (message.find(marker) != std::string::npos) {
                return message;
            }
        }
        return {};
    }

    std::vector<std::string> messages;
    std::vector<std::string> onPause;
    ub::InspectorSession* session = nullptr;
    std::optional<std::string> urlPrefix;
    int pauses = 0;
    int quits = 0;
    bool quit = false;
};

/// An inspector over the fixture's realm, with one session connected - or a
/// skip, when the backend has no inspector.
struct Attached {
    std::unique_ptr<ub::Inspector> inspector;
    std::unique_ptr<ub::InspectorSession> session;

    [[nodiscard]] explicit operator bool() const noexcept { return session != nullptr; }
};

[[nodiscard]] Attached Attach(ub_test::Fixture& fixture, Client& client) {
    Attached attached;
    if (!ub::Inspector::Supported()) {
        ub_test::ReportSkip("this backend has no inspector");
        return attached;
    }
    attached.inspector = ub::Inspector::New(fixture.iso(), client);
    REQUIRE(attached.inspector != nullptr);
    attached.inspector->ContextCreated(fixture.context, "main");
    attached.session = attached.inspector->Connect();
    REQUIRE(attached.session != nullptr);
    client.session = attached.session.get();
    return attached;
}

/// A dispatch made from another thread, and where and whether it ran.
struct Remote {
    ub::InspectorSession* session = nullptr;
    std::string message;
    std::thread::id ranOn;
    std::atomic<bool> ran{false};
};

void DispatchRemote(ub::Isolate& /*isolate*/, ub::CallbackData data) {
    auto* remote = data.As<Remote>();
    if (remote == nullptr) {
        return;
    }
    remote->ranOn = std::this_thread::get_id();
    remote->session->DispatchProtocolMessage(remote->message);
    remote->ran = true;
}

}  // namespace

UNIBIND_TEST_CASE(INSPECTOR, "inspector: an inspector exists exactly where the backend says it does") {
    // The one question every backend answers, and it links everywhere: a
    // program compiled once asks, and is told.
    ub_test::Fixture fixture;
    Client client;

    auto inspector = ub::Inspector::New(fixture.iso(), client);
    CHECK((inspector != nullptr) == ub::Inspector::Supported());
    MESSAGE("this backend ", std::string(ub::Inspector::Supported() ? "has an" : "has no"), " inspector");
    if (inspector == nullptr) {
        return;
    }

    // One per isolate, and another once that one has gone.
    Client second;
    CHECK(ub::Inspector::New(fixture.iso(), second) == nullptr);
    inspector.reset();
    CHECK(ub::Inspector::New(fixture.iso(), second) != nullptr);
}

UNIBIND_TEST_CASE(INSPECTOR, "inspector: Runtime.evaluate is answered through the client") {
    ub_test::Fixture fixture;
    Client client;
    const Attached attached = Attach(fixture, client);
    if (!attached) {
        return;
    }

    attached.session->DispatchProtocolMessage(R"({"id":1,"method":"Runtime.evaluate","params":{"expression":"6*7"}})");
    const std::string answer = client.ResponseTo(1);
    INFO(answer);
    CHECK(answer.find("\"value\":42") != std::string::npos);

    // It runs in the realm the inspector was shown, and sees its globals.
    ub_test::Expose(fixture.context, "fromEmbedder", ub_test::Str(fixture.iso(), "hello"));
    attached.session->DispatchProtocolMessage(
        R"({"id":2,"method":"Runtime.evaluate","params":{"expression":"fromEmbedder.length"}})");
    CHECK(client.ResponseTo(2).find("\"value\":5") != std::string::npos);

    // And what goes in is UTF-8: a two-byte character is one character.
    attached.session->DispatchProtocolMessage(
        "{\"id\":3,\"method\":\"Runtime.evaluate\",\"params\":{\"expression\":\"'\xC3\xA9'.length\"}}");
    CHECK(client.ResponseTo(3).find("\"value\":1") != std::string::npos);
}

UNIBIND_TEST_CASE(INSPECTOR, "inspector: a debugger statement pauses into the client's loop until it resumes") {
    ub_test::Fixture fixture;
    Client client;
    const Attached attached = Attach(fixture, client);
    if (!attached) {
        return;
    }

    attached.session->DispatchProtocolMessage(R"({"id":10,"method":"Debugger.enable"})");
    REQUIRE_FALSE(client.ResponseTo(10).empty());
    client.onPause = {R"({"id":11,"method":"Debugger.resume"})"};

    // The pause happens inside this call, and the embedder's loop is what ends
    // it; the script then finishes as though nothing had happened.
    CHECK(ub_test::EvalInt(fixture.context, "var before = 1; debugger; before + 1") == 2);
    CHECK(client.pauses == 1);
    CHECK(client.quits >= 1);
    CHECK(client.Saw("\"method\":\"Debugger.paused\""));
    CHECK(client.Saw("\"method\":\"Debugger.resumed\""));
    CHECK_FALSE(client.ResponseTo(11).empty());

    // A pause with nothing to feed it is left from outside the protocol.
    client.onPause.clear();
    CHECK(ub_test::EvalInt(fixture.context, "debugger; 3") == 3);
    CHECK(client.pauses == 2);
}

UNIBIND_TEST_CASE(INSPECTOR, "inspector: a dispatch requested from another thread reaches a busy isolate") {
    // The case RequestDispatch exists for: DevTools asks while a script is
    // running, on a thread that is not the isolate's, and gets an answer -
    // here, one that stops the script.
    ub_test::Fixture fixture;
    Client client;
    const Attached attached = Attach(fixture, client);
    if (!attached) {
        return;
    }

    Remote remote;
    remote.session = attached.session.get();
    remote.message =
        R"({"id":20,"method":"Runtime.evaluate","params":{"expression":"globalThis.stopLooping = true; 42"}})";
    std::atomic<bool> finished{false};

    ub::Inspector* inspector = attached.inspector.get();
    std::thread requester([inspector, &remote] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        inspector->RequestDispatch(&DispatchRemote, ub::CallbackData::For(remote));
    });
    // A request that never lands would leave the loop below spinning for ever;
    // this turns that into a failure instead of a hung suite.
    ub::Isolate* isolate = &fixture.iso();
    std::thread watchdog([isolate, &finished] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!finished && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!finished) {
            isolate->TerminateExecution();
        }
    });

    ub_test::Expose(fixture.context, "stopLooping", ub::False(fixture.iso()));
    const auto result = ub::Evaluate(fixture.context, "while (!globalThis.stopLooping) {} 'finished'");
    finished = true;
    requester.join();
    watchdog.join();
    fixture.iso().CancelTerminateExecution();

    REQUIRE(result.has_value());
    CHECK(ub_test::TextOf(*result) == "finished");
    CHECK(remote.ran);
    CHECK(remote.ranOn == std::this_thread::get_id());
    CHECK(client.ResponseTo(20).find("\"value\":42") != std::string::npos);
}

UNIBIND_TEST_CASE(INSPECTOR, "inspector: a dispatch requested while the isolate is idle runs at the next pump") {
    ub_test::Fixture fixture;
    Client client;
    const Attached attached = Attach(fixture, client);
    if (!attached) {
        return;
    }

    Remote remote;
    remote.session = attached.session.get();
    remote.message = R"({"id":30,"method":"Runtime.evaluate","params":{"expression":"'idle'"}})";

    ub::Inspector* inspector = attached.inspector.get();
    std::thread requester(
        [inspector, &remote] { inspector->RequestDispatch(&DispatchRemote, ub::CallbackData::For(remote)); });
    requester.join();

    // No script is running, so nothing has checked for it yet.
    CHECK_FALSE(remote.ran);
    fixture.iso().PumpJobs();
    CHECK(remote.ran);
    CHECK(remote.ranOn == std::this_thread::get_id());
    CHECK(client.ResponseTo(30).find("idle") != std::string::npos);

    // Once: the interrupt that was also requested finds nothing left to run.
    remote.ran = false;
    CHECK(ub_test::EvalInt(fixture.context, "1 + 1") == 2);
    fixture.iso().PumpJobs();
    CHECK_FALSE(remote.ran);
}

UNIBIND_TEST_CASE(INSPECTOR, "inspector: a script's URL is what the client says it is") {
    ub_test::Fixture fixture;
    Client client;
    client.urlPrefix = "file:///app/";
    const Attached attached = Attach(fixture, client);
    if (!attached) {
        return;
    }

    attached.session->DispatchProtocolMessage(R"({"id":40,"method":"Debugger.enable"})");
    CHECK(ub_test::EvalInt(fixture.context, "7") == 7);
    const auto ran = ub::Evaluate(fixture.context, "8", {.resourceName = "main.js"});
    REQUIRE(ran.has_value());

    CHECK(client.Saw("\"method\":\"Debugger.scriptParsed\""));
    CHECK(client.Saw("file:///app/main.js"));
}

UNIBIND_TEST_CASE(INSPECTOR, "inspector: realms come and go, and a stopped session is let go cleanly") {
    ub_test::Fixture fixture;
    Client client;
    Attached attached = Attach(fixture, client);
    if (!attached) {
        return;
    }

    attached.session->DispatchProtocolMessage(R"({"id":50,"method":"Runtime.enable"})");
    REQUIRE_FALSE(client.ResponseTo(50).empty());

    {
        auto second = ub::Context::New(fixture.iso());
        REQUIRE(second.has_value());
        attached.inspector->ContextCreated(*second, "second");
        CHECK(client.Saw("\"name\":\"second\""));
        attached.inspector->ContextDestroyed(*second);
        CHECK(client.Saw("\"method\":\"Runtime.executionContextDestroyed\""));
    }

    // With the second realm gone, the default is the one announced before it
    // again, so an evaluation naming no context still has somewhere to run.
    attached.session->DispatchProtocolMessage(R"({"id":51,"method":"Runtime.evaluate","params":{"expression":"2+3"}})");
    CHECK(client.ResponseTo(51).find("\"value\":5") != std::string::npos);

    attached.session->Stop();
    attached.session.reset();
    attached.inspector->ContextDestroyed(fixture.context);
    attached.inspector.reset();

    // An inspector made and let go many times gives back what it took.
    const auto cycle = [&fixture] {
        Client again;
        auto inspector = ub::Inspector::New(fixture.iso(), again);
        if (inspector == nullptr) {
            return;
        }
        inspector->ContextCreated(fixture.context, "cycled");
        auto session = inspector->Connect();
        again.session = session.get();
        session->DispatchProtocolMessage(R"({"id":1,"method":"Runtime.evaluate","params":{"expression":"1"}})");
        session->Stop();
        session.reset();
        inspector->ContextDestroyed(fixture.context);
    };
    constexpr int WINDOW = 20;
    for (int i = 0; i < WINDOW; ++i) {
        cycle();
    }
    const long long afterWarmup = ub_test::OutstandingAllocations();
    for (int i = 0; i < WINDOW; ++i) {
        cycle();
    }
    const long long growth = ub_test::OutstandingAllocations() - afterWarmup;
    INFO("outstanding allocations grew by ", growth, " over ", WINDOW, " inspectors");
    CHECK(growth < WINDOW);
}
