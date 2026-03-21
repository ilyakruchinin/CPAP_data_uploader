# Heap Fragmentation Re-Check (C54X) — Validated Follow-Up

> **Status**: Analysis + plan — no code changes yet
> **Date**: 2026-03-22
> **Purpose**: Re-check `docs/archive/44-HEAP-RECHECK-CO46.md` against the current branch,
> current logs, current build config, current linked artifacts, and the actual
> historical rationale in commits/release notes/specs.

---

## 1. Executive Summary

### Bottom line

`44-HEAP-RECHECK-CO46.md` is **directionally correct**, but it should **not** be used
as-is for implementation planning.

The most important validated conclusions are:

- **The current failure mode is real and repeatable**:
  repeated smart-mode "nothing to upload" cycles can fail at SD mount when
  `ma` drops to `36852` before `SD_MMC.begin()`.
- **The +101-byte boot diagnostics change is not the cause**.
- **TLS pre-warm was introduced for a real reason**:
  TLS handshake failures after SD mount + scan fragmentation.
- **But the current pre-warm design is now hurting the no-work path**:
  it runs every cycle, even when there is nothing to upload.
- **The best next fix is not a blind return to full “mount → full pre-flight → TLS on demand”**.
  That was tried before for a reason, and the current pre-flight still churns heap.
- **The best next fix is a staged design**:
  a **cheap, low-churn SD work probe before the upload task/TLS path**, so the
  no-work case never pays the cost of task-stack allocation or TLS pre-warm.
- **For power-sensitive hosts, the biggest additional gain is structural**:
  stop doing unnecessary no-work cycles at all, and offer a stricter low-power profile.

### Preferred implementation direction

The recommended next step is:

1. **Recover obvious heap headroom first**
   - remove stale WiFi sleep Kconfig options that are still active in the generated build
   - stop deleting/recreating `WiFiClientSecure` on every TLS reset
2. **Refactor pre-flight into a low-churn “minimal work probe”**
   - no `std::vector<String>` for the fast path
   - early exit on first proof of work
3. **Use a staged acquisition path**
   - if there is **no work**, never create the upload task and never touch TLS
   - if there **is** work, only then choose between task-based upload or blocking upload,
     and only allocate TLS when cloud work is actually confirmed
4. **Then evaluate whether full TLS pre-warm is still needed at all**
   - it may still be a fallback for real cloud sessions on weak heaps
   - but it should not be the default for the no-work case

---

## 2. Validation of `44-HEAP-RECHECK-CO46.md`

## 2.1 What `44` got right

| Item | Verdict | Notes |
|------|---------|-------|
| Failure signature at `ma=36852` | **Correct** | The log pattern is strong and repeatable. |
| `+101` bytes not causal | **Correct** | Static DRAM delta, nowhere near the 8KB swing seen at failure points. |
| TLS pre-warm history matters | **Correct** | Commit history and release notes confirm this was repeatedly added/removed to solve real handshake issues. |
| Stale WiFi sleep Kconfig settings may be hurting heap | **Correct, and stronger than stated** | Not only present in `platformio.ini`, but also present in generated `sdkconfig.defaults`, so they are active in the current build. |
| Asymmetric mbedTLS buffers are not safe in current hybrid build | **Correct** | `sdkconfig.defaults` confirms `CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN` is not set. The ABI-risk warning still stands. |
| Reusing `WiFiClientSecure` object is a reasonable improvement | **Correct** | Not sufficient alone, but low-risk and sensible. |

## 2.2 What `44` got partly right, but needs revision

| Item | Verdict | Revision |
|------|---------|----------|
| “Root cause is the task stack fragmenting the heap” | **Mostly right, but too narrow** | The 12KB task stack is the **dominant immediate trigger** in the failing no-work path, but the broader root cause is the interaction of: long-lived rebootless runtime, current service set, TLS lifetime, and pre-flight churn. |
| “Go back to SD-first, TLS-on-demand” as the primary fix | **Directionally right, but too blunt** | The current pre-flight still allocates `std::vector<String>` and multiple temporary `String`s. A direct return to full pre-flight-before-TLS is too risky without first reducing pre-flight churn. |
| Reduce lwIP pbuf pool further | **Plausible but secondary** | This is now an experimental tuning knob, not the main fix. The biggest gain is avoiding unnecessary cycles. |
| Investigate optional component stripping | **Potentially useful, but weak compared with runtime fixes** | Current linked artifacts show some framework libraries are still loaded regardless of `custom_component_remove`. This is not the highest-confidence path for heap recovery. |

