#include "BufferManager.h"
#include "Logger.h"

BufferManager::BufferManager(const String& dir) : bufferDir(dir) {
    if (!bufferDir.endsWith("/")) {
        bufferDir += "/";
    }
}

void BufferManager::purge() {
    LOGF("[BufferManager] Purging orphaned files in %s...", bufferDir.c_str());
    File root = SPIFFS.open(bufferDir);
    if (!root) {
        LOG("[BufferManager] Directory does not exist or empty");
        return;
    }
    
    if (!root.isDirectory()) {
        root.close();
        return;
    }

    std::vector<String> toDelete;
    File file = root.openNextFile();
    while (file) {
        if (!file.isDirectory()) {
            toDelete.push_back(file.path());
        }
        file.close();
        file = root.openNextFile();
    }
    root.close();

    int count = 0;
    for (const String& path : toDelete) {
        if (SPIFFS.remove(path)) {
            count++;
        } else {
            LOG_WARNF("[BufferManager] Failed to delete: %s", path.c_str());
        }
    }
    LOGF("[BufferManager] Purged %d files", count);
}

bool BufferManager::hasSpaceFor(size_t fileSize) {
    size_t totalBytes = SPIFFS.totalBytes();
    size_t usedBytes = SPIFFS.usedBytes();
    size_t freeBytes = (totalBytes > usedBytes) ? (totalBytes - usedBytes) : 0;
    
    if (freeBytes < SAFE_MARGIN_BYTES) {
        return false;
    }
    
    size_t usableSpace = freeBytes - SAFE_MARGIN_BYTES;
    return fileSize <= usableSpace;
}

bool BufferManager::copyToBuffer(fs::FS& sourceFS, const String& sourcePath, BufferedFile& outBufferedFile) {
    File sourceFile = sourceFS.open(sourcePath, FILE_READ);
    if (!sourceFile) {
        LOG_ERRORF("[BufferManager] Cannot open source file: %s", sourcePath.c_str());
        return false;
    }

    size_t fileSize = sourceFile.size();
    
    // Check space BEFORE allocating destination
    if (!hasSpaceFor(fileSize)) {
        LOG_WARNF("[BufferManager] Not enough SPIFFS space for %s (%lu bytes)", sourcePath.c_str(), (unsigned long)fileSize);
        sourceFile.close();
        return false;
    }

    // Extract filename to construct buffer path
    int slashIndex = sourcePath.lastIndexOf('/');
    String filename = (slashIndex >= 0) ? sourcePath.substring(slashIndex + 1) : sourcePath;
    String destPath = bufferDir + filename;

    // Remove existing file if any
    if (SPIFFS.exists(destPath)) {
        SPIFFS.remove(destPath);
    }

    File destFile = SPIFFS.open(destPath, FILE_WRITE);
    if (!destFile) {
        LOG_ERRORF("[BufferManager] Cannot open destination file: %s", destPath.c_str());
        sourceFile.close();
        return false;
    }

    // Copy data
    size_t bytesCopied = 0;
    uint8_t buf[8192];
    while (sourceFile.available()) {
        size_t bytesToRead = sourceFile.read(buf, sizeof(buf));
        if (bytesToRead > 0) {
            size_t bytesWritten = destFile.write(buf, bytesToRead);
            if (bytesWritten != bytesToRead) {
                LOG_ERRORF("[BufferManager] Write error at offset %lu", (unsigned long)bytesCopied);
                break;
            }
            bytesCopied += bytesWritten;
        }
    }

    sourceFile.close();
    destFile.close();

    if (bytesCopied != fileSize) {
        LOG_ERRORF("[BufferManager] Copy incomplete: %lu / %lu bytes", (unsigned long)bytesCopied, (unsigned long)fileSize);
        SPIFFS.remove(destPath);
        return false;
    }

    // Capture point-in-time snapshot
    outBufferedFile.sourcePath = sourcePath;
    outBufferedFile.bufferPath = destPath;
    outBufferedFile.size = fileSize;

    LOG_DEBUGF("[BufferManager] Buffered %s (%lu bytes) -> %s", sourcePath.c_str(), (unsigned long)fileSize, destPath.c_str());
    return true;
}

void BufferManager::deleteBufferedFile(const String& bufferPath) {
    if (SPIFFS.exists(bufferPath)) {
        if (!SPIFFS.remove(bufferPath)) {
            LOG_WARNF("[BufferManager] Failed to delete buffer file: %s", bufferPath.c_str());
        }
    }
}
