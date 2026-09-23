// The standard library's C extension modules, as the overlay port builds them:
// compiled into the static CPython as built-in modules (a static core cannot
// load a .pyd), and imported here inside an isolate - an own-GIL
// sub-interpreter with check_multi_interp_extensions on. See
// cmake/vcpkg-ports/README.md for the list and for the ones that refuse to load
// in such an interpreter.

#include "support.h"

#include <latch>
#include <string>
#include <thread>

using py_test::Eval;
using py_test::EvalError;
using py_test::EvalInt;
using py_test::EvalText;
using py_test::EvalTruth;
using py_test::Fixture;

namespace {

// Evaluate `source` in a fresh isolate on the calling thread and hand back the
// result's text, or "error: <message>". No doctest assertions: this runs on
// threads of its own.
std::string EvalInFreshIsolate(std::string_view source) {
    auto isolate = ub::Isolate::New();
    if (!isolate) {
        return "error: no isolate";
    }
    std::string out;
    {
        const ub::HandleScope scope(*isolate);
        auto context = ub::Context::New(*isolate);
        if (!context) {
            return "error: no context";
        }
        {
            const ub::ContextScope entered(*context);
            ub::TryCatch tc(*isolate);
            auto result = ub::Evaluate(*context, source, {.resourceName = "thread.py"});
            if (!result) {
                out = "error: " + (tc.HasCaught() ? tc.Message(*context).value_or("?") : std::string("?"));
            } else if (auto text = result->ToString(*context)) {
                out = text->Utf8Value();
            } else {
                out = "error: no text";
            }
        }
    }
    return out;
}

constexpr std::string_view kGatherProgram = R"(
import asyncio
async def work(n):
    await asyncio.sleep(0.01)
    return n * 2
async def main():
    return sum(await asyncio.gather(*(work(i) for i in range(5))))
asyncio.run(main())
)";

}  // namespace

TEST_CASE("stdlib: the extension modules are built in, and nothing looks for a .pyd") {
    Fixture f;
    CHECK(EvalText(f.context, R"(
import sys
wanted = ['_asyncio', '_overlapped', '_socket', 'select']
','.join(m for m in wanted if m not in sys.builtin_module_names)
)") == "");
    CHECK(EvalText(f.context, "import _socket\n_socket.__spec__.origin") == "built-in");
    CHECK(EvalText(f.context, "import select\nselect.__loader__.__name__") == "BuiltinImporter");
}

TEST_CASE("stdlib: socket - a socketpair carries bytes, and select sees them") {
    Fixture f;
    CHECK(EvalText(f.context, R"(
import socket, select
a, b = socket.socketpair()
a.sendall(b'ping')
readable, _, _ = select.select([b], [], [], 5)
got = b.recv(16) if readable == [b] else b''
a.close(); b.close()
got.decode()
)") == "ping");
    CHECK(EvalTruth(f.context, "socket.gethostname() != ''"));
    CHECK(EvalText(f.context, "socket.inet_ntoa(socket.inet_aton('127.0.0.1'))") == "127.0.0.1");
}

TEST_CASE("stdlib: asyncio - run, sleep and gather") {
    Fixture f;
    CHECK(EvalInt(f.context, kGatherProgram) == 20);
    CHECK(EvalText(f.context, "import _asyncio, asyncio\nasyncio.Future is _asyncio.Future") == "True");
    CHECK(EvalText(f.context, "type(asyncio.new_event_loop()).__name__") == "ProactorEventLoop");
}

TEST_CASE("stdlib: asyncio - a loopback TCP echo through streams") {
    Fixture f;
    CHECK(EvalText(f.context, R"(
import asyncio
async def handle(reader, writer):
    line = await reader.readline()
    writer.write(line.upper())
    await writer.drain()
    writer.close()
    await writer.wait_closed()
async def main():
    server = await asyncio.start_server(handle, '127.0.0.1', 0)
    port = server.sockets[0].getsockname()[1]
    async with server:
        reader, writer = await asyncio.open_connection('127.0.0.1', port)
        writer.write(b'hello over tcp\n')
        await writer.drain()
        reply = await reader.readline()
        writer.close()
        await writer.wait_closed()
    return reply.decode().strip()
asyncio.run(asyncio.wait_for(main(), 10))
)") == "HELLO OVER TCP");
}

TEST_CASE("stdlib: asyncio - two isolates run event loops on two threads at once") {
    std::latch start(2);
    std::string first;
    std::string second;
    auto body = [&start](std::string& out) {
        start.arrive_and_wait();
        out = EvalInFreshIsolate(kGatherProgram);
    };
    std::thread a(body, std::ref(first));
    std::thread b(body, std::ref(second));
    a.join();
    b.join();
    CHECK(first == "20");
    CHECK(second == "20");
}