## 2.3 What `44` overstated or ranked too highly

| Item | Verdict | Why |
|------|---------|-----|
| `fb_gfx` removal as a quick win | **Weak / likely ineffective as stated** | The current map shows `libfb_gfx.a` coming from the framework library bundle, not the managed component directory. Adding `espressif__fb_gfx` to `custom_component_remove` likely won’t affect it. |
| Custom TLS arena / mbedTLS fixed allocator | **Too aspirational for near-term plan** | With `WiFiClientSecure` in the loop, this is complex and fragile. Not the next step. |

---

## 3. Verified History: Where TLS Pre-Warm Came From and Why

## 3.1 Commit and release-note timeline

### 2026-03-02 — Initial introduction

**Commit:** `1f6a91e`

**Message:**
`feat: add TLS pre-warming and eliminate HTTPClient to reduce heap fragmentation`

**Why it was introduced:**

- Pre-warm TLS before SD mount so mbedTLS allocates into the cleanest heap region
- Eliminate `HTTPClient` heap churn in the cloud path
- Use raw TLS I/O and keep connections persistent

This is the first explicit branch point where the design shifted toward:

- "TLS first"
- "reduce heap churn from HTTP helpers"

### 2026-03-03 — Removed

**Commit:** `6e51c6f`

**Message:**
`feat: replace static TLS pre-warming with on-demand connection and add dynamic Web UI mode explanations`

**Why it was removed:**

- save ~11s and ~28KB heap when no cloud work exists
- belief that post-SD-mount heap was sufficient
- comments assumed asymmetric TLS buffers would fit

This was explicitly an attempt to make the no-work path cheaper.

### 2026-03-03 — Re-introduced in phased orchestrator

**Commit:** `faa6c86`

**Message:**
`feat: implement dual-backend phased upload with TLS pre-warming and heap-optimized buffer management`

**Why it was re-introduced:**

- cloud-first session design
- TLS handshake needed the cleanest heap
- task creation moved earlier to reduce failure risk

### Release note confirmation

**File:** `release/RELEASE_NOTES_v2.0i-alpha1.md`

This release note explicitly states:

- TLS handshake needs ~36KB contiguous heap
- SD mount + pre-flight scanning fragmented heap too much
- moving TLS before SD mount eliminated handshake failures
- PCNT re-check was added to reduce interference risk

### 2026-03-17 — Removed again

**Commit:** `d4cc025`

**Message:**
`Fix WiFi config key name for ESP-IDF compatibility`

Also removed TLS pre-warm and updated comments to the on-demand strategy.

### 2026-03-17 — Restored again

**Commit:** `51d756d`

**Message:**
`Restore TLS pre-warm before SD mount to prevent handshake failures`

**Why it was restored:**

- pre-flight scan still fragments heap badly enough to hurt TLS handshake reliability
- actual runtime was using symmetric 16KB + 16KB buffers
- on-demand TLS was not stable enough post-mount

## 3.2 Additional design/spec references

Two additional repo documents reinforce the historical rationale:

- `docs/specs/tls-prewarm-pcnt-recheck.md`
- `release/RELEASE_NOTES_v2.0i-alpha1.md`

These are important because they show the design was not arbitrary. The project had
already observed:

- TLS handshake failures after SD mount
- stale TLS buffers impacting SMB
- socket conflicts between TLS and SMB

## 3.3 Key historical conclusion

The branch history does **not** say:

- “TLS pre-warm was a mistake from the start”

It says:

- “TLS pre-warm solved one real problem”
- “but it created or worsened another real problem”

So the correct goal is **not** to blindly revert to old behavior.
The correct goal is to build a path where:

- the **no-work** case does not pay for TLS or task-stack fragmentation
- the **actual cloud-upload** case still has a reliable way to obtain enough contiguous heap

---

## 4. Current Code Reality on This Branch

## 4.1 Current upload path

Current `main.cpp` flow:

