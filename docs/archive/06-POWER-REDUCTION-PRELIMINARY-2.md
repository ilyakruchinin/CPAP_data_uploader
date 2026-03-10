# Power Optimization Options for FYSETC SD WIFI PRO in CPAP Data Uploader

## Board and SoC overview

The FYSETC SD WIFI PRO is an SD-form-factor board built around an ESP32-PICO device (variants include ESP32‑PICO‑D4 and ESP32‑PICO‑V3‑02) with 2.4 GHz Wi‑Fi and Bluetooth, on‑module 4 MB SPI flash, and an additional on‑board 8 GB SD flash connected via a multiplexer. The SD edge connector exposes standard SDIO/SPI pins plus a 3.3 V supply which the board documentation rates at 3.0–3.6 V with approximately 300 mA peak requirement, suggesting the design expects a reasonably beefy host power source. The board also includes a CH340/CH341 USB‑serial bridge and USB card reader circuitry, but those are only powered when the board is in the dev carrier and are not in play when the bare SD card is inside the CPAP.[^1][^2][^3][^4]

The SD WIFI PRO shares the SD card signals between the ESP32 and the host (printer/CPAP) using an on‑board multiplexer controlled by a GPIO and a small DIP switch on the dev board, so at any given time only one side has electrical access to the built‑in 8 GB flash. According to independent teardown notes, production boards use an ESP32‑PICO‑V3‑02 with integrated crystal, flash and RF, plus a discrete 3.3 V regulator, status LEDs and passives. This means essentially all of the SD card’s power budget goes into the ESP32 SoC, the 8 GB flash and a linear regulator – anything you can do to keep the ESP32 core, Wi‑Fi radio and flash idle or off directly translates into lower current draw.[^2][^5][^3][^4][^1]

## Relevant ESP32 power modes

The ESP32 family provides several Wi‑Fi‑aware low‑power modes that keep a station associated while reducing RF duty cycle and/or throttling the CPU clock. In **Modem‑sleep** the RF/PHY are powered down between DTIM beacons, with the CPU and system clock still running; in **DFS + Modem‑sleep** the CPU/APB frequencies can be reduced during idle periods; and in **Auto Light‑sleep** the chip suspends the CPU and shuts down high‑speed clocks between DTIMs while still automatically waking in time for beacons and traffic.[^6][^7]

Espressif’s measurements for an ESP32 station at 160 MHz in a shielded box show approximate average currents of 20–31 mA in pure Modem‑sleep depending on DTIM and DFS configuration, and around 2–3 mA in Auto Light‑sleep with Wi‑Fi connected, again depending on DTIM interval. Deep‑sleep drops further to around 5 µA (but loses Wi‑Fi association and RAM), and fully active Wi‑Fi RX/TX with CPU at max clock can draw on the order of 95–240 mA depending on workload. These numbers are at the module level and do not include external loads, but they give a realistic bound on what can be achieved by aggressive use of Wi‑Fi power save and sleep states.[^8][^7][^6]

## Wi‑Fi power‑saving configuration

In ESP‑IDF the primary API for Wi‑Fi power saving is `esp_wifi_set_ps(wifi_ps_type_t type)`, where you can select no power save, **WIFI_PS_MIN_MODEM** (wake every DTIM to avoid missing broadcast/multicast), or **WIFI_PS_MAX_MODEM** (wake at a configured listen interval, potentially skipping DTIMs for more savings). For MAX_MODEM you can set a larger `listen_interval` in the STA config so the ESP32 only wakes to beacons every N intervals, which significantly cuts RF on‑time at the cost of latency and possible loss of broadcast packets like ARP. In addition, enabling the generic power management (`CONFIG_PM_ENABLE`) and DFS lets the runtime scale CPU frequency down between Wi‑Fi bursts in combination with Modem‑sleep.[^6]

