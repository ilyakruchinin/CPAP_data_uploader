#include "TlsArena.h"
#include "Logger.h"

#include <string.h>
#include <stdlib.h>
#include <esp_heap_caps.h>
#include <mbedtls/platform.h>

// ============================================================================
// Arena Configuration
// ============================================================================
//
// mbedTLS allocates two large buffers during handshake:
//   Slot A: IN buffer  — 16384 bytes (+ mbedTLS header/padding overhead)
//   Slot B: OUT buffer — 4096 bytes with CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN=y
//                        (or 16384 if asymmetric is disabled)
//
// We size each slot at 17KB to comfortably fit the largest possible allocation
// including mbedTLS internal overhead (content_len + header + padding).
// With asymmetric buffers enabled, slot B only uses ~5KB of its 17KB capacity —
// the unused portion sits idle in .bss (zero runtime cost, just static memory).
// Total arena: 34KB in .bss — same memory that would have been on the heap,
// but now guaranteed contiguous and reusable without fragmentation.
//
// The threshold of 4KB catches the OUT buffer even with asymmetric config
// (4096 + overhead ≈ 4400 bytes).

static const size_t ARENA_SLOT_SIZE  = 17 * 1024;  // 17KB per slot
static const size_t ARENA_NUM_SLOTS  = 2;
static const size_t ARENA_THRESHOLD  = 4096;        // Allocations >= 4KB go to arena

// The arena itself — statically allocated in .bss, never touches the heap
static uint8_t arenaBuffer[ARENA_NUM_SLOTS][ARENA_SLOT_SIZE] __attribute__((aligned(4)));
static bool    arenaSlotInUse[ARENA_NUM_SLOTS] = {false, false};
static size_t  arenaSlotAllocSize[ARENA_NUM_SLOTS] = {0, 0};  // Actual requested size (for debug)

// ============================================================================
// Arena Allocator
// ============================================================================

static void* arena_calloc(size_t n, size_t size) {
    size_t total = n * size;

    // Small allocations: pass through to normal heap (DRAM, same as default mbedTLS allocator)
    if (total < ARENA_THRESHOLD) {
        return heap_caps_calloc(n, size, MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL);
    }

    // Large allocation: serve from arena slot
    if (total > ARENA_SLOT_SIZE) {
        LOG_ERRORF("[TlsArena] Allocation too large for arena: %u bytes (slot=%u)",
                   (unsigned)total, (unsigned)ARENA_SLOT_SIZE);
        // Fall back to system allocator — better than failing
        return heap_caps_calloc(n, size, MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL);
    }

    for (size_t i = 0; i < ARENA_NUM_SLOTS; i++) {
        if (!arenaSlotInUse[i]) {
            arenaSlotInUse[i] = true;
            arenaSlotAllocSize[i] = total;
            memset(arenaBuffer[i], 0, total);  // calloc semantics: zero-filled
            LOG_DEBUGF("[TlsArena] Slot %u allocated: %u bytes", (unsigned)i, (unsigned)total);
            return arenaBuffer[i];
        }
    }

    // All slots occupied — fall back to system allocator
    LOG_WARNF("[TlsArena] All %u slots in use, falling back to heap for %u bytes",
              (unsigned)ARENA_NUM_SLOTS, (unsigned)total);
    return heap_caps_calloc(n, size, MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL);
}

static void arena_free(void* ptr) {
    if (ptr == nullptr) return;

    // Check if pointer belongs to an arena slot
    for (size_t i = 0; i < ARENA_NUM_SLOTS; i++) {
        if (ptr == arenaBuffer[i]) {
            if (!arenaSlotInUse[i]) {
                LOG_ERRORF("[TlsArena] Double-free detected on slot %u!", (unsigned)i);
                return;
            }
            LOG_DEBUGF("[TlsArena] Slot %u freed: was %u bytes", (unsigned)i, (unsigned)arenaSlotAllocSize[i]);
            arenaSlotInUse[i] = false;
            arenaSlotAllocSize[i] = 0;
            return;
        }
    }

    // Not an arena pointer — pass through to normal heap free
    free(ptr);
}

// ============================================================================
// Public API
// ============================================================================

void tlsArenaInit() {
    // Install our custom allocator before any TLS activity
    int ret = mbedtls_platform_set_calloc_free(arena_calloc, arena_free);
    if (ret != 0) {
        LOG_ERRORF("[TlsArena] Failed to install arena allocator (ret=%d)", ret);
        return;
    }
    LOGF("[TlsArena] Arena installed: %u slots x %uKB = %uKB in .bss (threshold >= %u bytes)",
         (unsigned)ARENA_NUM_SLOTS, (unsigned)(ARENA_SLOT_SIZE / 1024),
         (unsigned)(ARENA_NUM_SLOTS * ARENA_SLOT_SIZE / 1024),
         (unsigned)ARENA_THRESHOLD);
}

void tlsArenaLogStatus() {
    for (size_t i = 0; i < ARENA_NUM_SLOTS; i++) {
        LOGF("[TlsArena] Slot %u: %s (%u bytes)",
             (unsigned)i,
             arenaSlotInUse[i] ? "IN USE" : "free",
             (unsigned)arenaSlotAllocSize[i]);
    }
}
