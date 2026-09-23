#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "unibind/unibind.h"

int main(int argc, char** argv) {
    const ub::Platform platform;
    doctest::Context context(argc, argv);
    return context.run();
}
