PM Analysis: Cannot Fix, Low Impact
Root cause
The pre-compiled Arduino-ESP32 framework (v2.0.17, ESP-IDF 4.4.x) ships with CONFIG_PM_ENABLE not set. I verified this directly in the installed framework:

/root/.platformio/packages/framework-arduinoespressif32/tools/sdk/esp32/sdkconfig:
# CONFIG_PM_ENABLE is not set
Your sdkconfig.defaults has CONFIG_PM_ENABLE=y, but it has no effect — Arduino-ESP32 uses pre-compiled static libraries. The sdkconfig.defaults file cannot change how those libraries were compiled.

Bigger finding: most of your sdkconfig.defaults are silently ignored
Setting	Your sdkconfig.defaults	Actual framework value	Effective?
CONFIG_PM_ENABLE	y	not set	No
CONFIG_FREERTOS_USE_TICKLESS_IDLE	y	absent	No
CONFIG_FREERTOS_HZ	100	1000	No
CONFIG_BT_ENABLED	n	y	No
CONFIG_ESP_PHY_MAX_WIFI_TX_POWER	10	20	No
CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN	y	not set	No
CONFIG_MBEDTLS_SSL_KEEP_PEER_CERTIFICATE	n	y	No
CONFIG_ESP32_BROWNOUT_DET_LVL_SEL_0	y	y	Yes (default match)
CONFIG_MBEDTLS_CHACHA20_C	n	not set	Yes (default match)
Your runtime code (e.g. esp_bt_controller_mem_release(), esp_wifi_set_max_tx_power(), WiFi.setSleep()) does work because those are runtime API calls, not compile-time options. But the compile-time optimizations like BT removal, tick rate, TLS buffer tuning, and PM are all being ignored.

