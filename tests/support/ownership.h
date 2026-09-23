#pragma once
/// \file
/// What a destruction test needs that the rest of the suite does not: a native
/// that records its own birth and death in a registry outliving every isolate.
///
/// Counting alone cannot tell "each destroyed once" from "one destroyed twice
/// and one leaked" - which is precisely the failure shared ownership makes
/// possible, and precisely the one a use-after-free comes from - so the
/// registry counts per id and the cases ask per id.
///
/// Nothing here names an engine type or branches on the backend.

#include <cstdint>
#include <thread>

#include "support/harness.h"
#include "unibind/unibind.h"

namespace ub_test {

// ---------------------------------------------------------------------------
// Who was born, who died, and how often
// ---------------------------------------------------------------------------

/// A registry of native lifetimes, keyed by an id each native carries.
///
/// Static and process-wide on purpose: it has to be readable *after* the
/// isolate that held the natives is gone, which is where the interesting half
/// of every ownership question is answered.
///
/// Single-threaded, like the suite. A native must be born and die on the
/// thread running the case.
class Lives {
   public:
    /// A fresh id, counted as alive.
    [[nodiscard]] static int Born();
    /// Record that the native carrying `id` was destroyed. Called from a
    /// destructor, so it never throws and never asserts - a double destruction
    /// is *counted*, and the case that cares asks.
    static void Died(int id) noexcept;

    [[nodiscard]] static int Deaths(int id) noexcept;
    [[nodiscard]] static int TotalBorn() noexcept;
    [[nodiscard]] static int TotalDeaths() noexcept;
    [[nodiscard]] static int Alive() noexcept;

    /// True if any id was destroyed more than once - the use-after-free a
    /// passing count would hide.
    [[nodiscard]] static bool AnyDestroyedTwice() noexcept;
    /// True if every id born has died exactly once.
    [[nodiscard]] static bool EachDestroyedExactlyOnce() noexcept;

    /// The thread each death happened on, and whether every one of them
    /// happened on `thread`.
    ///
    /// This is the only portable way to notice a native being destroyed on the
    /// wrong thread, which is a real hazard rather than tidiness: a box's
    /// `destroy` drops a `std::shared_ptr` whose other holders are the
    /// embedder's, so an engine that finalized off-thread would race them - and
    /// a race in a finalizer is exactly the failure that does not reproduce.
    [[nodiscard]] static bool AllDeathsOn(std::thread::id thread) noexcept;
    /// How many deaths happened somewhere else, for a message.
    [[nodiscard]] static int DeathsElsewhere(std::thread::id thread) noexcept;

    static void Reset() noexcept;
};

/// The native the ownership cases hand to the engine.
///
/// Non-copyable and non-movable, so its identity is its address: two wrappers
/// naming "the same native" means the same pointer, not an equal value.
struct Tracked {
    explicit Tracked(std::int32_t start = 0) : id(Lives::Born()), value(start) {}
    ~Tracked() { Lives::Died(id); }

    Tracked(const Tracked&) = delete;
    Tracked& operator=(const Tracked&) = delete;
    Tracked(Tracked&&) = delete;
    Tracked& operator=(Tracked&&) = delete;

    int id;
    std::int32_t value;
};

}  // namespace ub_test
