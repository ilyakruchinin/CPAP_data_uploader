#include "BusWidthDetector.h"
#include "pins_config.h"
#include "Logger.h"
#include <driver/sdmmc_host.h>
#include <sdmmc_cmd.h>
#include "soc/sdmmc_reg.h"

// Define register access macros if not already present
#define SDMMC_CMDARG_VAL  (*(volatile uint32_t*)(SDMMC_CMDARG_REG))
#define SDMMC_CMD_VAL     (*(volatile uint32_t*)(SDMMC_CMD_REG))
#define SDMMC_RESP0_VAL   (*(volatile uint32_t*)(SDMMC_RESP0_REG))
#define SDMMC_RINTSTS_VAL (*(volatile uint32_t*)(SDMMC_RINTSTS_REG))
#define SDMMC_TMOUT_VAL   (*(volatile uint32_t*)(SDMMC_TMOUT_REG))

// Hardware Status Bits
#define HW_CMD_DONE        SDMMC_INTMASK_CMD_DONE  // BIT(2)
#define HW_RTO             SDMMC_INTMASK_RTO       // BIT(8)
#define HW_HLE             SDMMC_INTMASK_HLE       // BIT(12)
#define HW_RESP_ERR        SDMMC_INTMASK_RESP_ERR  // BIT(1)
#define HW_RCRC            SDMMC_INTMASK_RCRC      // BIT(6)

// Errors indicating current RCA is wrong
#define SWEEP_ERR_FLAGS    (HW_RTO | HW_HLE | HW_RESP_ERR | HW_RCRC)

int BusWidthDetector::detectBusWidth() {
    LOG("\n--- [EXPERIMENTAL] STARTING BUS-WIDTH DETECTOR ---");
    
    // 1. Grab MUX securely
    LOG("Detector: Grabbing SD MUX...");
    digitalWrite(SD_SWITCH_PIN, SD_SWITCH_ESP_VALUE);
    delay(100); // Wait for electrical stabilization after MUX switch

    // 2. Initialize ESP-IDF SDMMC Host internally
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT; 
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

    // Set clock to standard speed. Driver sends dummy clocks during rate change.
    sdmmc_host_set_card_clk(SDMMC_HOST_SLOT_1, 20000); 
    delay(10); // Settle

    // 3. Phase 1: Bare-Metal RCA Sweep (CMD13)
    uint16_t found_rca = 0;
    uint32_t card_state = 0;
    
    LOG("Detector: Starting Optimized RCA Sweep...");
    
    // Set immediate response timeout (15 cycles = ~0.75us @ 20MHz)
    uint32_t orig_tmout = SDMMC_TMOUT_VAL;
    SDMMC_TMOUT_VAL = (orig_tmout & 0xFFFFFF00) | 0x0F;

    unsigned long start_time = millis();

    // CMD13: opcode=13, res_expected=1, check_crc=1, check_idx=1, hold=1, start=1
    // Bits: 13 | (1<<6) | (1<<8) | (1<<9) | (1<<29) | (1<<31)
    uint32_t cmd13_flags = 13 | (1 << 6) | (1 << 8) | (1 << 9) | (1 << 29) | (1 << 31);

    // Common RCAs to check first for instant detection
    uint16_t fast_path[] = {0x0001, 0x1234, 0x0002, 0xC0DE, 0x8000, 0x0000};
    
    for (int i = 0; i < (int)(sizeof(fast_path)/sizeof(fast_path[0])); i++) {
        uint16_t rca = fast_path[i];
        if (rca == 0 && i > 0) continue; // Skip 0 unless it's intended
        
        SDMMC_CMDARG_VAL = (uint32_t)rca << 16;
        SDMMC_RINTSTS_VAL = 0xFFFFFFFF; // Clear interrupts
        SDMMC_CMD_VAL = cmd13_flags;

        uint32_t sc = 0;
        while (!(SDMMC_RINTSTS_VAL & (HW_CMD_DONE | SWEEP_ERR_FLAGS)) && ++sc < 500);

        if ((SDMMC_RINTSTS_VAL & HW_CMD_DONE) && !(SDMMC_RINTSTS_VAL & SWEEP_ERR_FLAGS)) {
            found_rca = rca;
            card_state = (SDMMC_RESP0_VAL >> 9) & 0x0F;
            LOGF("Detector: Fast-Path HIT! Found RCA 0x%04X", found_rca);
            break;
        }
    }

    // Full sweep if fast-path missed
    if (found_rca == 0) {
        LOG("Detector: Fast-path missed. Full linear sweep (1->65535)...");
        for (uint32_t rca = 1; rca <= 0xFFFF; rca++) {
            // Periodic 3s total timeout check
            if (millis() - start_time > 3000) {
                LOG_ERROR("Detector: Sweep reached 3-second safety limit. Aborting.");
                break;
            }

            SDMMC_CMDARG_VAL = rca << 16;
            SDMMC_RINTSTS_VAL = 0xFFFFFFFF;
            SDMMC_CMD_VAL = cmd13_flags;

            uint32_t sc = 0;
            // Short burst check: AHB bus latency + 300 spin loops is plenty for 10us overhead
            while (!(SDMMC_RINTSTS_VAL & (HW_CMD_DONE | SWEEP_ERR_FLAGS)) && ++sc < 300);

            uint32_t status = SDMMC_RINTSTS_VAL;
            if ((status & HW_CMD_DONE) && !(status & SWEEP_ERR_FLAGS)) {
                found_rca = (uint16_t)rca;
                card_state = (SDMMC_RESP0_VAL >> 9) & 0x0F;
                LOGF("Detector: Discovered RCA 0x%04X", found_rca);
                break;
            }

            if (rca % 10000 == 0) LOGF("..%u0k", rca/10000);
        }
    }

    SDMMC_TMOUT_VAL = orig_tmout; // Restore
    unsigned long time_taken = millis() - start_time;
    
    if (found_rca == 0) {
        LOGF("\nDetector: No RCA found (sweep took %lums).", time_taken);
        cleanup_and_release(false);
        return 0;
    }

    LOGF("\nDetector: Found RCA 0x%04X in %lums. Card State: %lu", found_rca, time_taken, card_state);

    bool must_deselect = false;

    // 4. Phase 2: Probe with CMD17
    if (card_state == 3) { // Standby
        LOG("Detector: Card is Standby. Selecting...");
        if (send_cmd7(found_rca) == ESP_OK) {
            must_deselect = true;
        } else {
            LOG_ERROR("Detector: CMD7 selection failed.");
            cleanup_and_release(must_deselect);
            return 0;
        }
    } else if (card_state != 4) {
        LOGF("Detector: State %lu unsuitable for CMD17.", card_state);
        cleanup_and_release(false);
        return 0;
    }

    void* block_buf = heap_caps_malloc(512, MALLOC_CAP_DMA);
    if (!block_buf) {
        LOG_ERROR("Detector: DMA alloc failed.");
        cleanup_and_release(must_deselect);
        return 0;
    }

    int final_result = 0;

    // PROBE 1: 1-Bit Width
    LOG("Detector: Probing 1-Bit mode CMD17...");
    err = send_cmd17_probe(block_buf);
    if (err == ESP_OK) {
        LOG("Detector: 1-Bit PASS -> Likely AS10");
        final_result = 1;
    } else {
        LOGF("Detector: 1-Bit FAIL (%s). Flushing...", esp_err_to_name(err));
        send_cmd12(); 

        // PROBE 2: 4-Bit Width
        LOG("Detector: Switching host 4-Bit and probing...");
        sdmmc_host_set_bus_width(SDMMC_HOST_SLOT_1, 4);
        err = send_cmd17_probe(block_buf);
        if (err == ESP_OK) {
            LOG("Detector: 4-Bit PASS -> Likely AS11");
            final_result = 4;
        } else {
            LOGF("Detector: 4-Bit FAIL (%s).", esp_err_to_name(err));
            send_cmd12();
            final_result = 0;
        }
    }

    free(block_buf);
    cleanup_and_release(must_deselect);
    return final_result;
}