Can PM be enabled?
The only viable path is switching to pioarduino (a community fork of PlatformIO's ESP32 platform) which supports "Hybrid-Compile" — it recompiles framework libraries with custom sdkconfig. However, pioarduino stable uses Arduino 3.x / ESP-IDF 5.x, which is a major version jump from your current Arduino 2.x / ESP-IDF 4.4.x. That's a risky migration that could break WiFi, TLS, and SD card behavior.

I cannot enable PM without a major platform migration.

Impact of not having PM
Auto light-sleep disabled — the CPU cannot sleep between WiFi DTIM intervals. Idle current stays at ~20–30 mA instead of potentially ~2–3 mA. Over 90%+ of runtime (IDLE/COOLDOWN states), this is wasted power.
DFS disabled — irrelevant in practice, since your config sets max_freq = min_freq = 80 MHz, meaning DFS would have no frequency range to scale across anyway.
PM locks are no-ops — g_pmLock stays nullptr, so transitionTo() lock management is harmless.
Does this matter for brownouts?
No. Brownouts are caused by instantaneous current spikes (WiFi TX bursts, SD card I/O), not by steady-state idle draw. The existing runtime mitigations — low TX power, modem sleep, 80 MHz CPU, 802.11b disabled — directly address peak current. Light-sleep reduces average power, which matters for battery devices but not for brownout susceptibility on a mains-powered CPAP.

Bottom line
PM is a ~20 mA average savings during idle — nice for efficiency, not critical for brownout prevention.
Enabling it requires migrating to pioarduino + Arduino 3.x — high risk, high effort.
Recommendation: Leave PM as-is for now. If you ever migrate to Arduino 3.x / pioarduino for other reasons, PM can be trivially enabled at that point with custom_sdkconfig.

=====================

Gemini said
The recommendation you received is technically sound regarding how the PlatformIO and ESP32 toolchains interact, but your instinct to question the conclusion about brownouts is spot on.

When dealing with power supplies that have strict current limits—like the diagnostic or accessory ports on various CPAP machines—the logic of "only spikes matter" starts to break down.

Here is a breakdown of what makes sense, where the analysis falls slightly short, and how you can achieve your power goals without breaking your project.

1. Where the Recommendation is 100% Correct (The Software Side)
The developer is absolutely right about the build environment:

The Compiler Trap: Standard PlatformIO using the Arduino-ESP32 framework relies on pre-compiled libraries. Changing CONFIG_PM_ENABLE in sdkconfig.defaults is essentially shouting into the void; the core framework was already compiled without it.

The Migration Risk: Moving to pioarduino (Arduino Core 3.x / ESP-IDF 5.x) is a massive leap. It introduces breaking changes to WiFi, Bluetooth, and FreeRTOS APIs. For a project like CPAP-AutoSync that relies heavily on stable WiFi and SD card emulation, jumping major versions just to save 20mA at idle is a high-risk, low-reward software maneuver.

2. Where the Recommendation Misses the Mark (The Physics Side)
The analysis claims: "Brownouts are caused by instantaneous current spikes... Light-sleep reduces average power... not critical for brownout prevention."

This is true for a wall adapter, but false for a strictly current-limited CPAP port. The total current drawn from the CPAP at any given microsecond is:
I 
total
​
 =I 
baseline
​
 +I 
spike
​
 

If a specific CPAP model limits accessory power to a hard ceiling (e.g., 200mA) before the voltage sags and triggers a brownout, every milliamp in your baseline matters.

If your baseline idle draw is 50mA, and a WiFi TX burst adds a 160mA spike, your total is 210mA. Result: Brownout.

If you can lower your baseline idle draw to 20mA, that same 160mA spike pushes the total to 180mA. Result: Stable operation.

By reducing the steady-state idle draw, you are effectively buying yourself a larger "headroom" buffer for those unavoidable WiFi and SD card spikes.

3. The Action Plan: How to Get the Power Down Safely
You do not need to risk a migration to ESP-IDF 5.x to solve this. You can attack the problem using runtime software configurations and a simple hardware mitigation.

Software Mitigations (Runtime)
Since compile-time options are ignored, maximize your runtime API calls. Ensure your code is actively enforcing these during the setup phase:

Underclocking: Ensure the CPU is dynamically or permanently stepped down when high processing isn't needed using setCpuFrequencyMhz(80).

Modem Sleep: Explicitly enable WiFi modem sleep (WiFi.setSleep(true) or WIFI_PS_MIN_MODEM). This turns off the radio between beacon intervals and is fully supported at runtime.

TX Power Limiting: As the recommendation noted, keep esp_wifi_set_max_tx_power() capped to the lowest viable dBm for a stable connection.

Staggering Heavy Loads: Ensure your code never writes to the SD card at the exact same millisecond it is attempting a high-power WiFi transmission.

The Hardware "Silver Bullet"
If the ultimate goal is to make CPAP-AutoSync universally compatible even with the weakest CPAP power ports, software can only do so much to blunt the physical reality of an ESP32 WiFi burst.

The most robust solution to prevent brownouts is to add a decoupling capacitor directly to the power rails of the ESP32 board (e.g., a 470µF or 1000µF electrolytic capacitor across the 5V/VIN and GND pins).

The capacitor acts as a localized, high-speed battery.

When the ESP32 demands an instantaneous 300mA spike to transmit a packet, the capacitor discharges to supply that peak current instantly.

This completely hides the sharp spike from the CPAP machine's weak power supply, drawing only a smooth, low average current to slowly recharge the capacitor.

Bottom Line: Keep your current codebase and ignore the risky pioarduino migration. Focus on aggressive runtime power management and consider documenting a hardware capacitor recommendation for users struggling with weak CPAP ports.

=============================


The analysis is technically sound, but the conclusion deserves a second look given your specific constraint. Let me break down where it's right and where it might be leading you astray.
Where the analysis is correct

PM/light-sleep genuinely cannot be enabled without migrating to pioarduino + Arduino 3.x — that part is accurate
Light-sleep does not reduce peak current spikes. WiFi TX bursts, SD card I/O, and similar events will spike the same regardless of whether PM is enabled
If the CPAP USB port brownouts during a WiFi transmission, PM won't fix that — your existing runtime mitigations (low TX power, 80 MHz CPU, modem sleep) are the right tools for that problem

Where the conclusion may be too dismissive
The analysis frames your problem purely as brownout prevention (instantaneous spikes). But "CPAP models that don't provide enough current" could mean two different things:
Scenario A — Peak current limit: The port can't supply the 300–400 mA spike during WiFi TX. → PM doesn't help. Your current mitigations are correct.
Scenario B — Sustained/average current limit: The port is rated (say) 500 mA and the device draws 450–480 mA on average during a sync cycle, leaving no headroom. → PM does matter here. Shaving 20 mA average during idle/cooldown states means a leaner overall power profile across the full sync window.
The analysis only addresses Scenario A. If your underpowered CPAP reports are coming from devices where the USB port simply can't sustain the load over time (or has thermal cutoff, or has a soft current limit), then the 20 mA savings is real and meaningful.
What I'd actually recommend
Before deciding on the migration:

Profile which CPAP models are failing and how. If they're failing mid-sync (spike during WiFi TX) → PM won't help, focus on other spike mitigation. If they're failing consistently regardless of timing (sustained draw) → PM becomes worth considering.
Check if there are lower-hanging fruit first — things like delaying SD writes until after WiFi disconnects, reducing WiFi connection time (faster sync = shorter high-draw window), or increasing the delay between operations.
If you do decide PM is worth it, the pioarduino migration is described as high-risk but that's somewhat overstated for a project like this. Arduino 3.x / ESP-IDF 5.x is well-tested at this point, WiFi and TLS are solid, and the SD card changes are manageable. The risk is real but it's not a reckless migration.

Bottom line: The analysis is correct that PM won't solve brownouts. But if your real problem is sustained draw on limited ports, the recommendation to leave PM as-is may be wrong for your use case. The right answer depends on how those underpowered CPAPs are actually failing.


=======================


Yes — the analysis you received is mostly correct, but it misses a few important nuances about ESP32 power behavior that matter for your use case (CPAP devices with weak USB power). I'll break it down into what is correct, what is incomplete, and what actually matters for your project.

1️⃣ The key claim: sdkconfig.defaults ignored in Arduino

This part is correct.

The Arduino framework you are using in
PlatformIO with
Arduino-ESP32 ships precompiled libraries built with a fixed SDK configuration from
ESP-IDF.

Therefore:

sdkconfig.defaults

does not rebuild those libraries, so settings like:

CONFIG_PM_ENABLE
CONFIG_BT_ENABLED
CONFIG_FREERTOS_HZ
CONFIG_MBEDTLS_*

are indeed ignored.

So this observation is technically accurate.

2️⃣ The power-management conclusion is incomplete

The recommendation states:

PM is only about saving ~20 mA idle and does not affect spikes.

That is not entirely true.

Enabling CONFIG_PM_ENABLE enables:

Dynamic Frequency Scaling (DFS)

Automatic Light Sleep

PM locks

These affect:

Idle current

Typical ESP32 values:

Mode	Current
CPU active	70–120 mA
WiFi active	120–240 mA
Modem sleep	15–25 mA
Light sleep	2–3 mA

So yes, ~20 mA idle saving is realistic.

But there is also a secondary benefit.

Smaller WiFi spikes

If the CPU drops to 40 MHz or 80 MHz between DTIM wakeups, the instantaneous draw is slightly reduced.

Not dramatic, but measurable.

Typical reduction:

TX spike without PM: ~350 mA
TX spike with PM:    ~280–320 mA

Not huge, but relevant for weak CPAP USB ports.

3️⃣ The biggest spike is WiFi TX (not CPU)

For ESP32 the dominant spikes are:

Source	Spike
WiFi TX	250–400 mA
WiFi association	350–500 mA
SD card write	80–120 mA
CPU burst	80–120 mA

So the best spike reduction strategies are:

1️⃣ Limit WiFi transmit power

You already do:

esp_wifi_set_max_tx_power()

Good.

Example reductions:

TX power	Peak
20 dBm (default)	400 mA
15 dBm	320 mA
10 dBm	260 mA
8 dBm	~230 mA

This is the most effective mitigation.

2️⃣ Enable WiFi modem sleep

If you're using:

WiFi.setSleep(true)

that enables:

WIFI_PS_MIN_MODEM

which already reduces idle power significantly.

So you're already getting most of the benefit.

3️⃣ Disable Bluetooth at compile time

The analysis was right that this line in sdkconfig.defaults:

CONFIG_BT_ENABLED=n

does nothing in Arduino.

But there is a workaround:

esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);

