#include <unity.h>
#include "Arduino.h"
#include "MockTime.h"

// Include mock implementations
#include "../mocks/Arduino.cpp"

// Include the TimeBudgetManager implementation
#include "TimeBudgetManager.h"
#include "../../src/TimeBudgetManager.cpp"

void setUp(void) {
    // Reset time before each test
    MockTimeState::reset();
}

void tearDown(void) {
    // Cleanup after each test
}

// Test session initialization with different durations
void test_session_initialization_basic() {
    TimeBudgetManager manager;
    
    MockTimeState::setMillis(1000);
    manager.startSession(5);  // 5 seconds
    
    // Should have full budget available
    TEST_ASSERT_EQUAL(5000, manager.getRemainingBudgetMs());
    TEST_ASSERT_TRUE(manager.hasBudget());
}

void test_session_initialization_different_durations() {
    TimeBudgetManager manager;
    
    // Test 10 second session
    MockTimeState::setMillis(0);
    manager.startSession(10);
    TEST_ASSERT_EQUAL(10000, manager.getRemainingBudgetMs());
    
    // Test 30 second session
    MockTimeState::setMillis(5000);
    manager.startSession(30);
    TEST_ASSERT_EQUAL(30000, manager.getRemainingBudgetMs());
    
    // Test 1 second session
    MockTimeState::setMillis(10000);
    manager.startSession(1);
    TEST_ASSERT_EQUAL(1000, manager.getRemainingBudgetMs());
}

// Test budget remaining calculation (active time only)
void test_budget_remaining_calculation() {
    TimeBudgetManager manager;
    
    MockTimeState::setMillis(1000);
    manager.startSession(10);  // 10 seconds = 10000ms
    
    // Initially should have full budget
    TEST_ASSERT_EQUAL(10000, manager.getRemainingBudgetMs());
    TEST_ASSERT_EQUAL(0, manager.getActiveTimeMs());
    
    // Advance time by 3 seconds
    MockTimeState::advanceMillis(3000);
    TEST_ASSERT_EQUAL(7000, manager.getRemainingBudgetMs());
    TEST_ASSERT_EQUAL(3000, manager.getActiveTimeMs());
    
    // Advance time by another 5 seconds
    MockTimeState::advanceMillis(5000);
    TEST_ASSERT_EQUAL(2000, manager.getRemainingBudgetMs());
    TEST_ASSERT_EQUAL(8000, manager.getActiveTimeMs());
    
    // Advance time past budget
    MockTimeState::advanceMillis(3000);
    TEST_ASSERT_EQUAL(0, manager.getRemainingBudgetMs());
    TEST_ASSERT_EQUAL(11000, manager.getActiveTimeMs());
}

void test_has_budget() {
    TimeBudgetManager manager;
    
    MockTimeState::setMillis(0);
    manager.startSession(5);  // 5 seconds
    
    TEST_ASSERT_TRUE(manager.hasBudget());
    
    // Advance to just before budget exhaustion
    MockTimeState::advanceMillis(4999);
    TEST_ASSERT_TRUE(manager.hasBudget());
    
    // Advance to exactly budget exhaustion
    MockTimeState::advanceMillis(1);
    TEST_ASSERT_FALSE(manager.hasBudget());
    
    // Advance past budget
    MockTimeState::advanceMillis(1000);
    TEST_ASSERT_FALSE(manager.hasBudget());
}

