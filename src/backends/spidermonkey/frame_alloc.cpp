// The one translation unit in this backend that includes no SpiderMonkey
// header, and it has to stay that way - see frame_alloc.h for why. Adding an
// engine include here would silently redirect these two functions to
// moz_xmalloc and quietly un-test rule 9.

#include "frame_alloc.h"

#include <new>

namespace ub::detail {

void* FrameAllocate(std::size_t bytes) noexcept {
    // The throwing form, with the throw caught: an embedder that replaced
    // `operator new` may report failure either way, and this sees both.
    try {
        return ::operator new(bytes == 0 ? 1 : bytes);
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}

void FrameRelease(void* memory) noexcept {
    ::operator delete(memory);
}

}  // namespace ub::detail
