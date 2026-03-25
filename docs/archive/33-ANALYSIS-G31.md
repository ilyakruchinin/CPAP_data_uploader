# Power Optimisation Analysis G31

## Document Purpose

This document provides a fresh analysis of the power optimisation strategies for the CPAP Data Uploader, specifically incorporating the new perspectives raised in the Gemini analysis appended to `31-A-BUNCH-OF-ADVICES.md`. 

The core task is to evaluate the competing theories of "peak current spikes" versus "sustained average current / thermal limits" as the root cause of brownouts on specific CPAP machines, and to determine the true value of migrating to a framework that supports `CONFIG_PM_ENABLE`.

**Strict constraint**: This is a research document only. No code changes are made or implied as immediate actions.

---

## Executive Summary

### The Core Debate: Spikes vs. Sustained Current

Previous analyses (C54, CO46) operated on the standard assumption that ESP32 brownouts are caused by **instantaneous current spikes** (e.g., 350mA+ during WiFi TX or association) pulling the 3.3V rail below the reset threshold (2.43V).

The Gemini analysis introduces a crucial nuance: **What if the CPAP USB port has a hard, sustained current limit (e.g., a thermal fuse or strict 200mA cap)?**

In a strictly current-limited scenario:
$$ I_{total} = I_{baseline} + I_{spike} $$

If the CPAP port trips at exactly 200mA:
- Scenario A (No PM): 50mA baseline + 160mA spike = 210mA $\rightarrow$ **Brownout**
- Scenario B (With PM): 20mA baseline + 160mA spike = 180mA $\rightarrow$ **Stable**

This changes the math completely. If the problem is a strict total current ceiling rather than a transient voltage dip, **reducing average/idle baseline current becomes a critical mitigation strategy, not just an efficiency exercise.**

### The Framework Migration Decision

Because of the equation above, migrating to a framework that respects `CONFIG_PM_ENABLE` (like `pioarduino`) goes from being "a low-value efficiency tweak" to a **potentially critical fix for weak CPAP ports**.

The precompiled Arduino-ESP32 2.0.17 framework fundamentally blocks any reduction in baseline CPU power. It locks the CPU active and wastes ~20-30mA continuously.

### What is Wrong in the Gemini Advice?

While the physics argument is sound, some of Gemini's specific code recommendations are architecturally incompatible with this project:
1. **"Turn WiFi off when idle"**: Breaks the always-on Web UI, live SSE logs, and OTA update capabilities.
2. **"Burst uploads"**: Same issue. The device is not a battery-powered IoT sensor; it is a local network appliance.

---

## Detailed Analysis of Gemini's Claims

### 1. The Physics of Weak CPAP Ports

**Claim**: CPAP ports may have hard sustained current limits, making baseline idle current matter for preventing brownouts during spikes.

**Verdict**: **Highly Plausible.**
Medical devices and their power supplies are heavily regulated. A CPAP diagnostic port is not a standard 500mA PC USB port; it likely has strict short-circuit and thermal protection (e.g., a PTC resettable fuse or active current limiting IC). If that limit is aggressive (e.g., 250mA), the baseline current of the ESP32 consumes a significant portion of the available "headroom" before a spike trips the protection.

### 2. The Value of CONFIG_PM_ENABLE

**Claim**: PM saves ~20mA idle, which creates headroom. PM also slightly reduces TX spikes because the CPU isn't running full speed concurrently.

**Verdict**: **Correct.**
When `CONFIG_PM_ENABLE` is active, the FreeRTOS idle task automatically drops the CPU to the XTAL frequency (typically 10 MHz or 40 MHz) between tasks. 
- At 80 MHz (current fixed), CPU draws ~30mA.
- At 10 MHz (with PM), CPU draws ~2-3mA.
That is 25mA of extra headroom returned to the CPAP power supply budget, allowing a WiFi TX spike to be 25mA taller before tripping a port limit.

### 3. Lowering the PHY Rate

