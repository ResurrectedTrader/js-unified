#pragma once
/// \file
/// The representation of a handle: a slot ordinal in a frame.
///
/// This is the whole of `docs/lifetimes.md` in twenty lines. A handle does not
/// hold an engine value and does not hold the address of one; it holds the
/// frame that roots the value and the value's position in that frame. The
/// address of a slot is allowed to move (SpiderMonkey's RootedVector
/// reallocates), and the value inside it is allowed to move (both collectors
/// relocate objects); neither is visible here.

#include <cstdint>

#include "unibind/config.h"
#include "unibind/fwd.h"

namespace ub::detail {

using SlotIndex = std::uint32_t;

struct Slot {
    Frame* frame = nullptr;
    SlotIndex index = 0;
#if UNIBIND_HANDLE_CHECKS
    /// The frame's epoch at the time the handle was made. A frame's storage is
    /// the caller's stack, so a dead frame's address is routinely reused; the
    /// epoch is what distinguishes "this frame" from "whatever is there now".
    std::uint32_t epoch = 0;
#endif

    [[nodiscard]] constexpr bool IsEmpty() const noexcept { return frame == nullptr; }

    /// Identity of the *slot*, not of the value in it. Two handles to the same
    /// value made by two calls are different slots and compare unequal; ask
    /// `Local<T>::StrictEquals` for the JavaScript question.
    friend constexpr bool operator==(const Slot&, const Slot&) noexcept = default;
};

/// Backing store for a frame, living inside the `HandleScope` that owns it.
/// The size comes from the generated config; the backend static_asserts that
/// its own frame type fits.
struct FrameStorage {
    alignas(UNIBIND_FRAME_STORAGE_ALIGN) unsigned char bytes[UNIBIND_FRAME_STORAGE_SIZE];
};

}  // namespace ub::detail
