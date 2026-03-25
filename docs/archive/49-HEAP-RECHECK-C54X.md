# Heap Fragmentation Re-Check (G31) — Decision Memo After Re-Validation

> **Status**: Analysis + plan — no code changes yet
> **Date**: 2026-03-22
> **Purpose**: Re-check `44-HEAP-RECHECK-CO46.md`, `45-HEAP-RECHECK-C54X.md`,
> `46-HEAP-RECHECK-G31.md`, `47-HEAP-RECHECK-CO46.md`, and
> `48-HEAP-RECHECK-C54X.md` against the current branch, commit history, release
> notes, and current build/code reality. Produce one updated plan for fixing heap
> fragmentation without sacrificing Web UI accessibility.

---

## 1. Executive Summary

### Final verdict

The earlier docs are converging on the same real problem, but they still mix up
three different questions:

1. **What is the immediate trigger of the current SD mount failures?**
2. **What is the root cause that keeps returning?**
3. **What is the best implementation order versus the best permanent end state?**

After re-checking the current branch, the best summary is:

- **TLS pre-warm was introduced for a real reason**.
  It was added because post-`SD_MMC.begin()` and post-pre-flight heap
  fragmentation caused real TLS handshake failures.

- **Your preferred behavior is still the right end state**:
  only establish TLS when there is confirmed cloud work.

- **But that end state is not robust if TLS continues to use the general heap**.
  If TLS still allocates large mbedTLS buffers from the normal heap, the same
  order-of-operations problem can come back.

- **The immediate trigger in the current failure is the heap-backed upload task
  stack**.
  The `12KB` task stack created by `xTaskCreatePinnedToCore()` can split the
  largest free block and drop `ma` from `45044` to `36852`, after which SD mount
  fails.

- **The best permanent fix is still a reusable TLS arena / custom mbedTLS
  allocator**.
  That is the cleanest way to make “mount first, probe first, TLS only if work
  exists” reliable.

- **The best next implementation order is not arena first**.
  The best next order is:
  1. static upload task stack
  2. remove stale WiFi sleep IRAM settings
  3. keep `WiFiClientSecure` wrapper allocated
  4. validate the mbedTLS Kconfig candidates in a dedicated build branch
  5. then implement the TLS arena
  6. then move to true probe-first, on-demand TLS

- **The old blanket ABI warning around asymmetric / variable / keep-peer mbedTLS
  options is now stale enough that it should no longer be treated as doctrine**.
  It should be treated as a **build-and-smoke-test question**, not as a hard
  impossibility.

- **Power savings must come from doing less work, not from making the device
  dark**.
  A WiFi-off / Web-UI-off design still conflicts with the requirement that the
  Web UI remain accessible.

---

## 2. Re-Checked History: Where TLS Pre-Warm Came From and Why

## 2.1 Initial introduction

### Commit `1f6a91e`

**Subject**
`feat: add TLS pre-warming and eliminate HTTPClient to reduce heap fragmentation`

**What changed**
- added `preWarmTLS()`
- moved TLS establishment before SD mount
- removed `HTTPClient` from the cloud path
- replaced it with raw `WiFiClientSecure` I/O and stack-heavy parsing

**Why it was introduced**
- mbedTLS needed the cleanest contiguous heap region
- SD mount and follow-up scan activity were fragmenting heap first
- removing `HTTPClient` was also part of reducing heap churn in the cloud path

This is the first explicit branch point toward the current “TLS first” design.

## 2.2 First removal in favor of on-demand TLS

### Commit `6e51c6f`

**Subject**
`feat: replace static TLS pre-warming with on-demand connection and add dynamic Web UI mode explanations`

**What changed**
- removed TLS pre-warm from the upload task
- moved cloud TLS connection behind work confirmation
- commit message explicitly states the goal of saving time and heap on no-work
  cycles
- commit message also explicitly assumes asymmetric TLS buffers would fit
  post-mount

This proves that the branch already tried your preferred behavior:

```text
Mount / check work first → only connect TLS if cloud work exists
```

## 2.3 Re-introduction after failures

### Commit `faa6c86`

