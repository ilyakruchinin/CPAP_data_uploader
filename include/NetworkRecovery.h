#pragma once
#include <Arduino.h>

// ============================================================================
// Network Recovery Coordination
// ============================================================================
// Shared globals that prevent simultaneous WiFi cycling by SMBUploader and
// SleepHQUploader. Back-to-back cycles from independent modules cause
// ASSOC_LEAVE storms, clear the ARP cache while the other uploader still
// has active sockets, and produce EHOSTUNREACH (errno 113) on the next
// SMB connect attempt.

// True while a WiFi disconnect/reconnect cycle is in progress.
extern volatile bool g_wifiCyclingActive;

// millis() timestamp of the last completed WiFi cycle. Used to enforce a
// cooldown so rapid failures don't trigger consecutive cycles.
extern volatile unsigned long g_lastWifiCycleMs;

// True while SMBUploader has an active TCP connection to the SMB server.
// SleepHQUploader checks this before cycling WiFi: cycling while SMB is
// connected invalidates the SMB socket fd and corrupts the next file write.
extern volatile bool g_smbConnectionActive;

// Minimum gap between consecutive WiFi cycles. The AP needs this window to
// process re-association and rebuild ARP/routing state cleanly. Cycling
// faster triggers ASSOC_LEAVE storms and sustained EHOSTUNREACH.
static const unsigned long WIFI_CYCLE_COOLDOWN_MS = 45000;  // 45 seconds

// How long to wait for a concurrent cycle already in progress to finish.
static const unsigned long WIFI_CYCLE_WAIT_MS = 15000;      // 15 seconds

// Attempt a guarded WiFi disconnect/reconnect cycle.
//
// Skips (returning the current WiFi connection state) when any of:
//   - g_smbConnectionActive is true  (would invalidate the live SMB socket)
//   - another cycle is already in progress  (waits up to WIFI_CYCLE_WAIT_MS)
//   - cooldown has not elapsed since the previous cycle
//
// Set feedWatchdog=true when calling from the upload task so that both the
// hardware (esp_task_wdt_reset) and software (g_uploadHeartbeat) watchdogs
// are fed during the cycle wait.
//
// Returns true if WiFi is connected after the attempt.
bool tryCoordinatedWifiCycle(bool feedWatchdog = false);
