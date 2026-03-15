# CPAP Data Uploader v1.0i-beta3

## Critical Upgrade Notice: USB Flash Required
**Users upgrading to this version MUST perform a full flash via USB.** 

Do not use the Over-The-Air (OTA) update feature to upgrade from previous beta versions to v1.0i-beta3. This release includes a fix to the partition table. OTA updates only overwrite the application code and cannot update the partition table. If you perform an OTA update, the LittleFS partition will fail to mount.

**Instructions:**
1. Connect the ESP32 to your computer via USB.
2. Use the Web Flasher or PlatformIO to upload the firmware (which will correctly flash the updated `partitions_ota.csv`).

## Bug Fixes
* **LittleFS Mounting Fixed:** Corrected an issue where the LittleFS filesystem failed to mount on fresh installations due to a mismatch between the partition table label (`littlefs`) and the framework's default lookup (`spiffs`). This ensures that state and internal logs are successfully saved across reboots.

## Known Limitations & Analysis
* **Power Management (PM) Framework Unsupported:** You may notice `[INFO] PM configuration failed (err=262), CPU stays at 80MHz` in the boot logs. Investigation confirmed that the pre-compiled Arduino-ESP32 framework used by this project does not have the `CONFIG_PM_ENABLE` flag compiled in. 
  * **Impact:** Automatic light-sleep is unavailable, resulting in a slightly higher idle power draw (~20mA). However, this does *not* impact the device's susceptibility to brownouts, as existing runtime mitigations (low WiFi TX power, modem sleep, and a fixed 80MHz CPU clock) already address the peak current spikes that trigger brownouts. The error message is harmless.
