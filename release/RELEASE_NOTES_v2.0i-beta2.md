# CPAP AutoSync v2.0i-beta2

**Patch release on top of v2.0i-beta1.**
OTA update from any v2.0i-alpha or v2.0i-beta1 is supported — no USB flash required.

---

## What's Fixed in beta2

### WiFi Reason 208 (PMF Compatibility)

**Problem:** Users with WiFi 6 routers or WPA3-transitional routers experienced immediate WiFi connection failures on boot with reason 208 (`ASSOC_COMEBACK_TIME_TOO_LONG`). This was a new ESP-IDF 5.x issue that didn't exist in the older ESP-IDF 4.x stack (v1.0i-stable).

**Root cause:** The pioarduino Arduino framework sets `PMF (Protected Management Frames / 802.11w) capable = true` by default. Some routers negotiate PMF and send an association comeback time that exceeds ESP-IDF 5.x's internal threshold, causing an immediate disconnect.

**Fix:** Automatic PMF fallback retry:
- First connection attempt uses default config (PMF capable = true) — works for all routers without this issue
- If the first attempt fails with reason 208, the firmware automatically disables PMF and retries
- On success, logs `"Connected after disabling PMF — router may not fully support 802.11w"`
- Users whose routers work fine with PMF are completely unaffected

Also added human-readable labels for ESP-IDF 5.x-specific disconnect reason codes (206–209) so logs are clearer.

---

## Cumulative Changes (alpha1–beta1 + beta2)

See `RELEASE_NOTES_v2.0i-beta1.md` for the full cumulative changelog from alpha1 through beta1. This release adds only the PMF fallback fix on top of beta1.

---

## Files Changed (beta2 only)

- `include/WiFiManager.h` — Added `static volatile uint8_t _lastDisconnectReason` for retry logic
- `src/WiFiManager.cpp` — Capture disconnect reason in event handler, implement PMF fallback retry on reason 208, add human-readable labels for reason codes 206–209