**Subject**
`feat: implement dual-backend phased upload with TLS pre-warming and heap-optimized buffer management`

This reintroduced pre-warm in the phased cloud-first session design.

### Commit `51d756d`

**Subject**
`Restore TLS pre-warm before SD mount to prevent handshake failures`

**Why it was restored**
- pre-flight scanning still fragmented heap too much
- commit message explicitly says `ma` dropped from about `69KB` to `55KB`
- commit message explicitly says runtime was back to symmetric `16KB + 16KB`
  mbedTLS buffers
- handshake succeeded reliably when done before SD mount while `ma ≈ 98KB`

This is the clearest historical confirmation that TLS pre-warm was not arbitrary.
It was a workaround for real post-mount handshake instability.

## 2.4 Release-note and spec confirmation

### `release/RELEASE_NOTES_v2.0i-alpha1.md`

The current release note still documents:
- TLS handshake needs about `36KB` contiguous heap
- heap is cleanest before SD mount
- SD mount + pre-flight reduced heap to roughly `38–55KB`
- PCNT re-check was added so the handshake could happen before card grab without
  risking CPAP interference

### `docs/specs/tls-prewarm-pcnt-recheck.md`

The current design doc still describes the session as:

```text
TLS pre-warm → PCNT re-check → SD mount → pre-flight → upload
```

and explicitly explains that the purpose is to let mbedTLS take the cleanest
contiguous heap before SD DMA allocations fragment it.

## 2.5 Additional release-note evidence from SMB interaction

### `release/RELEASE_NOTES_v2.0i-alpha3.md`

This release note confirms another important heap lesson:
- stale TLS buffers could remain allocated into the SMB phase
- this kept `ma ≈ 36KB`
- unconditional `resetConnection()` before SMB was required

That strengthens the allocator-level case. It shows that the lifetime of mbedTLS
buffers is central to heap stability on this branch.

---

## 3. Current Branch Reality

## 3.1 Current lifecycle still pays the wrong costs in the no-work case

Current `main.cpp` / `FileUploader.cpp` flow is still:

```text
LISTENING
  → ACQUIRING
  → UPLOADING
      → create upload task on heap
      → TLS pre-warm
      → PCNT re-check
      → SD mount
      → pre-flight
      → cloud / smb phases
```

So the no-work case still pays for:
- upload task allocation
- TLS pre-warm attempt
- SD mount
- pre-flight scan

before it can conclude there is nothing to upload.

That is exactly the wrong cost shape for both:
- heap stability
- power draw

## 3.2 The upload task stack is still heap-backed

Current code still uses `xTaskCreatePinnedToCore()` with a `12288` byte stack.
That means the stack is still allocated from the heap.

This remains the clearest immediate trigger for the current failure pattern:

```text
before task create: ma = 45044
bad placement of 12KB stack
after task create / at TLS pre-warm entry: ma = 36852
SD mount fails
```

This is not speculative. It matches the observed failure signature in `44` and is
still consistent with current code.

## 3.3 Current pre-flight is still not a true minimal work probe

The current fast path in `FileUploader.cpp` still does meaningful heap churn:
- `preflightFolderHasWork()` walks `/DATALOG`
- it calls `scanFolderFiles()` for multiple conditions
- `scanFolderFiles()` returns `std::vector<String>`
- repeated `String` construction still happens
- recent-completed-folder checks still reopen and iterate folders

So “move the current pre-flight earlier” is **not** equivalent to “cheap probe.”
That distinction remains critical.

## 3.4 The current cloud path already fixed some older heap offenders

The current cloud code already does several things right:
- raw TLS instead of `HTTPClient`
- streamed multipart upload
- request/response parsing with stack buffers
- cloud upload chunk sizes already capped at `4096` and reduced further when
  heap is tighter

This matters because it narrows the remaining problem.
The biggest remaining heap pressure points are now:
- mbedTLS I/O buffers
- heap-backed task stack
- SD mount timing
- pre-flight churn

## 3.5 `resetTLS()` still deletes and recreates the wrapper

