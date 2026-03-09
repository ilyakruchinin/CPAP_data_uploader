#include "SDCardManager.h"
#include "Logger.h"
#include "pins_config.h"
#include <SD_MMC.h>
#include <driver/gpio.h>

// Global config reference to check enableSdCmd0Reset
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
    // At 1-bit 20 MHz, signal integrity margin is large.
    // Reducing drive from default ~20mA (CAP_2) to ~5mA (CAP_0) slows
    // edge rates, reducing di/dt current spikes on the 3.3V rail.
    gpio_set_drive_capability((gpio_num_t)SD_CMD_PIN, GPIO_DRIVE_CAP_0);
    gpio_set_drive_capability((gpio_num_t)SD_CLK_PIN, GPIO_DRIVE_CAP_0);
    gpio_set_drive_capability((gpio_num_t)SD_D0_PIN, GPIO_DRIVE_CAP_0);

    // ── POWER: Mount SD in 1-bit mode ──
    // 1-bit mode uses only D0 (no D1-D3), halving bus toggle current.
    // The card's bus-width negotiation is reset via CMD0 in releaseControl()
    // so the CPAP can re-enumerate cleanly in its native 4-bit mode.
    if (!SD_MMC.begin("/sdcard", SDIO_BIT_MODE_SLOW, false, SDMMC_FREQ_DEFAULT)) {
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

    SD_MMC.end();
    initialized = false;

    gpio_set_drive_capability((gpio_num_t)SD_CMD_PIN, GPIO_DRIVE_CAP_2);
    gpio_set_drive_capability((gpio_num_t)SD_CLK_PIN, GPIO_DRIVE_CAP_2);
    gpio_set_drive_capability((gpio_num_t)SD_D0_PIN, GPIO_DRIVE_CAP_2);
    gpio_set_drive_capability((gpio_num_t)SD_D1_PIN, GPIO_DRIVE_CAP_2);
    gpio_set_drive_capability((gpio_num_t)SD_D2_PIN, GPIO_DRIVE_CAP_2);
    gpio_set_drive_capability((gpio_num_t)SD_D3_PIN, GPIO_DRIVE_CAP_2);

    if (SD_MMC.begin("/sdcard", SDIO_BIT_MODE_FAST)) {
        SD_MMC.end();
    } else {
        LOG_WARN("SD handoff compatibility remount failed");
    }

    setControlPin(false);
    espHasControl = false;
    LOG("SD card control released to CPAP machine");
}

bool SDCardManager::hasControl() const { return espHasControl; }

fs::FS& SDCardManager::getFS() { return SD_MMC; }