// Test upload time estimation with various file sizes
void test_upload_time_estimation_default_rate() {
    TimeBudgetManager manager;
    
    // Default rate is 40 KB/s = 40960 bytes/s
    unsigned long defaultRate = 40 * 1024;
    
    // Test 40 KB file (should take exactly 1 second at 40 KB/s)
    unsigned long fileSize1 = 40 * 1024;
    unsigned long estimatedTime1 = manager.estimateUploadTimeMs(fileSize1);
    TEST_ASSERT_EQUAL(1000, estimatedTime1);
    
    // Test 20 KB file (should take exactly 0.5 seconds)
    unsigned long fileSize2 = 20 * 1024;
    unsigned long estimatedTime2 = manager.estimateUploadTimeMs(fileSize2);
    TEST_ASSERT_EQUAL(500, estimatedTime2);
    
    // Test 80 KB file (should take exactly 2 seconds)
    unsigned long fileSize3 = 80 * 1024;
    unsigned long estimatedTime3 = manager.estimateUploadTimeMs(fileSize3);
    TEST_ASSERT_EQUAL(2000, estimatedTime3);
}

void test_upload_time_estimation_various_sizes() {
    TimeBudgetManager manager;
    
    // Test small file (1 KB) - at 40 KB/s should be ~25ms
    unsigned long smallFile = 1 * 1024;
    unsigned long estimatedSmall = manager.estimateUploadTimeMs(smallFile);
    TEST_ASSERT_GREATER_THAN(0, estimatedSmall);
    TEST_ASSERT_LESS_THAN(100, estimatedSmall);  // Should be < 100ms
    
    // Test medium file (40 KB) - at 40 KB/s should be ~1000ms
    unsigned long mediumFile = 40 * 1024;
    unsigned long estimatedMedium = manager.estimateUploadTimeMs(mediumFile);
    TEST_ASSERT_GREATER_THAN(900, estimatedMedium);
    TEST_ASSERT_LESS_THAN(1100, estimatedMedium);  // Should be ~1000ms
    
    // Test large file (400 KB) - at 40 KB/s should be ~10000ms
    unsigned long largeFile = 400 * 1024;
    unsigned long estimatedLarge = manager.estimateUploadTimeMs(largeFile);
    TEST_ASSERT_GREATER_THAN(9000, estimatedLarge);
    TEST_ASSERT_LESS_THAN(11000, estimatedLarge);  // Should be ~10000ms
}

void test_can_upload_file() {
    TimeBudgetManager manager;
    
    MockTimeState::setMillis(0);
    manager.startSession(5);  // 5 seconds = 5000ms
    
    // Small file that fits in budget (10 KB, ~250ms at 40 KB/s)
    TEST_ASSERT_TRUE(manager.canUploadFile(10 * 1024));
    
    // Medium file that fits in budget (160 KB, ~4000ms at 40 KB/s)
    TEST_ASSERT_TRUE(manager.canUploadFile(160 * 1024));
    
    // Large file that doesn't fit (800 KB, ~20000ms at 40 KB/s)
    TEST_ASSERT_FALSE(manager.canUploadFile(800 * 1024));
    
    // Advance time and check again
    MockTimeState::advanceMillis(3000);  // 2 seconds remaining
    
    // Small file still fits
    TEST_ASSERT_TRUE(manager.canUploadFile(10 * 1024));
    
    // Medium file no longer fits
    TEST_ASSERT_FALSE(manager.canUploadFile(160 * 1024));
}

// Test transmission rate averaging over multiple uploads
void test_transmission_rate_single_upload() {
    TimeBudgetManager manager;
    
    MockTimeState::setMillis(0);
    manager.startSession(10);
    
    // Record upload: 512 KB in 1000ms = 512 KB/s
    manager.recordUpload(512 * 1024, 1000);
    
    // Estimate should now use the recorded rate
    unsigned long fileSize = 512 * 1024;
    unsigned long estimatedTime = manager.estimateUploadTimeMs(fileSize);
    TEST_ASSERT_EQUAL(1000, estimatedTime);
}

