# CPAP AutoSync v1.0i-beta1 Release Notes

## 🚀 Major Release — Dual-Backend Uploads, Complete Web UI Overhaul, Stability & Logging

This is a major milestone release building on v0.11.1-i. It introduces dual-backend (SMB + Cloud) simultaneous uploads, a fully redesigned web interface with real-time monitoring, 4-file log rotation, heap-optimized TLS, and dozens of UX improvements.

---

### ⚡ Dual-Backend Phased Upload

- **SMB and SleepHQ Cloud now upload in one session** — Cloud first (highest heap for TLS), then SMB (clean sockets for libsmb2). No more backend cycling or extra reboots between passes.
- **Auto-scaled time budget**: 2× configured `EXCLUSIVE_ACCESS_MINUTES` when both backends are enabled.
- **On-demand TLS**: TLS connection is established only when Cloud work is confirmed by pre-flight scan, saving ~11s and ~28KB heap when no cloud data needs uploading.

### 🖥️ Web UI Overhaul

- **Animated logo** with spinning ring, pulsing upload arrow, breathing waveform, and WiFi signal blink.
- **Dashboard**: FSM state badges now glow with color-matched animations (green/blue/amber) that persist smoothly between poll refreshes. Mode helper text highlights the current mode and upload window state with a glowing badge.
- **Danger Zone**: Force Upload and Reset State buttons with color-coded separator lines, expanded risk descriptions, and ☢ icon on Reset State. Force Upload references in helper text shown in orange with "(not recommended)" label.
- **SD Access tab** (renamed from "Monitor"): monitoring now persists across tab switches with a purple pulsing banner on other tabs. Start Monitoring is disabled during active uploads. Removed misleading "Red/Green" color indicator.
- **Profiler Wizard**: updated instructions — start therapy before profiling; safety margin reduced from +5s to +4s.
- **Config editor**: SD Card Access Warning modal shown before editing. Config and Profiler modals now properly constrained on mobile (no horizontal overflow).
- **Logs tab**: "Download All Logs" button highlighted in green as the primary action.
- **System tab**: CPU load graphs (Core 0/Core 1) with 2-minute history. Heap history chart with min/max tracking.
- **Dashboard layout**: "Time synced" moved from Upload Engine to System card. IP field removed (redundant). "Next upload" renamed to "Next full upload".
- **Merged `/api/diagnostics` into `/api/status`**: heap and CPU data now included in the single status poll — eliminates a separate 2-second polling request, reducing HTTP overhead by ~60%.
- **Button consistency**: all buttons (nav tabs, log controls, config editor, Danger Zone, OTA, Profiler) now use identical padding, font-size, and border-radius via a unified `.btn` class. OTA buttons no longer stretch full-width. Force Upload (▲) and Reset State (☢) icons wrapped in normalized spans to fix cross-browser height mismatch.
- **Reboot overlay**: now says "Device is unreachable or rebooting" with context-appropriate calming text.
- **Mobile**: logo now renders at full 72px on all screen sizes. Horizontal scrolling eliminated.

### 📝 Logging Improvements

- **4-file log rotation** (syslog.0..3.txt, 32KB each, 128KB total) replaces old dual-file system. One-time auto-migration on first boot.
- **Streaming backfill**: `/api/logs/full` shows progressive download with KB counter and spinner.
- **SSE live streaming**: real-time log updates via Server-Sent Events with automatic reconnect.
- **Reboot detection**: client-side log viewer detects reboots (boot banner or buffer wrap) and auto-refetches full NAND history for pre-reboot context.
- **Quieter upload logs**: per-file "Uploaded:" / "Upload complete:" confirmation lines are now suppressed for both Cloud and SMB unless `DEBUG=true` in config. "Uploading file:" lines remain visible for progress tracking.
- **Suppressed non-actionable warnings**: "Low memory" warnings for both SleepHQ TLS keep-alive and SMB directory creation are now DEBUG-only (informational, no behavioral change).
- **Download All Logs**: streams saved + current logs as a single file for troubleshooting.

### 🔧 Stability & Bug Fixes

- **TrafficMonitor idle tracking fix**: `resetIdleTracking()` now resets all state (`_lastSampleTime`, `_secondPulseAccumulator`, PCNT counter) — prevents false idle detection after COOLDOWN that caused premature uploads.
- **Zero-byte EDF fix**: empty .edf files no longer prevent folders from being marked complete, eliminating infinite re-upload loops.
- **Backend summary count fix**: inactive backend folder totals now exclude empty folders, matching the active backend's display formula.
- **Journal replay crash fix**: `replayJournal()` checks file existence before opening, preventing undefined behavior on first boot.
- **Config editor during uploads**: no longer aborts uploads — editor opens immediately while upload continues. Save & Reboot applies changes after the current session.

### ⚙️ Configuration Changes

| Setting | Old Default | New Default | Notes |
|:--------|:-----------|:-----------|:------|
| `MINIMIZE_REBOOTS` | `false` | **`true`** | Device now skips elective post-upload reboots by default. Stable in field testing. Override with `MINIMIZE_REBOOTS=false` if needed. |
| `WIFI_TX_PWR` | `MID` | `LOW` | (Changed in v0.11.1-i) 5.0 dBm default for lower peak current. |

### 🗑️ Removed

- **CPAPMonitor class**: CS_SENSE pin detection removed — unreliable in hardware and unused.
- **HTTPClient dependency**: replaced with raw WiFiClientSecure for all SleepHQ API calls, reducing heap fragmentation.

---

## ⚠️ Upgrade Notes

- **First boot after upgrade**: log files will auto-migrate from old format (syslog.A/B) to new 4-file rotation. This is automatic and transparent.
- **MINIMIZE_REBOOTS default changed**: if you previously relied on post-upload reboots for heap recovery, add `MINIMIZE_REBOOTS=false` to your config.txt. The device will warn in logs if heap fragmentation reaches concerning levels.
- **Config key renames**: `LOG_TO_SD_CARD` → `SAVE_LOGS`, `SKIP_REBOOT_BETWEEN_BACKENDS` → `MINIMIZE_REBOOTS`. Old key names are accepted as aliases.
