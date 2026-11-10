/**
 * Native unit tests for IotBackoffPolicy and IotLifecycle (pio test -e native).
 *
 * The failure behaviour of a battery-powered device with nobody watching it.
 * IotFakePower records the exit instead of taking it, so the assertions are
 * about what the device *decided*.
 */

#include <unity.h>

#include <cstring>

#include "../iot_fakes.h"
#include "iot_lifecycle.h"
#include "iot_rtc_slot.h"
#include "iot_rtc_state.h"

// *****************************************************************************

void setUp() {}
void tearDown() {}

/// A freshly powered device: RTC memory holds arbitrary bytes, which the
/// slot replaces with a zeroed state.
static IotRtcState freshRtcState()
{
    IotRtcSlot<IotRtcState> slot;
    memset(&slot, 0x5a, sizeof(slot));
    return slot.get();
}


// *****************************************************************************
// IotBackoffPolicy
// *****************************************************************************

void test_backoff_starts_at_the_initial_duration()
{
    IotBackoffPolicy policy;  // 60 s, factor 2, cap 24 h
    TEST_ASSERT_EQUAL_INT32(60, policy.next(-1));   // not in backoff
    TEST_ASSERT_EQUAL_INT32(60, policy.next(0));
}

void test_backoff_escalates_by_the_factor()
{
    IotBackoffPolicy policy;
    TEST_ASSERT_EQUAL_INT32(120, policy.next(60));
    TEST_ASSERT_EQUAL_INT32(240, policy.next(120));
    TEST_ASSERT_EQUAL_INT32(480, policy.next(240));
}

void test_backoff_saturates_at_the_maximum()
{
    IotBackoffPolicy policy{ 60, 3, 1000 };
    TEST_ASSERT_EQUAL_INT32(900, policy.next(300));
    TEST_ASSERT_EQUAL_INT32(1000, policy.next(900));   // capped
    TEST_ASSERT_EQUAL_INT32(1000, policy.next(1000));  // stays there
}

void test_backoff_does_not_overflow()
{
    // int32 multiplication would wrap here and yield a negative duration,
    // i.e. an immediate wakeup - the opposite of backing off
    IotBackoffPolicy policy{ 60, 10, 24 * 60 * 60 };
    TEST_ASSERT_EQUAL_INT32(24 * 60 * 60, policy.next(2000000000));
    TEST_ASSERT_TRUE(policy.next(2000000000) > 0);
}

void test_backoff_never_returns_a_zero_sleep()
{
    // a misconfiguration must not turn the backoff into a busy reboot loop
    IotBackoffPolicy policy{ 0, 0, 0 };
    TEST_ASSERT_TRUE(policy.next(-1) >= 1);
    TEST_ASSERT_TRUE(policy.next(0) >= 1);
    TEST_ASSERT_TRUE(policy.next(100) >= 1);
}

void test_backoff_factor_below_two_still_advances()
{
    IotBackoffPolicy policy{ 60, 1, 24 * 60 * 60 };
    TEST_ASSERT_EQUAL_INT32(60, policy.next(60));  // constant, never shrinking
}


// *****************************************************************************
// IotLifecycle: starting a cycle
// *****************************************************************************

void test_begin_counts_the_first_boot_after_power_on()
{
    IotRtcState rtc = freshRtcState();
    IotFakePower power;
    power.resetReasonValue = IotResetReason::PowerOn;
    IotLifecycle lifecycle(power, rtc);

    lifecycle.begin();

    TEST_ASSERT_EQUAL_UINT32(1, lifecycle.bootCount());
    TEST_ASSERT_EQUAL_INT32(0, lifecycle.backoffDuration_s());  // not in backoff
    TEST_ASSERT_FALSE(lifecycle.isAbnormalStart());
}

void test_begin_counts_up_across_deep_sleep_cycles()
{
    IotRtcState rtc = freshRtcState();
    IotFakePower power;
    IotLifecycle lifecycle(power, rtc);

    lifecycle.begin();
    TEST_ASSERT_EQUAL_UINT32(1, lifecycle.bootCount());

    // a deep sleep leaves the RTC state alone; the next boot sees it again
    power.resetReasonValue = IotResetReason::DeepSleep;
    IotLifecycle secondCycle(power, rtc);
    secondCycle.begin();
    TEST_ASSERT_EQUAL_UINT32(2, secondCycle.bootCount());
}