Current `SleepHQUploader::resetTLS()` still does:
- `stop()`
- `delete tlsClient`
- `tlsClient = nullptr`
- `setupTLS()` → `new WiFiClientSecure()`

That is not the root cause, but it is still avoidable wrapper churn that can add
small holes over time.

## 3.6 Stale WiFi sleep IRAM settings are still active

The current repo still carries:
- `CONFIG_ESP_WIFI_SLP_IRAM_OPT=y`
- `CONFIG_ESP_WIFI_SLP_DEFAULT_MIN_ACTIVE_TIME=8`

in:
- `platformio.ini`
- `sdkconfig.project`
- the generated build configuration
- the packaged generated `sdkconfig.h` used by the current build

The release notes also still describe these as already reverted, but the current
repo/build reality shows they are still active here.

So this is not just a documentation mismatch. It is a real config inconsistency.

## 3.7 “Compiled extras” are still low-value targets for runtime heap

The current branch already removes many managed components via
`custom_component_remove`, including:
- `esp-zigbee-lib`
- `fb_gfx`
- `esp_diagnostics`
- many others

Some framework archives still appear in the map, but earlier evidence and current
branch configuration still point to the same conclusion:
- these are not the dominant runtime heap problem
- component-stripping is low-value compared with allocator/session fixes

So Zigbee and similar extras are still **not** the main answer to this issue.

---

## 4. Re-Validation of Docs 44–48

## 4.1 `44-HEAP-RECHECK-CO46.md`

### What `44` got right

- the `ma=36852` failure signature is real
- task-stack placement is a real immediate trigger
- repeated no-work cycles are bad for both heap and power
- TLS pre-warm history matters
- stale WiFi sleep settings matter

### What `44` still gets wrong or leaves incomplete

- it leans too hard toward “go back to mount-first, TLS-on-demand” before fixing
  the allocation problem underneath
- it does not fully separate the no-work fast path from the work-exists path
- it still treats some component cleanup ideas as more valuable than they are

**Verdict:** strong diagnosis, incomplete final ranking.

## 4.2 `45-HEAP-RECHECK-C54X.md`

### What `45` got right

- a minimal work probe is the right architectural direction
- no-work suppression is a strong power/stability idea
- stale WiFi sleep settings should be removed
- reusing `WiFiClientSecure` is sensible hygiene

### What `45` now needs corrected

- it ranked the custom TLS arena too low
- it was too conservative about allocator-level fixes
- its ABI warning around asymmetric / variable / keep-peer mbedTLS settings is
  now stale relative to current build artifacts

**Verdict:** very good on staged behavior, too cautious on allocator-level fixes.

## 4.3 `46-HEAP-RECHECK-G31.md`

### What `46` got right

- the permanent fix is allocator-level, not just order shuffling
- unused components are mostly a red herring for runtime heap
- the TLS arena deserves top priority as an end-state design

### What `46` got wrong or overstated

- “Dark Listening” / WiFi-off power mode conflicts with the Web UI requirement
- the allocator description was too simplified; a threshold-based dispatcher is a
  more accurate implementation model than “just two allocations happen”

**Verdict:** correct philosophy, wrong power model, oversimplified implementation description.

## 4.4 `47-HEAP-RECHECK-CO46.md`

### What `47` got right

- best synthesis among the earlier docs
- static task stack deserves much more emphasis
- the TLS arena should not be left for later as an afterthought
- “Dark Listening” should be rejected for this product requirement
- asymmetric / variable / keep-peer options were important omissions in earlier
  docs

### What `47` still needs softened

`47` moved from the old “ABI forbidden” position almost all the way to “safe.”
The more defensible wording is:

- these options are **not automatically forbidden** on the current build
- but they are still **test candidates**, not proven-safe facts

**Verdict:** strongest predecessor, but slightly overconfident on Kconfig certainty.

## 4.5 `48-HEAP-RECHECK-C54X.md`

### What `48` got right

- best current overall direction
- clearly separates historical rationale from current failure mode
- correctly argues that “mount first, TLS later” is only robust if TLS is no
  longer at the mercy of the normal heap