Auto Light‑sleep couples the Wi‑Fi DTIM‑based wakeups with the system power manager so that, after a configurable idle time, the chip enters light sleep, suspending the CPU and turning off the RF, PLL and main crystal until the next DTIM or outbound packet wakes it. For a design like the CPAP Data Uploader, which mostly waits between bursts of SD reads and SMB/WebDAV writes, this is particularly attractive because it can keep average current in the low single‑digit mA range while preserving connectivity and quick wakeup for scheduled upload slots. Combining Modem‑sleep (MIN or MAX) with Auto Light‑sleep is explicitly supported and documented as the preferred low‑power connected configuration.[^7][^6]

## CPU frequency and dynamic scaling

The ESP32’s CPU can run at several clock frequencies (for example 80, 160, 240 MHz depending on module and configuration), and ESP‑IDF’s power management framework allows defining a maximum and minimum CPU frequency plus an option to enter light sleep when idle. In the recommended low‑power Wi‑Fi configuration, Espressif suggests a max frequency of 160 MHz, a min of 40 MHz, and DFS enabled so the runtime can drop to the lower clock once all performance locks (e.g. Wi‑Fi, peripherals) are released. With DFS enabled, average current in Modem‑sleep at DTIM 1 drops from roughly 31 mA to about 23 mA in their measurements, and similar relative reductions are seen at DTIM 3 and 10.[^7][^6]

For your workload the ESP32 rarely needs maximum compute performance: SD card directory scanning and SMB client operations are mostly IO‑bound, and only Wi‑Fi stack activity truly benefits from higher clocks during brief bursts. Setting a lower maximum CPU frequency (for example 80 or 160 MHz depending on what the toolchain and libraries support on PICO‑D4/V3‑02), enabling DFS, and making sure no code holds power‑management locks longer than necessary would reduce both active and base current without any hardware changes.[^8][^6][^7]

## Bluetooth and RF considerations

The SD WIFI PRO hardware supports both Wi‑Fi and Bluetooth, but only Wi‑Fi is used by the CPAP Data Uploader; there is no need to keep the BT radio powered. On the ESP32, Bluetooth and Wi‑Fi share much of the RF front‑end, and Espressif’s reference designs and measurements show that BT TX/RX modes have similar peak currents to Wi‑Fi (around 95–130 mA depending on output power), so disabling the Bluetooth controller and not starting any BT stack is a direct way to cut RF‑related current. In ESP‑IDF and the Arduino core, this is typically done by not calling any BT init/start APIs and, if linked, explicitly calling `esp_bt_controller_disable()` and `esp_bt_controller_deinit()` early in `app_main` before enabling Wi‑Fi.[^9][^1][^2][^8][^7]

Beyond Bluetooth, Wi‑Fi TX power is configurable: ESP‑IDF exposes a `Max Wi‑Fi TX power (dBm)` option and runtime APIs to set the transmit power cap, so you can lower the maximum TX power since the SD card is almost always within a few meters of the AP in a home bedroom environment. Reducing TX power reduces peak current during transmissions; combined with long DTIM/listen intervals and light sleep this lowers both instantaneous and average draw while still keeping throughput well above what you need for tens of megabytes of nightly data.[^8][^6]

## SD card interface and access patterns

The SD WIFI PRO uses a multiplexer to share the built‑in 8 GB SD flash between the ESP32 and the host, with the control GPIO (GPIO26 according to the pin table) switching the card’s SDIO lines either to the ESP32 or to the SD edge connector. The dev board’s DIP switch bank lets you choose whether the flash is wired to the ESP32 or to the edge connector by default, but in deployed CPAP use the ESP32 firmware is responsible for politely relinquishing the SD card to the host and then taking it back when needed. The card’s pinout table and docs do not describe alternative low‑power SDIO modes, so any current reduction here will come from firmware behaviour: minimising bus clock rate, avoiding unnecessary scans, and spending most of the time with the multiplexer pointing to the CPAP so the ESP32’s SD host and card I/O buffers can remain idle.[^5][^1][^2]

From a power perspective, two things matter: the current drawn by the 8 GB flash during active reads/writes, and the toggling of SDIO lines through the multiplexer. Neither are directly controllable beyond standard SD protocol choices (bus width, frequency), so your best lever is to batch SD operations into brief bursts (e.g. copy all new files once after therapy, then go idle) and keep the ESP32’s SD host driver unclocked and in a low‑power state outside those bursts. Since the CPAP only needs the SD present and accessible at specific times (during and right after therapy, and whenever menus read settings), you can schedule ESP32 reads carefully to avoid contention and unnecessary card wakeups.[^1][^2][^6][^7]