```text
handleListening
  → handleAcquiring
  → handleUploading
      → create 12KB upload task
      → uploadTaskFunction
          → TLS pre-warm
          → PCNT re-check
          → SD mount
          → runFullSession (pre-flight + upload)
```

This means the current no-work cycle still pays for:

- upload task allocation
- TLS pre-warm attempt
- SD mount
- pre-flight scan

before it can conclude “nothing to upload”.

That is exactly the wrong shape for the failure seen in `err-cpap_logs.txt.tmp`.

## 4.2 Current pre-flight is not low-churn

This is the most important correction to `44`.

The current pre-flight path in `FileUploader.cpp` still performs heap-churn-heavy patterns:

- `scanFolderFiles()` returns `std::vector<String>`
- `scanDatalogFolders()` builds `std::vector<String>`
- folder and file names are repeatedly converted into `String`
- completed+recent folder checks reopen folders and iterate file lists again

So while the current pre-flight is better than older JSON-heavy designs, it is still **not**
cheap enough to assume that “mount first, then full pre-flight, then TLS” is automatically safe.

## 4.3 Current TLS reset path still recreates the client wrapper

`SleepHQUploader::resetTLS()` currently does:

- `tlsClient->stop()`
- `delete tlsClient`
- `tlsClient = nullptr`
- `setupTLS()` → `new WiFiClientSecure()`

This means repeated reconnect/reset cycles may introduce small additional heap holes.
The large mbedTLS buffers are the bigger issue, but this wrapper churn is still unnecessary.

## 4.4 Current build really is carrying the stale WiFi sleep options

This is stronger than `44` claimed.

They are present in:

- `platformio.ini`
- generated `sdkconfig.defaults`

Specifically:

```ini
CONFIG_ESP_WIFI_SLP_IRAM_OPT=y
CONFIG_ESP_WIFI_SLP_DEFAULT_MIN_ACTIVE_TIME=8
```

So this is not just a documentation mismatch. It is in the current built configuration.

## 4.5 Current branch already has solid baseline power defaults

The current runtime is already much better than older power docs might imply.

Verified in current code:

- CPU default: **80 MHz**
- WiFi TX power default: **5 dBm**
- WiFi power saving default: **MIN_MODEM**
- 802.11b disabled
- mDNS timed out after **60s**
- auto light-sleep infrastructure present
- smart mode still keeps PM lock held in `LISTENING`

So the biggest remaining power issue is **not** “forgot to enable one obvious switch”.
It is the **architecture of repeated unnecessary sessions**, especially in smart mode.

## 4.6 Some “compiled extras” are not high-confidence heap wins

### `fb_gfx`

The map shows `libfb_gfx.a` is loaded from the framework library bundle.
That means adding `espressif__fb_gfx` to `custom_component_remove` is not clearly sufficient.

### `esp_diagnostics`

The map also shows `libespressif__esp_diagnostics.a` objects are still linked.
However, the visible `.bss` impact in the extracted sections is minimal.

### Conclusion

These are worth investigating later, but they are **not** the best next move for heap stability.
Runtime/session behavior dominates here.

---

## 5. What the Current Failure Really Means

## 5.1 Immediate trigger

The current no-work failure pattern is best explained as:

1. long rebootless runtime creates a fragmented heap landscape
2. upload task allocation sometimes cuts the largest free block down to `36852`
3. TLS pre-warm is already in play before SD mount
4. SD mount then fails because the remaining contiguous region is too small for SDMMC DMA allocations

So the **task stack is the immediate trigger**, but not the whole story.

## 5.2 Why the no-work case is uniquely bad

When there is no actual work:

- the device is paying the cost of **task allocation**
- then paying the cost of **TLS pre-warm**
- then paying the cost of **SD mount**
- only then learning that there was nothing to do

That is precisely the path we should optimize first.

## 5.3 The real design mistake

The mistake is not simply “pre-warm exists”.

The mistake is:

- **pre-warm exists before the system knows it needs cloud TLS**
- **the upload task exists before the system knows it needs upload work at all**

That combination is what makes the no-work path unstable and wasteful.

---

## 6. Ranked Implementation Options

## 6.1 Highest-confidence next actions

### Option A — Remove the stale WiFi sleep Kconfig options

