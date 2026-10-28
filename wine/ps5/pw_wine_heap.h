/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_WINE_HEAP_H
#define PW_WINE_HEAP_H
#include <stddef.h>
#include <stdint.h>

/* Heap for Wine's Unix side on the PS5, where the title's libc malloc stops
 * near 13 MiB. Small blocks (up to 64 KiB) come from power-of-two size
 * classes carved out of 1 MiB anonymous mappings; larger blocks get their
 * own mapping and are unmapped on free. One mutex serialises every call.
 * A pointer this heap did not return is never touched: free counts it as
 * foreign and returns, so memory allocated inside libc cannot corrupt it. */
typedef struct PwWineHeapStats {
    uint64_t live_bytes,peak_live_bytes,mapped_bytes,peak_mapped_bytes;
    uint64_t allocations,frees,failures,foreign_frees,large_live;
} PwWineHeapStats;

void *pw_wine_heap_malloc(size_t bytes);
void *pw_wine_heap_calloc(size_t count,size_t bytes);
void *pw_wine_heap_realloc(void *pointer,size_t bytes);
void pw_wine_heap_free(void *pointer);
/* A block aligned to alignment, a power of two; free and realloc take it
 * like any other. NULL for a bad alignment or no memory. */
void *pw_wine_heap_memalign(size_t alignment,size_t bytes);
/* Usable bytes of a block this heap returned, 0 for anything else. */
size_t pw_wine_heap_usable_size(const void *pointer);
void pw_wine_heap_stats(PwWineHeapStats *stats);

/* ⚠ Kept OUT of PwWineHeapStats on purpose. dlls/ntdll/unix/loader.c's
 * pw_report_heap() declares its own 9-field copy of that struct on the stack
 * instead of including this header, so adding a field here makes
 * pw_wine_heap_stats write past its buffer -- a stack smash that crashes the
 * title in the loader's refusal path. Growing this struct requires a full Wine
 * rebuild; this one does not.
 *
 * Why the last allocation was refused, and how often the direct-memory backing
 * declined so the heap fell back to anonymous (flexible) mappings:
 * heap_log_fallback prints only once per process, so a count is the only way
 * to see a repeated fallback. */
typedef struct PwWineHeapFailures {
    uint64_t last_failed_bytes,last_failure_reason;
    uint64_t last_map_bytes,last_map_errno,backing_refusals;
} PwWineHeapFailures;
void pw_wine_heap_failures(PwWineHeapFailures *out);

/* Optional backing for new spans and large blocks. map may refuse so the
 * heap can fall back to anonymous mappings. unmap returns 0 when it handled
 * the address, nonzero when the address belongs to the fallback mmap path.
 * Both callbacks run with the heap mutex held and must not allocate here. */
typedef struct PwWineHeapBacking {
    void *context;
    void *(*map)(void *context, size_t bytes);
    int (*unmap)(void *context, void *address, size_t bytes);
} PwWineHeapBacking;
void pw_wine_heap_set_backing(const PwWineHeapBacking *backing);

#endif
