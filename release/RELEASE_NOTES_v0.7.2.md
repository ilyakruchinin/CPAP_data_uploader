# Release Notes v0.7.2

## ✨ New Features

### 🔌 Local Network Discovery (mDNS)
- **What it is:** You can now access the device using a friendly hostname instead of an IP address.
- **Default:** `http://cpap.local`
- **Configurable:** Added a new `HOSTNAME` field to `config.json`.
  - Example: `"HOSTNAME": "airsense11"` -> `http://airsense11.local`
- **Automatic:** The device automatically advertises itself on the network after connecting to WiFi.
- **Robust:** Automatically restarts the discovery service if WiFi reconnects.

## 📚 Documentation
- Updated `README.md` and `release/README.md` with:
  - Configuration instructions for `HOSTNAME`.
  - Updated example configuration blocks.
  - Explanations of how to use the local discovery feature.
