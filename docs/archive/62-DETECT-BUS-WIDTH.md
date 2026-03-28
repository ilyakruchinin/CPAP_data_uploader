# Bus-Width Detection & Machine Auto-Identification

> **Predecessor**: [61-NEW-PLAN-FOR-AS10.md](61-NEW-PLAN-FOR-AS10.md) — Stealth RCA Discovery (The "Zero-CMD0" Mount)

## Purpose

This document extends the Zero-CMD0 architecture with a **bus-width detection phase** that allows the ESP32 to automatically identify whether it is connected to an AirSense 10 (1-bit SD mode) or AirSense 11 (4-bit SD mode) — without user configuration, without PCNT, and without disrupting the CPAP's SD card session.

Once detected, the result is cached in NVS, enabling the firmware to permanently tailor its upload strategy (PCNT-based idle detection for AS11, POR-timing-based detection for AS10) without any user intervention.

---

## Background: Why We Need This

### The PCNT Blind Spot

The SD-WIFI-PRO board's only passive sensor when MUX=CPAP is `CS_SENSE` (GPIO 33), hardwired to **DAT3** on the host side of the bus multiplexer.

| Host Bus Mode | DAT3 Behavior | PCNT Result |
|---|---|---|
| **4-bit** (AS11) | Toggles rapidly during data transfers | Activity detected — PCNT works |
| **1-bit** (AS10) | Permanently idle (held HIGH by pull-up) | Silence — PCNT is blind |

In 1-bit SD mode, DAT3 is structurally unused. No software configuration can make a signal appear on a wire the host isn't driving. PCNT-based idle detection is therefore **unreliable for AS10** — perpetual silence is indistinguishable from a genuinely idle bus.

### No Other Side-Channel Exists

| Signal | GPIO | Visible when MUX=CPAP? | Useful? |
|---|---|---|---|
| DAT3/CS | 33 (CS_SENSE) | Yes (dedicated sense trace) | Only for 4-bit hosts |
| CMD | 15 | No (through MUX) | — |
| CLK | 14 | No (through MUX) | — |
| DAT0 | 2 | No (through MUX) | — |
| DAT1 | 4 | No (through MUX) | — |
| DAT2 | 12 | No (through MUX) | — |
| SD_POWER | 27 | Yes | Power state only |

**There is zero way to detect AS10 SD activity before grabbing the MUX.** GPIO 33 is the only observation point, and it carries no information in 1-bit mode.

### What We Know About AS10 Bus Width

