#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "support.h"
#include "unibind/unibind.h"

int main(int argc, char** argv) {
    // Installed for the whole run - a Platform's fault handler is fixed for its
    // life - and counted in support.cpp for the cases that need to see one.
    ub::PlatformOptions options;
    options.onEngineFault = &py_test::OnEngineFault;
    const ub::Platform platform(options);
    doctest::Context context(argc, argv);
    return context.run();
}
