#include "SDCardManager.h"
#include "Logger.h"
#include "pins_config.h"
#include <SD_MMC.h>

SDCardManager::SDCardManager() : initialized(false), espHasControl(false), takeControlTime(0),
    statsSessionStart(0), statsTotalHoldMs(0), statsTotalReleaseMs(0), statsLastReleaseTime(0),
    statsLongestHold(0), statsShortestHold(ULONG_MAX), statsLongestRelease(0),
    statsTakeCount(0), statsReleaseCount(0) {}

void SDCardManager::setControlPin(bool espControl) {
    digitalWrite(SD_SWITCH_PIN, espControl ? SD_SWITCH_ESP_VALUE : SD_SWITCH_CPAP_VALUE);
    delay(100);  // Wait for switch to settle
}

bool SDCardManager::begin() {
    // Initialize control pins
    pinMode(SD_SWITCH_PIN, OUTPUT);
    pinMode(CS_SENSE, INPUT_PULLUP);
    
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

    // Check if CPAP machine is using SD card
    int csSenseState = digitalRead(CS_SENSE);
    LOGF("[SDCard] CS_SENSE pin: %s (CPAP %s SD card)", 
         csSenseState == LOW ? "LOW" : "HIGH",
         csSenseState == LOW ? "IS using" : "not using");
    if (csSenseState == LOW) {
        LOG("CPAP machine is using SD card, waiting...");
        return false;
    }

    // Track CPAP access time (release → take gap)
    if (statsLastReleaseTime > 0) {
        unsigned long releaseGap = millis() - statsLastReleaseTime;
        statsTotalReleaseMs += releaseGap;
        if (releaseGap > statsLongestRelease) statsLongestRelease = releaseGap;
    }

    // Take control of SD card
    setControlPin(true);
    espHasControl = true;
    takeControlTime = millis();
    statsTakeCount++;

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

    unsigned long holdDuration = millis() - takeControlTime;
    SD_MMC.end();
    setControlPin(false);
    espHasControl = false;
    statsLastReleaseTime = millis();
    statsReleaseCount++;
    statsTotalHoldMs += holdDuration;
    if (holdDuration > statsLongestHold) statsLongestHold = holdDuration;
    if (holdDuration < statsShortestHold) statsShortestHold = holdDuration;
    LOGF("SD card control released to CPAP machine (held %lu ms)", holdDuration);
    
    // Brief delay then check if CPAP immediately starts using the card
    delay(50);
    int csAfter = digitalRead(CS_SENSE);
    if (csAfter == LOW) {
        LOG_WARN("[SDCard] CPAP immediately accessed SD card after release - was likely waiting");
    }
}

bool SDCardManager::hasControl() const { return espHasControl; }

fs::FS& SDCardManager::getFS() { return SD_MMC; }

void SDCardManager::resetStatistics() {
    statsSessionStart = millis();
    statsTotalHoldMs = 0;
    statsTotalReleaseMs = 0;
    statsLastReleaseTime = 0;
    statsLongestHold = 0;
    statsShortestHold = ULONG_MAX;
    statsLongestRelease = 0;
    statsTakeCount = 0;
    statsReleaseCount = 0;
    
    // If SD is currently held, reset the hold start time so we don't
    // count pre-reset hold time in statistics, and count this as take #1.
    if (espHasControl) {
        takeControlTime = millis();
        statsTakeCount = 1;
    }
}

void SDCardManager::printStatistics() {
    unsigned long sessionDuration = millis() - statsSessionStart;
    
    // Account for current hold if SD is still held
    unsigned long totalHold = statsTotalHoldMs;
    if (espHasControl) {
        totalHold += millis() - takeControlTime;
    }
    
    unsigned long totalRelease = statsTotalReleaseMs;
    // If SD is released, add current release gap
    if (!espHasControl && statsLastReleaseTime > 0) {
        totalRelease += millis() - statsLastReleaseTime;
    }
    
    // Avoid division by zero
    if (sessionDuration == 0) sessionDuration = 1;
    
    int holdPct = (int)(totalHold * 100 / sessionDuration);
    int cpapPct = 100 - holdPct;
    
    LOGF("[SDCard] === Session Statistics ===");
    LOGF("[SDCard]   Duration: %lu ms", sessionDuration);
    LOGF("[SDCard]   ESP held SD: %lu ms (%d%%) across %d takes", totalHold, holdPct, statsTakeCount);
    LOGF("[SDCard]   CPAP had SD: %lu ms (%d%%) across %d releases", totalRelease, cpapPct, statsReleaseCount);
    if (statsTakeCount > 0) {
        LOGF("[SDCard]   Avg hold: %lu ms, Longest: %lu ms, Shortest: %lu ms",
             totalHold / statsTakeCount, statsLongestHold,
             statsShortestHold == ULONG_MAX ? 0 : statsShortestHold);
    }
    if (statsReleaseCount > 0 && statsLongestRelease > 0) {
        LOGF("[SDCard]   Longest CPAP window: %lu ms", statsLongestRelease);
    }
    LOGF("[SDCard] =========================");
}
