#pragma once

#include <cstdint>

namespace lbug {
namespace common {

/**
 * @brief Enum representing types of subqueries.
 *
 * This enum defines the different types of subqueries used in the query engine.
 */
enum class SubqueryType : uint8_t {
    COUNT = 1,  /**< Subquery that counts the number of matching rows. */
    EXISTS = 2, /**< Subquery that checks for the existence of at least one matching row. */
};

}
} // namespace lbug
