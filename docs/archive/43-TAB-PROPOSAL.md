# Tab / Browser Contention Proposal

This document proposes a replacement for the current multi-tab / multi-browser detection design.

**No code changes are included here.** This is a design-only proposal.

---

## Goals

- keep detection client-first
- remove SSE as a duplicate-client detector
- keep API route semantics stable
- detect duplicates as soon as the page loads
- use fixed-size, allocation-free server support only where necessary
- keep BroadcastChannel for same-browser tabs

---

## Keep

### BroadcastChannel

Keep BroadcastChannel as the first detection mechanism.

Why:

- instant
- zero server cost
- works before network round-trips
- ideal for same-browser duplicate tabs

---

## Remove

### SSE-based duplicate detection

Remove the current use of:

- `sse_seq` in `/api/status`
- SSE rapid-disconnect heuristic

Reason:

- duplicate detection should not depend on whether Logs/SSE is open
- SSE is a transport, not a policy signal
- it creates racey ownership behavior and mixes concerns

Recommendation:

- keep SSE only for live log transport
- do not use SSE for detecting duplicates

---

## New Model

Split the design into 3 parts:

1. **Stable endpoints**
2. **Recent tab-ID tracking**
3. **Client-side policy**

This is simpler than the current layered model.

---

## 1. Stable Endpoint Semantics

A route should always return the same kind of data regardless of upload state.

### Recommended routes

- `GET /api/logs/download-all`
  - all persisted files `syslog.0..3` + circular buffer
  - attachment download

- `GET /api/logs/file0`
  - only `syslog.0.txt`

- `GET /api/logs/buffer`
  - circular buffer only

- `GET /api/logs/recent`
  - `syslog.0.txt` + circular buffer
  - recommended for Logs tab backfill

- `GET /api/logs/poll`
  - lightweight polling route for live fallback

- `GET /api/logs/stream?tid=5A6B`
  - SSE live stream only

### Recommendation

I recommend adding `/api/logs/recent`.

It is worth having because:

- it matches the most common Logs-tab need
- it avoids two client requests
- it keeps semantics explicit

---

## 2. Recent Tab-ID Tracking

### Tab ID

On page load, JS generates a **4-character hexadecimal tab ID**.

Examples:

- `5A6B`
- `0F21`
- `C9D0`

### How to send it

Accept both:

- query param: `tid=5A6B`
- header: `X-Tab-Id: 5A6B`

But use **query param everywhere** in the client.

Reason:

- `EventSource` cannot set custom headers normally
- query params work for both fetch and SSE

Examples:

- `/api/status?tid=5A6B`
- `/api/logs/recent?tid=5A6B`
- `/api/logs/stream?tid=5A6B`

### What the server stores

A tiny fixed-size ring of recent sightings.

Each entry:

- `uint16_t tabId`
- `uint32_t seenAtMs`

Suggested size:

- **12 entries**

This stays very small and bounded.

### What gets recorded

In selected handlers, if `tid` is valid:

1. uppercase it
2. keep first 4 hex chars
3. parse to `uint16_t`
4. append `(id, millis())` to the ring

If invalid or missing, ignore it.

### Which requests should record IDs

At minimum:

- `/api/status`

Optionally also:

- `/api/logs/*`

`/api/status` alone is enough for early detection because it starts on page load.

### What `/api/status` should expose

Expose recent sightings compactly.

Example:

```json
{"state":"LISTENING","recent_tabs":"5A6B:0,5A6B:2,1F2C:3"}
```

Meaning: `tabId:secondsAgo`.

This keeps the server simple:

- fixed memory
- no heap
- no policy logic
- compact JSON

---

## 3. Client-Side Policy

The client should decide locally:

- whether to request historical logs
- whether to request SSE
- how often to poll `/api/status`

If a request should not happen, JS should simply not send it.

### Detection inputs

Use:

1. **BroadcastChannel** for same-browser duplicates
2. **recent tab IDs from `/api/status`** for cross-browser / cross-device duplicates

### Duplicate rule

On each `/api/status` response:

1. parse `recent_tabs`
2. ignore entries older than **N seconds**
3. count unique IDs in that window
4. if more than one unique ID exists, mark duplicate active

