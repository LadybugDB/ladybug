#include "common/md5.h"

#include "function/hash/vector_hash_functions.h"
#include "function/scalar_function.h"

using namespace lbug::common;

namespace lbug {
namespace function {

struct MD5Operator {
    static void operation(string_t& operand, string_t& result, ValueVector& resultVector) {
        MD5 hasher;
        hasher.addToMD5(reinterpret_cast<const char*>(operand.getData()), operand.len);
        StringVector::addString(&resultVector, result, std::string(hasher.finishMD5()));
    }
};

function_set MD5Function::getFunctionSet() {
    function_set functionSet;
    functionSet.push_back(std::make_unique<ScalarFunction>(name,
        std::vector<LogicalTypeID>{LogicalTypeID::STRING}, LogicalTypeID::STRING,
        ScalarFunction::UnaryStringExecFunction<string_t, string_t, MD5Operator>));
    return functionSet;
}

} // namespace function
} // namespace lbug
