# Regression Analysis and Validation — G31

## 1. Validation of Previous Findings (C54)

I have reviewed the previous analysis and the codebase, and I can confirm that the findings in the C54 report are **correct and valid**.

1. **Regression 1 (ULP/PCNT conflict)**: The ULP implementation actively breaks the PCNT detector. When `UlpMonitor` starts, it claims GPIO 33 and reconfigures it as an RTC input pad. This completely blinds the hardware PCNT peripheral. The "62s of bus silence" is simply the configured inactivity timeout expiring while the firmware is completely blind to actual SD traffic. 
2. **Regression 2 (Logging/SSE Fallback)**: The web UI logic was inadvertently configured to use the "nuclear option" (`/api/logs/full`) when a simple live SSE stream drops. This reads all LittleFS flash logs, which is extremely slow, power-hungry, and unnecessary for a simple reconnect.
3. **Regression 3 (Brownout Reboots)**: The C54 report correctly identified that brownouts were happening *before* the threshold change.

---

## 2. Brownout Detection State BEFORE the Change

Before explicitly setting the threshold to Level 7 in `sdkconfig.defaults`, the ESP-IDF default was in effect.

* **Default State**: Brownout detection was **ENABLED**.
* **Default Level**: **Level 0** (`CONFIG_ESP32_BROWNOUT_DET_LVL_SEL_0=y`).
* **Voltage Equivalent**: Level 0 corresponds to a trip voltage of approximately **2.43V**.

*(Note: Level 7 corresponds to approximately **2.73V**).*

The fact that the user's ESP was already experiencing `Brown-out reset (low voltage)` under `v1.0i-beta1` (before the change) means the CPAP machine's power supply rail is physically dropping below **2.43V**. This is a severe voltage sag. (The ESP32 officially requires a minimum of 3.0V, and SD cards officially require 2.7V).

---

## 3. Disabling vs. Lowering Brownout Detection

If we assume the ESP "might work" if brownout detection is completely disabled, we are making a very dangerous tradeoff.

### What happens if brownout is disabled?
If you completely remove brownout detection, the ESP32 will attempt to continue executing code even as the voltage drops to 2.2V, 2.0V, etc.
* **CPU Corruption**: Below ~2.4V, the ESP32's silicon logic timing breaks down. It may execute random instructions, misread memory, or corrupt variables.
* **SD Card Corruption**: This is the fatal risk. If the voltage drops below 2.7V while the ESP32 or the CPAP machine is writing to the SD card, the SD card's internal Flash Translation Layer (FTL) can be permanently corrupted. This bricks the SD card or destroys the filesystem.
* **LittleFS Corruption**: The ESP32 writing internal logs to flash during a voltage sag will likely corrupt the internal filesystem.

### Recommendation
**Do not disable brownout detection.**

Instead, we should **revert to Level 0 (~2.43V)**. 
* Level 0 is the lowest possible threshold. It gives the CPAP machine the maximum possible leeway to sag before triggering a reboot.
* It still provides a "last resort" safety net to forcefully stop the CPU before the voltage drops so low that it starts executing corrupted logic or sending garbled writes to the SD card.
* Reverting to Level 0 puts you exactly back to the stability profile you had in `v1.0i-beta1`, but combined with the new power-saving features (1-bit mode, DFS), the sags should naturally be less severe anyway.

---

## 4. Can ULP do PCNT?

**No, the ULP cannot reliably perform PCNT (Pulse Counting) for this use case.**

There are two ways to do pulse counting:

1. **Hardware PCNT**: The ESP32 has a dedicated hardware PCNT peripheral. However, the ULP **cannot access it**. The PCNT peripheral lives in the main digital domain, which is powered off or isolated from the ULP during deep/light sleep.
2. **Software PCNT (ULP Polling)**: The ULP can technically read the pin state in a loop and count changes.
   * **The Catch**: The ULP coprocessor runs at only **8 MHz**, and ULP instructions take multiple clock cycles to execute. The absolute maximum rate it can poll a pin is less than 1 MHz.
   * The SD card bus (and therefore the `CS_SENSE` line) toggles at **20 MHz to 50 MHz**.
   * If the ULP tries to sample a 20 MHz signal at 1 MHz, it violates the Nyquist limit. It will miss 95% of the edges. If the line happens to be HIGH at the exact microsecond the ULP looks at it, and HIGH again a microsecond later, the ULP thinks the bus is idle—even though the bus just transferred 20 bits of data in between.
   * Furthermore, running the ULP in a continuous infinite polling loop defeats its purpose. It would draw ~1-2mA continuously, which is barely better than the main CPU in light sleep.

**Conclusion on Activity Detection:** 
The main CPU's hardware **PCNT** is the *only* tool capable of safely and accurately monitoring the MHz-speed SD bus without missing edges. The main CPU can still enter FreeRTOS Light Sleep (DFS) while the PCNT peripheral runs in the background.

---

## 5. Mitigation Plan

To fix these regressions, the following actions should be taken (in order of importance):

### 1. Fix Regression 1 (SD Safety / ULP Removal)
* Completely remove the `UlpMonitor` class and all calls to it in `main.cpp`.
* Rely exclusively on `TrafficMonitor` (the PCNT implementation) for bus silence detection.
* PCNT works perfectly alongside standard FreeRTOS light sleep, so power consumption will still be very low during the `LISTENING` phase without sacrificing bus visibility.

### 2. Fix Regression 3 (Brownout Threshold)
* Modify `sdkconfig.defaults` to remove the Level 7 override.
* Explicitly set it back to the ESP-IDF default of Level 0 (`CONFIG_ESP32_BROWNOUT_DET_LVL_SEL_0=y`).
* This provides the maximum voltage leniency while protecting the SD card from low-voltage FTL corruption.

### 3. Fix Regression 2 (Logging UX)
* Update `web_ui.h` JavaScript.
* When the `EventSource` (SSE) connection drops, the `onerror` handler should gracefully fall back to polling `/api/logs` (which only returns the last few KB of RAM logs) instead of immediately triggering `fetchBackfill()` (which hits `/api/logs/full` and streams the entire flash disk).
* `/api/logs/full` should be strictly reserved for the very first time the user opens the logs tab, or if a physical reboot is explicitly detected.
