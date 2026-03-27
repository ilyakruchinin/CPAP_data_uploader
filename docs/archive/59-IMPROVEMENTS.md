# SD Card Release Path Improvements

**Context**: During investigation of AirSense 10 SD power-cycling (Discussion #29), several improvements to the SD release path in `SDCardManager::releaseControl()` were identified that are beneficial for **all CPAP models** (AS10 and AS11).

These changes were prototyped on the `as10-experimental` branch and should be cherry-picked into the main branch.

## 1. Bus Pin Pull-ups After Deinit ✅ Universal Benefit

**Problem**: After `SD_MMC.end()`, the SDMMC peripheral releases all GPIOs, leaving the 6 SD bus lines floating. During the ~300ms MUX transition, floating lines can pick up noise and generate spurious edges/glitches that the CPAP's SD controller may interpret as CRC errors.

**Fix**: Explicitly set all SD pins to `INPUT` with `PULLUP_ONLY` after `SD_MMC.end()`, holding the bus HIGH (SD idle convention) until the MUX switches.

```cpp
// After SD_MMC.end():
static const int sdPins[] = { SD_CMD_PIN, SD_CLK_PIN, SD_D0_PIN,
                               SD_D1_PIN, SD_D2_PIN, SD_D3_PIN };
for (int pin : sdPins) {
    gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)pin, GPIO_PULLUP_ONLY);
}
```

**Why it helps AS11 too**: Even though AS11's driver is more tolerant, preventing floating lines during handoff is universally good practice. Reduces risk of rare CRC-triggered retries.

## 2. Drive Strength Restore After MUX Switch ✅ Universal Benefit

**Problem**: Previously, GPIO drive strength was restored to `CAP_2` (default ~20mA) **before** the MUX switch. This meant the ESP32 was actively driving the bus lines at full strength during the transition window, which could cause bus contention if the MUX overlap is non-zero.

**Fix**: Move drive strength restoration to **after** `setControlPin(false)` (MUX switch). Since the MUX disconnects the ESP32 from the card at that point, restoring drive strength on the ESP32 side is invisible to the CPAP.

```cpp
// MUX switch first
setControlPin(false);
espHasControl = false;

// Then restore drive strength (safe — ESP32 is disconnected from card)
for (int pin : sdPins) {
    gpio_set_drive_capability((gpio_num_t)pin, GPIO_DRIVE_CAP_2);
}
```

**Why it helps AS11 too**: Eliminates any possibility of bus contention during the MUX handoff moment, regardless of MUX switching speed or overlap characteristics.

## 3. CMD0 Before Release ⚠️ AS10 Only (Toggle)

**NOT for main branch**. Confirmed to cause SD Card Errors on AS11 when enabled. Added as config toggle `SD_CMD0_ON_RELEASE=true` for AS10 experimentation only. Testing proved ineffective for the AS10 reboot loop (root cause is the boot-time MUX grab, not the release path state).

**Status**: Keep toggle code in experimental branch. Do not merge to main.

## 4. 1-bit Mode Remount Removal 🧹 Cleanup

The 1-bit compatibility remount in `releaseControl()` was dead code (no users use `ENABLE_1BIT_SD_MODE`). With CMD0, it would have been counterproductive (re-establishing a session then calling `SD_MMC.end()` without CMD0). Removed for cleanliness.

**For main branch**: Can be safely removed since nobody uses 1-bit mode.

## Required Headers

The following includes were added for the fixes above:

```cpp
#include <driver/sdmmc_host.h>  // For sdmmc_host_do_transaction (CMD0 toggle only)
#include <sdmmc_cmd.h>          // For sdmmc_command_t (CMD0 toggle only)
```

> [!NOTE]
> If only merging improvements 1 and 2 (no CMD0 toggle), these extra includes are not needed. The existing `<driver/gpio.h>` include is sufficient.

## Full Release Path (Recommended for Main Branch)

```cpp
void SDCardManager::releaseControl() {
    if (!espHasControl) return;

    unsigned long holdDurationMs = millis() - controlAcquiredAt;
    LOGF("Releasing SD card. Total mount duration: %lu ms", holdDurationMs);

    SD_MMC.end();
    initialized = false;

    // Hold SD bus lines HIGH (idle) during MUX transition
    static const int sdPins[] = { SD_CMD_PIN, SD_CLK_PIN, SD_D0_PIN,
                                   SD_D1_PIN, SD_D2_PIN, SD_D3_PIN };
    for (int pin : sdPins) {
        gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT);
        gpio_set_pull_mode((gpio_num_t)pin, GPIO_PULLUP_ONLY);
    }

    // MUX switch: hand card back to CPAP
    setControlPin(false);
    espHasControl = false;

    // Restore drive strength AFTER MUX switch
    for (int pin : sdPins) {
        gpio_set_drive_capability((gpio_num_t)pin, GPIO_DRIVE_CAP_2);
    }

    LOG("SD card control released to CPAP machine");
}
```
