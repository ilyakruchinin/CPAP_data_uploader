# Release Notes v0.7.1

## 🐛 Bug Fixes & Improvements

### 1. Fixed Infinite Upload Loop for Recent Files
- **Issue:** The previous limit of 32 file entries in RAM was too low for typical SD cards, causing the system to "forget" files it had just uploaded, leading to infinite re-upload loops for recent DATALOG files.
- **Fix:** Increased `MAX_FILE_ENTRIES` to **250**. This allows tracking ~250 individual "Fresh" files (DATALOG + Settings) in RAM simultaneously, significantly improving stability for sessions with many recent files.
- **Technical Detail:** Also increased `MAX_JOURNAL_EVENTS` (200) and `COMPACTION_LINE_THRESHOLD` (250) to support the larger state without excessive journal compaction.

### 2. Strict Dependency for Cloud Imports (Fixing "Reset State" Behavior)
- **Issue:** There was ambiguity about when Settings and Root files should be uploaded. A "Reset State" or a standalone setting change could theoretically trigger an upload without therapy data, potentially creating "empty" imports that cloud providers might reject.
- **Fix:** Enforced a **strict dependency**: Settings and Root files are now **only** uploaded if at least one DATALOG file is successfully uploaded and triggers an import creation.
- **Behavior:**
    - If DATALOG files are uploaded -> Import Created -> Settings/Root appended -> Valid Import.
    - If NO DATALOG files are uploaded -> No Import Created -> Settings/Root **skipped**.
- **Impact:** This ensures that all cloud imports contain valid therapy data. "Reset State" will correctly re-upload Settings/Root files **only if** it also finds and uploads DATALOG data (which it will, because the reset clears the history of DATALOG uploads).

## 📚 Documentation

- Updated `docs/02-ARCHITECTURE.md` to accurately reflect:
    - The hybrid data tracking strategy (RAM for Fresh data, Disk for Old data).
    - The strict dependency of Settings/Root uploads on DATALOG activity.