## Practical firmware‑level levers for this project

Published documentation on the CPAP Data Uploader emphasises features like automatic Windows/NAS uploads, OTA updates, and respecting CPAP access to the SD card but does not detail power tuning, meaning there is room for explicit low‑power work in the firmware. Based on the ESP32 documentation and SD WIFI PRO hardware design, the following levers are feasible and should be compatible with your architecture:[^10][^11][^12][^13]

- Fully disable Bluetooth by not linking or starting BT components and calling the BT controller disable/deinit APIs as early as possible, so only Wi‑Fi consumes RF power.[^2][^1][^7][^8]
- Force Wi‑Fi into **WIFI_PS_MIN_MODEM** or **WIFI_PS_MAX_MODEM** power‑save mode after association, choosing MIN_MODEM if you need reliable broadcast/multicast reception, or MAX_MODEM with a long listen interval if you can tolerate missed broadcasts in exchange for lower current.[^6]
- Enable Espressif’s power management and DFS so the CPU clock can scale down between bursts, with a low `min_freq_mhz` (for example 40 MHz) and an appropriate `max_freq_mhz` (80–160 MHz) that still meets timing for SMB/WebDAV and SD operations.[^7][^6]
- Enable Auto Light‑sleep with a short idle timeout so that, once SD and network tasks quiesce, the chip enters light sleep and only wakes on DTIMs or outbound traffic.[^6][^7]
- Reduce maximum Wi‑Fi TX power to the minimum that still maintains a robust connection to a nearby home AP, using ESP‑IDF’s `Max Wi-Fi TX power` configuration or the corresponding runtime APIs.[^6]
- Aggressively batch SD card accesses: scan for new files and copy them in a single session once per day (or per therapy session), then release the SD multiplexer back to the CPAP and power down the SD host and filesystem layers.[^5][^1][^2]
- Avoid background polling of the SD card while the CPAP is running; instead, use a simple state machine keyed off time-of-day and last‑upload markers so the ESP32 spends most of the night in light sleep with Wi‑Fi idle.[^7][^6]
- Minimise logging, debug web UI work and other housekeeping outside of explicit maintenance windows, since these keep the CPU and sometimes Wi‑Fi awake longer than necessary.[^11][^10]

## Additional system‑level ideas

Because some AirSense 11 units appear to have marginal 3.3 V power budget on their SD slots, you may need to combine firmware and system‑level mitigations to stay within their protection limits. Hosting the SD WIFI PRO in an external SD‑to‑microSD adapter with additional decoupling and possibly slightly longer traces introduces some series resistance and capacitance, which can reduce inrush and RF burst current spikes seen by the CPAP’s slot regulator at the cost of marginal signal integrity headroom. Careful choice of low‑ESR but not ultra‑low‑ESR decoupling near the card, plus firmware‑side limits on TX power and burst length, can help stay under whatever over‑current or brownout detector the CPAP uses.[^13][^14][^15][^2]

Another system‑level lever is to bias the design toward short, scheduled upload windows when the machine is idle and the blower and heater loads are off, so that the host’s internal PSU has more margin for the SD card’s current peaks. Since your firmware already tracks uploaded files and supports scheduled uploads with timezone adjustments, you can line up the upload task to run once shortly after therapy ends and then leave the ESP32 in deep sleep (with SD relinquished) for the rest of the day, waking only on power‑cycle or at the next nightly window.[^14][^11][^13]

## Summary of feasible options

The FYSETC SD WIFI PRO hardware does not expose any undocumented low‑power magic switches, but the ESP32 SoC and Wi‑Fi stack provide a rich set of levers you can exploit from firmware. For the CPAP Data Uploader project, the most impactful options are: fully disabling Bluetooth, enabling Wi‑Fi power save (Modem‑sleep and Auto Light‑sleep), lowering CPU and Wi‑Fi TX power where possible, batching SD and network operations into short bursts, and using deep sleep between daily upload cycles while leaving the SD mux in the CPAP‑owned position. Combining these with some modest system‑level tweaks to decoupling and timing should significantly reduce peak and average current draw on sensitive AirSense 11 units while preserving the zero‑touch upload experience.[^1][^2][^8][^7][^6]