**Verdict:** Do this early.

**Why:**

- directly recovers ~2–4KB DRAM / heap headroom
- already identified historically as harmful to contiguous heap
- small power penalty compared with the much larger waste of repeated no-work cycles

**Power tradeoff:**

- likely slightly worse idle WiFi sleep efficiency
- but this is outweighed if it helps eliminate repeated 2-minute acquire attempts

### Option B — Stop deleting/recreating `WiFiClientSecure`

**Verdict:** Low-risk, worthwhile.

**Change in principle:**

- allocate wrapper once
- keep it alive for process lifetime
- `stop()` connections without `delete/new` churn

**Expected impact:**

- small-to-moderate heap stability improvement
- not enough alone, but good hygiene

### Option C — Add heap instrumentation around the real decision points

**Verdict:** Required before invasive change.

Log around:

- before minimal work probe
- after minimal work probe
- before task create
- after task create
- before TLS connect
- after TLS connect
- before SD mount
- after SD mount

This is needed to decide whether the preferred staged design can avoid a fallback pre-warm path.

---

## 6.2 Preferred root-cause fix

### Option D — Replace full pre-flight with a low-churn minimal work probe

**Verdict:** This should be the central next implementation.

Instead of:

- scanning folders into vectors
- reopening folders repeatedly
- collecting file lists into `std::vector<String>`

build a **minimal probe** that answers only:

- “is there any work for SMB?”
- “is there any work for cloud?”

It should:

- stream directory iteration
- use fixed buffers where practical
- early-return on first positive match
- avoid returning vectors from the fast path

Examples of acceptable positive proof:

- incomplete folder with at least one `.edf`
- pending folder that now has files
- completed+recent folder with a changed file
- mandatory root/settings file changed for SMB

This would materially reduce heap churn in the pre-TLS decision phase.

### Option E — Add a staged “probe first, then choose path” upload flow

**Verdict:** This is better than the direct recommendation in `44`.

#### Proposed staged flow

```text
1. LISTENING confirms idle
2. Main context takes SD control (no upload task yet, no TLS yet)
3. Run minimal work probe only
4. If no work:
     release SD
     enter cooldown/listening
     done
5. If work exists:
     choose execution mode:
       - async task if heap is healthy
       - blocking/in-place upload if heap is marginal
6. Only allocate TLS when cloud work is actually confirmed
```

### Why this is better than `44`’s primary recommendation

`44` mainly suggested:

- mount SD
- run pre-flight
- then do TLS on demand

That is directionally right, but it still assumes the current pre-flight is cheap enough.
It is not.

The improved version is:

- **first make the work-check cheap and low-churn**
- **then move TLS behind that cheap decision**

This is the safer path.

### Why this directly fixes the current failure

In the common no-work cycle:

- no upload task is created
- no TLS is created
- the heap does not suffer the task-stack split before the decision is made

So the dominant trigger in the failing path disappears.

---

## 6.3 Strong additional option that `44` did not emphasize enough

### Option F — Suppress repeated no-work cycles unless new host activity occurred

**Verdict:** Architecturally strong, but more involved.

The current smart-mode loop keeps reacquiring the SD after every cooldown even when:

- nothing new has happened on the bus
- the previous cycle already concluded “nothing to upload”

That is poor for both:

- heap stability
- power consumption

### Better model

After a `NOTHING_TO_DO` result, do not reacquire just because another silence interval elapsed.
Require **evidence of new CPAP bus activity** first.

Conceptually:

```text
Nothing-to-do result
  → wait for new host activity latch
  → then wait for silence threshold again
  → then attempt acquisition
```

### Why this is powerful

It attacks the actual bad behavior in the log:

- repeated useless cycles
- repeated task/TLS/SD churn
- repeated power burn

### Caveat

Current `COOLDOWN` suspends PCNT to allow light-sleep, so activity occurring during cooldown
would be missed. To do this properly, the FSM would need one of:

- PCNT active during cooldown
- cooldown merged into listening with a time gate
- a new activity latch strategy that survives the low-power path

So this is not the first patch, but it is a strong structural direction.

---

## 6.4 Power-aware options worth considering

### Option G — Offer a stricter low-power / low-risk profile

**Verdict:** High-value for weak hosts, but product-behavior change.