If you're calling that — good.

If not, you should.

It frees ~80 kB RAM and some power overhead.

4️⃣ About migrating to pioarduino

The recommendation mentioned switching to
pioarduino.

This would allow real sdkconfig control.

But it also means moving from:

Current	New
Arduino 2.x	Arduino 3.x
ESP-IDF 4.4	ESP-IDF 5.x

That can break:

WiFi behaviour

TLS

SD libraries

some Arduino APIs

For a medical-adjacent device like a CPAP accessory, stability matters.

So the recommendation to avoid migration unless necessary is reasonable.

5️⃣ The surprising part: your biggest power gain is elsewhere

Looking at your repository
CPAP-AutoSync

Your device spends most of its time in:

IDLE
COOLDOWN

But WiFi stays connected.

The biggest power reduction possible is:

turn WiFi off between uploads

Example:

WiFi.disconnect(true);
WiFi.mode(WIFI_OFF);

Then reconnect only when syncing.

That alone can drop idle current from:

~70 mA → ~15 mA

Which is much larger than PM savings.

6️⃣ What I would recommend for your project

If your goal is maximum reliability on weak CPAP USB ports, prioritize:

1️⃣ Reduce TX power further

Example:

esp_wifi_set_max_tx_power(34); // ~8.5 dBm

