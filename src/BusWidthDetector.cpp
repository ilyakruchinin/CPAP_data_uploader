#include "BusWidthDetector.h"
#include "pins_config.h"
#include "Logger.h"
#include <driver/sdmmc_host.h>
#include <sdmmc_cmd.h>

// ESP32 SDMMC Peripheral Registers (Base: 0x3FF68000)
#define SDMMC_BASE       0x3FF68000
#define SDMMC_CMDARG_REG (*(volatile uint32_t*)(SDMMC_BASE + 0x28))
#define SDMMC_CMD_REG    (*(volatile uint32_t*)(SDMMC_BASE + 0x2C))
#define SDMMC_RESP0_REG  (*(volatile uint32_t*)(SDMMC_BASE + 0x30))
#define SDMMC_RINTSTS    (*(volatile uint32_t*)(SDMMC_BASE + 0x44))

// Basic bit flags for CMD/RINTSTS
#define CMD_DONE         (1 << 0)
#define HLE_ERR          (1 << 12)
// RESP_ERR_FLAGS = EBE(15) | SBE(13) | RTO(8) | DCRC(7) | RCRC(6) | RE(1) => mostly 1, 6, 8
#define RESP_ERR_FLAGS   ((1 << 1) | (1 << 6) | (1 << 8))

int BusWidthDetector::detectBusWidth() {
    LOG("\n--- [EXPERIMENTAL] STARTING BUS-WIDTH DETECTOR ---");
    
    // 1. Grab MUX securely
    LOG("Detector: Grabbing SD MUX...");
    digitalWrite(SD_SWITCH_PIN, SD_SWITCH_ESP_VALUE);
    delay(50); // Settle time

    // 2. Initialize ESP-IDF SDMMC Host internally
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT; // 20 MHz initially
    host.flags = SDMMC_HOST_FLAG_1BIT;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;

    esp_err_t err = sdmmc_host_init();
    if (err != ESP_OK) {
        LOG_ERROR("Detector: sdmmc_host_init failed");
        cleanup_and_release(false);
        return 0;
    }

    err = sdmmc_host_init_slot(SDMMC_HOST_SLOT_1, &slot_config);
    if (err != ESP_OK) {
        LOG_ERROR("Detector: sdmmc_host_init_slot failed");
        sdmmc_host_deinit();
        cleanup_and_release(false);
        return 0;
    }

    // Attempt to set clock to 25 MHz for faster probing
    sdmmc_host_set_card_clk(SDMMC_HOST_SLOT_1, 25000);

    // 3. Phase 1: Bare-Metal RCA Sweep (CMD13)
    uint16_t found_rca = 0;
    uint32_t card_state = 0;
    
    LOG("Detector: Sweeping 65k RCAs via CMD13 bare-metal...");
    unsigned long start_time = millis();

    // CMD13: opcode=13, expected_rsp=1 (bit 6), check_crc=1 (bit 8), use_hold=1 (bit 29), start=1 (bit 31)
    uint32_t cmd13_flags = 13 | (1 << 6) | (1 << 8) | (1 << 29) | (1 << 31);

    for (uint32_t rca = 1; rca <= 0xFFFF; rca++) {
        SDMMC_CMDARG_REG = rca << 16;
        SDMMC_RINTSTS = 0xFFFFFFFF; // Clear interrupts
        SDMMC_CMD_REG = cmd13_flags;

        // Spin wait for completion or Hardware Locked Error
        while (!(SDMMC_RINTSTS & CMD_DONE) && !(SDMMC_RINTSTS & HLE_ERR)) { }

        uint32_t status = SDMMC_RINTSTS;
        if ((status & CMD_DONE) && !(status & RESP_ERR_FLAGS)) {
            // Valid response! Card responded.
            found_rca = (uint16_t)rca;
            card_state = (SDMMC_RESP0_REG >> 9) & 0x0F;
            break;
        }
    }

    unsigned long time_taken = millis() - start_time;
    
    if (found_rca == 0) {
        LOGF("Detector: No RCA found (sweep took %lums). Result = 0-bit / Uninitialized / Absent.", time_taken);
        cleanup_and_release(false);
        return 0; // Standard unmounted card or no CPAP powered on
    }

    LOGF("Detector: Found RCA 0x%04X in %lums. Card State: %lu", found_rca, time_taken, card_state);

    bool must_deselect = false;

    // 4. Phase 2: Probe with CMD17
    // If card is in Standby (3), we must select it to Transfer (4)
    if (card_state == 3) {
        LOG("Detector: Card is in Standby. Selecting via CMD7...");
        if (send_cmd7(found_rca) == ESP_OK) {
            must_deselect = true;
        } else {
            LOG_ERROR("Detector: Failed to select card.");
            cleanup_and_release(must_deselect);
            return 0;
        }
    } else if (card_state != 4) {
        LOGF("Detector: Card is in unexpected state %lu. Aborting.", card_state);
        cleanup_and_release(false);
        return 0;
    }

    // Allocate DMA-capable buffer for 512-byte block read
    void* block_buf = heap_caps_malloc(512, MALLOC_CAP_DMA);
    if (!block_buf) {
        LOG_ERROR("Detector: DMA allocation failed.");
        cleanup_and_release(must_deselect);
        return 0;
    }

    int final_result = 0;

    // PROBE 1: 1-Bit Width
    LOG("Detector: Probing CMD17 in 1-Bit mode...");
    // Host is already 1-bit from initialization
    err = send_cmd17_probe(block_buf);
    if (err == ESP_OK) {
        LOG("Detector: 1-Bit CMD17 probe PASS! (AS10 / 1-Bit Mode confirmed)");
        final_result = 1;
    } else {
        LOGF("Detector: 1-Bit CMD17 probe FAIL (err: %s). Suspect 4-bit card stranded, flushing with CMD12...", esp_err_to_name(err));
        send_cmd12(); // MANDATORY flush of the stranded card

        // PROBE 2: 4-Bit Width
        LOG("Detector: Switching host to 4-Bit mode and probing CMD17...");
        sdmmc_host_set_bus_width(SDMMC_HOST_SLOT_1, 4);
        
        err = send_cmd17_probe(block_buf);
        if (err == ESP_OK) {
            LOG("Detector: 4-Bit CMD17 probe PASS! (AS11 / 4-Bit Mode confirmed)");
            final_result = 4;
        } else {
            LOGF("Detector: 4-Bit CMD17 probe FAIL (err: %s).", esp_err_to_name(err));
            send_cmd12(); // MANDATORY flush
            final_result = 0;
        }
    }

    free(block_buf);

    LOGF("Detector: Finished. Identified Bus Width = %d", final_result);
    LOG("--------------------------------------------------\n");

    cleanup_and_release(must_deselect);
    return final_result;
}

