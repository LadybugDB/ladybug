#include "common/types/interval_t.h"

#include "common/assert.h"
#include "common/exception/conversion.h"
#include "common/exception/overflow.h"
#include "common/exception/runtime.h"
#include "common/string_utils.h"
#include "common/types/cast_helpers.h"
#include "common/types/int128_t.h"
#include "common/types/timestamp_t.h"
#include "function/arithmetic/add.h"
#include "function/arithmetic/multiply.h"
#include "function/arithmetic/subtract.h"
#include "function/cast/functions/cast_from_string_functions.h"
#include "function/cast/functions/cast_functions.h"
#include "re2.h"

namespace lbug {
namespace common {

interval_t::interval_t() = default;

interval_t::interval_t(int32_t months_p, int32_t days_p, int64_t micros_p)
    : months(months_p), days(days_p), micros(micros_p) {}

bool interval_t::operator==(const interval_t& rhs) const {
    // Equality is evaluated on normalized entries so that it agrees with the
    // ordering below (e.g. INTERVAL '1 month' equals INTERVAL '30 days',
    // INTERVAL '25 hours' equals INTERVAL '1 day 1 hour'). Without this,
    // a == b could be false while neither a > b nor b > a holds, making
    // a < b and b < a simultaneously true (see #1690).
    int64_t lMonths = 0, lDays = 0, lMicros = 0;
    int64_t rMonths = 0, rDays = 0, rMicros = 0;
    Interval::normalizeIntervalEntries(*this, lMonths, lDays, lMicros);
    Interval::normalizeIntervalEntries(rhs, rMonths, rDays, rMicros);
    return lMonths == rMonths && lDays == rDays && lMicros == rMicros;
}

bool interval_t::operator!=(const interval_t& rhs) const {
    return !(*this == rhs);
}

bool interval_t::operator>(const interval_t& rhs) const {
    return Interval::greaterThan(*this, rhs);
}

bool interval_t::operator<=(const interval_t& rhs) const {
    return !(*this > rhs);
}

bool interval_t::operator<(const interval_t& rhs) const {
    return !(*this >= rhs);
}

bool interval_t::operator>=(const interval_t& rhs) const {
    return *this > rhs || *this == rhs;
}

interval_t interval_t::operator+(const interval_t& rhs) const {
    interval_t result{};
    try {
        // NB: the checked Add overloads take non-const references, so operate
        // on locals to avoid silently falling back to the raw template.
        int32_t lMonths = months, rMonths = rhs.months;
        int32_t lDays = days, rDays = rhs.days;
        int64_t lMicros = micros, rMicros = rhs.micros;
        function::Add::operation(lMonths, rMonths, result.months);
        function::Add::operation(lDays, rDays, result.days);
        function::Add::operation(lMicros, rMicros, result.micros);
    } catch (const OverflowException& e) {
        throw OverflowException{"Interval value is out of range"};
    }
    return result;
}

timestamp_t interval_t::operator+(const timestamp_t& rhs) const {
    return rhs + *this;
}

date_t interval_t::operator+(const date_t& rhs) const {
    return rhs + *this;
}

interval_t interval_t::operator-(const interval_t& rhs) const {
    interval_t result{};
    try {
        // NB: see operator+ on why locals are required here.
        int32_t lMonths = months, rMonths = rhs.months;
        int32_t lDays = days, rDays = rhs.days;
        int64_t lMicros = micros, rMicros = rhs.micros;
        function::Subtract::operation(lMonths, rMonths, result.months);
        function::Subtract::operation(lDays, rDays, result.days);
        function::Subtract::operation(lMicros, rMicros, result.micros);
    } catch (const OverflowException& e) {
        throw OverflowException{"Interval value is out of range"};
    }
    return result;
}

interval_t interval_t::operator/(const int64_t& rhs) const {
    if (rhs == 0) {
        throw RuntimeException("Divide by zero.");
    }
    // Integer division distributes higher-unit remainders into the next smaller
    // unit (months into days at 30 days/month, days into micros at
    // MICROS_PER_DAY). All computation is done in signed 128-bit arithmetic
    // (via the portable common::int128_t, which has an MSVC fallback, unlike
    // compiler-specific __int128): the previous unsigned implementation
    // implicitly converted negative operands to huge unsigned values, producing
    // garbage (see #1691, #1692).
    interval_t result{};
    // |monthsRemainder| <= INT32_MAX, so monthsRemainder * 30 + days fits
    // comfortably in int64 (no 128-bit arithmetic needed here).
    int64_t monthsRemainder = (int64_t)months % rhs;
    int64_t daysTotal = monthsRemainder * (int64_t)Interval::DAYS_PER_MONTH + (int64_t)days;
    int64_t daysQuotient = daysTotal / rhs;
    int64_t daysRemainder = daysTotal % rhs;
    // |daysRemainder| < |rhs| can still be up to ~2^36, so
    // daysRemainder * MICROS_PER_DAY + micros needs up to ~73 bits.
    int128_t microsTotal =
        int128_t(daysRemainder) * int128_t(Interval::MICROS_PER_DAY) + int128_t(micros);
    int128_t microsQuotient128 = microsTotal / int128_t(rhs);
    int64_t microsQuotient = 0;
    if (!Int128_t::tryCast(microsQuotient128, microsQuotient)) {
        throw OverflowException{"Interval value is out of range"};
    }
    int64_t monthsQuotient = (int64_t)months / rhs;
    if (monthsQuotient > INT32_MAX || monthsQuotient < INT32_MIN || daysQuotient > INT32_MAX ||
        daysQuotient < INT32_MIN) {
        throw OverflowException{"Interval value is out of range"};
    }
    result.months = (int32_t)monthsQuotient;
    result.days = (int32_t)daysQuotient;
    result.micros = microsQuotient;
    return result;
}

void Interval::addition(interval_t& result, uint64_t number, std::string specifierStr) {
    // number is cast to int64_t below; values above INT64_MAX would wrap to
    // negative and silently subtract instead of throwing.
    if (number > static_cast<uint64_t>(INT64_MAX)) {
        throw OverflowException{"Interval value is out of range"};
    }
    StringUtils::toLower(specifierStr);
    // All updates go through checked arithmetic so extreme inputs throw
    // OverflowException instead of silently wrapping (see #1691, #1694).
    auto addMonths = [&](int64_t amount) {
        int64_t current = result.months;
        int64_t updated = 0;
        function::Add::operation(current, amount, updated);
        if (updated > INT32_MAX || updated < INT32_MIN) {
            throw OverflowException{"Interval value is out of range"};
        }
        result.months = (int32_t)updated;
    };
    auto addDays = [&](int64_t amount) {
        int64_t current = result.days;
        int64_t updated = 0;
        function::Add::operation(current, amount, updated);
        if (updated > INT32_MAX || updated < INT32_MIN) {
            throw OverflowException{"Interval value is out of range"};
        }
        result.days = (int32_t)updated;
    };
    auto addMicros = [&](int64_t amount) {
        int64_t current = result.micros;
        function::Add::operation(current, amount, result.micros);
    };
    try {
        if (specifierStr == "year" || specifierStr == "years" || specifierStr == "y") {
            int64_t product = 0;
            function::Multiply::operation((int64_t)number, (int64_t)MONTHS_PER_YEAR, product);
            addMonths(product);
        } else if (specifierStr == "month" || specifierStr == "months" || specifierStr == "mon") {
            addMonths((int64_t)number);
        } else if (specifierStr == "day" || specifierStr == "days" || specifierStr == "d") {
            addDays((int64_t)number);
        } else if (specifierStr == "hour" || specifierStr == "hours" || specifierStr == "h") {
            int64_t product = 0;
            function::Multiply::operation((int64_t)number, MICROS_PER_HOUR, product);
            addMicros(product);
        } else if (specifierStr == "minute" || specifierStr == "minutes" || specifierStr == "m") {
            int64_t product = 0;
            function::Multiply::operation((int64_t)number, MICROS_PER_MINUTE, product);
            addMicros(product);
        } else if (specifierStr == "second" || specifierStr == "seconds" || specifierStr == "s") {
            int64_t product = 0;
            function::Multiply::operation((int64_t)number, MICROS_PER_SEC, product);
            addMicros(product);
        } else if (specifierStr == "millisecond" || specifierStr == "milliseconds" ||
                   specifierStr == "ms" || specifierStr == "msec") {
            int64_t product = 0;
            function::Multiply::operation((int64_t)number, MICROS_PER_MSEC, product);
            addMicros(product);
        } else if (specifierStr == "microsecond" || specifierStr == "microseconds" ||
                   specifierStr == "us") {
            addMicros((int64_t)number);
        } else {
            throw ConversionException(
                "Unrecognized interval specifier string: " + specifierStr + ".");
        }
    } catch (const OverflowException& e) {
        throw OverflowException{"Interval value is out of range"};
    }
}

template<class T>
T intervalTryCastInteger(int64_t input) {
    if (std::is_same<T, int8_t>()) {
        int8_t result = 0;
        function::CastToInt8::operation<int64_t>(input, result);
        return result;
    } else if (std::is_same<T, int16_t>()) {
        int16_t result = 0;
        function::CastToInt16::operation<int64_t>(input, result);
        return result;
    } else if (std::is_same<T, int32_t>()) {
        int32_t result = 0;
        function::CastToInt32::operation<int64_t>(input, result);
        return result;
    } else if (std::is_same<T, int64_t>()) {
        int64_t result = 0;
        function::CastToInt64::operation<int64_t>(input, result);
        return result;
    } else {
        throw ConversionException("The destination is not an integer");
    }
}

template<class T>
void intervalTryAddition(T& target, int64_t input, int64_t multiplier, int64_t fraction = 0) {
    int64_t addition = 0;
    try {
        function::Multiply::operation(input, multiplier, addition);
    } catch (const OverflowException& e) {
        throw OverflowException{"Interval value is out of range"};
    }
    T additionBase = intervalTryCastInteger<T>(addition);
    try {
        function::Add::operation(target, additionBase, target);
    } catch (const OverflowException& e) {
        throw OverflowException{"Interval value is out of range"};
    }
    if (fraction) {
        //	Add in (fraction * multiplier) / MICROS_PER_SEC
        //	This is always in range
        addition = (fraction * multiplier) / Interval::MICROS_PER_SEC;
        additionBase = intervalTryCastInteger<T>(addition);
        try {
            function::Add::operation(target, additionBase, target);
        } catch (const OverflowException& e) {
            throw OverflowException{"Interval fraction is out of range"};
        }
    }
}

interval_t Interval::fromCString(const char* str, uint64_t len) {
    interval_t result;
    uint64_t pos = 0;
    uint64_t startPos = 0;
    bool foundAny = false;
    int64_t number = 0;
    int64_t fraction = 0;
    DatePartSpecifier specifier{};
    std::string specifierStr{};

    result.days = 0;
    result.micros = 0;
    result.months = 0;

    if (len == 0) {
        throw ConversionException("Error occurred during parsing interval. Given empty string.");
    }

    if (str[pos] == '@') {
        pos++;
    }

parse_interval:
    for (; pos < len; pos++) {
        char c = str[pos];
        if (isspace(c)) {
            // skip spaces
            continue;
        } else if (isdigit(c)) {
            // start parsing a number
            goto interval_parse_number;
        } else {
            // unrecognized character, expected a number or end of string
            throw ConversionException("Error occurred during parsing interval. Given: \"" +
                                      StringUtils::safeCStringToString(str, len) + "\".");
        }
    }
    goto end_of_string;

interval_parse_number:
    startPos = pos;
    for (; pos < len; pos++) {
        char c = str[pos];
        if (isdigit(c)) {
            // the number continues
            continue;
        } else if (c == ':') {
            // colon: we are parsing a time
            goto interval_parse_time;
        } else {
            // finished the number, parse it from the string
            function::CastString::operation(string_t{str + startPos, pos - startPos}, number);
            fraction = 0;
            if (c == '.') {
                // we expect some microseconds
                int32_t mult = 100000;
                for (++pos; pos < len && isdigit(str[pos]); ++pos, mult /= 10) {
                    if (mult > 0) {
                        fraction += int64_t(str[pos] - '0') * mult;
                    }
                }
            }
            goto interval_parse_identifier;
        }
    }
    goto interval_parse_identifier;

interval_parse_time: {
    // parse the remainder of the time as a Time type
    dtime_t time;
    uint64_t tmpPos = 0;
    if (!Time::tryConvertInterval(str + startPos, len - startPos, tmpPos, time)) {
        throw ConversionException("Error occurred during parsing time. Given: \"" +
                                  std::string(str + startPos, len - startPos) + "\".");
    }
    result.micros += time.micros;
    foundAny = true;
    goto end_of_string;
}

interval_parse_identifier:
    for (; pos < len; pos++) {
        char c = str[pos];
        if (isspace(c)) {
            // skip spaces at the start
            continue;
        } else {
            break;
        }
    }
    // now parse the identifier
    startPos = pos;
    for (; pos < len; pos++) {
        char c = str[pos];
        if (!isspace(c)) {
            // keep parsing the string
            continue;
        } else {
            break;
        }
    }
    specifierStr = std::string(str + startPos, pos - startPos);

    // Specifier string is empty, missing field name
    if (specifierStr.empty()) {
        throw ConversionException("Error occurred during parsing interval. Field name is missing.");
    }

    tryGetDatePartSpecifier(specifierStr, specifier);

    switch (specifier) {
    case DatePartSpecifier::MILLENNIUM:
        intervalTryAddition<int32_t>(result.months, number, MONTHS_PER_MILLENIUM, fraction);
        break;
    case DatePartSpecifier::CENTURY:
        intervalTryAddition<int32_t>(result.months, number, MONTHS_PER_CENTURY, fraction);
        break;
    case DatePartSpecifier::DECADE:
        intervalTryAddition<int32_t>(result.months, number, MONTHS_PER_DECADE, fraction);
        break;
    case DatePartSpecifier::YEAR:
        intervalTryAddition<int32_t>(result.months, number, MONTHS_PER_YEAR, fraction);
        break;
    case DatePartSpecifier::QUARTER:
        intervalTryAddition<int32_t>(result.months, number, MONTHS_PER_QUARTER, fraction);
        // Reduce to fraction of a month
        fraction *= MONTHS_PER_QUARTER;
        fraction %= MICROS_PER_SEC;
        intervalTryAddition<int32_t>(result.days, 0, DAYS_PER_MONTH, fraction);
        break;
    case DatePartSpecifier::MONTH:
        intervalTryAddition<int32_t>(result.months, number, 1);
        intervalTryAddition<int32_t>(result.days, 0, DAYS_PER_MONTH, fraction);
        break;
    case DatePartSpecifier::DAY:
        intervalTryAddition<int32_t>(result.days, number, 1);
        intervalTryAddition<int64_t>(result.micros, 0, MICROS_PER_DAY, fraction);
        break;
    case DatePartSpecifier::WEEK:
        intervalTryAddition<int32_t>(result.days, number, DAYS_PER_WEEK, fraction);
        // Reduce to fraction of a day
        fraction *= DAYS_PER_WEEK;
        fraction %= MICROS_PER_SEC;
        intervalTryAddition<int64_t>(result.micros, 0, MICROS_PER_DAY, fraction);
        break;
    case DatePartSpecifier::HOUR:
        intervalTryAddition<int64_t>(result.micros, number, MICROS_PER_HOUR, fraction);
        break;
    case DatePartSpecifier::MINUTE:
        intervalTryAddition<int64_t>(result.micros, number, MICROS_PER_MINUTE, fraction);
        break;
    case DatePartSpecifier::SECOND:
        intervalTryAddition<int64_t>(result.micros, number, MICROS_PER_SEC, fraction);
        break;
    case DatePartSpecifier::MILLISECOND:
        intervalTryAddition<int64_t>(result.micros, number, MICROS_PER_MSEC, fraction);
        break;
    case DatePartSpecifier::MICROSECOND:
        // Round the fraction
        number += (fraction * 2) / MICROS_PER_SEC;
        intervalTryAddition<int64_t>(result.micros, number, 1);
        break;
    default:
        throw ConversionException("Unrecognized interval specifier string: " + specifierStr + ".");
    }

    foundAny = true;
    goto parse_interval;

end_of_string:
    if (!foundAny) {
        throw ConversionException("Error occurred during parsing interval. Given: \"" +
                                  StringUtils::safeCStringToString(str, len) + "\".");
    }
    return result;
}

std::string Interval::toString(interval_t interval) {
    char buffer[70];
    uint64_t length = IntervalToStringCast::Format(interval, buffer);
    return std::string(buffer, length);
}

// helper function of interval comparison: canonicalize to a unique
// (months, days, micros) triple with all non-zero components sharing the sign
// of the total, so that equality/ordering/hashing agree (a month counts as 30
// days, a day as MICROS_PER_DAY micros). A purely local carry (days into
// months, micros into days) is insufficient: it leaves mixed-sign triples such
// as (1 month, -1 day) != (29 days) and (29 days, 24 hours) != (30 days).
// Canonicalizing via the total micros value fixes both gaps.
void Interval::normalizeIntervalEntries(interval_t input, int64_t& months, int64_t& days,
    int64_t& micros) {
    int128_t total = int128_t(input.months) * int128_t(Interval::MICROS_PER_MONTH) +
                     int128_t(input.days) * int128_t(Interval::MICROS_PER_DAY) +
                     int128_t(input.micros);
    int128_t months128 = total / int128_t(Interval::MICROS_PER_MONTH);
    int128_t rem = total % int128_t(Interval::MICROS_PER_MONTH);
    int128_t days128 = rem / int128_t(Interval::MICROS_PER_DAY);
    int128_t micros128 = rem % int128_t(Interval::MICROS_PER_DAY);
    // The canonical components always fit in int64 (|months| <= ~2.3e9,
    // |days| <= 29, |micros| < MICROS_PER_DAY); the casts cannot fail.
    Int128_t::tryCast(months128, months);
    Int128_t::tryCast(days128, days);
    Int128_t::tryCast(micros128, micros);
}

bool Interval::greaterThan(const interval_t& left, const interval_t& right) {
    int64_t lMonths = 0, lDays = 0, lMicros = 0;
    int64_t rMonths = 0, rDays = 0, rMicros = 0;
    normalizeIntervalEntries(left, lMonths, lDays, lMicros);
    normalizeIntervalEntries(right, rMonths, rDays, rMicros);
    if (lMonths > rMonths) {
        return true;
    } else if (lMonths < rMonths) {
        return false;
    }
    if (lDays > rDays) {
        return true;
    } else if (lDays < rDays) {
        return false;
    }
    return lMicros > rMicros;
}

void Interval::tryGetDatePartSpecifier(std::string specifier, DatePartSpecifier& result) {
    StringUtils::toLower(specifier);
    if (specifier == "year" || specifier == "yr" || specifier == "y" || specifier == "years" ||
        specifier == "yrs") {
        result = DatePartSpecifier::YEAR;
    } else if (specifier == "month" || specifier == "mon" || specifier == "months" ||
               specifier == "mons") {
        result = DatePartSpecifier::MONTH;
    } else if (specifier == "day" || specifier == "days" || specifier == "d" ||
               specifier == "dayofmonth") {
        result = DatePartSpecifier::DAY;
    } else if (specifier == "decade" || specifier == "dec" || specifier == "decades" ||
               specifier == "decs") {
        result = DatePartSpecifier::DECADE;
    } else if (specifier == "century" || specifier == "cent" || specifier == "centuries" ||
               specifier == "c") {
        result = DatePartSpecifier::CENTURY;
    } else if (specifier == "millennium" || specifier == "mil" || specifier == "millenniums" ||
               specifier == "millennia" || specifier == "mils" || specifier == "millenium" ||
               specifier == "milleniums") {
        result = DatePartSpecifier::MILLENNIUM;
    } else if (specifier == "microseconds" || specifier == "microsecond" || specifier == "us" ||
               specifier == "usec" || specifier == "usecs" || specifier == "usecond" ||
               specifier == "useconds") {
        result = DatePartSpecifier::MICROSECOND;
    } else if (specifier == "milliseconds" || specifier == "millisecond" || specifier == "ms" ||
               specifier == "msec" || specifier == "msecs" || specifier == "msecond" ||
               specifier == "mseconds") {
        result = DatePartSpecifier::MILLISECOND;
    } else if (specifier == "second" || specifier == "sec" || specifier == "seconds" ||
               specifier == "secs" || specifier == "s") {
        result = DatePartSpecifier::SECOND;
    } else if (specifier == "minute" || specifier == "min" || specifier == "minutes" ||
               specifier == "mins" || specifier == "m") {
        result = DatePartSpecifier::MINUTE;
    } else if (specifier == "hour" || specifier == "hr" || specifier == "hours" ||
               specifier == "hrs" || specifier == "h") {
        result = DatePartSpecifier::HOUR;
    } else if (specifier == "week" || specifier == "weeks" || specifier == "w" ||
               specifier == "weekofyear") {
        // ISO week number
        result = DatePartSpecifier::WEEK;
    } else if (specifier == "dow" || specifier == "dayofweek") {
        // day of the week (0-6, Sunday = 0)
        result = DatePartSpecifier::DOW;
    } else if (specifier == "quarter" || specifier == "quarters") {
        // quarter of the year (1-4)
        result = DatePartSpecifier::QUARTER;
    } else {
        throw ConversionException("Unrecognized interval specifier string: " + specifier + ".");
    }
}

int32_t Interval::getIntervalPart(DatePartSpecifier specifier, interval_t interval) {
    switch (specifier) {
    case DatePartSpecifier::YEAR:
        return interval.months / Interval::MONTHS_PER_YEAR;
    case DatePartSpecifier::MONTH:
        return interval.months % Interval::MONTHS_PER_YEAR;
    case DatePartSpecifier::DAY:
        return interval.days;
    case DatePartSpecifier::DECADE:
        return interval.months / Interval::MONTHS_PER_DECADE;
    case DatePartSpecifier::CENTURY:
        return interval.months / Interval::MONTHS_PER_CENTURY;
    case DatePartSpecifier::MILLENNIUM:
        return interval.months / Interval::MONTHS_PER_MILLENIUM;
    case DatePartSpecifier::QUARTER:
        return getIntervalPart(DatePartSpecifier::MONTH, interval) / Interval::MONTHS_PER_QUARTER +
               1;
    case DatePartSpecifier::MICROSECOND:
        return interval.micros % Interval::MICROS_PER_MINUTE;
    case DatePartSpecifier::MILLISECOND:
        return getIntervalPart(DatePartSpecifier::MICROSECOND, interval) /
               Interval::MICROS_PER_MSEC;
    case DatePartSpecifier::SECOND:
        return getIntervalPart(DatePartSpecifier::MICROSECOND, interval) / Interval::MICROS_PER_SEC;
    case DatePartSpecifier::MINUTE:
        return (interval.micros % Interval::MICROS_PER_HOUR) / Interval::MICROS_PER_MINUTE;
    case DatePartSpecifier::HOUR:
        return interval.micros / Interval::MICROS_PER_HOUR;
    default:
        // DOW and WEEK are accepted by tryGetDatePartSpecifier (shared with
        // date/timestamp) but are not defined for intervals. Throw instead of
        // hitting UNREACHABLE_CODE, which aborts the process (see #1694).
        throw ConversionException("date_part specifier is not supported for INTERVAL input.");
    }
}

int64_t Interval::getMicro(const interval_t& val) {
    int64_t years = val.months / MONTHS_PER_YEAR;
    int64_t months = val.months % MONTHS_PER_YEAR;
    return val.micros + years * DAYS_PER_YEAR * MICROS_PER_DAY +
           months * DAYS_PER_MONTH * MICROS_PER_DAY + val.days * MICROS_PER_DAY;
}

int64_t Interval::getNanoseconds(const interval_t& val) {
    return getMicro(val) * NANOS_PER_MICRO;
}

const regex::RE2& Interval::regexPattern1() {
    static regex::RE2 retval(
        "(?i)((0|[1-9]\\d*) "
        "+(YEARS?|YRS?|Y|MONS?|MONTHS?|DAYS?|D|DAYOFMONTH|DECADES?|DECS?|CENTURY|CENTURIES|CENT|C|"
        "MILLENN?IUMS?|MILS?|MILLENNIA|MICROSECONDS?|US|USECS?|USECONDS?|MILLISECONDS?|MS|SECONDS?|"
        "SECS?|S|MINUTES?|MINS?|M|HOURS?|HRS?|H|WEEKS?|WEEKOFYEAR|W|QUARTERS?))( +(0|[1-9]\\d*) "
        "+(YEARS?|YRS?|Y|MONS?|MONTHS?|DAYS?|D|DAYOFMONTH|DECADES?|DECS?|CENTURY|CENTURIES|CENT|C|"
        "MILLENN?IUMS?|MILS?|MILLENNIA|MICROSECONDS?|US|USECS?|USECONDS?|MILLISECONDS?|MS|SECONDS?|"
        "SECS?|S|MINUTES?|MINS?|M|HOURS?|HRS?|H|WEEKS?|WEEKOFYEAR|W|QUARTERS?))*( "
        "+\\d+:\\d{2}:\\d{2}(\\.\\d+)?)?");
    return retval;
}

const regex::RE2& Interval::regexPattern2() {
    static regex::RE2 retval("\\d+:\\d{2}:\\d{2}(\\.\\d+)?");
    return retval;
}

} // namespace common
} // namespace lbug