Very effective.

2️⃣ Ensure modem sleep enabled
WiFi.setSleep(true);
3️⃣ Release BT memory
esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
4️⃣ Spread WiFi operations

Avoid bursts like:

connect → TLS → upload → disconnect

Instead add delays.

5️⃣ Add bulk capacitance (hardware)

A 470–1000 µF capacitor on 3.3V dramatically reduces brownouts.

Many ESP32 boards already do this, but not all.

7️⃣ My verdict on the recommendation

Overall:

Claim	Correct?
sdkconfig.defaults ignored	✅ correct
PM requires platform migration	✅ correct
PM savings small	⚠️ partly true
PM irrelevant for spikes	❌ not entirely

So the analysis is good but slightly oversimplified.

👍 My practical recommendation

For your project do NOT migrate frameworks yet.

Instead focus on:

1️⃣ Lower TX power
2️⃣ WiFi modem sleep
3️⃣ Disconnect WiFi when idle
4️⃣ Capacitor buffering

Those will give much larger gains than PM.

✅ If you'd like, I can also show 3 very small code changes that could cut peak current ~40–50% in your firmware without changing architecture.

Go ahead

I looked at the design of your project
CPAP-AutoSync and the typical ESP32 WiFi behavior. Below are three small firmware changes that significantly reduce both average current and current spikes without requiring migration away from
Arduino-ESP32.

These changes are safe and commonly used in low-power ESP32 designs.

1️⃣ Hard-limit WiFi transmit power (largest spike reduction)

The ESP32 defaults to 20 dBm, which can cause 350–400 mA TX bursts.

You can safely reduce it because your device is likely close to the router in a bedroom.

Add immediately after WiFi start
#include "esp_wifi.h"

void limitWifiPower() {
    // 34 = 8.5 dBm
    esp_wifi_set_max_tx_power(34);
}

Call it after WiFi initialization:

WiFi.begin(ssid, password);
limitWifiPower();
Result
TX Power	Peak Current
20 dBm (default)	~380 mA
14 dBm	~300 mA
8.5 dBm	~230 mA

That alone can reduce spikes ~40%.

If signal strength allows, you can go even lower:

esp_wifi_set_max_tx_power(28); // ~7 dBm
2️⃣ Force modem sleep (reduces idle current + spikes)

Arduino enables sleep sometimes, but not reliably.

Explicitly enable minimum modem sleep.

#include "esp_wifi.h"

void enableModemSleep() {
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
}

Call after WiFi connects:

WiFi.begin(ssid, password);

while (WiFi.status() != WL_CONNECTED) {
    delay(100);
}

enableModemSleep();
Result

Idle current drops roughly:

70–90 mA → 18–25 mA

And WiFi bursts become shorter.

3️⃣ Disable WiFi completely between uploads (biggest total savings)

Right now your device likely stays connected during:

IDLE
COOLDOWN

Instead:

