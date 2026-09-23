#pragma once
/// \file
/// Where a handle frame's spill buffer comes from.
///
/// Declared apart from `internal.h` on purpose: `frame_alloc.cpp` must be the
/// one translation unit in this backend that includes **no SpiderMonkey
/// header**. `mozilla/cxxalloc.h`, which `jsapi.h` pulls in transitively,
/// defines `operator new` and `operator delete` as always-inline forwards to
/// `moz_xmalloc`. Any `::operator new` written in a TU that has seen a
/// SpiderMonkey header is therefore *not* the program's replaceable
/// `operator new`: it is `moz_xmalloc`, inlined, and an embedder or a test that
/// replaced `operator new` never sees it.
///
/// That matters because it is how `docs/lifetimes.md` rule 9 gets tested: the
/// suite provokes frame exhaustion by replacing `operator new`, and a frame
/// that allocated through `moz_xmalloc` - or through the engine's
/// `js_malloc`, which is what `JS::RootedVector` does by default - is
/// unreachable from it. Routing the spill buffer through here makes the one
/// lever the suite already has work on this backend too, with nothing new for
/// a test to call.

#include <cstddef>

namespace ub::detail {

/// Null on failure, and it does not throw: `Frame::Push` is `noexcept` and
/// answers exhaustion with an empty handle, not with an exception crossing the
/// engine.
[[nodiscard]] void* FrameAllocate(std::size_t bytes) noexcept;
void FrameRelease(void* memory) noexcept;

}  // namespace ub::detail
