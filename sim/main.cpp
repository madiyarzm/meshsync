// Laptop simulator. Placeholder until Phase 4: proves the core builds and runs
// on a laptop with no hardware headers.

#include <cstdio>

#include "core_constants.h"

int main() {
    std::printf("construct simulator: %d classes, %d features per drawing\n",
                construct::NUM_CLASSES, construct::NUM_FEATURES);
    return 0;
}