esp_err_t BusWidthDetector::send_cmd12() {
    sdmmc_command_t cmd = {};
    cmd.opcode = 12; // STOP_TRANSMISSION
    cmd.flags = SCF_CMD_AC | SCF_RSP_R1B;
    return sdmmc_host_do_transaction(SDMMC_HOST_SLOT_1, &cmd);
}

esp_err_t BusWidthDetector::send_cmd7(uint16_t rca) {
    sdmmc_command_t cmd = {};
    cmd.opcode = 7; // SELECT/DESELECT_CARD
    cmd.arg = (uint32_t)rca << 16;
    cmd.flags = (rca != 0) ? (SCF_CMD_AC | SCF_RSP_R1B) : (SCF_CMD_AC);
    return sdmmc_host_do_transaction(SDMMC_HOST_SLOT_1, &cmd);
}

esp_err_t BusWidthDetector::send_cmd17_probe(void* block_buf) {
    sdmmc_command_t cmd = {};
    cmd.opcode = 17; // READ_SINGLE_BLOCK
    cmd.arg = 0;     // Sector 0
    cmd.flags = SCF_CMD_ADTC | SCF_RSP_R1;
    cmd.blklen = 512;
    cmd.data = block_buf;
    cmd.datalen = 512;
    cmd.timeout_ms = 500;
    return sdmmc_host_do_transaction(SDMMC_HOST_SLOT_1, &cmd);
}

void BusWidthDetector::cleanup_and_release(bool must_deselect) {
    if (must_deselect) {
        LOG("Detector: Deselecting card (CMD7(0))...");
        send_cmd7(0);
    }
    
    // Shut down ESP-IDF SDMMC host driver cleanly.
    // sdmmc_host_deinit cleans up DMA, GPIO routing, and clocks.
    sdmmc_host_deinit();

    // The document explicitly requests: "Tri-state all SDMMC GPIO pins to high-impedance"
    // sdmmc_host_deinit() puts pins in GPIO mode, but we ensure they are floating inputs.
    pinMode(SD_CMD_PIN, INPUT);
    pinMode(SD_CLK_PIN, INPUT);
    pinMode(SD_D0_PIN, INPUT);
    pinMode(SD_D1_PIN, INPUT);
    pinMode(SD_D2_PIN, INPUT);
    pinMode(SD_D3_PIN, INPUT);

    // Release MUX back to CPAP
    LOG("Detector: Releasing MUX back to CPAP.");
    digitalWrite(SD_SWITCH_PIN, SD_SWITCH_CPAP_VALUE);
    delay(50); // Settle time
}