void test_transmission_rate_averaging() {
    TimeBudgetManager manager;
    
    MockTimeState::setMillis(0);
    manager.startSession(30);
    
    // Record 5 uploads with different rates
    // Upload 1: 512 KB in 1000ms = 512 KB/s = 524288 bytes/s
    manager.recordUpload(512 * 1024, 1000);
    
    // Upload 2: 256 KB in 500ms = 512 KB/s = 524288 bytes/s
    manager.recordUpload(256 * 1024, 500);
    
    // Upload 3: 1024 KB in 2000ms = 512 KB/s = 524288 bytes/s
    manager.recordUpload(1024 * 1024, 2000);
    
    // Upload 4: 512 KB in 500ms = 1024 KB/s = 1048576 bytes/s
    manager.recordUpload(512 * 1024, 500);
    
    // Upload 5: 512 KB in 2000ms = 256 KB/s = 262144 bytes/s
    manager.recordUpload(512 * 1024, 2000);
    
    // Average should be (524288 + 524288 + 524288 + 1048576 + 262144) / 5 = 576716.8 bytes/s
    // For 576716.8 bytes/s, 512 KB should take ~909ms
    unsigned long fileSize = 512 * 1024;
    unsigned long estimatedTime = manager.estimateUploadTimeMs(fileSize);
    
    // Allow some tolerance for integer division
    TEST_ASSERT_GREATER_THAN(850, estimatedTime);
    TEST_ASSERT_LESS_THAN(950, estimatedTime);
}

void test_transmission_rate_history_limit() {
    TimeBudgetManager manager;
    
    MockTimeState::setMillis(0);
    manager.startSession(60);
    
    // Record 7 uploads (more than history size of 5)
    // First two should be dropped from average
    for (int i = 0; i < 7; i++) {
        // Each upload: 512 KB in 1000ms = 512 KB/s
        manager.recordUpload(512 * 1024, 1000);
    }
    
    // Average should still be based on last 5 uploads only
    unsigned long fileSize = 512 * 1024;
    unsigned long estimatedTime = manager.estimateUploadTimeMs(fileSize);
    TEST_ASSERT_EQUAL(1000, estimatedTime);
}

void test_transmission_rate_varying_speeds() {
    TimeBudgetManager manager;
    
    MockTimeState::setMillis(0);
    manager.startSession(30);
    
    // Record uploads with very different speeds
    // Fast upload: 512 KB in 250ms = 2048 KB/s
    manager.recordUpload(512 * 1024, 250);
    
    // Slow upload: 512 KB in 4000ms = 128 KB/s
    manager.recordUpload(512 * 1024, 4000);
    
    // Medium upload: 512 KB in 1000ms = 512 KB/s
    manager.recordUpload(512 * 1024, 1000);
    
    // Average: (2097152 + 131072 + 524288) / 3 = 917504 bytes/s
    // For 512 KB at 917504 bytes/s: ~571ms
    unsigned long fileSize = 512 * 1024;
    unsigned long estimatedTime = manager.estimateUploadTimeMs(fileSize);
    
    TEST_ASSERT_GREATER_THAN(500, estimatedTime);
    TEST_ASSERT_LESS_THAN(650, estimatedTime);
}

void test_record_upload_zero_time() {
    TimeBudgetManager manager;
    
    MockTimeState::setMillis(0);
    manager.startSession(10);
    
    // Record upload with zero elapsed time (should be ignored)
    manager.recordUpload(512 * 1024, 0);
    
    // Should still use default rate (40 KB/s = 40960 bytes/s)
    // 40 KB file should take 1000ms at default rate
    unsigned long fileSize = 40 * 1024;
    unsigned long estimatedTime = manager.estimateUploadTimeMs(fileSize);
    TEST_ASSERT_EQUAL(1000, estimatedTime);  // Default 40 KB/s
}

// Test retry multiplier application
void test_retry_multiplier_basic() {
    TimeBudgetManager manager;
    
    MockTimeState::setMillis(0);
    manager.startSession(5, 1);  // 5 seconds, multiplier 1
    TEST_ASSERT_EQUAL(5000, manager.getRemainingBudgetMs());
    
    MockTimeState::setMillis(1000);
    manager.startSession(5, 2);  // 5 seconds, multiplier 2
    TEST_ASSERT_EQUAL(10000, manager.getRemainingBudgetMs());
    
    MockTimeState::setMillis(2000);
    manager.startSession(5, 3);  // 5 seconds, multiplier 3
    TEST_ASSERT_EQUAL(15000, manager.getRemainingBudgetMs());
}

