#ifndef ULP_MONITOR_H
#define ULP_MONITOR_H

#include <Arduino.h>

/**
 * UlpMonitor - ULP coprocessor-based CS_SENSE activity detector
 * 
 * Offloads GPIO 33 (RTC_GPIO8) monitoring to the ESP32's Ultra Low Power
 * coprocessor, allowing the main CPU to enter deep/light sleep during
 * LISTENING state while still detecting CPAP SD bus activity.
 * 
 * The ULP program runs at ~10 Hz (100ms intervals), reading the RTC_GPIO
 * input register. When activity is detected (pin state changes), it sets
 * a flag in RTC slow memory that the main CPU can read on wake.
 * 
 * Power savings: ~30-50 mA (main CPU) → ~150 µA (ULP only) during idle
 * monitoring, enabling sub-1 mA system current in LISTENING state.
 * 
 * Requires: GPIO 33 configured as RTC GPIO input (already the case for CS_SENSE)
 */
class UlpMonitor {
public:
    UlpMonitor();
    
    /**
     * Load and start the ULP program.
     * Call once during setup() after TrafficMonitor.begin().
     * @return true if ULP program loaded and started successfully
     */
    bool begin();
    
    /**
     * Stop the ULP program.
     * Call before entering states where PCNT-based monitoring is preferred.
     */
    void stop();
    
    /**
     * Check if the ULP detected any activity since last check.
     * Reads the activity flag from RTC slow memory and clears it.
     * @return true if CS_SENSE activity was detected
     */
    bool checkActivity();
    
    /**
     * Get the number of activity detections counted by the ULP.
     * This is a cumulative counter stored in RTC slow memory.
     */
    uint32_t getActivityCount();
    
    /**
     * Check if the ULP program is currently running.
     */
    bool isRunning() const { return _running; }

private:
    bool _running;
    
    // RTC slow memory layout (word offsets from ULP program base)
    // These must match the ULP program's memory layout
    static const uint32_t ULP_DATA_OFFSET = 0;      // Start of data section
    static const uint32_t ULP_ACTIVITY_FLAG = 0;     // Activity detected flag (1 = active)
    static const uint32_t ULP_ACTIVITY_COUNT = 1;    // Cumulative activity count
    static const uint32_t ULP_LAST_PIN_STATE = 2;    // Last observed pin state
    static const uint32_t ULP_PROGRAM_OFFSET = 4;    // Start of program section
};

#endif // ULP_MONITOR_H
