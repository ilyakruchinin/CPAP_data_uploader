#ifndef TLS_ARENA_H
#define TLS_ARENA_H

#include <stddef.h>

// ============================================================================
// TLS Arena — Custom mbedTLS allocator to prevent heap fragmentation
// ============================================================================
//
// mbedTLS allocates two large I/O buffers during TLS handshake:
//   - IN buffer:  16384 bytes (CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN)
//   - OUT buffer: 16384 bytes (or 4096 if asymmetric buffers enabled)
//
// When these are allocated from the general heap, they fragment it permanently.
// After TLS disconnect, the freed regions don't merge back into one contiguous
// block, leaving a fragmentation floor of ma≈45KB that is marginal for SD mount.
//
// This arena reserves a static buffer in .bss and intercepts mbedTLS allocations
// via mbedtls_platform_set_calloc_free(). Large allocations (≥8KB) are served
// from fixed arena slots; smaller allocations pass through to the normal ESP-IDF
// mbedTLS allocator (esp_mbedtls_mem_calloc/free).
//
// Result: TLS buffers never touch the general heap, making mount order and
// heap fragmentation irrelevant to TLS stability.
// ============================================================================

// Call once early in setup(), before any TLS/WiFi activity.
// Installs the arena allocator via mbedtls_platform_set_calloc_free().
void tlsArenaInit();

// Debug: log arena slot status and sizes
void tlsArenaLogStatus();

#endif // TLS_ARENA_H