void test_retry_multiplier_various_values() {
    TimeBudgetManager manager;
    
    // Test multiplier 4
    MockTimeState::setMillis(0);
    manager.startSession(10, 4);
    TEST_ASSERT_EQUAL(40000, manager.getRemainingBudgetMs());
    
    // Test multiplier 5
    MockTimeState::setMillis(1000);
    manager.startSession(10, 5);
    TEST_ASSERT_EQUAL(50000, manager.getRemainingBudgetMs());
    
    // Test multiplier 10
    MockTimeState::setMillis(2000);
    manager.startSession(10, 10);
    TEST_ASSERT_EQUAL(100000, manager.getRemainingBudgetMs());
}

void test_retry_multiplier_with_time_progression() {
    TimeBudgetManager manager;
    
    MockTimeState::setMillis(0);
    manager.startSession(5, 3);  // 5 seconds * 3 = 15 seconds
    
    TEST_ASSERT_EQUAL(15000, manager.getRemainingBudgetMs());
    TEST_ASSERT_TRUE(manager.hasBudget());
    
    // Advance time
    MockTimeState::advanceMillis(10000);  // 10 seconds elapsed
    TEST_ASSERT_EQUAL(5000, manager.getRemainingBudgetMs());
    TEST_ASSERT_TRUE(manager.hasBudget());
    
    // Advance past budget
    MockTimeState::advanceMillis(6000);  // 16 seconds total
    TEST_ASSERT_EQUAL(0, manager.getRemainingBudgetMs());
    TEST_ASSERT_FALSE(manager.hasBudget());
}

// Test wait time calculation (fixed 5 minutes)
void test_wait_time_calculation() {
    TimeBudgetManager manager;
    
    MockTimeState::setMillis(0);
    manager.startSession(5);  // 5 seconds
    
    // Wait time is fixed at 5 minutes = 300000ms
    TEST_ASSERT_EQUAL(300000, manager.getWaitTimeMs());
}

void test_wait_time_various_durations() {
    TimeBudgetManager manager;
    
    // Wait time is always 5 minutes regardless of session duration
    MockTimeState::setMillis(0);
    manager.startSession(10);
    TEST_ASSERT_EQUAL(300000, manager.getWaitTimeMs());
    
    MockTimeState::setMillis(1000);
    manager.startSession(30);
    TEST_ASSERT_EQUAL(300000, manager.getWaitTimeMs());
    
    MockTimeState::setMillis(2000);
    manager.startSession(1);
    TEST_ASSERT_EQUAL(300000, manager.getWaitTimeMs());
}

void test_wait_time_with_retry_multiplier() {
    TimeBudgetManager manager;
    
    // Wait time is fixed at 5 minutes even with retry multiplier
    MockTimeState::setMillis(0);
    manager.startSession(5, 3);  // 5 seconds * 3 = 15 seconds
    
    // Wait time is still 5 minutes = 300000ms
    TEST_ASSERT_EQUAL(300000, manager.getWaitTimeMs());
}

// Test active time tracking with pause/resume
void test_active_time_pause_resume() {
    TimeBudgetManager manager;
    
    MockTimeState::setMillis(0);
    manager.startSession(30);  // 30 seconds
    
    // Run for 2 seconds
    MockTimeState::advanceMillis(2000);
    TEST_ASSERT_EQUAL(2000, manager.getActiveTimeMs());
    TEST_ASSERT_EQUAL(28000, manager.getRemainingBudgetMs());
    
    // Pause (release SD card)
    manager.pauseActiveTime();
    
    // Wait 1 second (this should NOT count against budget)
    MockTimeState::advanceMillis(1000);
    TEST_ASSERT_EQUAL(2000, manager.getActiveTimeMs());  // Still 2 seconds
    TEST_ASSERT_EQUAL(28000, manager.getRemainingBudgetMs());  // Still 28 seconds
    
    // Resume (retake SD card)
    manager.resumeActiveTime();
    
    // Run for another 3 seconds
    MockTimeState::advanceMillis(3000);
    TEST_ASSERT_EQUAL(5000, manager.getActiveTimeMs());  // 2 + 3 = 5 seconds
    TEST_ASSERT_EQUAL(25000, manager.getRemainingBudgetMs());  // 30 - 5 = 25 seconds
}

