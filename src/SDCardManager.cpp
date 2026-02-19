#include "SDCardManager.h"
#include "Logger.h"
#include "pins_config.h"
#include <SD_MMC.h>

SDCardManager::SDCardManager() : initialized(false), espHasControl(false) {}

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

    // Initialize SD_MMC
    if (!SD_MMC.begin("/sdcard", SDIO_BIT_MODE_FAST)) {  // false = 4-bit mode
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
    return true;
}

void SDCardManager::releaseControl() {
    if (!espHasControl) {
        return;
    }

    SD_MMC.end();
    setControlPin(false);
    espHasControl = false;
    LOG("SD card control released to CPAP machine");
}

bool SDCardManager::hasControl() const { return espHasControl; }

fs::FS& SDCardManager::getFS() { return SD_MMC; }
