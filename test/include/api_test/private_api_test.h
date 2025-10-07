#pragma once

#include "graph_test/private_graph_test.h"

namespace lbug {
namespace testing {

class PrivateApiTest : public DBTest {
public:
    void SetUp() override {
        BaseGraphTest::SetUp();
        createDBAndConn();
        initGraph();
    }

    std::string getInputDir() override {
        return TestHelper::appendLbugRootPath("dataset/tinysnb/");
    }
};

} // namespace testing
} // namespace lbug