void test_active_time_multiple_pauses() {
    TimeBudgetManager manager;
    
    MockTimeState::setMillis(0);
    manager.startSession(30);
    
    // Active period 1: 2 seconds
    MockTimeState::advanceMillis(2000);
    TEST_ASSERT_EQUAL(2000, manager.getActiveTimeMs());
    
    // Pause 1: 500ms (not counted)
    manager.pauseActiveTime();
    MockTimeState::advanceMillis(500);
    TEST_ASSERT_EQUAL(2000, manager.getActiveTimeMs());
    
    // Resume and active period 2: 3 seconds
    manager.resumeActiveTime();
    MockTimeState::advanceMillis(3000);
    TEST_ASSERT_EQUAL(5000, manager.getActiveTimeMs());
    
    // Pause 2: 1000ms (not counted)
    manager.pauseActiveTime();
    MockTimeState::advanceMillis(1000);
    TEST_ASSERT_EQUAL(5000, manager.getActiveTimeMs());
    
    // Resume and active period 3: 4 seconds
    manager.resumeActiveTime();
    MockTimeState::advanceMillis(4000);
    TEST_ASSERT_EQUAL(9000, manager.getActiveTimeMs());
    
    // Total active time: 2 + 3 + 4 = 9 seconds
    // Total wall time: 2 + 0.5 + 3 + 1 + 4 = 10.5 seconds
    // Budget remaining: 30 - 9 = 21 seconds
    TEST_ASSERT_EQUAL(21000, manager.getRemainingBudgetMs());
}

void test_active_time_pause_without_resume() {
    TimeBudgetManager manager;
    
    MockTimeState::setMillis(0);
    manager.startSession(30);
    
    // Run for 5 seconds
    MockTimeState::advanceMillis(5000);
    TEST_ASSERT_EQUAL(5000, manager.getActiveTimeMs());
    
    // Pause
    manager.pauseActiveTime();
    
    // Wait 10 seconds while paused
    MockTimeState::advanceMillis(10000);
    
    // Active time should still be 5 seconds
    TEST_ASSERT_EQUAL(5000, manager.getActiveTimeMs());
    TEST_ASSERT_EQUAL(25000, manager.getRemainingBudgetMs());
}

void test_active_time_double_pause() {
    TimeBudgetManager manager;
    
    MockTimeState::setMillis(0);
    manager.startSession(30);
    
    // Run for 2 seconds
    MockTimeState::advanceMillis(2000);
    TEST_ASSERT_EQUAL(2000, manager.getActiveTimeMs());
    
    // Pause
    manager.pauseActiveTime();
    MockTimeState::advanceMillis(1000);
    
    // Pause again (should be idempotent)
    manager.pauseActiveTime();
    MockTimeState::advanceMillis(1000);
    
    // Active time should still be 2 seconds
    TEST_ASSERT_EQUAL(2000, manager.getActiveTimeMs());
}

void test_active_time_double_resume() {
    TimeBudgetManager manager;
    
    MockTimeState::setMillis(0);
    manager.startSession(30);
    
    // Run for 2 seconds
    MockTimeState::advanceMillis(2000);
    manager.pauseActiveTime();
    MockTimeState::advanceMillis(1000);
    
    // Resume
    manager.resumeActiveTime();
    
    // Resume again (should be idempotent)
    manager.resumeActiveTime();
    
    // Run for 3 seconds
    MockTimeState::advanceMillis(3000);
    
    // Should have 2 + 3 = 5 seconds active
    TEST_ASSERT_EQUAL(5000, manager.getActiveTimeMs());
}

