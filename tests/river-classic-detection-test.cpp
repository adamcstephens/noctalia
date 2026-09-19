#include "compositors/compositor_detect.h"
#include "tests/test_check.h"

#include <string_view>

int main(int argc, char** argv) {
  TEST_CHECK(argc == 2);
  TEST_CHECK(compositors::name(compositors::detect()) == std::string_view(argv[1]));
}
