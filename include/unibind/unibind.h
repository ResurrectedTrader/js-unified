#pragma once
/// \file
/// One embedding API over several JavaScript engines. Include this.
///
/// The engine is chosen when the library is built, not when it is used: link
/// `unibind::unibind` and you get whichever backend was configured. Nothing in these
/// headers names an engine type, and the build proves it - see the
/// `unibind_headers_only` target.
///
/// Start with `docs/lifetimes.md`; the handle model is the one thing here that
/// is not obvious from the names.
///
/// ---------------------------------------------------------------------------
/// What compiles these headers
/// ---------------------------------------------------------------------------
///
/// **Clang, including clang-cl. Not MSVC's `cl.exe`**, at any language level,
/// as of toolsets 14.44 and 14.50 - so this is worth knowing before you plan a
/// build around it, rather than after.
///
/// It is a compiler bug rather than a portability problem in the API. `cl.exe`
/// eagerly instantiates `std::optional<Local<T>>` where it appears as the
/// declared return type of a member of `Local<T>` - `ToString`, `Get`,
/// `GetPrototype` and their neighbours - while `Local<T>` is still being
/// defined, and then reports C7637 and C2139 because the type is not complete
/// yet. Declaring a function that returns an incomplete type is legal and does
/// not require instantiating it; clang agrees, `cl.exe` does not.
///
/// The workaround, if MSVC support is ever needed, is known and costed: make
/// each of those members a template on a defaulted parameter its return type
/// depends on, which defers the instantiation. Call sites do not change. It was
/// not taken because it turns roughly ten members of the most-used type in the
/// API into member templates - visible forever, in the header everyone reads,
/// to work around a bug in a compiler nothing here is built with. The build
/// pins ClangCL for this reason among others.

#include "unibind/class.h"
#include "unibind/config.h"
#include "unibind/context.h"
#include "unibind/exception.h"
#include "unibind/function.h"
#include "unibind/fwd.h"
#include "unibind/handle.h"
#include "unibind/isolate.h"
#include "unibind/script.h"
#include "unibind/template.h"
#include "unibind/types.h"
#include "unibind/value.h"
