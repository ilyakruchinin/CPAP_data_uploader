# CPAP Data Uploader v1.0i-beta2 Release Notes

## Power, Stability & Quality-of-Life Improvements

This release focuses on reducing power consumption, improving WiFi reliability on battery/CPAP-powered hardware, and fixing several UI and behavioural issues reported since beta1.

---

### WiFi Power Redesign

- **New default TX power: 5 dBm ("MID")** — down from the previous default. This significantly reduces peak current draw and is sufficient for most bedroom setups (router within 3–5 metres).
- **Five power levels now available** — you have more fine-grained control over WiFi range vs. power draw:
  - `LOWEST` (-1 dBm) — router on same nightstand
  - `LOW` (2 dBm) — router within 1–2 metres
  - `MID` (5 dBm) — **default**, typical bedroom
  - `HIGH` (8.5 dBm) — router in adjacent room
  - `MAX` (10 dBm) — through walls, last resort
- **Hardware TX power cap lowered to 10 dBm** — the ESP32's RF calibration spike at boot is now capped at the absolute minimum the hardware allows. Previously it was 11 dBm. This reduces the brief power surge that occurs before your configured TX power takes effect.

### Boot Sequence Power Improvements

- **CPU frequency boost deferred** — the CPU now stays at 80 MHz throughout WiFi initialisation, then boosts to your configured speed afterward. Previously the CPU and WiFi powered up simultaneously, compounding current spikes.
- **mDNS delayed by 200ms after WiFi connects** — gives the 3.3V power rail a moment to recover from the WiFi association burst before mDNS fires its multicast announcement.

### Brownout Detection Control

- **Default brownout level restored to Level 0 (~2.43V)** — beta1 used Level 7 (~2.74V), which triggered too many unnecessary resets on CPAP-powered hardware.
- **New config option: `BROWNOUT_DETECT=OFF`** — if your device still resets frequently due to power dips, you can now disable the brownout detector entirely via config.txt. When disabled, a **persistent orange warning banner** appears on the web dashboard reminding you of the risk.

### Improved Log Streaming (SSE)

- **SSE stays connected across tab switches** — previously, switching away from the Logs tab and back would kill the live stream and trigger a full log re-download. Now it stays connected in the background.
- **Smarter reconnect** — if the live stream drops, the browser retries 3 times before falling back to polling. Previously it immediately triggered a heavy full-log re-download.
- **Server-side keepalive** — the device sends a heartbeat every 15 seconds on idle connections, preventing browsers and proxies from closing the stream due to inactivity.
- **SSE no longer suppressed in brownout-recovery mode** — SSE is the lightest log transport available, so it no longer makes sense to disable it when the device is trying to conserve power.

### "REBOOTING" Banner Fix

- **No more stuck "REBOOTING" badge** — previously, a single missed status poll could show "REBOOTING" even though the device was fine. Now:
  - 1–4 missed polls show "Offline — reconnecting..." instead
  - Only 5+ consecutive failures (or an expected reboot) show the REBOOTING overlay
  - The badge always updates on the next successful poll (previously it could get stuck showing stale text)
- **Mobile tab resume fixed** — switching to another app and back no longer triggers a false "REBOOTING" state

### Force Upload in Scheduled Mode

- **Force Upload now works outside the upload window** — previously, tapping Force Upload in scheduled mode outside your configured hours did nothing. Now it uploads your **most recent data** (today + yesterday) immediately. Older backlog data still waits for the regular window.

### Removed Legacy Config Aliases

The following old config key names are **no longer accepted** and will produce a warning in the log. Update your `config.txt` if you use any of these:

| Old Key | Replace With |
|:--------|:------------|
| `GMT_OFFSET` | `GMT_OFFSET_HOURS` |
| `SAVE_LOGS` | `PERSISTENT_LOGS` |
| `LOG_TO_SD_CARD` | `PERSISTENT_LOGS` |
| `SKIP_REBOOT_BETWEEN_BACKENDS` | `MINIMIZE_REBOOTS` |

The WiFi power saving aliases `MODEM`, `OFF`, `HIGH` for `WIFI_PWR_SAVING` and `MAXIMUM`, `MEDIUM` for `WIFI_TX_PWR` have also been removed. Use the canonical names: `NONE`/`MID`/`MAX` and `LOWEST`/`LOW`/`MID`/`HIGH`/`MAX`.

### Build & Packaging

- **Standard (non-OTA) firmware removed** — the release now contains only OTA-enabled firmware. The standard build offered minimal benefit (slightly more app space) at the cost of losing web-based updates. All users should use the OTA firmware.
- **ULP coprocessor monitor removed** — an internal component that conflicted with SD card operations has been removed. This was not user-facing but improves reliability.

---

## Upgrade Notes

- **OTA upgrade from beta1:** Upload `firmware-ota-upgrade.bin` via the web interface at `http://cpap.local/ota`. No USB/Serial needed.
- **Config changes:** If your `config.txt` uses any of the removed aliases listed above, update them to the new key names before upgrading. The device will still boot, but the old keys will be silently ignored (with a warning in the log).
- **WIFI_TX_PWR default changed:** The default is now `MID` (5 dBm). If you had no `WIFI_TX_PWR` line in your config and were relying on the old default, your WiFi range is unchanged or slightly increased (previous constructor default was also 5 dBm under a different name). If you experience connection issues, add `WIFI_TX_PWR = HIGH` or `WIFI_TX_PWR = MAX` to your config.txt.