### Recommended window

- **10 to 12 seconds**

This detects active competing clients without keeping stale duplicates alive too long.

---

## Recommended Client States

Use 3 states instead of the current naming/model.

### Solo
Only this tab is active.

Allowed:

- normal status polling
- backfill
- polling logs
- SSE
- download-all

### Duplicate Owner
Another active client exists, but this tab already owns SSE.

Allowed:

- keep current SSE alive
- keep basic status polling at slower rate

Blocked:

- heavy historical log pulls
- new backfill requests

### Duplicate Non-Owner
Another active client exists, and this tab does not own SSE.

Allowed:

- banner
- occasional status polling

Blocked:

- SSE request
- backfill
- heavy log requests
- optionally download-all button

---

## SSE Ownership

The desired behavior is:

- if a tab already has SSE and it remains healthy, let it continue
- prevent other tabs from stealing it

This cannot be guaranteed by client-only logic because two tabs can race.

So:

- duplicate **detection** should be client-first
- SSE **ownership** should use a tiny server-side rule

That is not server-side throttling. It is transport ownership.

### Option A — client-only discipline

Tabs try not to request SSE when duplicate is active.

Pros:

- simple

Cons:

- races at startup
- second tab can still steal SSE

**Not recommended.**

### Option B — single-owner SSE

Grant SSE only if no active owner exists.
Reject other IDs while the owner is healthy.

Pros:

- matches desired behavior
- easy to understand

Cons:

- needs a little server state

### Option C — single-owner SSE with lease

Track:

- `ownerTid`
- `ownerLastSeenMs`
- active SSE client/socket
- `sseActive`

Grant SSE if:

- no owner exists, or
- owner disconnected, or
- lease expired, or
- requester has the same `tid` as owner

Otherwise reject.

**Recommended option: C.**

### Denied SSE response

Return a tiny non-stream response, e.g.:

- `409 Conflict`

with body:

```text
SSE busy
```

This is better than replacing the owner.

---

## Recommended Client Behavior

### At page load

1. generate `tid`
2. start BroadcastChannel immediately
3. call `/api/status?tid=XXXX`
4. inspect `recent_tabs`
5. decide duplicate state

This gives:

- instant same-browser detection
- early cross-browser detection before Logs is opened

### When duplicate becomes active

If this tab already owns SSE and SSE is healthy:

- keep SSE
- stop heavy log requests
- slow status polling
- show banner

If this tab does not own SSE:

- do not request SSE
- do not request backfill/recent-history routes
- optionally disable download-all
- slow status polling
- show banner

### When duplicate clears

- hide banner
- restore normal status polling
- allow backfill again
- allow SSE again if user is on Logs

---

## Polling Rate Recommendation

Recommended:

- **normal:** 3 seconds
- **duplicate:** 15 seconds

This still strongly discourages duplicates while keeping recovery practical.

BroadcastChannel still gives near-instant recovery for same-browser tabs.

---

## Why This Design Is Better

- detection no longer depends on Logs/SSE
- SSE goes back to being transport only
- route semantics become explicit and stable
- client-side suppression stays primary
- server-side support stays tiny and bounded
- SSE stealing is prevented cleanly

---

## Recommended Next Step

If accepted, implementation should be split into 3 small tasks:

1. **Endpoint cleanup**
   - add/rename explicit log routes
   - remove conditional payload behavior from shared routes

2. **Recent tab tracking**
   - parse `tid`
   - add fixed recent-ID ring
   - expose compact recent-ID field via `/api/status`

3. **Client policy + SSE ownership**
   - generate `tid` at page load
   - move duplicate detection to BroadcastChannel + `/api/status`
   - remove SSE-sequence / rapid-disconnect detection
   - add non-stealable SSE owner rule

---

## Final Recommendation

I recommend this exact direction:

- **Keep BroadcastChannel**
- **Remove SSE-based duplicate detection**
- **Add explicit tab IDs to requests**
- **Expose recent tab sightings via `/api/status`**
- **Move duplicate policy into client JS**
- **Use a tiny server-side single-owner SSE lease so the current SSE owner keeps working and other tabs cannot steal it**