void test_begin_does_not_act_on_an_abnormal_reset()
{
    // the caller decides, once its logging works
    IotRtcState rtc = freshRtcState();
    IotFakePower power;
    power.resetReasonValue = IotResetReason::Watchdog;
    IotLifecycle lifecycle(power, rtc);

    lifecycle.begin();

    TEST_ASSERT_TRUE(lifecycle.isAbnormalStart());
    TEST_ASSERT_FALSE(power.wasCalled());  // nothing happened yet
}

void test_brownout_and_panic_are_abnormal_starts()
{
    IotRtcState rtc = freshRtcState();
    IotFakePower power;

    for (IotResetReason reason : { IotResetReason::Panic, IotResetReason::Brownout,
                                   IotResetReason::Watchdog })
    {
        power.resetReasonValue = reason;
        IotLifecycle lifecycle(power, rtc);
        lifecycle.begin();
        TEST_ASSERT_TRUE(lifecycle.isAbnormalStart());
    }

    for (IotResetReason reason : { IotResetReason::PowerOn, IotResetReason::DeepSleep,
                                   IotResetReason::Software })
    {
        power.resetReasonValue = reason;
        IotLifecycle lifecycle(power, rtc);
        lifecycle.begin();
        TEST_ASSERT_FALSE(lifecycle.isAbnormalStart());
    }
}

void test_begin_counts_crashes_but_not_orderly_starts()
{
    IotRtcState rtc = freshRtcState();
    IotFakePower power;

    for (IotResetReason reason : { IotResetReason::PowerOn, IotResetReason::Panic,
                                   IotResetReason::DeepSleep, IotResetReason::Watchdog,
                                   IotResetReason::Software, IotResetReason::Brownout,
                                   IotResetReason::External })
    {
        power.resetReasonValue = reason;
        IotLifecycle lifecycle(power, rtc);
        lifecycle.begin();
    }

    IotLifecycle last(power, rtc);
    TEST_ASSERT_EQUAL_UINT32(3, last.crashCount());
    TEST_ASSERT_EQUAL_UINT32(7, last.bootCount());
}

void test_failure_counters_survive_deep_sleep_but_not_a_power_loss()
{
    IotRtcState rtc = freshRtcState();
    IotFakePower power;
    IotLifecycle lifecycle(power, rtc);
    lifecycle.begin();
    lifecycle.countPanic();
    lifecycle.countPanic();
    lifecycle.countUndervoltage();
    lifecycle.deepSleep(60);

    power.resetReasonValue = IotResetReason::DeepSleep;
    IotLifecycle nextCycle(power, rtc);
    nextCycle.begin();
    TEST_ASSERT_EQUAL_UINT32(2, nextCycle.panicCount());
    TEST_ASSERT_EQUAL_UINT32(1, nextCycle.undervoltageCount());
    TEST_ASSERT_EQUAL_UINT32(0, nextCycle.crashCount());

    IotRtcState afterPowerLoss = freshRtcState();
    IotLifecycle rebooted(power, afterPowerLoss);
    rebooted.begin();
    TEST_ASSERT_EQUAL_UINT32(0, rebooted.panicCount());
    TEST_ASSERT_EQUAL_UINT32(0, rebooted.undervoltageCount());
}

// *****************************************************************************
// IotLifecycle: ending a cycle
// *****************************************************************************

void test_deep_sleep_records_the_cycle_and_clears_the_backoff()
{
    IotRtcState rtc = freshRtcState();
    IotFakePower power;
    IotLifecycle lifecycle(power, rtc);
    lifecycle.begin();
    rtc.panicSleepDuration_s = 240;   // we were in backoff
    power.uptimeValue_ms = 812;

    lifecycle.deepSleep(300);

    TEST_ASSERT_TRUE(IotFakePower::Action::DeepSleep == power.lastCall().action);
    TEST_ASSERT_EQUAL_UINT32(300, power.lastCall().duration_s);
    TEST_ASSERT_EQUAL_INT32(0, rtc.panicSleepDuration_s);  // orderly exit clears it
    TEST_ASSERT_EQUAL_INT32(300, rtc.lastSleepDuration_s);
    TEST_ASSERT_EQUAL_INT64(812, rtc.activeDuration_ms);
}

