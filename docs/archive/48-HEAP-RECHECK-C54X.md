# Heap Fragmentation Re-Check (C54X) — Revalidated Final Plan

> **Status**: Analysis + plan — no code changes yet
> **Date**: 2026-03-22
> **Purpose**: Re-check `44-HEAP-RECHECK-CO46.md`, `45-HEAP-RECHECK-C54X.md`,
> `46-HEAP-RECHECK-G31.md`, and `47-HEAP-RECHECK-CO46.md` against the current
> branch, current build artifacts, commit history, release notes, and current
> code paths. Produce one corrected plan that prioritizes heap stability,
> reliable TLS only when actual work exists, power savings, and continued Web UI
> accessibility.

---

## 1. Executive Summary

### Bottom line

The previous four documents converged on the right general diagnosis, but they do
**not** all rank the solutions correctly.

The most important validated conclusions are:

1. **TLS pre-warm was introduced for a real reason**.
   It was added because TLS handshakes after `SD_MMC.begin()` and pre-flight scan
   fragmentation were failing. The historical rationale is real, documented, and
   repeated in both commits and release notes.

2. **The current failure is not solved by order changes alone**.
   The current no-work failure is triggered by a combination of:
   - long-lived heap fragmentation from prior TLS sessions
   - heap-backed upload task creation
   - then SD mount needing a large contiguous block

3. **If the goal is “only allocate TLS when actual work exists,” the strongest
   path is not “hope post-mount heap is good enough.”**
   The strongest path is to **decouple TLS from the normal heap** so the order of
   SD mount vs TLS becomes irrelevant.

4. **The custom TLS arena is the most important structural fix**.
   `45` ranked it too low. `46` was right to prioritize it, but overstated the
   simplicity of the allocator. The real implementation should be a small
   threshold-based allocator, not a naive “only two allocations happen” model.

5. **The upload task stack is the immediate trigger in the observed SD-mount
   failures**.
   Replacing `xTaskCreatePinnedToCore()` with `xTaskCreateStaticPinnedToCore()` is
   a high-value, low-risk fix and should be ranked much higher than earlier docs
   did.

6. **The old blanket claim that asymmetric / variable mbedTLS buffer options are
   categorically ABI-unsafe is now too strong**.
   The current build tree contains locally compiled:
   - `.pio/build/pico32-ota/lib243/NetworkClientSecure/NetworkClientSecure.cpp.o`
   - `.pio/build/pico32-ota/lib243/NetworkClientSecure/ssl_client.cpp.o`

   Their dependency file shows `ssl_client.cpp` compiling against the generated
   package `sdkconfig.h`, and that header already reflects project-specific
   custom sdkconfig values like:
   - `CONFIG_ESP_PHY_MAX_WIFI_TX_POWER=10`
   - `CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=4`
   - `CONFIG_LWIP_PBUF_POOL_SIZE=12`
   - cipher removals

   That means the old repo comments about “FORBIDDEN due to ABI mismatch” should
   no longer be treated as authoritative doctrine. These options should now be
   treated as **build-and-smoke-test candidates**, not as automatic no-go items.

7. **Power savings should come from eliminating pointless cycles, not by turning
   WiFi/Web UI off**.
   `46`’s “Dark Listening” conflicts with the requirement that the Web UI remain
   accessible. The power plan should instead reduce unnecessary SD/TLS work while
   keeping WiFi + Web UI alive in a low-power state.

---

## 2. Verified History: Where TLS Pre-Warm Came From and Why

## 2.1 Initial introduction

### Commit `1f6a91e`

**Subject:**
`feat: add TLS pre-warming and eliminate HTTPClient to reduce heap fragmentation`

**What it did:**
- introduced `preWarmTLS()`
- moved TLS establishment before SD mount
- removed `HTTPClient`
- replaced cloud calls with raw TLS I/O using stack buffers

