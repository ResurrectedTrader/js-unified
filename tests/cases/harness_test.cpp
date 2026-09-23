/// \file
/// The suite testing itself.
///
/// Two properties of the harness are worth an assertion of their own, because
/// if either one quietly broke the rest of the suite would keep reporting
/// success while proving less than it claims.

#include "support/harness.h"

#include <string>

TEST_CASE("harness: the suite knows which backend it is running against") {
    // Reported, never branched on. A test that needs to know which engine it is
    // running on has failed at its job.
    CHECK(ub::Platform::BackendName() == std::string(UNIBIND_TEST_BACKEND));
    CHECK_FALSE(std::string(UNIBIND_TEST_CAPABILITIES_PRESENT).empty());
}

// `NOTHING` is a capability no backend can have - see tests/cmake/Capabilities.cmake.
// So this case is compiled out and reports itself skipped everywhere, which is
// what an unimplemented area is supposed to look like: listed under the same
// name on every backend, visible to CTest as a skip, and a `SKIPPED` cell in
// the parity matrix rather than a missing row.
UNIBIND_TEST_CASE(NOTHING, "harness: a case whose area the backend lacks is skipped, not missing") {
    FAIL("this body must never run: no backend defines the capability that gates it");
}