void test_restart_and_shutdown_report_no_sleep_duration()
{
    IotRtcState rtc = freshRtcState();
    IotFakePower power;
    IotLifecycle lifecycle(power, rtc);
    lifecycle.begin();

    lifecycle.restart();
    TEST_ASSERT_TRUE(IotFakePower::Action::Restart == power.lastCall().action);
    TEST_ASSERT_EQUAL_INT32(0, rtc.lastSleepDuration_s);

    lifecycle.shutdown();
    TEST_ASSERT_TRUE(IotFakePower::Action::Shutdown == power.lastCall().action);
    TEST_ASSERT_EQUAL_INT32(0, rtc.lastSleepDuration_s);
}

void test_a_failure_exit_keeps_the_backoff_state()
{
    // e.g. shutting down on undervoltage: the cycle did not end because the
    // work was done, so the backoff must not be reset
    IotRtcState rtc = freshRtcState();
    IotFakePower power;
    IotLifecycle lifecycle(power, rtc);
    lifecycle.begin();
    rtc.panicSleepDuration_s = 240;

    lifecycle.shutdown(/*isFailure=*/true);

    TEST_ASSERT_TRUE(IotFakePower::Action::Shutdown == power.lastCall().action);
    TEST_ASSERT_EQUAL_INT32(240, rtc.panicSleepDuration_s);
}

void test_exit_hooks_run_before_the_device_sleeps()
{
    IotRtcState rtc = freshRtcState();
    IotFakePower power;
    IotLifecycle lifecycle(power, rtc);
    lifecycle.begin();

    std::vector<int> order;
    lifecycle.addExitHook([&order, &power] {
        order.push_back(1);
        // still before the exit: this is what lets the logger flush
        TEST_ASSERT_FALSE(power.wasCalled());
    });
    lifecycle.addExitHook([&order] { order.push_back(2); });

    lifecycle.deepSleep(60);

    TEST_ASSERT_EQUAL_size_t(2, order.size());
    TEST_ASSERT_EQUAL_INT(1, order[0]);   // registration order
    TEST_ASSERT_EQUAL_INT(2, order[1]);
    TEST_ASSERT_TRUE(power.wasCalled());
}

void test_exit_hooks_also_run_on_the_failure_path()
{
    // a panic must still get its buffered logs out
    IotRtcState rtc = freshRtcState();
    IotFakePower power;
    IotLifecycle lifecycle(power, rtc);
    lifecycle.begin();

    bool flushed = false;
    lifecycle.addExitHook([&flushed] { flushed = true; });

    lifecycle.enterBackoff();

    TEST_ASSERT_TRUE(flushed);
}


// *****************************************************************************
// IotLifecycle: the backoff across cycles
// *****************************************************************************

void test_backoff_escalates_over_consecutive_failures()
{
    IotRtcState rtc = freshRtcState();
    IotFakePower power;
    IotBackoffPolicy policy{ 60, 3, 600 };

    // three cycles that each end in a failure; the RTC state carries over
    const uint32_t expected[] = { 60, 180, 540 };
    for (unsigned cycle = 0; cycle < 3; cycle++)
    {
        IotLifecycle lifecycle(power, rtc);
        lifecycle.setBackoffPolicy(policy);
        lifecycle.begin();
        power.reset();

        uint32_t slept = lifecycle.enterBackoff();

        TEST_ASSERT_EQUAL_UINT32(expected[cycle], slept);
        TEST_ASSERT_EQUAL_UINT32(expected[cycle], power.lastCall().duration_s);
        TEST_ASSERT_EQUAL_INT32((int32_t)expected[cycle], rtc.panicSleepDuration_s);
    }

    // and it saturates rather than growing without bound
    IotLifecycle lifecycle(power, rtc);
    lifecycle.setBackoffPolicy(policy);
    lifecycle.begin();
    TEST_ASSERT_EQUAL_UINT32(600, lifecycle.enterBackoff());
}

