#include "common/exception/exception.h"

namespace lbug {
namespace common {

Exception::Exception(std::string msg) : exception(), exception_message_(std::move(msg)) {}

} // namespace common
} // namespace lbug