- correctly keeps Web UI accessibility as a hard constraint
- correctly downgrades component stripping to low priority
- correctly treats the current ABI warning as stale enough to be re-tested

### What `48` I would refine, not reverse

`48` is fundamentally the best base document, but I would refine one thing:

- keep **TLS arena** as the highest-value permanent fix
- but keep **static task stack + stale WiFi sleep setting removal** as the best
  immediate implementation order before the more invasive arena work

That is a sequencing refinement, not a direction change.

**Verdict:** best baseline. `49` is mostly a decision memo that clarifies order and risk.

---

## 5. The Actual Heap Model

## 5.1 Immediate trigger versus root cause

The current situation is easiest to understand as two layers:

### Immediate trigger

```text
heap-backed upload task stack lands badly
→ largest contiguous block shrinks
→ ma drops from 45044 to 36852
→ SD_MMC.begin() fails
```

### Root cause

```text
earlier TLS sessions allocated/freed large mbedTLS buffers from the normal heap
→ heap fragmentation floor settles at an already marginal ma
→ later task creation and SD mount fight over the remaining large block
```

So the root cause is not simply:
- “SD mount fragments heap”
- or “TLS allocates a lot”

It is:
- **TLS keeps damaging the general heap layout over time**, and then
- **heap-backed task creation occasionally pushes the system below the SD mount
  threshold**.

## 5.2 Why your objection to probe-first is valid

Your objection remains exactly right:

- to know whether cloud work exists, we must inspect SD state
- mounting SD already fragments heap
- that was the entire reason TLS pre-warm was introduced historically

That means the end-state you want:

```text
Only allocate TLS when there is actual work
```

is only robust if we first solve:

```text
How do we stop TLS from depending on the normal heap layout?
```

That is why the reusable TLS arena remains the strongest structural fix.

## 5.3 What can be reserved/pre-allocated effectively and reused?

Yes — there are several meaningful things that can be pre-allocated and reused:

### Strong candidates

1. **Upload task stack**
   - move it from heap to static DRAM via `xTaskCreateStaticPinnedToCore()`
   - eliminates a known largest-block splitter

2. **TLS large-buffer region**
   - reserve a fixed arena at boot and use it for large mbedTLS allocations
   - this is the most important reusable reservation strategy

3. **`WiFiClientSecure` wrapper object**
   - allocate once, keep it alive, stop/reset connection state without repeated
     `delete/new`
   - this is a smaller win, but still sensible

### Weak candidates

4. **Optional framework component stripping**
   - low confidence as a runtime heap win
   - mostly build-time / flash-size hygiene, not the main answer here

So the answer to “can we reserve/pre-allocate more effectively/early and reuse?”
is **yes**, and the high-value answers are the static task stack and the TLS arena.

---

## 6. mbedTLS Configuration Re-Check

## 6.1 The old repo warning is stale enough to re-test

The repo still says in `platformio.ini` / `sdkconfig.project` that options such
as:
- `CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN`
- `CONFIG_MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH`
- `CONFIG_MBEDTLS_SSL_KEEP_PEER_CERTIFICATE`

are “FORBIDDEN” due to ABI mismatch with `WiFiClientSecure`.

However, the current build now provides direct evidence against treating that as
an unqualified truth:

- `.pio/build/pico32-ota/lib243/NetworkClientSecure/NetworkClientSecure.cpp.o`
  exists
- `.pio/build/pico32-ota/lib243/NetworkClientSecure/ssl_client.cpp.o` exists
- `ssl_client.cpp.d` shows `ssl_client.cpp` is compiled against the generated
  packaged `sdkconfig.h`
- that `sdkconfig.h` already reflects project-customized build values such as:
  - `CONFIG_ESP_PHY_MAX_WIFI_TX_POWER=10`
  - `CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=4`
  - current mbedTLS cipher choices

So the right conclusion is now:

- **not** “these settings are definitely safe”
- **not** “these settings are definitely forbidden”
- but **“these settings should be validated in a dedicated build/test branch”**

## 6.2 Which mbedTLS options are worth testing?

### `CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN`

**Potential value:** very high

