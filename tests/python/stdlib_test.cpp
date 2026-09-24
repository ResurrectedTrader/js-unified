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
wanted = ['_asyncio', '_bz2', '_ctypes', '_decimal', '_elementtree', '_hashlib', '_lzma', '_msi',
          '_multiprocessing', '_overlapped', '_queue', '_socket', '_sqlite3', '_ssl', '_uuid', '_wmi',
          '_zoneinfo', 'pyexpat', 'select', 'unicodedata', 'winsound', 'zlib']
','.join(m for m in wanted if m not in sys.builtin_module_names)
)") == "");
    CHECK(EvalText(f.context, "import _socket\n_socket.__spec__.origin") == "built-in");
    CHECK(EvalText(f.context, "import select\nselect.__loader__.__name__") == "BuiltinImporter");
    // Everything that imports here, imported - and not one of them from a file.
    CHECK(EvalText(f.context, R"(
import asyncio, bz2, hashlib, lzma, multiprocessing, queue, socket, sqlite3, ssl, unicodedata, uuid
import winsound, zlib, zoneinfo
','.join(sorted(n for n, m in list(sys.modules.items())
                if (getattr(m, '__file__', None) or '').lower().endswith('.pyd')))
)") == "");
}

TEST_CASE("stdlib: the modules that keep state in C globals refuse an isolate") {
    // Single-phase init (_ctypes, _decimal, _msi, and the core's _datetime and
    // _tracemalloc), or multi-phase but not isolated yet (pyexpat,
    // _elementtree: gh-103092) or not declared safe under a per-interpreter
    // GIL (_wmi). An isolate is an own-GIL sub-interpreter with
    // check_multi_interp_extensions on, so each import fails cleanly - see
    // cmake/vcpkg-ports/README.md.
    Fixture f;
    for (const char* module :
         {"_ctypes", "_decimal", "_msi", "_datetime", "_tracemalloc", "pyexpat", "_elementtree", "_wmi"}) {
        CAPTURE(module);
        CHECK(EvalError(f.context, std::string("import ") + module) ==
              std::string("ImportError: module ") + module + " does not support loading in subinterpreters");
    }
    // Twice: a refused module must not be left half-made for the next attempt.
    CHECK(EvalError(f.context, "import ctypes").find("does not support loading in subinterpreters") !=
          std::string::npos);
    // decimal falls back to its pure-Python twin; ElementTree imports, but has
    // no parser to parse with.
    CHECK(EvalText(f.context, "import decimal\nstr(decimal.Decimal('1.1') + decimal.Decimal('2.2'))") == "3.3");
    CHECK(EvalText(f.context, "import sys\n'_pydecimal' in sys.modules") == "True");
    CHECK(EvalError(f.context, "import xml.etree.ElementTree as ET\nET.fromstring('<a/>')")
              .find("No module named expat") != std::string::npos);
    // datetime likewise, and _zoneinfo - which needs _datetime's C API -
    // says so as an ImportError, so zoneinfo uses its pure-Python ZoneInfo.
    CHECK(EvalText(f.context, "import datetime\nstr(datetime.date(2024, 2, 29) + datetime.timedelta(days=1))") ==
          "2024-03-01");
    CHECK(EvalError(f.context, "import _zoneinfo") ==
          "ImportError: _zoneinfo needs the C datetime API, which this interpreter does not have");
}

