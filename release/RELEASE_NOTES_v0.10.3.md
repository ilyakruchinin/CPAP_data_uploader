# CPAP Data Uploader v0.10.3 Release Notes

## Features & Improvements

### ☁️ Cloud No-Work Session Optimization
**Feature:** Preflight check now verifies that newly seen DATALOG folders contain actual `.edf` files before declaring work. Empty folders are marked as pending immediately during preflight.
**Benefit:** Prevents unnecessary Cloud TLS connections and import sessions when there are no real files to upload. Avoids soft-reboot cycles when the Cloud backend has nothing to do.

### 🔌 On-Demand Backend Connections
**Feature:** Removed network pre-connect from LISTENING phase. Both SMB and Cloud backends now connect on-demand when actual work is confirmed.
- SMB connects lazily in FileUploader when first file needs uploading
- Cloud connects after preflight confirms actual work exists
**Benefit:** Simplifies FSM logic and eliminates unnecessary network connections during idle cycles.