Why it matters:
- current cloud path already streams in `4096`-byte chunks
- the framework TLS write path already chunks writes in `4096`-byte pieces
- reducing the OUT buffer from `16384` to `4096` saves about `12KB`

**Best interpretation:**
- promising
- likely relevant
- should be tested
- not enough by itself to replace the arena as the permanent solution

### `CONFIG_MBEDTLS_SSL_KEEP_PEER_CERTIFICATE=n`

**Potential value:** modest but real

Why it matters:
- saves roughly `1–2KB` per connection
- low conceptual risk because the code does not inspect peer certs after the
  handshake

### `CONFIG_MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH=y`

**Potential value:** moderate

Why it matters:
- can shrink buffers after handshake
- useful for steady-state footprint

Limitation:
- it does not remove the need to survive the initial handshake allocation

## 6.3 How these options fit into the overall plan

These settings are best treated as one of two things:

1. **A cheap validation branch that may recover margin quickly**
2. **A way to shrink the future TLS arena size if the build/tests pass**

They are not a replacement for the arena if the goal is a robust, architecture-
proof solution.

---

## 7. Re-Ranked Options

## 7.1 Best permanent fix

### Option A — Reusable TLS arena / custom mbedTLS allocator

**Verdict:** best permanent structural fix.

Why:
- directly solves the TLS fragmentation root cause
- decouples TLS from the general heap layout
- makes your preferred final behavior robust:

```text
mount SD → cheap work probe → only connect TLS if cloud work exists
```

A more accurate implementation model than earlier docs used is:
- reserve a small static arena early
- route large mbedTLS allocations into it
- route small allocations to the normal allocator

This should be described as a threshold-based dispatcher, not as “mbedTLS only
allocates exactly two things.”

## 7.2 Best immediate low-risk fixes

### Option B — Static upload task stack

**Verdict:** implement first.

Why:
- directly removes the current `ma` collapse trigger
- low risk
- no behavior change
- improves determinism immediately

### Option C — Remove stale WiFi sleep IRAM settings

**Verdict:** implement early.

Why:
- historical evidence already says these cost `~2–4KB`
- current repo/build still carries them
- this is a clear inconsistency worth fixing

### Option D — Keep `WiFiClientSecure` wrapper allocated

**Verdict:** worthwhile hygiene.

Why:
- reduces small wrapper churn
- not the main fix
- cheap enough that it belongs in the first wave, not at the end

## 7.3 Best architectural behavior change

### Option E — True minimal work probe

**Verdict:** required, but only as a true low-churn path.

It should:
- stream directory checks
- avoid `std::vector<String>` on the fast path
- return as soon as one positive proof of work is found
- answer only:
  - any cloud work?
  - any SMB work?

Do **not** just move the current pre-flight earlier and call it done.

### Option F — Probe first, then TLS on demand

**Verdict:** correct end-state behavior.

But this becomes truly robust only when at least one of the following is true:
- TLS arena implemented
- or heap margin significantly improved and validated under repeated cycles

## 7.4 Good but secondary power improvement

### Option G — No-work suppression until new host activity

**Verdict:** strong second-phase change.

Why:
- prevents pointless reacquisition loops
- reduces repeated SD/TLS churn
- biggest power win while keeping WiFi alive

Constraint:
- current cooldown suspends PCNT, so the state model must change if “wait for new
  activity before retry” is implemented

## 7.5 Test candidates, not first assumptions

### Option H — Asymmetric / keep-peer / variable-buffer mbedTLS config

**Verdict:** high-value test branch, not doctrine.

Use these to:
- recover heap margin
- reduce required arena size
- determine whether a lighter fix is enough in practice

But do not assume they alone permanently solve the root problem.

## 7.6 Low-value or misleading directions

### Option I — Component stripping as the main fix

**Verdict:** low value for runtime heap.

### Option J — Further lwIP reductions before allocator/session fixes

**Verdict:** secondary tuning knob only.

### Option K — WiFi-off / Web-UI-dark mode

**Verdict:** reject for this product requirement.

---

## 8. Best Permanent Design vs Best Implementation Order

This is the main clarification that `49` adds.