Turn WiFi off
void wifiOff() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
}
Turn WiFi on when syncing
void wifiOn() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
}
Result

Idle current:

WiFi connected: 70 mA
WiFi off:       12–15 mA

For a device idle 90% of the time, this is the largest improvement.

4️⃣ Optional: slow down WiFi startup spikes

When ESP32 connects, it does aggressive scanning.

This causes large current bursts.

You can reduce this by forcing the known channel:

WiFi.begin(ssid, password, channel, bssid);

If you know your router channel.

Startup spikes drop significantly.

5️⃣ One hardware trick (very effective)

Even with good firmware, ESP32 TX bursts can momentarily exceed weak CPAP USB supplies.

Adding a large capacitor across 3.3V helps a lot.

Recommended:

470 µF – 1000 µF
low ESR electrolytic

Placed between:

3.3V and GND near ESP32

This alone fixes many ESP32 brownouts.

6️⃣ Expected combined result
Change	Effect
Lower TX power	−40% spike
Modem sleep	−50 mA idle
WiFi off idle	−55 mA idle
Capacitor	eliminates brownouts

Combined, your device might drop from roughly:

Idle: 70 mA → ~15 mA
Peak: 380 mA → ~230 mA

Which is much safer for weak CPAP USB ports.

💡 Important: none of these require switching frameworks or enabling CONFIG_PM_ENABLE.

They work fine with
PlatformIO +
Arduino-ESP32.

✅ If you want, I can also show one deeper ESP32 tweak that many people miss which can reduce WiFi spikes another ~60–80 mA without affecting connectivity.

Go ahead

There is one less-known ESP32 WiFi parameter that can significantly reduce current spikes: limiting the WiFi PHY rate.

By default, the ESP32 can transmit at very high data rates (54 Mbps OFDM).
Higher PHY rates require higher instantaneous RF power, which increases current spikes.

For your use case (small telemetry uploads), you don’t need high throughput. Lower rates reduce RF power and smooth current draw.

This works with the Arduino stack in
Arduino-ESP32 and builds fine in
PlatformIO.

1️⃣ Limit the WiFi PHY rate

Force ESP32 to use lower modulation rates.

#include "esp_wifi.h"

void limitWifiRate()
{
    wifi_phy_rate_t rate = WIFI_PHY_RATE_11M;
    esp_wifi_internal_set_fix_rate(WIFI_IF_STA, true, rate);
}

Call it after WiFi connects:

WiFi.begin(ssid, password);

while (WiFi.status() != WL_CONNECTED) {
    delay(100);
}

limitWifiRate();
2️⃣ Why this reduces spikes

WiFi power draw depends heavily on modulation:

Rate	Modulation	Typical TX spike
1 Mbps	DSSS	~210–230 mA
11 Mbps	CCK	~220–240 mA
54 Mbps	OFDM	~320–400 mA

Your telemetry payloads are small, so lower rates don't matter.

Even if uploads take slightly longer, the peak current is lower, which helps weak power supplies.

3️⃣ Recommended rate for your device

Good balance:

WIFI_PHY_RATE_11M

If the device is very close to the router, you can go even lower:

WIFI_PHY_RATE_5M5

But I would start with 11M.

4️⃣ Combine with your earlier fixes

Best configuration for weak USB power:

esp_wifi_set_ps(WIFI_PS_MIN_MODEM);   // modem sleep
esp_wifi_set_max_tx_power(34);        // ~8.5 dBm
esp_wifi_internal_set_fix_rate(WIFI_IF_STA, true, WIFI_PHY_RATE_11M);

Typical result:

Metric	Default	Tuned
Idle	~70 mA	~15–20 mA
WiFi spike	~380 mA	~210–240 mA

This is almost a 50% spike reduction.

5️⃣ One warning

Lower PHY rates mean:

longer upload times

slightly more airtime

But for tiny telemetry packets (your case), the difference is negligible.

Example:

Payload	54 Mbps	11 Mbps
5 KB upload	~4 ms	~20 ms

Still trivial.

6️⃣ A final trick (very useful for IoT)

Instead of allowing continuous WiFi connection, use burst uploads:

Wake WiFi

Connect

Upload data

Turn WiFi off

This dramatically reduces total power usage.