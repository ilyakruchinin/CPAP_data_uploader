# New Architecture Proposal for AS10: Stealth RCA Discovery (The "Zero-CMD0" Mount)

## The Objective
To completely eliminate the AirSense 10 (AS10) error-recovery power cycle by reading `config.txt` without ever sending `CMD0`. By preserving the CPAP’s established Relative Card Address (RCA) and existing state, the ESP32 can transparently query the card and return the MUX without the CPAP ever registering a disruption.

## Risk Assessment & Mitigation

| Risk | Mitigation | Status |
|---|---|---|
| **1. RCA destruction** | No `CMD0` is sent. We brute-force discover the existing RCA via `CMD13`, returning the card undisturbed. | ✅ Solved |
| **2. Mid-transaction sever** | Current PCNT quiet-period detection guarantees the bus is idle before the MUX is flipped. | ✅ Solved |
| **3. FAT32 cache incoherency** | Strictly read-only implementation. No OS-level mounting, no cluster allocation, no File Access (atime) timestamp updates. | ✅ Eliminated |
| **4. CD switch no-recovery** | With Risks 1–3 eliminated, there are no bus timeouts or CRC errors generated. The CD switch remaining depressed works in our favor. | ✅ Mooted |

## The Implementation Plan

### 1. Timing Feasibility (The Math)
The RCA is a 16-bit value with 65,535 possibilities (`0x0001`–`0xFFFF`).
We can probe the SD card using `CMD13 (SEND_STATUS)`, which is a non-destructive, addressed command that returns the card's status register without altering state. At the default safe 400kHz initialization clock, one command+response pair takes ~50μs. A full brute-force sweep of all 65k addresses takes **~3 seconds**. At 25MHz, it takes milliseconds.

### 2. The Execution Sequence
Once PCNT confirms bus silence and the ESP32 takes the MUX, the following bare-metal sequence executes:

1. **Configure SDMMC Peripheral**: Set to 4-bit bus, start clock at 25MHz (default speed).
2. **Brute-Force RCA Probe**: Loop `CMD13 (RCA)` from `0x0001` to `0xFFFF` at 400kHz (identification clock).
   - If the card responds with an R2 status, the valid RCA is found.
3. **Switch Clock & Attempt Read**: Switch clock to 25MHz and attempt a `CMD17` (Read Single Block) to read Sector 0.
   - If CRC errors or garbage data are returned, switch clock to 50MHz (High Speed) and retry.
4. **Minimal FAT32 Read**: Read `config.txt` using a custom, read-only sector navigator:
   - Read Boot Sector (Sector 0) to locate FAT and Root Directory.
   - Parse Root Directory to locate `config.txt` clusters.
   - Read specific sectors containing the payload.
5. **State Preservation**: Leave the card exactly as we found it. Do not send `CMD7(0)` if it was selected when we arrived.
6. **Handover**: Tri-state all ESP32 SDMMC GPIO pins to high-impedance. Return MUX control.

When the CPAP resumes, the card holds the exact same RCA, in the exact same state, with a perfectly intact physical FAT structure. The intervention is transparent.

## Technical Hurdles & Scope

This represents a significant, deep-embedded engineering project rather than a simple firmware patch. The core challenges include:

* **Bare-Metal SDMMC Command Dispatch**: Bypassing the monolithic ESP-IDF driver to send arbitrary raw commands (`CMD13`, `CMD17`) without triggering the ESP32 hardware initialization sequence (`CMD0`). Source for the ESP-IDF driver (`components/driver/sdmmc/`) and the ESP32 Technical Reference Manual (Chapter 28) will act as the blueprint.
* **Minimalist FAT32 Reader**: Writing a ~300-line custom C++ function to navigate cluster chains and read a single file without mounting a full OS-level VFS.
* **Blind Bus Configuration**: Resolving whether the CPAP left the card in 25MHz or 50MHz mode by checking for CRC framing errors on the first data read attempt.
* **Empirical Validation**: Relying on the assumption that the AS10 does not perform undocumented background checks (e.g., verifying a host-maintained cycle counter) that would expose the brief ESP32 clock activity.

## Conclusion
This approach moves from "conceptually impossible using standard libraries" to **"a highly viable engineering project using bare-metal register interaction."** It is the most robust, non-intrusive software solution to the AS10 error-recovery loop, completely negating the need for physical hardware modifications to the CPAP machine.