**Why it was introduced:**
- mbedTLS needed the cleanest possible contiguous heap
- SD mount and subsequent scan activity were fragmenting heap first
- raw TLS I/O was introduced specifically to cut heap churn from the cloud path

This is the first strong branch point toward the “TLS first” design.

## 2.2 First removal

### Commit `6e51c6f`

**Subject:**
`feat: replace static TLS pre-warming with on-demand connection and add dynamic Web UI mode explanations`

**Why it was removed:**
- save time and heap when there is no cloud work
- connect cloud TLS only after pre-flight confirms actual work
- commit message explicitly states the assumption that asymmetric mbedTLS buffers
  would fit at post-SD-mount heap levels

This is important because it proves the branch already tried the exact user goal:
**don’t establish TLS unless there is actual work**.

## 2.3 Re-introduction after real failures

### Commit `faa6c86`

**Subject:**
`feat: implement dual-backend phased upload with TLS pre-warming and heap-optimized buffer management`

This reintroduced TLS pre-warm in the phased orchestrator period.

### Commit `51d756d`

**Subject:**
`Restore TLS pre-warm before SD mount to prevent handshake failures`

**Why it was restored:**
- pre-flight scanning still fragmented heap badly enough to hurt TLS reliability
- commit message explicitly says pre-flight scanning dropped `ma` from about
  `69KB` to `55KB`
- commit message explicitly says the runtime was back to symmetric `16KB + 16KB`
  TLS buffers

This is the clearest confirmation that TLS pre-warm was not cosmetic. It was a
response to real post-mount handshake failures.

## 2.4 Release-note confirmation

### `release/RELEASE_NOTES_v2.0i-alpha1.md`

This release note explicitly says:
- TLS handshake needs about `36KB` contiguous heap
- heap is cleanest before SD mount (`~98KB max_alloc` noted there)
- SD mount + pre-flight had fragmented heap to `~38–55KB`
- PCNT re-check was added so the CPAP could resume activity safely during the
  TLS handshake window

### `docs/specs/tls-prewarm-pcnt-recheck.md`

This spec repeats the same rationale and makes it explicit that the current
lifecycle is:

```text
TLS pre-warm → PCNT re-check → SD mount → pre-flight → upload
```

## 2.5 Additional heap history around SMB

### `release/RELEASE_NOTES_v2.0i-alpha3.md`

This note confirms another important heap lesson:
- stale TLS buffers could remain allocated even when the pre-warmed socket died
- this reduced `ma` into the SMB phase
- unconditional `resetConnection()` before SMB was added to force TLS cleanup

That does not weaken the case for the TLS arena. It strengthens it: the current
stack has already observed that mbedTLS buffer lifetime is central to heap
health.

---

## 3. Current Code Reality on This Branch

## 3.1 Current upload lifecycle

Current flow in `main.cpp` is still:

```text
LISTENING
  → ACQUIRING
  → UPLOADING
      → create upload task on heap
      → TLS pre-warm
      → PCNT re-check
      → SD mount
      → pre-flight work detection
      → cloud / smb phases
```

So the current no-work case still pays for:
- heap-backed upload task creation
- TLS pre-warm attempt
- SD mount
- pre-flight scan

before it can conclude there is nothing to do.

## 3.2 Upload task is still heap-backed

Current code still uses `xTaskCreatePinnedToCore()` with a `12288` byte stack.
That stack is still allocated from the heap.

This matters because the observed failure pattern is:
- `ma = 45044` before task creation
- `ma = 36852` at TLS pre-warm entry in failed cycles

That is exactly the kind of drop a heap-backed 12KB task stack can cause when it
lands inside the largest contiguous region.

## 3.3 Current pre-flight is still not “minimal”

The fast path in `FileUploader.cpp` still does meaningful heap churn:
- `preflightFolderHasWork()` iterates `/DATALOG`
- for incomplete, pending, and recent completed folders it calls
  `scanFolderFiles()`
