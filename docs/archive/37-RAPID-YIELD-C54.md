# Rapid Yield PR Review (`amanuense/CPAP_data_uploader` PR #45)

## Summary

PR #45 proposes a rapid SD-bus yield mechanism for cases where the CPAP attempts to access the SD card while the ESP32 currently owns the bus for upload.

The basic idea is:

- detect a CPAP SD access attempt on `GPIO33` / `CS_SENSE`
- set a global yield flag from an ISR
- have upload loops notice that flag quickly
- return a `YIELD_NEEDED` result
- let the FSM release the SD card and retry later from saved state

This is intended to prevent CPAP-side errors such as `SD card not detected` during long upload windows.

## What the PR Changes

The PR adds:

- a `FALLING`-edge ISR on `CS_SENSE`
- a global `g_cpapYieldRequest` flag
- a new `UploadResult::YIELD_NEEDED`
- checks for the yield flag in:
  - SMB upload chunk loop
  - some `FileUploader` folder loops
- FSM handling to:
  - release the bus
  - enter cooldown
  - retry later

## Intended Behavior

The proposed flow is:

```text
CPAP asserts CS on GPIO33
-> ISR sets yield flag
-> active upload loop notices flag
-> returns YIELD_NEEDED
-> FSM releases SD bus
-> cooldown
-> next session resumes from saved upload state
```

The PR claims this can happen fast enough to satisfy the CPAP's SD timeout expectations.

## What Problem It Is Trying to Solve

This PR is not about heap, TLS, or RAM optimization.

It is trying to solve a different issue:

- the ESP acquires exclusive SD ownership
- the CPAP wakes up and tries to write to the card during that window
- because the mux points to the ESP, the CPAP is blocked
- the CPAP may then report SD access errors

So the PR's goal is to make the uploader more polite by surrendering the card mid-session when the CPAP requests it.

## Does the Idea Make Sense?

At a high level, yes.

The underlying product idea is reasonable:

- only upload when the bus appears idle
- if the CPAP becomes active again, release the card as soon as safely possible
- retry later from persisted state

That aligns with the broader design goal of minimizing how long the ESP blocks the CPAP.

## Why This Specific PR Does Not Map Well to the Current Codebase

### 1. It Targets an Older Architecture

The current branch is significantly different.

Current architecture already has:

- `TrafficMonitor` using PCNT on `CS_SENSE`
- explicit idle/silence detection before SD takeover
- an upload task that owns the full lifecycle:
  - SD mount
  - pre-flight
  - cloud
  - SMB
  - release

PR #45 was written against an older control flow and does not fully match today's responsibilities.

### 2. It Does Not Fully Cover the Current Long-Running Paths

This is the biggest technical problem.

The PR checks:

- SMB chunk upload loop
- some folder-level loops in `FileUploader`

But current code spends significant time in:

- SleepHQ streaming upload
- pre-flight scans
- state reconciliation
- cloud session management

In particular, the current `SleepHQUploader::httpMultipartUpload()` has its own streaming loop with:

- SD reads
- TLS writes
- retries
- socket backpressure waits

So the PR's practical claim of `yield within one chunk` is not credible for the current code unless equivalent logic is threaded through the current cloud uploader path too.

### 3. It Does Not Make Pre-Flight Scanning Truly Preemptible

Current SD hold time is not just file transfer.

The code also spends time in:

- folder scans
- file scans
- pending/completed state checks

The older PR does not robustly make these phases yield-aware.

### 4. The ISR Policy Is Very Aggressive

The PR triggers on a single `FALLING` edge on `CS_SENSE`.

Current design is more conservative:

- PCNT counts activity in hardware
- the FSM waits for a sustained idle window before takeover

That is more robust than reacting to one edge immediately.

A raw edge-triggered yield mechanism risks:

- false positives
- thrashing between upload and cooldown
- yielding on noise or very short transients

### 5. Resume-from-Checkpoint Is Only Partially True

The PR assumes yielding is naturally resumable because upload state is persisted.

That is only partly true.

The current state managers do help with retrying completed work, but mid-session yield is trickier when combined with:

- active cloud import/session state
- multipart HTTP uploads in progress
- open SMB remote files
- recent-folder rescans
- partially completed per-file operations

Those paths are currently designed primarily around:

- clean success
- retry on failure/timeout
- whole-session boundaries

That is not automatically the same as safe low-latency preemption.

## Relevance to the Current Heap Investigation

Very low.

This PR does not address:

- TLS contiguous heap pressure
- mbedTLS memory requirements
- fragmentation
- the `ma >= 38 KB` target

If anything, porting this now would add more moving parts and more failure modes without helping the RAM objective.

## Recommendation

### Direct Port of PR #45

Not recommended.

Reasons:

- incomplete coverage for the current architecture
- especially weak coverage of the current cloud upload path
- high risk of edge-case bugs and bus-yield thrashing
- unrelated to the present heap/TLS optimization goal

### Borrow the Idea Later?

Maybe, but only as a fresh design, not as a patch-port.

If this behavior is ever pursued in the current branch, it should be implemented as a first-class feature with:

- a better yield trigger policy
- full lifecycle coverage
- safe unwind points
- explicit retry/backoff behavior
- detailed telemetry/logging

## Bottom Line

- the intent of the PR is reasonable
- the patch itself is not a good fit for the current branch
- it should not be ported directly
- it is not relevant to the current heap/TLS optimization work except as a separate future design idea