esp_err_t BusWidthDetector::send_cmd12() {
    sdmmc_command_t cmd = {};
    cmd.opcode = 12;
    cmd.flags = SCF_CMD_AC | SCF_RSP_R1B;
    return sdmmc_host_do_transaction(SDMMC_HOST_SLOT_1, &cmd);
}

esp_err_t BusWidthDetector::send_cmd7(uint16_t rca) {
    sdmmc_command_t cmd = {};
    cmd.opcode = 7;
    cmd.arg = (uint32_t)rca << 16;
    cmd.flags = (rca != 0) ? (SCF_CMD_AC | SCF_RSP_R1B) : (SCF_CMD_AC);
    return sdmmc_host_do_transaction(SDMMC_HOST_SLOT_1, &cmd);
}

esp_err_t BusWidthDetector::send_cmd17_probe(void* block_buf) {
    sdmmc_command_t cmd = {};
    cmd.opcode = 17;
    cmd.arg = 0; // Block 0
    cmd.flags = SCF_CMD_ADTC | SCF_RSP_R1;
    cmd.blklen = 512;
    cmd.data = block_buf;
    cmd.datalen = 512;
    cmd.timeout_ms = 500;
    return sdmmc_host_do_transaction(SDMMC_HOST_SLOT_1, &cmd);
}

void BusWidthDetector::cleanup_and_release(bool must_deselect) {
    if (must_deselect) send_cmd7(0); // Deselect
    sdmmc_host_deinit();
    pinMode(SD_CMD_PIN, INPUT);
    pinMode(SD_CLK_PIN, INPUT);
    pinMode(SD_D0_PIN, INPUT);
    pinMode(SD_D1_PIN, INPUT);
    pinMode(SD_D2_PIN, INPUT);
    pinMode(SD_D3_PIN, INPUT);
    digitalWrite(SD_SWITCH_PIN, SD_SWITCH_CPAP_VALUE);
}