- `scanFolderFiles()` returns `std::vector<String>`
- `hasFileChanged()` checks run on completed recent files

So simply moving the current pre-flight earlier is **not** equivalent to a
minimal work probe.

## 3.4 Current cloud path already removed older big offenders

Some older recommendations are already done in current code:
- raw TLS is already used instead of `HTTPClient`
- multipart upload is already streamed
- file upload chunk size is capped at `4096` max and shrinks adaptively to `2048`
  or `1024` at lower `ma`
- request/response parsing uses stack buffers heavily

This means the remaining heap problem is now much more concentrated around:
- TLS I/O buffers
- task creation timing
- SD mount timing
- pre-flight churn

## 3.5 Current power defaults are already moderate

Verified in current code:
- CPU default: `80 MHz`
- WiFi TX power default: `POWER_MID` (`5 dBm`)
- WiFi power saving default: `SAVE_MID` (`MIN_MODEM`)
- 802.11b disabled in `WiFiManager`
- mDNS time-limited
- PCNT suspended only in low-power states

So the biggest remaining power waste is **architectural churn**, not one missing
WiFi setting.

## 3.6 Cooldown / no-work suppression is more complex than earlier docs implied

`main.cpp` suspends `TrafficMonitor` when entering low-power states like
`COOLDOWN`, which means a strict “wait for new host activity before re-acquire”
strategy is not a trivial one-line addition.

This does not make the idea bad. It means it belongs in a deliberate second
phase, not as the very first patch.

## 3.7 A previous cleanup suggestion is now outdated

`platformio.ini` already contains:
- `espressif/fb_gfx`
- `espressif/esp-zigbee-lib`
- `espressif/esp_diagnostics`

in `custom_component_remove`.

So older suggestions like “add `fb_gfx` to remove list” are stale.

Also, the current `firmware.map` still shows some framework archives like
`esp_diagnostics` loaded, but the observed objects shown there have zero `.bss`
and zero `.data` in the excerpted sections. That makes them weak heap targets.

---

## 4. Validation of Docs 44 / 45 / 46 / 47

## 4.1 `44-HEAP-RECHECK-CO46.md`

### What `44` got right

- task stack fragmentation is a real immediate trigger
- TLS pre-warm history matters
- stale WiFi sleep options matter
- repeated no-work cycles are bad for both stability and power

### What `44` ranked too highly or left incomplete

- it leaned too hard toward “go back to SD-first, TLS-on-demand” before solving
  the deeper allocation problem
- it treated the TLS arena as aspirational instead of as the likely permanent fix
- it still had stale component-removal suggestions that are outdated on the
  current branch

**Verdict:** strong first diagnosis, incomplete final ranking.

## 4.2 `45-HEAP-RECHECK-C54X.md`

### What `45` got right

- minimal work probe is the right architectural direction
- no-work suppression is a strong power/stability concept
- stale WiFi sleep options should be removed
- reusing `WiFiClientSecure` is sensible hygiene

### What `45` got wrong

- it ranked the custom TLS arena too low
- it treated the arena as too invasive for the current stack
- it kept the blanket ABI-risk warning on mbedTLS buffer-size options without
  re-checking the current build artifacts

**Verdict:** excellent on staged behavior, too conservative on allocator-level fixes.

## 4.3 `46-HEAP-RECHECK-G31.md`

### What `46` got right

- the highest-benefit fix really is allocator-level
- unused components are largely a red herring for runtime heap
- the TLS arena deserves top priority

### What `46` got wrong or overstated

- “simple 2-slot allocator” is too simplified as an implementation description
- “Dark Listening” conflicts with the explicit Web UI accessibility requirement

**Verdict:** right philosophy, needs refinement on implementation details and power mode.

## 4.4 `47-HEAP-RECHECK-CO46.md`

### What `47` got right

