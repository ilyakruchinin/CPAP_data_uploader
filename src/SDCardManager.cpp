#include "SDCardManager.h"
#include "Logger.h"
#include "pins_config.h"
#include <SD_MMC.h>

SDCardManager::SDCardManager() : initialized(false), espHasControl(false), isReadOnly(true), takeControlAt(0) {}

void SDCardManager::setControlPin(bool espControl) {
    digitalWrite(SD_SWITCH_PIN, espControl ? SD_SWITCH_ESP_VALUE : SD_SWITCH_CPAP_VALUE);
    delay(300);  // Wait for MUX switch to settle and CPAP to reinitialize after returning control
}

// Bit-bang CMD0 (GO_IDLE_STATE) to the SD card to force it into a clean Idle state
// This prevents the CPAP from throwing an "SD Card Error" when it takes over
// the multiplexer while the SD card is still in the Transfer state listening to the ESP32.
void SDCardManager::sendCMD0() {
    LOG("SDCardManager: Bit-banging CMD0 (GO_IDLE_STATE) before release...");

    // CMD0 frame: 0x40 (start bit 0, transmission bit 1, command index 000000)
    // 0x00 0x00 0x00 0x00 (argument 0)
    // 0x95 (CRC7 1001010, end bit 1)
    const uint8_t cmd0_frame[] = { 0x40, 0x00, 0x00, 0x00, 0x00, 0x95 };

    // Take over the SD pins as standard GPIOs
    pinMode(SD_CMD_PIN, OUTPUT);
    pinMode(SD_CLK_PIN, OUTPUT);
    digitalWrite(SD_CMD_PIN, HIGH);
    digitalWrite(SD_CLK_PIN, LOW);

    // According to SD spec, we need to send at least 74 clock cycles with CMD high
    // to wake up the card before sending the first command.
    for (int i = 0; i < 80; i++) {
        digitalWrite(SD_CLK_PIN, HIGH);
        delayMicroseconds(2);
        digitalWrite(SD_CLK_PIN, LOW);
        delayMicroseconds(2);
    }

    // Shift out the 48-bit (6-byte) CMD0 frame, MSB first
    for (int i = 0; i < 6; i++) {
        uint8_t b = cmd0_frame[i];
        for (int j = 7; j >= 0; j--) {
            // Set CMD pin to the current bit
            digitalWrite(SD_CMD_PIN, (b & (1 << j)) ? HIGH : LOW);
            delayMicroseconds(2); // Setup time
            
            // Pulse clock
            digitalWrite(SD_CLK_PIN, HIGH);
            delayMicroseconds(2);
            digitalWrite(SD_CLK_PIN, LOW);
            delayMicroseconds(2); // Hold time
        }
    }

    // Provide 8 extra clock cycles (one byte) with CMD high for the card to process
    digitalWrite(SD_CMD_PIN, HIGH);
    for (int i = 0; i < 8; i++) {
        digitalWrite(SD_CLK_PIN, HIGH);
        delayMicroseconds(2);
        digitalWrite(SD_CLK_PIN, LOW);
        delayMicroseconds(2);
    }

    // We don't wait for the R1 response (0x01) because we are immediately
    // handing the bus over to the CPAP machine via the multiplexer.
    // The card is now resetting internally.
    
    // Set pins back to high-Z / input to avoid driving the bus while CPAP is connected
    pinMode(SD_CMD_PIN, INPUT);
    pinMode(SD_CLK_PIN, INPUT);
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

bool SDCardManager::takeControl(bool readOnly) {
    if (espHasControl) {
        if (isReadOnly == readOnly) {
            return true;  // Already have control in the requested mode
        } else {
            // Need to change mode, release and re-take
            LOG_WARN("SDCardManager: Mode change requested while holding control, remounting...");
            return remount(readOnly);
        }
    }

    // Activity detection is handled by TrafficMonitor + FSM BEFORE this call.
    // By the time takeControl() is called, the FSM has already confirmed bus silence.

    // Take control of SD card
    setControlPin(true);
    espHasControl = true;
    isReadOnly = readOnly;

    // Wait for SD card to stabilize after control switch
    // SD cards need time to stabilize voltage and complete internal initialization
    delay(500);

    // Initialize SD_MMC.
    // Note: The standard SD_MMC.begin doesn't expose a read-only parameter directly.
    // However, esp_vfs_fat_sdmmc_mount can mount as read-only.
    // For now, SD_MMC handles FAT mounting. We can use standard SD_MMC.begin 
    // which mounts as R/W by default if the card isn't physically locked.
    // To truly enforce read-only at the VFS level, we might need custom IDF calls,
    // but the intention here is to guarantee *we* don't write to it via our app logic.
    // We will still pass the flag conceptually and prevent app-level writes.
    // (If using IDF v5+ or custom VFS, we can enforce it lower down).
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

    LOGF("SD card mounted successfully (Mode: %s)", isReadOnly ? "Read-Only" : "Read/Write");
    initialized = true;
    takeControlAt = millis();
    return true;
}

bool SDCardManager::remount(bool readOnly) {
    if (!espHasControl) {
        return takeControl(readOnly);
    }
    
    LOGF("SDCardManager: Remounting SD card as %s", readOnly ? "Read-Only" : "Read/Write");
    
    // Unmount
    SD_MMC.end();
    
    // We don't need to flip the multiplexer back to CPAP, just wait a moment
    delay(100);
    
    isReadOnly = readOnly;
    
    // Remount
    if (!SD_MMC.begin("/sdcard", SDIO_BIT_MODE_FAST)) {
        LOG("SD card remount failed");
        releaseControl(); // Hardware release if software mount fails
        return false;
    }
    
    LOG("SD card remount successful");
    return true;
}

void SDCardManager::releaseControl() {
    if (!espHasControl) {
        return;
    }

    SD_MMC.end();
    
    // Inject CMD0 to physically reset the NAND flash state before flipping multiplexer
    sendCMD0();
    
    setControlPin(false);
    espHasControl = false;
    isReadOnly = true; // Reset to default safe state
    unsigned long heldMs = (takeControlAt > 0) ? (millis() - takeControlAt) : 0;
    takeControlAt = 0;
    LOGF("SD card control released to CPAP machine (held %lums)", heldMs);
}

bool SDCardManager::hasControl() const { return espHasControl; }

fs::FS& SDCardManager::getFS() { return SD_MMC; }
