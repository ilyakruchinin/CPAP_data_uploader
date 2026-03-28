#include "BusWidthDetector.h"
#include "pins_config.h"
#include "Logger.h"
#include <driver/sdmmc_host.h>
#include <sdmmc_cmd.h>
#include "soc/sdmmc_reg.h"
#include "soc/sdmmc_struct.h"
#include "hal/sdmmc_ll.h"

// Define register access macros for bare-metal CMD13 sweep
#define SD_CMDARG_VAL  (*(volatile uint32_t*)(SDMMC_CMDARG_REG))
#define SD_CMD_VAL     (*(volatile uint32_t*)(SDMMC_CMD_REG))
#define SD_RINTSTS_VAL (*(volatile uint32_t*)(SDMMC_RINTSTS_REG))
#define SD_RESP0_VAL   (*(volatile uint32_t*)(SDMMC_RESP0_REG))
#define SD_TMOUT_VAL   (*(volatile uint32_t*)(SDMMC_TMOUT_REG))
#define SD_CTYPE_VAL   (*(volatile uint32_t*)(SDMMC_CTYPE_REG))

// Hardware Status Bits
#define HW_CMD_DONE        SDMMC_INTMASK_CMD_DONE  // BIT(2)
#define HW_RTO             SDMMC_INTMASK_RTO       // BIT(8)
#define HW_HLE             SDMMC_INTMASK_HLE       // BIT(12)
#define HW_RESP_ERR        SDMMC_INTMASK_RESP_ERR  // BIT(1)
#define HW_RCRC            SDMMC_INTMASK_RCRC      // BIT(6)

// Errors indicating current RCA is wrong during sweep
#define SWEEP_ERR_FLAGS    (HW_RTO | HW_HLE | HW_RESP_ERR | HW_RCRC)