- best overall synthesis so far
- correctly elevated static task stack
- correctly corrected `45` on the value of the TLS arena
- correctly rejected “Dark Listening” for this product requirement
- correctly identified additional promising mbedTLS settings:
  - asymmetric content length
  - variable buffer length
  - keep-peer-certificate

### What `47` should be corrected on

`47` leaned a bit too far from the old “forbidden” warning to the other extreme.
The safer final wording is:

- these mbedTLS struct-affecting settings are **not automatically forbidden** on
  the current build
- but they are still **build-and-smoke-test candidates**, not unconditional
  guarantees

The current evidence makes them plausible and worth testing, but not yet proven
safe until rebuilt and smoke-tested.

**Verdict:** closest to correct overall, but should soften certainty on the mbedTLS
Kconfig candidates.

---

## 5. Reconstructed Heap Model

## 5.1 The current observed chain

The most important reconstructed chain remains:

```text
Session with real cloud work
  → TLS buffers allocate and later free from normal heap
  → heap fragmentation floor settles around ma ≈ 45044

Next cycle
  → upload task stack allocates from heap
  → sometimes lands safely, ma stays ≈ 45044
  → sometimes splits largest block, ma drops to ≈ 36852
  → SD mount then fails
```

This is why the issue is not solved by saying only:
- “do TLS earlier”
- or “do SD earlier”

The heap has already been structurally damaged by prior TLS buffer placement.

## 5.2 Why mount-first alone is not enough

The user’s objection is correct:
- to know whether cloud work exists, we must inspect SD state
- SD mount itself fragments heap
- that was the whole reason TLS pre-warm was introduced

Therefore, if the final design goal is:

```text
Only allocate TLS when actual work exists
```

then the robust solution is:

```text
Make TLS no longer depend on the general-purpose heap layout
```

That is exactly what the TLS arena does.

## 5.3 Why static task stack matters independently

Even if TLS pre-warm remained in place temporarily, static task stack would still:
- remove one major source of largest-block splits
- stabilize `ma` before both TLS and SD mount
- improve determinism immediately

So static task stack is not just an optimization. It is a direct attack on the
observed trigger.

---

## 6. Rechecked Option Matrix

## 6.1 Highest-value structural fixes

### Option A — Custom TLS arena / custom mbedTLS allocator

**Verdict:** highest-value permanent fix.

**Why:**
- directly solves the TLS fragmentation root cause
- makes mount order largely irrelevant for cloud TLS
- lets the final architecture become “probe first, TLS only if needed” without
  gambling on post-mount contiguous heap

**Refined implementation model:**
- use `mbedtls_platform_set_calloc_free()`
- allocate a small static arena early
- route large mbedTLS allocations into fixed slots or a tiny arena allocator
- pass smaller allocations through to normal mbedTLS heap allocator

**Important correction to `46`:**
- concept is right
- implementation should be described as a threshold-based allocator, not merely
  “two allocations happen”

### Option B — Static upload task stack

**Verdict:** do this early.

**Why:**
- directly removes the immediate `ma` collapse trigger seen in the log
- low risk
- no behavior change
- pairs extremely well with either current pre-warm or future arena-based design

### Option C — Minimal work probe before expensive upload path

**Verdict:** do this, but only after or alongside better heap stabilization.

**Why:**
- removes TLS cost from the no-work path
- removes upload-task cost from the no-work path if designed correctly
- biggest power win while keeping WiFi/Web UI alive

**Important constraint:**
The current pre-flight is not yet minimal enough. It needs its own dedicated
lightweight path, not a mere relocation of the existing scan.

---

## 6.2 High-value configuration candidates

### Option D — Remove stale WiFi sleep IRAM settings

Currently still active in:
- `platformio.ini`
- `sdkconfig.project`
- generated `sdkconfig.defaults`
- generated package `sdkconfig.h`

**Verdict:** do this early.

**Why:**
- already known historically to recover `~2–4KB` DRAM
- current branch still carries them despite `d8dbf82`
- this is a real inconsistency, not just a doc mismatch