void test_an_orderly_cycle_ends_the_backoff()
{
    IotRtcState rtc = freshRtcState();
    IotFakePower power;

    IotLifecycle failing(power, rtc);
    failing.begin();
    failing.enterBackoff();
    TEST_ASSERT_EQUAL_INT32(60, rtc.panicSleepDuration_s);

    // the device recovers and completes a cycle normally
    IotLifecycle recovered(power, rtc);
    recovered.begin();
    recovered.deepSleep(300);
    TEST_ASSERT_EQUAL_INT32(0, rtc.panicSleepDuration_s);

    // so the next failure starts over at the initial duration, not where it left off
    IotLifecycle failingAgain(power, rtc);
    failingAgain.begin();
    TEST_ASSERT_EQUAL_UINT32(60, failingAgain.enterBackoff());
}

void test_a_power_loss_starts_the_backoff_over()
{
    // RTC memory does not survive a power-on reset, which is wanted here:
    // someone physically touched the device
    IotRtcState rtc = freshRtcState();
    IotFakePower power;
    IotLifecycle lifecycle(power, rtc);
    lifecycle.begin();
    lifecycle.enterBackoff();
    lifecycle.enterBackoff();
    TEST_ASSERT_EQUAL_INT32(120, rtc.panicSleepDuration_s);

    IotRtcState afterPowerLoss = freshRtcState();
    IotLifecycle rebooted(power, afterPowerLoss);
    rebooted.begin();
    TEST_ASSERT_EQUAL_INT32(0, rebooted.backoffDuration_s());
    TEST_ASSERT_EQUAL_UINT32(1, rebooted.bootCount());
}

void test_custom_power_adapter_replaces_the_default()
{
    IotRtcState rtc = freshRtcState();
    IotFakePower defaultPower;
    IotFakePower customPower;
    IotLifecycle lifecycle(defaultPower, rtc);
    lifecycle.setPower(customPower);
    lifecycle.begin();

    lifecycle.deepSleep(120);

    TEST_ASSERT_FALSE(defaultPower.wasCalled());
    TEST_ASSERT_TRUE(customPower.wasCalled());
    TEST_ASSERT_EQUAL_UINT32(120, customPower.lastCall().duration_s);
}


// *****************************************************************************

int main(int argc, char **argv)
{
    UNITY_BEGIN();

    RUN_TEST(test_backoff_starts_at_the_initial_duration);
    RUN_TEST(test_backoff_escalates_by_the_factor);
    RUN_TEST(test_backoff_saturates_at_the_maximum);
    RUN_TEST(test_backoff_does_not_overflow);
    RUN_TEST(test_backoff_never_returns_a_zero_sleep);
    RUN_TEST(test_backoff_factor_below_two_still_advances);

    RUN_TEST(test_begin_counts_the_first_boot_after_power_on);
    RUN_TEST(test_begin_counts_up_across_deep_sleep_cycles);
    RUN_TEST(test_begin_does_not_act_on_an_abnormal_reset);
    RUN_TEST(test_brownout_and_panic_are_abnormal_starts);
    RUN_TEST(test_begin_counts_crashes_but_not_orderly_starts);
    RUN_TEST(test_failure_counters_survive_deep_sleep_but_not_a_power_loss);

    RUN_TEST(test_deep_sleep_records_the_cycle_and_clears_the_backoff);
    RUN_TEST(test_restart_and_shutdown_report_no_sleep_duration);
    RUN_TEST(test_a_failure_exit_keeps_the_backoff_state);
    RUN_TEST(test_exit_hooks_run_before_the_device_sleeps);
    RUN_TEST(test_exit_hooks_also_run_on_the_failure_path);

    RUN_TEST(test_backoff_escalates_over_consecutive_failures);
    RUN_TEST(test_an_orderly_cycle_ends_the_backoff);
    RUN_TEST(test_a_power_loss_starts_the_backoff_over);
    RUN_TEST(test_custom_power_adapter_replaces_the_default);

    return UNITY_END();
}