int BusWidthDetector::detectBusWidth() {
    LOG("\n--- [EXPERIMENTAL] STARTING BUS-WIDTH DETECTOR ---");
    
    // 1. Grab MUX (Physical Layer)
    LOG("Detector: Grabbing SD MUX...");
    pinMode(SD_SWITCH_PIN, OUTPUT);
    digitalWrite(SD_SWITCH_PIN, SD_SWITCH_ESP_VALUE); // ESP32 takes the card
    delay(200); // Settle MUX relay/switch (increased for mechanical relays)

    // 2. Formal Host and Slot Initialization (Link Layer)
    // We MUST use these functions to route the SDMMC peripheral to the GPIO Matrix.
    // Manual register writes to CMD/CLK will fail if the matrix is not configured!
    esp_err_t err = sdmmc_host_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        LOGF("Detector: sdmmc_host_init failed: 0x%x", err);
        cleanup_and_release(false);
        return -1;
    }

    // Configure Slot 1 Pins (ESP32 standard for this board)
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1; // Start in 1-bit mode for probing safely
    slot_config.clk = GPIO_NUM_14;
    slot_config.cmd = GPIO_NUM_15;
    slot_config.d0  = GPIO_NUM_2;
    slot_config.d1  = GPIO_NUM_4;
    slot_config.d2  = GPIO_NUM_12;
    slot_config.d3  = GPIO_NUM_13;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    
    err = sdmmc_host_init_slot(SDMMC_HOST_SLOT_1, &slot_config);
    if (err != ESP_OK) {
        LOGF("Detector: sdmmc_host_init_slot failed: 0x%x", err);
        cleanup_and_release(false);
        return -1;
    }

    // Set probing clock (4 MHz)
    err = sdmmc_host_set_card_clk(SDMMC_HOST_SLOT_1, 4000); 
    if (err != ESP_OK) {
        LOGF("Detector: sdmmc_host_set_card_clk (4 MHz) failed: 0x%x", err);
        cleanup_and_release(false);
        return -1;
    }
    delay(20);

    // 3. Send 80 Initialization Clocks (CIU sync)
    // Critical: Bit 15 (send_init) tells the hardware to send 80 clocks to the card.
    // Card state machines are often left in inconsistent states after MUX switching.
    uint32_t init_clks = (1u << 31) | (1 << 15) | (1 << 13) | (SDMMC_HOST_SLOT_1 << 0);
    SD_CMD_VAL = init_clks;
    
    uint32_t start_init = millis();
    while (SD_CMD_VAL & (1u << 31)) { // Wait for HW to finish sending init clocks
        if (millis() - start_init > 100) break;
    }
    delay(10); // Final settle

    LOG("Detector: Starting Optimized RCA Sweep...");
    
    // Set hardware response timeout to 127 card clock cycles.
    // At 4 MHz, 127 cycles is 31.75\u00b5s, which is enough to capture the 48-bit response.
    uint32_t orig_tmout = SD_TMOUT_VAL;
    SD_TMOUT_VAL = (orig_tmout & 0xFFFFFF00) | 0x7F;

    unsigned long start_time = millis();
    uint16_t found_rca = 0;
    bool rca_found = false;
    uint32_t card_status = 0;
    uint32_t timeouts = 0;
    uint32_t crc_errors = 0;

    // CMD13: opcode=13, res_expected=1, check_crc=1, check_idx=1, hold=1, start=1
    uint32_t cmd13_flags = 13 |          // Command 13 (SEND_STATUS)
                           (1 << 6) |    // Response expected
                           (1 << 8) |    // Check response CRC
                           (1 << 9) |    // Check response index
                           (1 << 13) |   // Wait for previous data to complete
                           (1 << 29) |   // Use Hold Register
                           (1 << 31);    // Start Command

    // Common RCAs to check first for instant detection.
    // Most SD cards use RCAs 1, 2, or 3. Checking 1-16 first captures 99% of cards.
    uint16_t fast_path[] = {0x0001, 0x0002, 0x0003, 0x0004, 0x0005, 0x0006, 0x0007, 0x0008, 
                            0x0009, 0x000A, 0x000B, 0x000C, 0x000D, 0x000E, 0x000F, 0x0010,
                            0x1234, 0xC0DE, 0x8000, 0x0000};
    
    for (int i = 0; i < (int)(sizeof(fast_path)/sizeof(fast_path[0])); i++) {
        uint16_t rca = fast_path[i];
        SD_CMDARG_VAL = (uint32_t)rca << 16;
        SD_RINTSTS_VAL = 0xFFFFFFFF; // Clear interrupts
        SD_CMD_VAL = cmd13_flags;

        // Wait for HW to latch the command (Bit 31 clears when sent)
        uint32_t latch_sc = 0;
        while ((SD_CMD_VAL & (1u << 31)) && ++latch_sc < 100);

        uint32_t sc = 0;
        while (!(SD_RINTSTS_VAL & (HW_CMD_DONE | SWEEP_ERR_FLAGS)) && ++sc < 500);

        uint32_t status = SD_RINTSTS_VAL;
        if ((status & HW_CMD_DONE) && !(status & SWEEP_ERR_FLAGS)) {
            found_rca = rca;
            rca_found = true;
            card_status = SD_RESP0_VAL;
            LOGF("Detector: Fast-Path HIT! Found RCA 0x%04X", rca);
            break;
        }
    }

    // Full sweep if fast-path missed
    if (found_rca == 0) {
        LOG("Detector: Fast-path missed. Full linear sweep (1->65535)...");
        for (uint16_t rca = 1; rca < 0xFFFF; rca++) {
            // Periodic 10s total timeout check (safety buffer for full sweep)
            if ((rca & 0xFF) == 0 && (millis() - start_time > 10000)) {
                LOG_ERROR("Detector: Sweep reached 10-second safety limit. Aborting.");
                break;
            }

            SD_CMDARG_VAL = (uint32_t)rca << 16;
            SD_RINTSTS_VAL = 0xFFFFFFFF;
            SD_CMD_VAL = cmd13_flags;

            // Wait for HW to latch the command (Bit 31 clears when sent)
            uint32_t latch_sc = 0;
            while ((SD_CMD_VAL & (1u << 31)) && ++latch_sc < 100);

            uint32_t sc = 0;
            // hardware timeout (127 cycles) handles this mostly. 200 software polls is ~100us.
            while (!(SD_RINTSTS_VAL & (HW_CMD_DONE | SWEEP_ERR_FLAGS)) && ++sc < 200);

            uint32_t status = SD_RINTSTS_VAL;
            if ((status & HW_CMD_DONE) && !(status & SWEEP_ERR_FLAGS)) {
                found_rca = rca;
                rca_found = true;
                card_status = SD_RESP0_VAL;
                LOGF("Detector: Discovered RCA 0x%04X", rca);
                break;
            }
            if (status & HW_RTO) timeouts++;
            else if (status & HW_RCRC) crc_errors++;
            else if (!(status & HW_CMD_DONE)) {
                // This means latch_sc reached 100 or sc reached 200 without HW noticing anything
                timeouts++; 
            }

            if (rca % 10000 == 0) LOGF("..%uk", rca/1000);
        }
    }

    SD_TMOUT_VAL = orig_tmout; // Restore original timeout

    if (!rca_found) {
        LOGF("Detector: No RCA found after sweep. Diagnostics: Timeouts=%u, CRC_Errors=%u", timeouts, crc_errors);
        cleanup_and_release(false);
        return 0; // Uninitialized or card reader
    }

    uint8_t state = (card_status >> 9) & 0x0F;
    const char* states[] = {"Idle", "Ready", "Ident", "Stby", "Tran", "Data", "Rcv", "Prg", "Dis", "Reserved"};
    LOGF("Detector: Card state %d (%s), RCA 0x%04X", state, (state < 10 ? states[state] : "Unknown"), found_rca);

    // 4. Phase 2: Bus-Width Detection (CRC Probe via CMD17)
    // Logic: In AS11 (4-bit mode), reading in 1-bit mode will trigger a CRC error.
    
    // We try a tiny 1-MHz read at Sector 0 (MBR)
    uint32_t cmd17_flags = 17 | (1 << 6) | (1 << 8) | (1 << 9) | (1 << 13) | (1 << 29) | (1 << 31);
    
    // 1-Bit Probe (Current Mode)
    SD_CMDARG_VAL = 0; // Sector 0
    SD_RINTSTS_VAL = 0xFFFFFFFF;
    SD_CMD_VAL = cmd17_flags;
    
    uint32_t sc = 0;
    while (!(SD_RINTSTS_VAL & (HW_CMD_DONE | SWEEP_ERR_FLAGS)) && ++sc < 1000);
    
    bool crc_fail_1bit = (SD_RINTSTS_VAL & HW_RCRC);
    
    if (!crc_fail_1bit && (SD_RINTSTS_VAL & HW_CMD_DONE)) {
        LOG("Detector: 1-Bit Read SUCCESS -> Confirmed AirSense 10 (1-bit mode)");
        cleanup_and_release(false);
        return 1;
    }

    // Try 4-Bit Mode on ESP32 host to verify if card is already in 4-bit
    LOG("Detector: 1-Bit Read failed CRC (Expected for AS11). Testing 4-Bit Host Mode...");
    SD_CTYPE_VAL = (1 << 0); // Set slot 1 to 4-bit mode inside ESP32 Host
    delay(5);
    
    SD_RINTSTS_VAL = 0xFFFFFFFF;
    SD_CMD_VAL = cmd17_flags;
    sc = 0;
    while (!(SD_RINTSTS_VAL & (HW_CMD_DONE | SWEEP_ERR_FLAGS)) && ++sc < 1000);
    
    bool success_4bit = (SD_RINTSTS_VAL & HW_CMD_DONE) && !(SD_RINTSTS_VAL & HW_RCRC);
    
    if (success_4bit) {
        LOG("Detector: 4-Bit Read SUCCESS -> Confirmed AirSense 11 (4-bit mode)");
        cleanup_and_release(false);
        return 4;
    }

    LOG("Detector: Both 1-bit and 4-bit probes failed. Card might be in incompatible state.");
    cleanup_and_release(false);
    return 0;
}

void BusWidthDetector::cleanup_and_release(bool must_deselect) {
    LOG("Detector: Cleaning up hardware resources...");

    // Restore SDMMC bus settings
    SD_CTYPE_VAL = 0; // Reset to 1-bit mode internally
    
    // De-initialize slot and host to release resources for Arduino SD_MMC library
    sdmmc_host_deinit_slot(SDMMC_HOST_SLOT_1);
    sdmmc_host_deinit();
    
    delay(10);
    LOG("Detector: Hardware de-initialized.");
}