### Option E — Reuse `WiFiClientSecure` wrapper instead of delete/new churn

Current `resetTLS()` still:
- stops connection
- deletes client wrapper
- recreates wrapper

**Verdict:** worthwhile hygiene, but not the main fix.

**Why:**
- reduces micro-holes
- does not solve the 16KB + 16KB buffer problem
- still worth doing once implementation starts

### Option F — Asymmetric mbedTLS buffers (`OUT = 4096`)

**Verdict:** promoted from “forbidden” to “testable high-value candidate.”

**Why it is attractive:**
- request headers are tiny
- multipart upload preamble is tiny
- actual streamed upload buffer max is already `4096`
- generic TLS raw write path in `ssl_client.cpp` also chunks writes at `4096`
- reducing OUT buffer from `16384` to `4096` saves about `12KB`

**Why it is no longer reasonable to call this automatically forbidden:**
- current build compiles `NetworkClientSecure` locally into `.pio/build`
- its dependency file shows it compiling against the generated package
  `sdkconfig.h`
- that `sdkconfig.h` already reflects project custom settings

**Final ranking:**
- do **not** treat this as already proven
- do treat it as a serious candidate for a dedicated build + smoke test

### Option G — Disable `CONFIG_MBEDTLS_SSL_KEEP_PEER_CERTIFICATE`

**Verdict:** similar to Option F, but smaller benefit.

**Benefit:**
- likely saves roughly `1–2KB` persistent heap per connection

**Ranking:**
- worth testing
- secondary to arena and static stack
- easier to justify than many low-value cleanup ideas

### Option H — Enable `CONFIG_MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH`

**Verdict:** promising, but secondary.

**Why secondary:**
- it helps after the handshake
- it does **not** remove the need to survive the initial handshake allocation
- the current core pain is initial allocation order and residual fragmentation

So this is useful, but not as central as the arena or asymmetric buffers.

---

## 6.3 Medium-value behavioral improvements

### Option I — No-work suppression until new host activity

**Verdict:** strong second-phase improvement.

**Why:**
- reduces pointless reacquisition cycles
- major power win
- reduces cumulative heap churn

**Constraint:**
Current cooldown suspends PCNT, so new host activity would be missed unless the
state model changes.

This remains a good idea, but it is not the first patch.

### Option J — Keep Web UI alive, but run “lazy network” behavior

**Verdict:** recommended power approach.

Instead of turning WiFi off:
- keep WiFi on in modem-sleep
- keep Web UI accessible
- let there be small wake delays if needed
- cut power by eliminating pointless TLS/SD work, not by making the device dark

This satisfies the user’s explicit product constraint.

---

## 6.4 Low-value or misleading options

### Option K — Component stripping for Zigbee / similar extras

**Verdict:** low value for heap.

The user’s suspicion was reasonable, but the main result remains:
- many of these are stripped or contribute little/no heap
- the current map evidence does not support this as a primary runtime heap win

### Option L — Further lwIP pbuf pool reduction

**Verdict:** secondary experimental knob.

It may help, but it is not the main answer to the current failure. It should not
be ranked above arena, static stack, or minimal probe.

### Option M — “Dark Listening” / WiFi-off-by-default

**Verdict:** reject for this product requirement.

Reason:
- Web UI must remain accessible
- this directly conflicts with the user requirement

---

## 7. Corrected Ranking

## Do first

1. **Custom TLS arena design and implementation**
2. **Static upload task stack**
3. **Remove stale WiFi sleep IRAM settings**
4. **Design true minimal work probe**

## Test early in parallel or just before implementation

5. **Asymmetric mbedTLS buffers (`OUT=4096`)**
6. **Disable KEEP_PEER_CERTIFICATE**
7. **Consider VARIABLE_BUFFER_LENGTH**

These should now be treated as:
- likely viable in the current build
- deserving real build verification
- not blanket-forbidden

## Do next

