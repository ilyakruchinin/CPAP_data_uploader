#include "UlpMonitor.h"
#include "Logger.h"
#include "pins_config.h"

#include <esp32/ulp.h>
#include <ulp_common.h>
#include <driver/rtc_io.h>
#include <soc/rtc_io_reg.h>
#include <soc/sens_reg.h>

// GPIO 33 = RTC_GPIO8.  In RTC_GPIO_IN_REG the RTC GPIO input bits
// start at bit position 14 (RTC_GPIO_IN_NEXT_S).  RTC_GPIO8 is therefore
// at bit 14 + 8 = 22.
#define RTC_GPIO_NUM         8       // RTC GPIO index for GPIO 33
#define RTC_GPIO_BIT         (14 + RTC_GPIO_NUM)  // Bit position in RTC_GPIO_IN_REG

// ============================================================================
// ULP Program — macro-based (compiled by the main toolchain)
// ============================================================================
// Memory layout in RTC_SLOW_MEM (32-bit words):
//   [0] = activity_flag   (set to 1 when edge detected)
//   [1] = activity_count  (incremented on each edge)
//   [2] = last_pin_state  (previous reading of the GPIO)
//   [3] = (reserved)
//   [4+] = ULP program instructions
//
// Algorithm:
//   1. Read GPIO 33 via RTC_GPIO_IN_REG bit 22
//   2. Load previous pin state from RTC_SLOW_MEM[2]
//   3. XOR current with previous → if non-zero, edge detected
//   4. On edge: set activity_flag, increment activity_count
//   5. Store current state as new previous
//   6. HALT — timer restarts the program after the configured period

// Label numbers used in the ULP program
#define LABEL_NO_CHANGE  1
#define LABEL_DONE       2

static const ulp_insn_t ulp_program[] = {
    // ── Read current pin state (bit 22 of RTC_GPIO_IN_REG) ──
    I_RD_REG(RTC_GPIO_IN_REG, RTC_GPIO_BIT, RTC_GPIO_BIT),  // R0 = 0 or 1
    I_MOVR(R2, R0),                                          // R2 = current pin state

    // ── Load previous pin state from RTC_SLOW_MEM[2] ──
    I_MOVI(R3, 0),                       // R3 = base address (0)
    I_LD(R1, R3, 2),                     // R1 = RTC_SLOW_MEM[2] (last_pin_state, lower 16 bits)
    I_ANDI(R1, R1, 0x1),                 // Mask to single bit

    // ── Compare current vs previous ──
    I_SUBR(R0, R2, R1),                  // R0 = current - previous
    // If R0 == 0 (no change), skip to LABEL_NO_CHANGE
    M_BL(LABEL_NO_CHANGE, 1),            // Branch if R0 < 1 (i.e. R0 == 0)

    // ── Edge detected: set flag and increment counter ──
    I_MOVI(R0, 1),
    I_ST(R0, R3, 0),                     // RTC_SLOW_MEM[0] = 1 (activity_flag)
    I_LD(R0, R3, 1),                     // R0 = RTC_SLOW_MEM[1] (activity_count)
    I_ADDI(R0, R0, 1),                   // R0++
    I_ST(R0, R3, 1),                     // RTC_SLOW_MEM[1] = R0

    // ── Store current pin state ──
    M_LABEL(LABEL_NO_CHANGE),
    I_ST(R2, R3, 2),                     // RTC_SLOW_MEM[2] = current pin state

    // ── Halt and wait for timer to restart ──
    M_LABEL(LABEL_DONE),
    I_HALT(),
};

UlpMonitor::UlpMonitor() : _running(false) {}

bool UlpMonitor::begin() {
    // Configure GPIO 33 as RTC GPIO input for ULP access
    rtc_gpio_init((gpio_num_t)CS_SENSE);
    rtc_gpio_set_direction((gpio_num_t)CS_SENSE, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pulldown_dis((gpio_num_t)CS_SENSE);
    rtc_gpio_pullup_dis((gpio_num_t)CS_SENSE);
    // Hold the pad configuration during sleep
    rtc_gpio_hold_en((gpio_num_t)CS_SENSE);

    // Clear data area in RTC slow memory (words 0-3)
    for (int i = 0; i < 4; i++) {
        RTC_SLOW_MEM[i] = 0;
    }

    // Load ULP program into RTC slow memory starting at word offset 4
    size_t programSize = sizeof(ulp_program) / sizeof(ulp_insn_t);
    esp_err_t err = ulp_process_macros_and_load(
        ULP_PROGRAM_OFFSET,   // Load address (word offset)
        ulp_program,
        &programSize
    );
    if (err != ESP_OK) {
        LOG_ERRORF("[ULP] Failed to load ULP program: %d", err);
        return false;
    }

    // Set ULP wakeup period to 100ms (10 Hz sampling)
    // Period 0 is the default timer used by I_HALT()
    ulp_set_wakeup_period(0, 100000);  // 100,000 µs = 100ms

    // Start ULP program
    err = ulp_run(ULP_PROGRAM_OFFSET);
    if (err != ESP_OK) {
        LOG_ERRORF("[ULP] Failed to start ULP program: %d", err);
        return false;
    }

    _running = true;
    LOG("[ULP] CS_SENSE monitor started (GPIO 33 / RTC_GPIO8, 10 Hz)");
    return true;
}

void UlpMonitor::stop() {
    if (!_running) return;

    // There is no direct "stop ULP" API. We set the wakeup period to 0
    // which effectively disables the timer. The current ULP execution will
    // HALT and never be restarted.
    ulp_set_wakeup_period(0, 0);
    _running = false;

    // Release RTC GPIO hold so PCNT-based TrafficMonitor can use the pin
    rtc_gpio_hold_dis((gpio_num_t)CS_SENSE);
    // Reconfigure as normal GPIO input for PCNT
    rtc_gpio_deinit((gpio_num_t)CS_SENSE);
    pinMode(CS_SENSE, INPUT);

    LOG_DEBUG("[ULP] CS_SENSE monitor stopped");
}

bool UlpMonitor::checkActivity() {
    if (!_running) return false;

    // Read activity flag (lower 16 bits of RTC_SLOW_MEM[0])
    uint32_t flag = RTC_SLOW_MEM[ULP_DATA_OFFSET + ULP_ACTIVITY_FLAG] & 0xFFFF;
    if (flag) {
        // Clear the flag
        RTC_SLOW_MEM[ULP_DATA_OFFSET + ULP_ACTIVITY_FLAG] = 0;
        return true;
    }
    return false;
}

uint32_t UlpMonitor::getActivityCount() {
    // Lower 16 bits contain the value stored by ULP
    return RTC_SLOW_MEM[ULP_DATA_OFFSET + ULP_ACTIVITY_COUNT] & 0xFFFF;
}
