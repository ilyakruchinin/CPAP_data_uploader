#ifndef SDCARD_MANAGER_H
#define SDCARD_MANAGER_H

#include <Arduino.h>
#include <FS.h>

class SDCardManager {
private:
    bool initialized;
    bool espHasControl;
    bool isReadOnly;
    unsigned long takeControlAt;

    void setControlPin(bool espControl);
    void sendCMD0();

public:
    SDCardManager();
    
    bool begin();
    
    // Takes control of the SD card. By default, mounts as Read-Only to protect the CPAP FAT table.
    // Set readOnly=false ONLY for critical operations like writing config.txt from the web UI.
    bool takeControl(bool readOnly = true);
    
    // Briefly remounts the currently held SD card. Requires releasing and re-taking internally.
    // Used by the Web UI to switch to R/W mode just long enough to save config.txt.
    bool remount(bool readOnly);
    
    void releaseControl();
    bool hasControl() const;
    fs::FS& getFS();
};

#endif // SDCARD_MANAGER_H
