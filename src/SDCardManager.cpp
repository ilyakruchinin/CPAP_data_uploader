#include "SDCardManager.h"
#include "Logger.h"
#include "pins_config.h"
#include <SD_MMC.h>
#include <driver/gpio.h>
#include <driver/sdmmc_host.h>
#include <sdmmc_cmd.h>

// Global config reference for SD mode selection
#include "Config.h"
extern Config config;

SDCardManager::SDCardManager() : 
    initialized(false), 
    espHasControl(false),
    controlAcquiredAt(0) {
}

void SDCardManager::setControlPin(bool espControl) {
    digitalWrite(SD_SWITCH_PIN, espControl ? SD_SWITCH_ESP_VALUE : SD_SWITCH_CPAP_VALUE);
    delay(300);  // Wait for MUX switch to settle and CPAP to reinitialize after returning control
}

bool SDCardManager::begin() {
    // Initialize control pins
    pinMode(SD_SWITCH_PIN, OUTPUT);
    pinMode(CS_SENSE, INPUT);
    
    // Explicitly release control to CPAP machine on boot
    // This ensures the CPAP machine has access to the SD card immediately
    digitalWrite(SD_SWITCH_PIN, SD_SWITCH_CPAP_VALUE);
    espHasControl = false;
    
    #ifdef SD_POWER_PIN
    pinMode(SD_POWER_PIN, OUTPUT);
    digitalWrite(SD_POWER_PIN, HIGH);  // Power on SD card
    #endif
    return true;
}

bool SDCardManager::takeControl() {
    if (espHasControl) {
        return true;  // Already have control
    }

    // Activity detection is handled by TrafficMonitor + FSM BEFORE this call.
    // By the time takeControl() is called, the FSM has already confirmed bus silence.

    // Take control of SD card
    setControlPin(true);
    espHasControl = true;

    // Wait for SD card to stabilize after control switch
    // SD cards need time to stabilize voltage and complete internal initialization
    delay(500);

    // ── POWER: Reduce GPIO drive strength on SD pins before mount ──
    // Reducing drive from default ~20mA (CAP_2) to ~5mA (CAP_0) slows
    // edge rates, reducing di/dt current spikes on the 3.3V rail.
    gpio_set_drive_capability((gpio_num_t)SD_CMD_PIN, GPIO_DRIVE_CAP_0);
    gpio_set_drive_capability((gpio_num_t)SD_CLK_PIN, GPIO_DRIVE_CAP_0);
    gpio_set_drive_capability((gpio_num_t)SD_D0_PIN, GPIO_DRIVE_CAP_0);
    gpio_set_drive_capability((gpio_num_t)SD_D1_PIN, GPIO_DRIVE_CAP_0);
    gpio_set_drive_capability((gpio_num_t)SD_D2_PIN, GPIO_DRIVE_CAP_0);
    gpio_set_drive_capability((gpio_num_t)SD_D3_PIN, GPIO_DRIVE_CAP_0);

    // Mount SD in 1-bit or 4-bit mode based on config.
    // 4-bit is default and safer for CPAP handoff.
    // 1-bit uses less ESP-side bus current but requires a compatibility remount on release.
    bool use1Bit = config.getEnable1BitSdMode();
    if (!SD_MMC.begin("/sdcard", use1Bit ? SDIO_BIT_MODE_SLOW : SDIO_BIT_MODE_FAST, false, SDMMC_FREQ_DEFAULT, 2)) {
        LOG("SD card mount failed");
        releaseControl();
        return false;
    }

    uint8_t cardType = SD_MMC.cardType();
    if (cardType == CARD_NONE) {
        LOG("No SD card attached");
        releaseControl();
        return false;
    }

    LOG("SD card mounted successfully");
    initialized = true;
    controlAcquiredAt = millis();
    return true;
}

void SDCardManager::releaseControl() {
    if (!espHasControl) {
        return;
    }

    unsigned long holdDurationMs = millis() - controlAcquiredAt;
    LOGF("Releasing SD card. Total mount duration: %lu ms", holdDurationMs);

    // ── AS10 FIX: Send CMD0 (GO_IDLE_STATE) before unmounting ──
    // The SDMMC peripheral is still initialized at this point.
    // CMD0 resets the SD card's internal state machine back to "Idle" so that
    // when the MUX hands the card back to the CPAP, it sees a freshly-reset
    // card (no stale RCA, no leftover Transfer/Standby state).
    // Without this, AirSense 10 (STM32-based) hits a 300ms DMA timeout on
    // the first write because the card ignores commands with the wrong RCA,
    // triggering its error handler → full SDIO power-cycle.
    // Disabled by default for AS11 compatibility. Enable via SD_CMD0_ON_RELEASE=true.
    if (config.getSdCmd0OnRelease()) {
        sdmmc_command_t cmd = {};
        cmd.opcode = 0;  // CMD0 (GO_IDLE_STATE)
        cmd.arg = 0;
        cmd.flags = SCF_CMD_BC;          // Broadcast, no response expected
        esp_err_t err = sdmmc_host_do_transaction(1, &cmd);  // Slot 1
        if (err == ESP_OK) {
            LOG("[SD] CMD0 (GO_IDLE) sent before release");
        } else {
            LOGF("[SD] CMD0 failed (0x%x) — card may not be in idle state", err);
        }
    }

    SD_MMC.end();
    initialized = false;

    // ── AS10 FIX: Hold SD bus lines high (idle) during MUX transition ──
    // After SD_MMC.end() the SDMMC peripheral releases the GPIOs, leaving
    // them floating.  We explicitly set them as inputs with pull-ups so the
    // bus lines stay HIGH (SD idle convention) until the MUX switches.
    // This prevents spurious CRC errors from noise/glitches on the AS10.
    static const int sdPins[] = { SD_CMD_PIN, SD_CLK_PIN, SD_D0_PIN,
                                  SD_D1_PIN, SD_D2_PIN, SD_D3_PIN };
    for (int pin : sdPins) {
        gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT);
        gpio_set_pull_mode((gpio_num_t)pin, GPIO_PULLUP_ONLY);
    }

    // ── MUX switch: hand card back to CPAP ──
    setControlPin(false);
    espHasControl = false;

    // Restore GPIO drive strength for the next takeControl() cycle.
    // This happens AFTER the MUX switch so we don't drive the bus during handoff.
    for (int pin : sdPins) {
        gpio_set_drive_capability((gpio_num_t)pin, GPIO_DRIVE_CAP_2);
    }

    LOG("SD card control released to CPAP machine");
}

bool SDCardManager::hasControl() const { return espHasControl; }

fs::FS& SDCardManager::getFS() { return SD_MMC; }