Reverse-engineering of the AS10 firmware (via the [airbreak](https://github.com/osresearch/airbreak/) project) reveals the following initialization sequence:

```
ACMD51 → read SCR register
  check SD_BUS_WIDTHS.bit2
    if set → ACMD6(0x02)   → switch to 4-bit mode
           → ACMD42(0x2A)  → disable pull-up on DAT3
           → read SD_STATUS → if fails → ACMD6(0x00) fallback to 1-bit
```

The AS10 firmware **attempts** 4-bit mode but has a **fallback to 1-bit**. Despite the SD-WIFI-PRO card supporting 4-bit (the ESP32 uses it successfully), empirical evidence (PCNT showing zero DAT3 activity during therapy) strongly suggests the AS10 **settles on 1-bit mode** on this specific board — possibly due to MUX signal integrity affecting the `SD_STATUS` validation.

**Not definitively confirmed.** A logic analyzer trace during AS10 boot would settle this. However, this detection scheme eliminates the need for external measurement — the ESP32 determines the mode autonomously.

---

## The Detection Method: CRC-Based Bus-Width Probing

### Core Principle

The SD card **remembers** the bus width negotiated during initialization. It sends data according to its current mode regardless of what the host expects. By deliberately mismatching the host's bus width configuration and observing the CRC result, we can infer the card's actual mode:

| ESP32 Configuration | Card's Actual Mode | What Happens | CRC Result |
|---|---|---|---|
| 1-bit | **1-bit** | Data flows correctly on DAT0 | **Pass** → card is 1-bit |
| 1-bit | **4-bit** | Card sends on DAT0–DAT3; ESP32 only reads DAT0 | **Fail** → card is 4-bit |

### Why 1-Bit First Is Mandatory (Landmine #1: The Stranded Card)

The order of probing is **critical**. Probing in the wrong order can leave the card stranded in an unrecoverable state.

#### Safe: Host 1-bit, Card 4-bit

```
Card sends 512 bytes across 4 wires → 1,024 clock cycles
ESP32 expects 512 bytes on 1 wire  → 4,096 clock cycles

Card finishes at clock 1,024 → returns to Transfer state → waits
ESP32 reads 3,072 more clocks of pulled-up HIGH bits → CRC fails
Card state: PRESERVED (already back in Transfer)
```

The card completes its transfer early and returns to Transfer state. The ESP32 harmlessly over-clocks into silence. **No damage.**

#### DANGEROUS: Host 4-bit, Card 1-bit

```
ESP32 expects 512 bytes on 4 wires → 1,024 clock cycles
Card sends 512 bytes on 1 wire     → 4,096 clock cycles

ESP32 finishes at clock 1,024 → stops providing clock
Card has only sent 128 of 512 bytes → FROZEN in "Sending-Data" state
```

The card is stranded mid-transfer. If the MUX returns to the CPAP, the card will dump its remaining stale data onto DAT0 using the CPAP's clock pulses, corrupting whatever command the CPAP is trying to send. **Guaranteed crash.**

#### The Safety Net: CMD12 (STOP_TRANSMISSION)

Even though our probe order (1-bit first) avoids the dangerous scenario, **any failed CMD17 must be followed by CMD12** as a mandatory safety net. `CMD12` is a special abort command that:

- Can be issued at any time, in any card state
- Forces the card to immediately abort any in-progress data transfer
- Resets the card's state machine back to **Transfer** state

This covers edge cases where CMD17 fails for reasons other than bus width mismatch (e.g., sector read error, timeout, electrical glitch).

**Rule: Every failed CMD17 → immediately issue CMD12 → then proceed.**

---

## The Complete Detection Sequence

### Phase 0: Pre-Conditions

- MUX is grabbed (SD_SWITCH_PIN = ESP control)
- No `CMD0` is sent — ever
- All operations use bare-metal SDMMC register access (see Landmine #2 below)

### Phase 1: RCA Discovery (Bare-Metal CMD13 Sweep)

```
Configure SDMMC peripheral: 1-bit mode, 25MHz clock
Clear all pending interrupts
For RCA = 0x0001 to 0xFFFF:
    Write CMD13(RCA) directly to SDMMC_CMD_REG
    Poll SDMMC_RINTSTS for command-done flag (no RTOS yield)
    If valid R1 response received:
        Read CURRENT_STATE from R1 bits [12:9]
        Store RCA and card_state
        Break
    Clear interrupt flags, continue
```

**Timing at 25MHz**: 96 bits per CMD13 cycle × 65,535 attempts = **~252ms worst case** (bare-metal polling). Typical case (RCA found early in sweep) is much faster.

### Phase 2: Bus-Width Detection (CRC Probing)

```
// Card state is known from Phase 1 (Standby=3 or Transfer=4)
If card_state == Standby:
    Send CMD7(RCA) to select card → card enters Transfer state
    Remember: must_deselect = true

// Probe 1: Try 1-bit mode
Configure SDMMC for 1-bit, 25MHz
Send CMD17(sector 0) → read 512 bytes
If CRC pass:
    detected_bus_width = 1-bit
    // Try verifying clock speed is correct (data is valid MBR)
Else:
    Send CMD12 (STOP_TRANSMISSION) → force card back to Transfer
    // CRC failed → card is likely 4-bit, or wrong clock speed

    // Probe 2: Try 1-bit @ 50MHz (in case it was clock, not bus width)
    Configure SDMMC for 1-bit, 50MHz
    Send CMD17(sector 0)
    If CRC pass:
        detected_bus_width = 1-bit
    Else:
        Send CMD12 (STOP_TRANSMISSION)

        // Probe 3: Try 4-bit @ 25MHz
        Configure SDMMC for 4-bit, 25MHz
        Send CMD17(sector 0)
        If CRC pass:
            detected_bus_width = 4-bit
        Else:
            Send CMD12 (STOP_TRANSMISSION)

            // Probe 4: Try 4-bit @ 50MHz
            Configure SDMMC for 4-bit, 50MHz
            Send CMD17(sector 0)
            If CRC pass:
                detected_bus_width = 4-bit
            Else:
                Send CMD12 (STOP_TRANSMISSION)
                // All probes failed → abort, return MUX
```

**Probe order rationale**: 1-bit attempts are always safe (card finishes early if actually 4-bit). By the time we reach 4-bit probes, we've already ruled out 1-bit mode, so the 4-bit configuration should match. The CMD12 after every failure guarantees a clean state regardless.

### Phase 3: Read config.txt (Minimal FAT32 Reader)

Using the now-confirmed bus width and clock speed:

```
1. Read Sector 0 (MBR/Boot Sector) → locate FAT and Root Directory offset
2. Read Root Directory sectors → find config.txt directory entry → get start cluster + size
3. Walk FAT chain → read data sectors containing config.txt content
4. Parse config into memory
```

~5–15 sector reads total. At 25MHz in 4-bit mode, each 512-byte read takes ~10μs. Total read time: **<1ms**.

### Phase 4: State Restoration & MUX Return

```
// Restore card to exact state CPAP left it in
If must_deselect:
    Send CMD7(0x0000) → deselect card → return to Standby
// Else: card was in Transfer state, leave it there

// Release bus
Tri-state all SDMMC GPIO pins (CMD, CLK, DAT0–DAT3) to high-impedance
Set SD_SWITCH_PIN = CPAP control
```

---

## Landmine #2: RTOS Timing Trap

### The Problem

The ESP-IDF `sdmmc_host_do_transaction()` API performs the following for **every single command**:

1. Acquire RTOS mutex (`xSemaphoreTake`)
2. Set up DMA linked lists
3. Clear interrupt flags
4. Yield CPU to RTOS scheduler
5. Wait for hardware interrupt
6. Context-switch back to caller
7. Release mutex

This overhead is **300–500μs per command**, even for a simple 48-bit CMD13 that takes 3.84μs on the wire at 25MHz.

| Method | Per-Command Time | Full 65K Sweep | MUX Hold Time |
|---|---|---|---|
| ESP-IDF API | ~400μs | **~26 seconds** | Unacceptable |
| Bare-metal polling | ~4μs (at 25MHz) | **~252ms** | Acceptable |

A 26-second MUX hold during therapy is catastrophic. Even if the AS10 handles error recovery, holding the bus for that long maximizes the probability of colliding with a CPAP write operation.

### The Solution: Bare-Metal Register Access

The CMD13 sweep must bypass ESP-IDF entirely and operate as a tight `while()` loop writing directly to the ESP32 SDMMC hardware registers:

```c
// Pseudocode — bare-metal CMD13 sweep
#define SDMMC_BASE       0x3FF68000
#define SDMMC_CMDARG_REG (*(volatile uint32_t*)(SDMMC_BASE + 0x28))
#define SDMMC_CMD_REG    (*(volatile uint32_t*)(SDMMC_BASE + 0x2C))
#define SDMMC_RESP0_REG  (*(volatile uint32_t*)(SDMMC_BASE + 0x30))
#define SDMMC_RINTSTS    (*(volatile uint32_t*)(SDMMC_BASE + 0x44))

uint16_t found_rca = 0;
for (uint32_t rca = 1; rca <= 0xFFFF; rca++) {
    SDMMC_CMDARG_REG = rca << 16;           // RCA in upper 16 bits
    SDMMC_RINTSTS    = 0xFFFFFFFF;          // Clear all interrupt flags
    SDMMC_CMD_REG    = CMD13_FLAGS;         // Fire CMD13
    while (!(SDMMC_RINTSTS & CMD_DONE));    // Spin-wait for completion
    if (!(SDMMC_RINTSTS & RESP_ERR_FLAGS)) {
        found_rca = (uint16_t)rca;
        break;
    }
}
```

The ESP32 Technical Reference Manual (Chapter 28: SDMMC) and the ESP-IDF source (`components/driver/sdmmc/sdmmc_host.c`) provide the exact register definitions and command flag encoding.

Once the RCA is found, subsequent operations (CMD7, CMD12, CMD17) can either continue bare-metal or transition to a lightweight wrapper. The critical path is the 65K-iteration sweep — everything after it is a handful of commands.

---

## Detection Results & Firmware Behavior

### NVS Caching

The detected bus width and machine type are written to NVS on first detection:

| NVS Key | Value | Meaning |
|---|---|---|
| `sd_bus_width` | `1` or `4` | Detected SD bus width |
| `cpap_type` | `AS10` or `AS11` | Inferred machine type |

Subsequent boots read the cached value, skipping the detection phase entirely. The detection only re-runs if NVS is cleared or if the cached value is absent.

### Behavioral Implications

| Detected Mode | Machine | PCNT Reliability | Upload Trigger Strategy |
|---|---|---|---|
| **0-bit (no RCA)** | Uninitialized / Card Reader | N/A | Wait for user/CPAP to mount, or mount natively via ESP |
| **1-bit** | AS10 | Unreliable (DAT3 idle) | POR timing: wait >90s without power-cycle, or scheduled mode |
| **4-bit** | AS11 | Reliable (DAT3 toggles) | Standard PCNT idle detection |

This eliminates the `AS10_MODE` user configuration flag — the firmware determines it autonomously.

---

## Updated Risk Matrix

Building on the [original risk assessment](61-NEW-PLAN-FOR-AS10.md):

| Risk | Mitigation | Status |
|---|---|---|
| **1. RCA destruction** | No `CMD0`. Brute-force existing RCA via `CMD13`. | ✅ Solved |
| **2. Mid-transaction sever** | PCNT (AS11) / POR timing (AS10) / CMD13 self-diagnosis | ✅ Solved |
| **3. FAT32 cache incoherency** | Strictly read-only. No writes, no timestamps, no mounting. | ✅ Eliminated |
| **4. CD switch no-recovery** | Risks 1–3 gone → nothing to recover from. | ✅ Mooted |
| **5. Stranded card (NEW)** | Probe 1-bit first (safe order). CMD12 after every failed CMD17. | ✅ Mitigated |
| **6. RTOS timing trap (NEW)** | Bare-metal register sweep (~252ms), no ESP-IDF API in hot loop. | ✅ Mitigated |

---

## Transparency Verification

The following table confirms that **no command in the detection sequence modifies card state**:

| Command | Purpose | Changes RCA? | Changes Bus Width? | Changes Clock Mode? | Modifies Card Data? | Changes State Machine? |
|---|---|---|---|---|---|---|
| CMD13 | RCA probe | No | No | No | No | No |
| CMD7 | Select/deselect | No | No | No | No | Yes (tracked & restored) |
| CMD17 | Read sector | No | No | No | No | No (returns to Transfer) |
| CMD12 | Abort transfer | No | No | No | No | Yes (forces Transfer — safe) |

Clock speed is host-driven; the card does not store or validate it. Bus width is only changed by `ACMD6`, which we never send. The card is byte-for-byte, state-for-state identical after the detection sequence.

---

## Implementation Scope

| Component | Estimated Effort | Description |
|---|---|---|
| Bare-metal SDMMC command engine | Medium | Register-level CMD13/CMD7/CMD12/CMD17 dispatch, polling-based. Blueprint: ESP-IDF `sdmmc_host.c` + ESP32 TRM Ch.28. |
| CRC-based bus-width prober | Low | 4-attempt CMD17 sequence with CMD12 safety net. |
| Minimal FAT32 sector reader | Medium | ~300 lines: parse MBR → locate FAT → walk cluster chain → read file. |
| NVS cache integration | Low | Store/retrieve detected bus width and machine type. |
| GPIO tri-state cleanup | Low | Reset SDMMC pins to high-impedance before MUX return. |
| **Total** | **~500–700 lines of new code** | Self-contained module, no modifications to existing upload logic. |

## Conclusion

This bus-width detection scheme transforms the Zero-CMD0 architecture from a manually-configured AS10 workaround into a **fully autonomous machine identification system**. By exploiting the CRC behavior of bus-width mismatches, the ESP32 can determine in under 300ms whether it is connected to an AS10 or AS11 — without user configuration, without PCNT, and without disturbing the CPAP's SD card session.

Combined with the CMD12 flush policy and bare-metal register access, the detection is both safe (no stranded card risk) and fast (no RTOS timing bloat). The result is cached permanently in NVS, enabling the firmware to adapt its behavior for the lifetime of the device.
