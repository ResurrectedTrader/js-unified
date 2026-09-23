/// \file
/// Global operator new/delete, replaced so two tests have something to work
/// with that no engine offers.
///
/// **Counting.** Neither engine will tell us whether a frame gave back the
/// storage it took for its overflow slots, or whether a `Global` gave back its
/// node - those are C++ heap allocations in the backend, invisible to any heap
/// statistic the engine reports. Counting them here is the only portable way to
/// ask, and it is portable precisely because it asks nothing of the engine.
///
/// **Failing on purpose.** `docs/lifetimes.md` rule 9 says a frame that cannot
/// grow yields an *empty* handle and reports out of memory, never a slot that
/// reads as `undefined`. The only way to reach that branch from a test is to
/// make an allocation fail, so `FailNextAllocations` does.
///
/// This is a whole-program replacement, so every allocation in the test binary
/// goes through it, engine included. A counting test therefore measures the
/// *difference* across a loop, never an absolute; a failing test arms the
/// regime for exactly one call and disarms it immediately.

#include <malloc.h>

#include <atomic>
#include <cstdlib>
#include <new>

namespace {

std::atomic<long long> g_outstanding{0};

/// How many of the next allocations must fail. Not atomic-safe against other
/// threads by design: the suite is single-threaded, and an injected failure
/// that wandered onto an engine's background thread would be untestable noise.
long long g_failuresArmed = 0;
long long g_failuresFired = 0;
/// How many allocations to let through before the failures start. Walking this
/// across a call is how a test reaches the *second* allocation an operation
/// makes, which is where the interesting failures are - the first one usually
/// fails before anything has been handed to the engine.
long long g_allocationsToSkip = 0;

/// True when this allocation is the one that has to fail.
[[nodiscard]] bool ShouldFail() noexcept {
    if (g_failuresArmed <= 0) {
        return false;
    }
    if (g_allocationsToSkip > 0) {
        --g_allocationsToSkip;
        return false;
    }
    --g_failuresArmed;
    ++g_failuresFired;
    return true;
}

void* Allocate(std::size_t size) {
    if (size == 0) {
        size = 1;
    }
    if (ShouldFail()) {
        throw std::bad_alloc();
    }
    void* memory = std::malloc(size);
    if (memory == nullptr) {
        throw std::bad_alloc();
    }
    g_outstanding.fetch_add(1, std::memory_order_relaxed);
    return memory;
}

void* AllocateAligned(std::size_t size, std::size_t alignment) {
    if (size == 0) {
        size = 1;
    }
    if (ShouldFail()) {
        throw std::bad_alloc();
    }
    void* memory = _aligned_malloc(size, alignment);
    if (memory == nullptr) {
        throw std::bad_alloc();
    }
    g_outstanding.fetch_add(1, std::memory_order_relaxed);
    return memory;
}

void* AllocateNoThrow(std::size_t size) noexcept {
    if (size == 0) {
        size = 1;
    }
    if (ShouldFail()) {
        return nullptr;
    }
    void* memory = std::malloc(size);
    if (memory != nullptr) {
        g_outstanding.fetch_add(1, std::memory_order_relaxed);
    }
    return memory;
}

void* AllocateAlignedNoThrow(std::size_t size, std::size_t alignment) noexcept {
    if (size == 0) {
        size = 1;
    }
    if (ShouldFail()) {
        return nullptr;
    }
    void* memory = _aligned_malloc(size, alignment);
    if (memory != nullptr) {
        g_outstanding.fetch_add(1, std::memory_order_relaxed);
    }
    return memory;
}

void Release(void* memory) noexcept {
    if (memory == nullptr) {
        return;
    }
    g_outstanding.fetch_sub(1, std::memory_order_relaxed);
    std::free(memory);
}

void ReleaseAligned(void* memory) noexcept {
    if (memory == nullptr) {
        return;
    }
    g_outstanding.fetch_sub(1, std::memory_order_relaxed);
    _aligned_free(memory);
}

}  // namespace

namespace ub_test {

long long OutstandingAllocations() noexcept {
    return g_outstanding.load(std::memory_order_relaxed);
}

void FailNextAllocations(long long count, long long skip) noexcept {
    g_failuresArmed = count;
    g_allocationsToSkip = skip;
    g_failuresFired = 0;
}

long long StopFailingAllocations() noexcept {
    g_failuresArmed = 0;
    g_allocationsToSkip = 0;
    return g_failuresFired;
}

}  // namespace ub_test

void* operator new(std::size_t size) {
    return Allocate(size);
}
void* operator new[](std::size_t size) {
    return Allocate(size);
}
void* operator new(std::size_t size, std::align_val_t alignment) {
    return AllocateAligned(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
    return AllocateAligned(size, static_cast<std::size_t>(alignment));
}

void* operator new(std::size_t size, const std::nothrow_t& /*nothrow*/) noexcept {
    return AllocateNoThrow(size);
}
void* operator new[](std::size_t size, const std::nothrow_t& /*nothrow*/) noexcept {
    return AllocateNoThrow(size);
}
void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t& /*nothrow*/) noexcept {
    return AllocateAlignedNoThrow(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t& /*nothrow*/) noexcept {
    return AllocateAlignedNoThrow(size, static_cast<std::size_t>(alignment));
}

void operator delete(void* block) noexcept {
    Release(block);
}
void operator delete[](void* block) noexcept {
    Release(block);
}
void operator delete(void* block, std::size_t /*size*/) noexcept {
    Release(block);
}
void operator delete[](void* block, std::size_t /*size*/) noexcept {
    Release(block);
}
void operator delete(void* block, const std::nothrow_t& /*nothrow*/) noexcept {
    Release(block);
}
void operator delete[](void* block, const std::nothrow_t& /*nothrow*/) noexcept {
    Release(block);
}
void operator delete(void* block, std::align_val_t /*alignment*/) noexcept {
    ReleaseAligned(block);
}
void operator delete[](void* block, std::align_val_t /*alignment*/) noexcept {
    ReleaseAligned(block);
}
void operator delete(void* block, std::size_t /*size*/, std::align_val_t /*alignment*/) noexcept {
    ReleaseAligned(block);
}
void operator delete[](void* block, std::size_t /*size*/, std::align_val_t /*alignment*/) noexcept {
    ReleaseAligned(block);
}
void operator delete(void* block, std::align_val_t /*alignment*/, const std::nothrow_t& /*nothrow*/) noexcept {
    ReleaseAligned(block);
}
void operator delete[](void* block, std::align_val_t /*alignment*/, const std::nothrow_t& /*nothrow*/) noexcept {
    ReleaseAligned(block);
}