---

## References

1. [FYSETC/SD-WIFI-PRO](https://github.com/FYSETC/SD-WIFI-PRO) - Based on ESP32 PICO D4, with 4MB Flash · 2.4G Wifi & Bluetooth · SD 7.0 size standard, SDIO/SPI comp...

2. [SD WiFi Pro](https://wiki.fysetc.com/docs/SD-WiFi-Pro) - SD WIFI PRO 1. Introduction This is a brand new SD WIFI, we call it SD WIFI PRO, or SWP for short. I...

3. [ESP32­PICO­D4](https://rarecomponents.com/docs/esp32-pico-d4_datasheet_en.pdf)

4. [SD WIFI PRO - /dev/hack](https://wiki.devhack.net/SD_WIFI_PRO) - fysetc https://www.fysetc.com/products/fysetc-upgrade-sd-wifi-pro-with-card-reader-module-run-wirele...

5. [FYSETC SD-WIFI-PRO](https://esp3d.io/ESP3D/Version_3.X/hardware/esp_boards/esp32-pico/sd-wifi-pro/) - ESP32-PICO with 4MB flash memory, ceramic antenna, SD Card with no serial header, the sharing of SD ...

6. [Introduction to Low Power Mode in Wi-Fi Scenarios - ESP32](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/low-power-mode/low-power-mode-wifi.html)

7. [Sleep Modes - ESP32 - — ESP-IDF Programming Guide v5 ...](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/sleep_modes.html)

8. [ESP32 Modem Sleep Mode](https://deepbluembedded.com/esp32-sleep-modes-power-consumption/) - ESP32 Sleep Modes & Power Consumption in Each Mode. ESP32 Low Power Modes, ESP32 Deep Sleep, ESP32 M...

9. [FYSETC Upgrade SD WIFI PRO with Card-Reader - Run ...](https://www.fysetc.com/products/fysetc-upgrade-sd-wifi-pro-with-card-reader-module-run-wireless-by-esp32-chip-web-server-reader-uploader-3d-printer-parts) - It is based on ESP32-PICO-D4 and SD 7.0 size standard, with additional 8MB Flash, 2MB PSRAM, which c...

10. [Announcing: CPAP auto uploader](https://www.reddit.com/r/SleepApnea/comments/1p95r3s/announcing_cpap_auto_uploader/) - Announcing: CPAP auto uploader

11. [Announcing CPAP Data-Uploader v0.4.1 - Reddit](https://www.reddit.com/r/CPAP/comments/1pxk36j/announcing_cpap_datauploader_v041/) - Secure credential storage in ESP32 flash memory (optional). Respects CPAP machine access to SD card....

12. [Announcing: CPAP auto uploader](https://www.reddit.com/r/CPAP/comments/1p959my/announcing_cpap_auto_uploader/) - Announcing: CPAP auto uploader

13. [Open Source Automatic CPAP Data Uploader - Apnea Board](https://www.apneaboard.com/forums/Thread-Open-Source-Automatic-CPAP-Data-Uploader) - https://github.com/amanuense/CPAP_data_uploader. The device is called SD WiFi Pro and sold by fysetc...

14. [Open Source Automatic CPAP Data Uploader - Printable Version](https://www.apneaboard.com/forums/printthread.php?tid=51341) - https://github.com/amanuense/CPAP_data_uploader. The device is called SD WiFi Pro and sold by fysetc...

15. [FYSETC SD WIFI PRO с модул, четец за карти, работещи чрез безжична мрежа на чип ESP32, актуализиране на уеб сървър, четец и за зареждане на части на 3D принтер](https://kolocamp.fi/Product_60514-latest/) - FYSETC SD WIFI PRO с модул, четец за карти, работещи чрез безжична мрежа на чип ESP32, актуализиране...

