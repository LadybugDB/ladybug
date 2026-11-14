#include <limits>

#include "common/exception/conversion.h"
#include "common/exception/overflow.h"
#include "common/types/date_t.h"
#include "common/types/timestamp_t.h"
#include "gtest/gtest.h"

using namespace lbug::common;

TEST(TimestampTests, FromDatetime) {
    // day is out of range
    try {
        Timestamp::fromDateTime(Date::fromDate(1968, 12, 42), Time::fromTime(21, 32, 51));
        FAIL();
    } catch (ConversionException& e) {
        ASSERT_STREQ(e.what(), "Conversion exception: Date out of range: 1968-12-42.");
    } catch (std::exception& e) {
        FAIL();
    }

    // 2021 is not a leap year, February only has 28 days.
    try {
        Timestamp::fromDateTime(Date::fromDate(2021, 2, 29), Time::fromTime(21, 32, 51));
        FAIL();
    } catch (ConversionException& e) {
        ASSERT_STREQ(e.what(), "Conversion exception: Date out of range: 2021-2-29.");
    } catch (std::exception& e) {
        FAIL();
    }

    // hour is out of range
    try {
        Timestamp::fromDateTime(Date::fromDate(1968, 12, 22), Time::fromTime(25, 32, 51));
        FAIL();
    } catch (ConversionException& e) {
        ASSERT_STREQ(e.what(),
            "Conversion exception: Time field value out of range: 25:32:51[.0].");
    } catch (std::exception& e) {
        FAIL();
    }

    // second is out of range
    try {
        Timestamp::fromDateTime(Date::fromDate(2021, 2, 28), Time::fromTime(5, 52, 70));
        FAIL();
    } catch (ConversionException& e) {
        ASSERT_STREQ(e.what(), "Conversion exception: Time field value out of range: 5:52:70[.0].");
    } catch (std::exception& e) {
        FAIL();
    }

    // microsecond is out of rarnge
    try {
        Timestamp::fromDateTime(Date::fromDate(2021, 2, 28), Time::fromTime(5, 52, 42, 1000002));
        FAIL();
    } catch (ConversionException& e) {
        ASSERT_STREQ(e.what(),
            "Conversion exception: Time field value out of range: 5:52:42[.1000002].");
    } catch (std::exception& e) {
        FAIL();
    }

    EXPECT_EQ(
        Timestamp::fromDateTime(Date::fromDate(2020, 10, 22), Time::fromTime(21, 32, 51)).value,
        1603402371000000);
    EXPECT_EQ(Timestamp::fromDateTime(Date::fromDate(1978, 5, 22), Time::fromTime(00, 54, 32, 7891))
                  .value,
        264646472007891);
}

TEST(TimestampTests, Convert) {
    date_t date;
    dtime_t time;

    // 1603402371000000 is 2020-10-22 21:32:51
    Timestamp::convert(timestamp_t(1603402371000000), date, time);
    EXPECT_EQ(date.days, 18557);
    EXPECT_EQ(time.micros, 77571000000);

    // 4501210883005612 is 2112-08-21 08:21:23.005612
    Timestamp::convert(timestamp_t(4501210883005612), date, time);
    EXPECT_EQ(date.days, 52097);
    EXPECT_EQ(time.micros, 30083005612);
}

TEST(TimestampTests, FromCString) {
    EXPECT_EQ(
        Timestamp::fromCString("2112-08-21 08:21:23.005612", strlen("2112-08-21 08:21:23.005612"))
            .value,
        4501210883005612);
    EXPECT_EQ(Timestamp::fromCString("1992-04-28T09:22:56", strlen("1992-04-28T09:22:56")).value,
        704452976000000);
    EXPECT_EQ(Timestamp::fromCString("2023-08-12", strlen("2023-08-12")).value, 1691798400000000);

    // Timestamps with timezone.
    EXPECT_EQ(
        Timestamp::fromCString("2112-08-21 08:21:23.005612Z", strlen("2112-08-21 08:21:23.005612Z"))
            .value,
        4501210883005612);
    EXPECT_EQ(Timestamp::fromCString("2112-08-21 08:21:23.005612Z+00:00",
                  strlen("2112-08-21 08:21:23.005612Z+00:00"))
                  .value,
        4501210883005612);
    EXPECT_EQ(Timestamp::fromCString("2112-08-21 08:21:23.005612Z+02:00",
                  strlen("2112-08-21 08:21:23.005612Z+02:00"))
                  .value,
        4501203683005612);
    EXPECT_EQ(
        Timestamp::fromCString("1992-04-28T09:22:56-09:00", strlen("1992-04-28T09:22:56-09:00"))
            .value,
        704485376000000);
    EXPECT_EQ(
        Timestamp::fromCString("1992-04-28T09:22:56-09", strlen("1992-04-28T09:22:56-09")).value,
        704485376000000);
    EXPECT_EQ(Timestamp::fromCString("1992-04-28T09:22:56-09:00   ",
                  strlen("1992-04-28T09:22:56-09:00   "))
                  .value,
        704485376000000);
    EXPECT_EQ(
        Timestamp::fromCString("1992-04-28T09:22:56-06:30", strlen("1992-04-28T09:22:56-06:30"))
            .value,
        704476376000000);
}

TEST(TimestampTests, toString) {
    EXPECT_EQ(Timestamp::toString(timestamp_t(1691798400000000)), "2023-08-12 00:00:00");
    EXPECT_EQ(Timestamp::toString(timestamp_t(4501210883005612)), "2112-08-21 08:21:23.005612");
    EXPECT_EQ(Timestamp::toString(timestamp_t(704452976000000)), "1992-04-28 09:22:56");
}

TEST(TimestampTests, FromEpochSecondsOverflow) {
    // Sibling of the EpochMsOverflow cases in test/test_files/function/timestamp.test,
    // for the seconds path: fromEpochSeconds multiplies by MICROS_PER_SEC (1000000)
    // through the checked Multiply overload and must throw on overflow instead of
    // wrapping. No SQL surface can construct a TIMESTAMP_SEC value large enough to
    // reach this overflow (every SQL construction divides an int64 microsecond
    // timestamp by MICROS_PER_SEC first), so the case lives in this unit test.
    try {
        Timestamp::fromEpochSeconds(std::numeric_limits<int64_t>::max());
        FAIL();
    } catch (OverflowException& e) {
        ASSERT_STREQ(e.what(), "Overflow exception: Value 9223372036854775807 * 1000000 "
                               "is not within INT64 range.");
    } catch (std::exception& e) {
        FAIL();
    }

    try {
        Timestamp::fromEpochSeconds(std::numeric_limits<int64_t>::min());
        FAIL();
    } catch (OverflowException& e) {
        ASSERT_STREQ(e.what(), "Overflow exception: Value -9223372036854775808 * 1000000 "
                               "is not within INT64 range.");
    } catch (std::exception& e) {
        FAIL();
    }

    // In-range control: 253370764800 sec is 9999-01-01 00:00:00 in microseconds.
    EXPECT_EQ(Timestamp::fromEpochSeconds(253370764800).value, 253370764800000000);
}