**Claim**: Forcing a lower WiFi PHY modulation rate (e.g., `WIFI_PHY_RATE_11M` CCK instead of 54M OFDM) reduces instantaneous RF power and thus current spikes.

**Verdict**: **Theoretically correct, but complex in practice.**
Lower modulation schemes (DSSS, CCK) do require less linear amplification, resulting in lower peak current per millisecond. 
- 54M OFDM peak: ~320-400mA
- 11M CCK peak: ~220-240mA
**However**, the transmission takes roughly 5x longer. If the CPAP's power protection is thermal-based (PTC fuse), a longer 240mA draw might trip it just as easily as a shorter 400mA draw. It is worth testing, but it is not a guaranteed silver bullet.

### 4. Hardware Capacitor

**Claim**: A 470-1000µF capacitor masks spikes from the CPAP port.

**Verdict**: **100% Correct and confirmed by previous analyses.**
A bulk capacitor acts as a local charge reservoir. The ESP32 draws from the cap during the microsecond-level TX spike, and the cap slowly recharges from the CPAP port at a low average current. This directly defeats both transient voltage sags AND strict port current limits.

---

## The Verdict on Framework Migration

Previous analysis (C54/CO46) concluded: *Migration is high risk, high effort, low reward (because PM only saves average power).*

Revised conclusion (G31): **Migration is moderate risk, moderate effort, HIGH reward (because saving average power creates vital spike headroom on constrained ports).**

### Why `pioarduino` is the right path:

The `pioarduino` project (specifically the stable branch using ESP-IDF 5.x) is the only clean way to get source-compiled SDK configuration while remaining in the PlatformIO/Arduino ecosystem.

By migrating, we unlock:
1. `CONFIG_PM_ENABLE=y` (25mA headroom gain)
2. `CONFIG_BT_ENABLED=n` (Frees ~30KB RAM and flash, reducing heap exhaustion crashes)
3. `CONFIG_FREERTOS_HZ=100` (Reduces CPU wakeups)
4. Native mbedTLS asymmetric buffer support (Eliminates the fragile python rebuild script)

---

## Actionable Recommendations (In Order of Priority)

Even with the new understanding of current budgets, we must respect the architecture. We cannot turn off WiFi. We cannot sleep the device.

### 1. The Hardware Fix (Unchanged)
**Action**: Add a 330-470µF low-ESR capacitor across the 3.3V and GND pins of the ESP32.
**Why**: Physics beats software. This solves both the transient sag and the sustained current limit problems simultaneously.

### 2. Prepare for Framework Migration (New Priority)
**Action**: Create an experimental branch to migrate the project to `pioarduino` v55.03.37.
**Why**: The 25mA baseline reduction from PM is too valuable to ignore if users are hitting hard current limits on their CPAP machines.
**Requirements**: 
- Update `platformio.ini`
- Migrate `TrafficMonitor` PCNT legacy API to the new IDF 5 API.
- Verify `sdkconfig.defaults` are finally respected.
- Test dual SMB/Cloud upload stability.

### 3. Test PHY Rate Limiting (Low Effort Experiment)
**Action**: Implement a config option to force `WIFI_PHY_RATE_11M`.
**Why**: For users with extremely weak ports who cannot solder a capacitor, trading upload speed for a 30% reduction in peak TX current is a viable fallback. 
**Note**: Can be implemented in the current framework without migration.

### 4. Reject "WiFi Off" Advice
**Action**: Maintain the current `MIN_MODEM` / `MAX_MODEM` sleep strategy.
**Why**: Shutting off WiFi completely breaks the fundamental value proposition of the device (Web UI, SSE live logs, OTA readiness).

## Summary of the Shift in Thinking

- **Old Model**: "Only spikes matter. PM reduces idle. Therefore PM doesn't help."
- **New Model**: "Total current matters. Spikes sit on top of idle. Reducing idle lowers the total. Therefore PM helps prevent spikes from hitting the port's ceiling."

This subtle but profound shift justifies the effort required to migrate to a modern, source-compiled framework.
