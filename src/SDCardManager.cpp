#include "SDCardManager.h"
#include "Logger.h"
#include "pins_config.h"
#include <SD_MMC.h>

// Global config reference to check enableSdCmd0Reset
#include "Config.h"
extern Config config;

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

    // Initialize SD_MMC (mode1bit=false for 4-bit mode, format_if_mount_failed=false, 
    // maxOpenFiles=5). Note: SD_MMC in Arduino core currently doesn't expose a dedicated
    // read-only mount parameter in begin(). We rely on not writing to the FS.
    // If the core supports it in v3.x, we'd pass it here, but for now we enforce
    // logic read-only by moving all state/log files to LittleFS.
    if (!SD_MMC.begin("/sdcard", SDIO_BIT_MODE_FAST)) {
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
    
    // Perform software reset of SD card state machine before handing back to CPAP
    // if configured to do so. This crashes the card's Transfer state back to Idle,
    // forcing the CPAP to cleanly remount it instead of failing on RCA mismatch.
    if (config.getEnableSdCmd0Reset()) {
        LOG_DEBUG("Bit-banging CMD0 (GO_IDLE_STATE) to force SD card protocol reset...");
        
        // Reconfigure CMD pin as standard GPIO output
        pinMode(SD_CMD_PIN, OUTPUT);
        digitalWrite(SD_CMD_PIN, HIGH);
        delayMicroseconds(10);
        
        // CMD0 Frame: 01000000 00000000 00000000 00000000 00000000 10010101
        // (Start:0, Tx:1, Cmd:0 | Arg:0 | CRC:0x4A, End:1)
        const uint8_t cmd0[6] = { 0x40, 0x00, 0x00, 0x00, 0x00, 0x95 };
        
        // Very basic SPI bit-bang. Since we don't have the clock running, 
        // we'll just toggle the CMD line to simulate the frame.
        // Actually, SD cards expect the clock to be running during commands.
        // A more reliable way is to configure the SPI peripheral temporarily,
        // or bit-bang both CLK and CMD.
        
        pinMode(SD_CLK_PIN, OUTPUT);
        digitalWrite(SD_CLK_PIN, LOW);
        
        // Send 74 dummy clocks with CMD high to ensure card is ready
        digitalWrite(SD_CMD_PIN, HIGH);
        for(int i=0; i<74; i++) {
            digitalWrite(SD_CLK_PIN, HIGH);
            delayMicroseconds(2);
            digitalWrite(SD_CLK_PIN, LOW);
            delayMicroseconds(2);
        }
        
        // Send CMD0
        for (int i = 0; i < 6; i++) {
            uint8_t b = cmd0[i];
            for (int bit = 7; bit >= 0; bit--) {
                digitalWrite(SD_CMD_PIN, (b & (1 << bit)) ? HIGH : LOW);
                delayMicroseconds(2);
                digitalWrite(SD_CLK_PIN, HIGH);
                delayMicroseconds(2);
                digitalWrite(SD_CLK_PIN, LOW);
            }
        }
        
        // Send 8 dummy clocks to finish
        digitalWrite(SD_CMD_PIN, HIGH);
        for(int i=0; i<8; i++) {
            digitalWrite(SD_CLK_PIN, HIGH);
            delayMicroseconds(2);
            digitalWrite(SD_CLK_PIN, LOW);
            delayMicroseconds(2);
        }
        
        // Return pins to floating state before MUX switch
        pinMode(SD_CMD_PIN, INPUT);
        pinMode(SD_CLK_PIN, INPUT);
    }

        return;
    }

    SD_MMC.end();
    setControlPin(false);
    espHasControl = false;
    LOG("SD card control released to CPAP machine");
}

bool SDCardManager::hasControl() const { return espHasControl; }

fs::FS& SDCardManager::getFS() { return SD_MMC; }
