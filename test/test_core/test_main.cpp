// Native unit tests for lib/core. Run: pio test -e native

#include <unity.h>

#include "core_constants.h"

void setUp() {}
void tearDown() {}

// The C++ core must use the same data format as the Python prototype.
void test_dimensions_match_prototype() {
    TEST_ASSERT_EQUAL_INT(10, construct::NUM_CLASSES);
    TEST_ASSERT_EQUAL_INT(64, construct::NUM_FEATURES);
    TEST_ASSERT_EQUAL_INT(16, construct::MAX_PIXEL);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_dimensions_match_prototype);
    return UNITY_END();
}