void test_active_time_with_budget_exhaustion() {
    TimeBudgetManager manager;
    
    MockTimeState::setMillis(0);
    manager.startSession(10);  // 10 seconds
    
    // Run for 8 seconds
    MockTimeState::advanceMillis(8000);
    TEST_ASSERT_TRUE(manager.hasBudget());
    
    // Pause for 5 seconds (not counted)
    manager.pauseActiveTime();
    MockTimeState::advanceMillis(5000);
    TEST_ASSERT_TRUE(manager.hasBudget());  // Still have budget
    
    // Resume and run for 3 more seconds (total 11 seconds active)
    manager.resumeActiveTime();
    MockTimeState::advanceMillis(3000);
    
    // Budget should be exhausted (11 > 10)
    TEST_ASSERT_FALSE(manager.hasBudget());
    TEST_ASSERT_EQUAL(0, manager.getRemainingBudgetMs());
}

void test_active_time_with_retry_multiplier() {
    TimeBudgetManager manager;
    
    MockTimeState::setMillis(0);
    manager.startSession(10, 3);  // 10 seconds * 3 = 30 seconds
    
    // Run for 5 seconds
    MockTimeState::advanceMillis(5000);
    TEST_ASSERT_EQUAL(5000, manager.getActiveTimeMs());
    TEST_ASSERT_EQUAL(25000, manager.getRemainingBudgetMs());
    
    // Pause for 2 seconds
    manager.pauseActiveTime();
    MockTimeState::advanceMillis(2000);
    TEST_ASSERT_EQUAL(5000, manager.getActiveTimeMs());
    
    // Resume and run for 20 more seconds
    manager.resumeActiveTime();
    MockTimeState::advanceMillis(20000);
    TEST_ASSERT_EQUAL(25000, manager.getActiveTimeMs());
    TEST_ASSERT_EQUAL(5000, manager.getRemainingBudgetMs());
    TEST_ASSERT_TRUE(manager.hasBudget());
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    
    // Session initialization tests
    RUN_TEST(test_session_initialization_basic);
    RUN_TEST(test_session_initialization_different_durations);
    
    // Budget remaining tests
    RUN_TEST(test_budget_remaining_calculation);
    RUN_TEST(test_has_budget);
    
    // Upload time estimation tests
    RUN_TEST(test_upload_time_estimation_default_rate);
    RUN_TEST(test_upload_time_estimation_various_sizes);
    RUN_TEST(test_can_upload_file);
    
    // Transmission rate averaging tests
    RUN_TEST(test_transmission_rate_single_upload);
    RUN_TEST(test_transmission_rate_averaging);
    RUN_TEST(test_transmission_rate_history_limit);
    RUN_TEST(test_transmission_rate_varying_speeds);
    RUN_TEST(test_record_upload_zero_time);
    
    // Retry multiplier tests
    RUN_TEST(test_retry_multiplier_basic);
    RUN_TEST(test_retry_multiplier_various_values);
    RUN_TEST(test_retry_multiplier_with_time_progression);
    
    // Wait time calculation tests
    RUN_TEST(test_wait_time_calculation);
    RUN_TEST(test_wait_time_various_durations);
    RUN_TEST(test_wait_time_with_retry_multiplier);
    
    // Active time tracking tests (pause/resume for periodic SD release)
    RUN_TEST(test_active_time_pause_resume);
    RUN_TEST(test_active_time_multiple_pauses);
    RUN_TEST(test_active_time_pause_without_resume);
    RUN_TEST(test_active_time_double_pause);
    RUN_TEST(test_active_time_double_resume);
    RUN_TEST(test_active_time_with_budget_exhaustion);
    RUN_TEST(test_active_time_with_retry_multiplier);
    
    return UNITY_END();
}
