#ifndef SDCARD_MANAGER_H
#define SDCARD_MANAGER_H

#include <Arduino.h>
#include <FS.h>

class SDCardManager {
private:
    bool initialized;
    bool espHasControl;
    unsigned long takeControlTime;  // millis() when control was taken

    void setControlPin(bool espControl);
    
    // Statistics
    unsigned long statsSessionStart;  // millis() when resetStatistics() was called
    unsigned long statsTotalHoldMs;   // cumulative SD hold time
    unsigned long statsTotalReleaseMs; // cumulative CPAP access time
    unsigned long statsLastReleaseTime; // millis() of last releaseControl()
    unsigned long statsLongestHold;
    unsigned long statsShortestHold;
    unsigned long statsLongestRelease;
    int statsTakeCount;
    int statsReleaseCount;

public:
    SDCardManager();
    
    bool begin();
    bool takeControl();
    void releaseControl();
    bool hasControl() const;
    fs::FS& getFS();
    
    // Statistics
    void resetStatistics();
    void printStatistics();
};

#endif // SDCARD_MANAGER_H
