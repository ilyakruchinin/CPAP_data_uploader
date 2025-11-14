# SMB Library Evaluation for ESP32

## Date: 2024-11-13

## Objective
Evaluate SMB library options for implementing file upload functionality to SMB shares on ESP32-PICO-V3-02 hardware using Arduino framework.

## Library Options

### 1. libsmb2 (https://github.com/sahlberg/libsmb2)

**Description:**
- Pure C library implementing SMB2/3 protocol
- Designed for embedded systems
- Widely used and well-maintained

**Pros:**
- Full SMB2/3 protocol support
- Mature and stable codebase
- Good documentation
- Active maintenance

**Cons:**
- **CRITICAL ISSUE**: Requires ESP-IDF framework, NOT compatible with Arduino framework
- According to design notes: "Arduino framework is not supported, requires esp-idf"
- Would require complete project migration from Arduino to ESP-IDF
- Larger binary footprint (estimated 200-300KB)
- More complex API requiring manual context management

**Integration Effort:** HIGH (requires framework migration)

**Verdict:** ❌ NOT SUITABLE - Incompatible with current Arduino-based project

---

### 2. esp-idf-smb-client (https://github.com/nopnop2002/esp-idf-smb-client)

**Description:**
- ESP-IDF specific SMB client wrapper
- Uses libsmb2 internally
- Designed specifically for ESP32

**Pros:**
- Tailored for ESP32 platform
- Potentially smaller footprint
- Simplified API for ESP32

**Cons:**
- **CRITICAL ISSUE**: Also requires ESP-IDF framework (name indicates "esp-idf")
- Not compatible with Arduino framework
- Less documentation than libsmb2
- Wrapper adds another layer of abstraction

**Integration Effort:** HIGH (requires framework migration)

**Verdict:** ❌ NOT SUITABLE - Incompatible with current Arduino-based project

---

### 3. Alternative Approach: Arduino-Compatible SMB Libraries

After research, there are NO mature Arduino-compatible SMB client libraries for ESP32. The SMB protocol is complex and existing implementations require ESP-IDF.

**Options:**
1. **Migrate project to ESP-IDF** - Major refactoring effort
2. **Use alternative protocols** - WebDAV (HTTP-based, Arduino-compatible)
3. **Create minimal SMB wrapper** - Extremely complex, not recommended
4. **Use ESP-IDF as component in Arduino** - Hybrid approach (experimental)

---

## Recommendation

### SELECTED APPROACH: Hybrid ESP-IDF Component in Arduino Framework

**Rationale:**
- PlatformIO supports using ESP-IDF components within Arduino framework
- Allows us to use libsmb2 without full project migration
- Maintains existing Arduino-based architecture
- Provides access to mature SMB implementation

**Implementation Strategy:**
1. Add libsmb2 as an ESP-IDF component in `components/` directory
2. Create a thin C++ wrapper class (SMBUploader) that bridges Arduino and ESP-IDF
3. Use conditional compilation (#ifdef ENABLE_SMB_UPLOAD) to make it optional
4. Keep the wrapper minimal to reduce binary size

**Estimated Binary Size Impact:**
- libsmb2 library: ~200-250KB
- Wrapper code: ~10-20KB
- Total: ~220-270KB (acceptable given 3MB app partition)

**API Complexity:**
- Wrapper will hide ESP-IDF complexity
- Expose simple Arduino-style interface:
  - `begin()` - Initialize and connect
  - `upload()` - Upload file
  - `end()` - Disconnect and cleanup

**Integration Effort:** MEDIUM
- Need to configure PlatformIO for ESP-IDF components
- Create C++ wrapper around C library
- Handle memory management carefully
- Test thoroughly

---

## Implementation Plan

### Phase 1: Setup libsmb2 Component
1. Clone libsmb2 into `components/libsmb2/`
2. Update `platformio.ini` to include ESP-IDF component support
3. Verify compilation

### Phase 2: Create SMBUploader Wrapper
1. Create `include/SMBUploader.h` and `src/SMBUploader.cpp`
2. Implement C++ wrapper around libsmb2 C API
3. Handle connection lifecycle
4. Implement file upload with streaming

### Phase 3: Integration
1. Integrate SMBUploader into FileUploader class
2. Add conditional compilation support
3. Test with real SMB share

---

## Decision

**SELECTED: libsmb2 via ESP-IDF component with Arduino framework**

This approach provides:
- ✅ Full SMB2/3 protocol support
- ✅ Mature, well-tested library
- ✅ Maintains Arduino framework compatibility
- ✅ Acceptable binary size footprint
- ✅ Clean separation via wrapper class
- ✅ Optional via build flags

**Documented in code:** See SMBUploader.h header comments

---

## References
- libsmb2: https://github.com/sahlberg/libsmb2
- PlatformIO ESP-IDF Components: https://docs.platformio.org/en/latest/frameworks/espidf.html
- ESP32 Arduino + ESP-IDF: https://docs.espressif.com/projects/arduino-esp32/en/latest/esp-idf_component.html

