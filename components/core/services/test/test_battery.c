#include "unity.h"
#include "services/battery.h"

#include <limits.h>

static void assert_soc(battery_filter_t *filter, int pct, int level) {
    TEST_ASSERT_EQUAL_INT(pct, battery_feed_pct(filter, pct));
    TEST_ASSERT_EQUAL_INT(pct, battery_percent(filter));
    TEST_ASSERT_EQUAL_INT(level, battery_level(filter));
}

TEST_CASE("battery SOC jumps are displayed immediately", "[battery]") {
    battery_filter_t filter;
    battery_init(&filter);

    assert_soc(&filter, 0, 1);
    assert_soc(&filter, 80, 5);
    assert_soc(&filter, 20, 2);
    assert_soc(&filter, 100, 5);
    assert_soc(&filter, 0, 1);
}

TEST_CASE("battery SOC thresholds have no hysteresis in either direction", "[battery]") {
    const int thresholds[] = {20, 40, 60, 80};
    battery_filter_t filter;
    battery_init(&filter);

    for (unsigned i = 0; i < sizeof thresholds / sizeof thresholds[0]; ++i) {
        int boundary = thresholds[i];
        int lower_level = (int)i + 1;
        assert_soc(&filter, boundary - 1, lower_level);
        assert_soc(&filter, boundary, lower_level + 1);
        assert_soc(&filter, boundary + 1, lower_level + 1);
        assert_soc(&filter, boundary, lower_level + 1);
        assert_soc(&filter, boundary - 1, lower_level);
    }
}

TEST_CASE("battery invalid SOC preserves the previous reading and filter state", "[battery]") {
    const int invalid[] = {-1, INT_MIN, 101, INT_MAX};
    battery_filter_t filter;
    battery_init(&filter);

    /* Check both before the first valid reading and with existing ADC state. */
    for (int initialized = 0; initialized < 2; ++initialized) {
        if (initialized) {
            battery_feed(&filter, 4200);
            assert_soc(&filter, 20, 2);
        }
        battery_filter_t previous = filter;
        for (unsigned i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) {
            TEST_ASSERT_EQUAL_INT(previous.pct, battery_feed_pct(&filter, invalid[i]));
            TEST_ASSERT_EQUAL_INT(previous.pct, battery_percent(&filter));
            TEST_ASSERT_EQUAL_INT(previous.level, filter.level);
            TEST_ASSERT_EQUAL_INT(previous.ema, filter.ema);
            TEST_ASSERT_EQUAL_INT(previous.primed, filter.primed);
        }
    }
}

TEST_CASE("battery SOC does not prime the ADC voltage filter", "[battery]") {
    battery_filter_t filter;
    battery_init(&filter);

    assert_soc(&filter, 80, 5);
    TEST_ASSERT_FALSE(filter.primed);
    TEST_ASSERT_EQUAL_INT(0, filter.ema);
    TEST_ASSERT_EQUAL_INT(0, battery_feed(&filter, 3000));
    TEST_ASSERT_EQUAL_INT(3000, filter.ema);
    TEST_ASSERT_EQUAL_INT(1, battery_level(&filter));
}

TEST_CASE("battery SOC is independent of an existing ADC voltage EMA", "[battery]") {
    battery_filter_t filter;
    battery_init(&filter);

    TEST_ASSERT_EQUAL_INT(100, battery_feed(&filter, 4200));
    assert_soc(&filter, 20, 2);
    assert_soc(&filter, 80, 5);
    TEST_ASSERT_TRUE(filter.primed);
    TEST_ASSERT_EQUAL_INT(4200, filter.ema);

    /* The legacy ADC path still smooths volts, never the preceding SOC. */
    TEST_ASSERT_EQUAL_INT(88, battery_feed(&filter, 3000));
    TEST_ASSERT_EQUAL_INT(4050, filter.ema);
    assert_soc(&filter, 0, 1);
    TEST_ASSERT_EQUAL_INT(4050, filter.ema);
}
