#include "test_helper/test_helper.h"

using namespace lbug::testing;
using namespace lbug::common;

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--gtest_list_tests") {
        std::filesystem::remove_all(TestHelper::getRootTempDir());
    }
    return 0;
}