TEST_CASE("stdlib: ssl and hashlib - OpenSSL, built in") {
    Fixture f;
    CHECK(EvalTruth(f.context, "import ssl\nssl.OPENSSL_VERSION.startswith('OpenSSL 3')"));
    // create_default_context loads the Windows certificate stores (crypt32).
    CHECK(EvalTruth(f.context, R"(
ctx = ssl.create_default_context()
ctx.verify_mode == ssl.CERT_REQUIRED and ctx.check_hostname and ctx.cert_store_stats()['x509_ca'] > 0
)"));
    CHECK(EvalText(f.context, R"(
import hashlib
hashlib.sha256(b'abc').hexdigest()
)") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(EvalText(f.context, "hashlib.sha256.__name__") == "openssl_sha256");
    CHECK(EvalText(f.context, "hashlib.pbkdf2_hmac('sha1', b'password', b'salt', 1, 20).hex()") ==
          "0c60c80f961f0e71f3a9b524af6012062fe037a6");
}

TEST_CASE("stdlib: sqlite3 - an in-memory database") {
    Fixture f;
    CHECK(EvalInt(f.context, R"(
import sqlite3
db = sqlite3.connect(':memory:')
db.execute('create table t (n integer)')
db.executemany('insert into t values (?)', [(i,) for i in range(1, 11)])
total = db.execute('select sum(n) from t').fetchone()[0]
db.close()
total
)") == 55);
    CHECK(EvalTruth(f.context, "sqlite3.sqlite_version_info >= (3, 40)"));
}

TEST_CASE("stdlib: compression - zlib, bz2 and lzma round trips") {
    Fixture f;
    CHECK(EvalText(f.context, R"(
import zlib, bz2, lzma
data = b'unibind ' * 1000
ok = []
for name, mod in (('zlib', zlib), ('bz2', bz2), ('lzma', lzma)):
    packed = mod.compress(data)
    if len(packed) < len(data) and mod.decompress(packed) == data:
        ok.append(name)
','.join(ok)
)") == "zlib,bz2,lzma");
}

TEST_CASE("stdlib: unicodedata, queue, uuid, zoneinfo, multiprocessing, winsound") {
    Fixture f;
    CHECK(EvalText(f.context, "import unicodedata\nunicodedata.name('\\u00e9')") == "LATIN SMALL LETTER E WITH ACUTE");
    CHECK(EvalText(f.context, "unicodedata.normalize('NFD', '\\u00e9') == 'e\\u0301'") == "True");

    CHECK(EvalInt(f.context, R"(
import queue, _queue
q = queue.SimpleQueue()
assert queue.SimpleQueue is _queue.SimpleQueue
for i in range(3):
    q.put(i)
q.get() + q.get() + q.get()
)") == 3);

    // uuid1 is _uuid's UuidCreateSequential (rpcrt4).
    CHECK(EvalText(f.context, "import uuid\n(uuid.uuid4().version, uuid.uuid1().version, uuid._UuidCreate is not None)") ==
          "(4, 1, True)");

    // No tz database on Windows without the tzdata package, so a zone built
    // from a TZif v1 blob: no transitions, one type, UTC. In an isolate this
    // is zoneinfo's pure-Python ZoneInfo (see above).
    CHECK(EvalText(f.context, R"(
import io, struct, zoneinfo
from datetime import datetime, timedelta
tzif = b'TZif' + b'\0' * 16 + struct.pack('>6l', 0, 0, 0, 0, 1, 4) + struct.pack('>lBB', 0, 0, 0) + b'UTC\0'
zone = zoneinfo.ZoneInfo.from_file(io.BytesIO(tzif), key='Test/UTC')
moment = datetime(2024, 6, 1, 12, tzinfo=zone)
(zoneinfo.ZoneInfo is zoneinfo._zoneinfo.ZoneInfo, moment.utcoffset() == timedelta(0), moment.tzname())
)") == "(True, True, 'UTC')");

    // Nothing is spawned: a lock is a Windows semaphore made by _multiprocessing.
    CHECK(EvalTruth(f.context, R"(
import _multiprocessing, multiprocessing
lock = multiprocessing.Lock()
lock.acquire(timeout=1) and (lock.release() is None)
)"));

    CHECK(EvalTruth(f.context, "import winsound\nwinsound.PlaySound(None, winsound.SND_PURGE) is None"));
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
    CHECK(EvalText(f.context, "loop = asyncio.new_event_loop()
kind = type(loop).__name__
loop.close()
kind") ==
          "ProactorEventLoop");
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
