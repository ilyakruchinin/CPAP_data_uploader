#ifndef BUFFER_MANAGER_H
#define BUFFER_MANAGER_H

#include <Arduino.h>
#include <FS.h>
#include <SPIFFS.h>
#include <vector>

struct BufferedFile {
    String sourcePath; // Original path on SD card
    String bufferPath; // Path in SPIFFS
    uint32_t size;     // Exact size captured at the time of buffering (Point-in-Time snapshot)
    String checksum;   // Checksum captured at the time of buffering (for non-DATALOG files)
};

class BufferManager {
private:
    static const size_t SAFE_MARGIN_BYTES = 50 * 1024; // Leave 50KB free
    String bufferDir;

public:
    BufferManager(const String& dir = "/buffer");
    
    // Purge any orphaned files in the buffer directory
    void purge();
    
    // Check if a file of the given size can safely fit in the remaining SPIFFS space
    bool hasSpaceFor(size_t fileSize);
    
    // Copy a file from SD to SPIFFS and return the point-in-time snapshot details
    bool copyToBuffer(fs::FS& sourceFS, const String& sourcePath, BufferedFile& outBufferedFile);
    
    // Delete a specific buffered file after successful upload
    void deleteBufferedFile(const String& bufferPath);
};

#endif // BUFFER_MANAGER_H
