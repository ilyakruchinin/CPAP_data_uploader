#ifndef BUS_WIDTH_DETECTOR_H
#define BUS_WIDTH_DETECTOR_H

#include <Arduino.h>
#include <esp_err.h>

class BusWidthDetector {
public:
    /**
     * Grabs the MUX, performs the stealth RCA/Bus Width detection,
     * logs the results locally, and returns the MUX to its original state.
     * 
     * Returns:
     * - 0 if uninitialized (no CPAP, or standard card reader, or failed probe)
     * - 1 if 1-bit mode detected (AS10)
     * - 4 if 4-bit mode detected (AS11)
     */
    static int detectBusWidth();

private:
    static esp_err_t send_cmd12();
    static esp_err_t send_cmd7(uint16_t rca);
    static esp_err_t send_cmd17_probe(void* block_buf);
    static void cleanup_and_release(bool must_deselect);
};

#endif // BUS_WIDTH_DETECTOR_H