## 8.1 Best permanent design

```text
LISTENING (WiFi + Web UI accessible, low-power network behavior)
  → idle confirmed
  → mount SD
  → true minimal work probe
      → no work:
           release SD
           cooldown / listening
      → work exists:
           create static upload task if needed
           allocate cloud TLS on demand
           upload
           release SD
```

**Critical condition:**
cloud TLS allocations must be shielded from the normal heap layout via a fixed,
reusable TLS arena or otherwise validated equivalent margin.

## 8.2 Best implementation order

### Phase 1 — Stabilize the current heap first

1. static upload task stack
2. remove stale WiFi sleep IRAM / min-active-time settings
3. stop `WiFiClientSecure` wrapper delete/new churn
4. keep or improve heap logs around:
   - before task create
   - after task create
   - before TLS connect
   - after TLS connect
   - before SD mount
   - after SD mount

### Phase 2 — Validate mbedTLS config candidates in a dedicated branch

1. test `CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN`
2. test `CONFIG_MBEDTLS_SSL_KEEP_PEER_CERTIFICATE=n`
3. optionally test `CONFIG_MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH=y`
4. verify build stability and repeated connect/disconnect behavior

This phase is not the final fix by itself. It is a fast validation branch that
may either:
- recover useful margin quickly, or
- reduce the size needed for the TLS arena

### Phase 3 — Implement the permanent allocator fix

1. implement TLS arena / custom mbedTLS allocator
2. reserve it early at boot
3. route large mbedTLS allocations into it
4. verify repeated sessions no longer degrade `ma`

### Phase 4 — Reclaim the desired user behavior

1. implement a true minimal work probe
2. move cloud TLS behind confirmed work
3. remove unconditional TLS pre-warm
4. keep WiFi/Web UI alive throughout

### Phase 5 — Reduce pointless cycles and power burn

1. redesign cooldown/listening so new activity gates reacquire
2. keep Web UI accessible in low-power mode
3. only do SD/TLS work when there is evidence it matters

---

## 9. What I Would Recommend Doing Next

If the goal is to stop struggling with fragmented heap **and** keep the Web UI
accessible, my recommended next order is:

1. **Static upload task stack**
2. **Remove stale WiFi sleep IRAM settings**
3. **Keep `WiFiClientSecure` allocated for object lifetime**
4. **Create a dedicated branch to validate asymmetric / keep-peer / variable-buffer mbedTLS settings**
5. **Implement TLS arena**
6. **Implement true minimal work probe**
7. **Remove unconditional TLS pre-warm and connect TLS only when work exists**
8. **Add no-work suppression gated by new host activity**

This sequence gives the best balance of:
- quick risk reduction
- structural root-cause fix
- user-preferred on-demand TLS behavior
- power savings
- Web UI accessibility

---

## 10. What I Would Not Recommend

1. **Do not** blindly revert to “mount first, then full current pre-flight, then
   TLS on demand” as the final answer.
   That was historically unstable for a reason.

2. **Do not** treat Zigbee / optional component stripping as the main workstream.
   It is too weak compared with allocator/session fixes.

3. **Do not** treat the old ABI warning as unquestionable truth.
   Current build artifacts no longer support that blanket assumption.

4. **Do not** use a WiFi-off / Web-UI-dark power profile for this firmware.
   It violates the accessibility requirement.

---

## 11. Final Conclusion

The current branch history tells a very consistent story:

- **on-demand TLS after SD mount** was desired
- **it failed under fragmented heap**
- **pre-warm was added as a workaround**
- **that workaround now harms the no-work path and still does not eliminate all
  instability**

So the real answer is not to choose between:
- “always pre-warm TLS”
- or
- “blindly mount first and hope TLS fits later”

The real answer is:

1. **stabilize the heap immediately**
2. **stop TLS from fragmenting the general heap**
3. **then move TLS behind confirmed work**
4. **reduce power by eliminating pointless cycles, not by turning the device dark**

That is the most robust path to:
- more contiguous heap
- reliable SD mount
- reliable cloud TLS
- lower idle waste
- still-accessible Web UI