The current firmware already has decent power defaults.
The remaining big gains are structural.

A stricter profile could include:

- scheduled mode strongly preferred
- no mDNS
- reduced web UI behavior
- no SSE/live log stream by default
- optional OTA-disabled build/profile
- WiFi only around upload windows or manual wake

This would reduce both:

- average power draw
- heap churn from always-on services

### Option H — Shorter or configurable mDNS lifetime

**Verdict:** Reasonable, but secondary.

Current mDNS already times out after 60s. That is good.
Reducing to 30s or making it profile-dependent is worthwhile, but this is not the main heap fix.

### Option I — 1-bit SD mode as an electrical safety profile

**Verdict:** Worth considering for weak CPAP hardware, but not a primary heap fix.

This is about bus stress / current profile more than contiguous heap.

---

## 6.5 Secondary / experimental options

### Option J — Further reduce lwIP pbuf pool

**Verdict:** Possible, but secondary.

Current value is already `12`, not the old `32`.
Reducing further may help, but the safer and larger gain is avoiding unnecessary sessions.

### Option K — Investigate framework-level extras (`fb_gfx`, diagnostics)

**Verdict:** Cleanup item, not primary fix.

Useful if later measurements show meaningful DRAM/BSS impact.
Not the first move.

### Option L — Custom TLS allocator / fixed TLS arena

**Verdict:** Not next.

Too invasive for the current stack and wrapper architecture.
Only worth revisiting if the staged work-probe design still cannot stabilize cloud sessions.

---

## 7. Recommended Next Plan

## Phase 1 — Ground truth + obvious heap recovery

1. remove:
   - `CONFIG_ESP_WIFI_SLP_IRAM_OPT=y`
   - `CONFIG_ESP_WIFI_SLP_DEFAULT_MIN_ACTIVE_TIME=8`
2. clear build cache:
   - `sdkconfig.pico32-ota`
3. keep `sdkconfig.project` aligned
4. stop recreating `WiFiClientSecure`
5. add heap logs around:
   - minimal probe
   - task create
   - TLS connect
   - SD mount

## Phase 2 — Fix the no-work path first

Implement a **minimal SD work probe** before the upload task and before TLS.

Design goals:

- no `std::vector<String>` in the fast path
- early exit on first proof of work
- no task allocation when there is no work
- no TLS allocation when there is no cloud work

## Phase 3 — Choose cloud execution path based on real heap data

After Phase 2 instrumentation, choose one of:

### Path 3A — Preferred if heap is sufficient after probe

- mount
- minimal probe
- confirm cloud work
- TLS on demand
- upload

### Path 3B — Hybrid fallback if cloud handshake is still marginal

- mount
- minimal probe
- confirm cloud work
- if heap below safe threshold: use blocking/in-place upload path
- else use async task path

This is a much better fallback than unconditional pre-warm on every cycle.

## Phase 4 — Structural/power improvement

Add a no-work suppression strategy so repeated reacquisition does not happen without
new host activity.

For weak hardware, add an optional stricter profile:

- scheduled mode
- minimal services
- lower always-on convenience
- lower brownout and fragmentation exposure

---

## 8. Recommended Ranking

## Do now

- remove stale WiFi sleep Kconfig options
- keep `WiFiClientSecure` allocated
- instrument heap around probe/task/TLS/SD
- design low-churn minimal work probe

## Do next

- staged probe-first acquisition path
- choose async vs blocking path based on measured heap

## Do after that

- no-work suppression based on new host activity
- optional stricter low-power profile

## Leave for later

- further lwIP reductions
- framework component cleanup
- custom TLS allocator

---

## 9. Final Conclusion

The right answer is **not**:

- “keep unconditional TLS pre-warm forever”
- or
- “blindly revert to old SD-first full pre-flight”

The right answer is:

- **make the work-detection path cheap**
- **do that before creating the upload task**
- **do not allocate TLS until cloud work is confirmed**
- **retain a fallback path only if measured heap proves it is still needed**

That gives the best balance of:

- heap stability
- correctness
- power savings
- minimum unnecessary SD/TLS churn

It also addresses the exact failure mode in the current log: repeated no-work cycles
should not be paying the most expensive memory costs in the system.
