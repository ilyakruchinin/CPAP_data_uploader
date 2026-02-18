#include "NetworkRecovery.h"
#include "Logger.h"
#include <WiFi.h>
#include <esp_task_wdt.h>

volatile bool g_wifiCyclingActive = false;
volatile unsigned long g_lastWifiCycleMs = 0;
volatile bool g_smbConnectionActive = false;

// Software watchdog heartbeat — defined in main.cpp, updated here so the
// upload-task watchdog does not fire during a long cycle wait.
extern volatile unsigned long g_uploadHeartbeat;

static inline void feedAll(bool feedWatchdog) {
    if (feedWatchdog) {
        esp_task_wdt_reset();
        g_uploadHeartbeat = millis();
    }
}

bool tryCoordinatedWifiCycle(bool feedWatchdog) {
    // Guard: SMB holds a live TCP connection — cycling invalidates its socket.
    if (g_smbConnectionActive) {
        LOG_WARN("[NetRecovery] WiFi cycle skipped — SMB connection active (would corrupt SMB socket)");
        return WiFi.status() == WL_CONNECTED;
    }

    // Guard: another module is already cycling WiFi — wait briefly, then return.
    if (g_wifiCyclingActive) {
        LOG_WARN("[NetRecovery] WiFi cycle already in progress — waiting up to 15 s...");
        unsigned long waitStart = millis();
        while (g_wifiCyclingActive && millis() - waitStart < WIFI_CYCLE_WAIT_MS) {
            feedAll(feedWatchdog);
            delay(100);
        }
        if (g_wifiCyclingActive) {
            LOG_WARN("[NetRecovery] Timed out waiting for in-progress WiFi cycle — skipping");
        }
        return WiFi.status() == WL_CONNECTED;
    }

    // Guard: enforce cooldown between consecutive cycles.
    if (g_lastWifiCycleMs > 0 &&
        millis() - g_lastWifiCycleMs < WIFI_CYCLE_COOLDOWN_MS) {
        unsigned long remaining = WIFI_CYCLE_COOLDOWN_MS - (millis() - g_lastWifiCycleMs);
        LOG_WARNF("[NetRecovery] WiFi cycle skipped — cooldown active (%lu s remaining)",
                  remaining / 1000);
        return WiFi.status() == WL_CONNECTED;
    }

    LOG_WARN("[NetRecovery] Cycling WiFi to clear poisoned socket state...");
    g_wifiCyclingActive = true;

    WiFi.disconnect(false);  // Disconnect without erasing saved credentials
    feedAll(feedWatchdog);
    delay(1000);

    WiFi.reconnect();
    unsigned long wifiWait = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - wifiWait < 10000) {
        feedAll(feedWatchdog);
        delay(100);
    }

    g_lastWifiCycleMs = millis();
    g_wifiCyclingActive = false;

    if (WiFi.status() == WL_CONNECTED) {
        LOG_INFO("[NetRecovery] WiFi cycle complete — reconnected successfully");
        return true;
    }

    LOG_ERROR("[NetRecovery] WiFi cycle complete but failed to reconnect");
    return false;
}