8. **Probe-first staged acquisition flow**
9. **Keep `WiFiClientSecure` allocated for process lifetime**
10. **No-work suppression after architecture is stable**

## Leave for later

11. Further lwIP reductions
12. Framework/component cleanup hunts
13. More aggressive power profiles that compromise always-on convenience

---

## 8. Preferred Final Architecture

The best architecture that satisfies all constraints is:

```text
LISTENING (WiFi + Web UI alive in low-power mode)
  → idle confirmed
  → mount SD
  → minimal work probe
      → no work:
           release SD
           cooldown / listening
      → work exists:
           create static upload task if needed
           allocate cloud TLS on demand
           upload
           release SD
```

But the key detail is this:

```text
Cloud TLS allocation should come from a fixed reusable TLS arena,
not from the general heap.
```

That is what makes the user’s preferred design actually robust.

Without the arena, “only connect TLS when needed” remains vulnerable to the same
historical failure that caused pre-warm to be introduced in the first place.

---

## 9. Proposed Implementation Plan

## Phase 1 — Stabilize allocation behavior first

1. replace heap-backed upload task with static task
2. remove stale WiFi sleep IRAM/min-active-time settings
3. stop delete/new churn for `WiFiClientSecure`
4. add or retain heap logs around:
   - before task create
   - after task create
   - before TLS connect
   - after TLS connect
   - before SD mount
   - after SD mount

**Expected result:** immediate reduction in SD mount failure probability, even
before deeper structural change.

## Phase 2 — Implement the root-cause fix

1. implement custom TLS allocator / TLS arena
2. allocate arena early at boot
3. move large mbedTLS allocations into that arena
4. verify repeated connect/disconnect cycles no longer reduce `ma`

**Expected result:** TLS no longer fragments the system heap.

## Phase 3 — Reclaim the desired behavior

1. implement true minimal work probe
2. move cloud TLS allocation behind confirmed work
3. remove unconditional TLS pre-warm
4. keep the Web UI alive the whole time

**Expected result:** user-preferred behavior becomes reliable instead of risky.

## Phase 4 — Reduce pointless cycles and power burn

1. redesign cooldown/listening interaction so new host activity can gate reacquire
2. keep WiFi/Web UI accessible in low-power mode
3. only do expensive SD/TLS work when there is evidence it matters

---

## 10. What I Would Not Recommend

1. **Do not** rely only on “mount first, then TLS if heap permits” as the final fix.
   That is exactly the design that historically failed and led to pre-warm.

2. **Do not** make component stripping the primary workstream.
   It is too weak compared with allocator-level fixes.

3. **Do not** use a WiFi-off / Web-UI-dark power strategy for this firmware.
   It conflicts with the product requirement.

4. **Do not** keep treating asymmetric / keep-peer / variable-buffer mbedTLS
   settings as sacred no-go territory without re-testing them on the current build.
   The current build artifacts no longer support that blanket assumption.

---

## 11. Final Conclusion

The right final answer is not:
- “keep unconditional TLS pre-warm forever”
- nor
- “blindly revert to mount-first TLS-on-demand and hope”

The right answer is:

1. **stabilize the heap immediately** with static task stack and removal of stale
   WiFi sleep IRAM settings
2. **fix the root cause** with a reusable TLS arena
3. **then move to the desired behavior**: only establish TLS when actual work is
   confirmed
4. **preserve power and Web UI access** by reducing pointless cycles, not by
   turning the interface off

That gives the best balance of:
- heap stability
- reliability
- user-preferred on-demand TLS behavior
- power savings
- Web UI accessibility

**Recommended next implementation order:**
1. static task stack
2. remove stale WiFi sleep IRAM settings
3. implement TLS arena
4. implement minimal work probe and remove unconditional TLS pre-warm
5. then evaluate asymmetric / keep-peer / variable-buffer Kconfig candidates in
   a dedicated test branch if still needed or if they can shrink the arena size

