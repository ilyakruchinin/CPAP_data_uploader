# CPAP Data Uploader v0.9.0 Release Notes

## Overview
Major performance release with progressive web app, intelligent heap management, and efficient backend initialization.

## Key Improvements

### 🌐 Progressive Web App Interface
New web interface that no longer requires dynamic content creation:
- All buffers pre-allocated to prevent heap fragmentation
- Auto-refreshing status updates
- Works reliably even under low memory conditions

### 🔄 Automatic Soft-Reboot for Heap Management
Due to heap fragmentation constraints, implemented intelligent soft-reboot system:
- Auto-detects when contiguous heap is insufficient for next backend
- Performs seamless soft-reboot with `esp_reset_reason()` fast-boot
- Skips all wait times during reboot (stabilization, Smart Wait, NTP)
- Processes SMB and Cloud in separate sessions without user noticing

### 🚀 Pre-flight Upload Scans
SD-only scans before any network activity:
- Only authenticates to Cloud when files need uploading
- SMB only connects when work exists
- Recent completed folders checked for file size changes

### 🛠️ SMB Transport Resilience
Enhanced error handling for transport failures:
- Skip `smb2_close` on poisoned transports
- WiFi cycling recovery on reconnect failures  
- Up to 3 SMB connect attempts with backoff

### 📊 Upload State Optimization
Size-only tracking for recent DATALOG files, reduced MD5 persistence to mandatory/SETTINGS only.
