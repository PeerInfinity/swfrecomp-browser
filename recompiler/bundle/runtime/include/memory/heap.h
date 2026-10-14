#ifndef HEAP_H
#define HEAP_H

#include <stddef.h>
#include <stdbool.h>

// Forward declaration
typedef struct SWFAppContext SWFAppContext;

/**
 * Convenience macros for heap allocation
 *
 * These macros require app_context to be in scope.
 */
#define HALLOC(s) heap_alloc(app_context, s)
#define HCALLOC(n, s) heap_calloc(app_context, n, s)
#define FREE(p) heap_free(app_context, p)

/**
 * Memory Heap Manager
 *
 * Wrapper around o1heap allocator using virtual memory for efficient allocation.
 *
 * Design:
 * - Reserves 1 GB virtual address space upfront (cheap, no physical RAM)
 * - Commits all pages immediately (still cheap, still no physical RAM!)
 * - Initializes o1heap with full 1 GB space (no expansion needed)
 * - Physical RAM only allocated on first access (lazy allocation by OS)
 * - Heap state stored in app_context for proper lifecycle management
 *
 * Key benefit: Lazy physical allocation by OS spreads memory overhead across frames,
 * reducing stutter compared to traditional malloc approaches. Committing the full space
 * upfront is faster and simpler than incremental expansion.
 */

/**
 * Initialize the heap system
 *
 * Reserves and commits 1 GB of virtual address space. Physical RAM is allocated
 * lazily by the OS as memory is accessed.
 *
 * @param app_context The SWF application context to store heap state
 * @param initial_size Unused (kept for API compatibility)
 * @return true on success, false on failure
 */
bool heap_init(SWFAppContext* app_context, size_t initial_size);

/**
 * Allocate memory from the heap
 *
 * Semantics similar to malloc():
 * - Returns pointer aligned to O1HEAP_ALIGNMENT
 * - Returns NULL on allocation failure
 * - Size of 0 returns NULL (standard behavior)
 *
 * @param app_context The SWF application context containing heap state
 * @param size Number of bytes to allocate
 * @return Pointer to allocated memory, or NULL on failure
 */
void* heap_alloc(SWFAppContext* app_context, size_t size);

/**
 * Allocate zero-initialized memory from the heap
 *
 * Semantics similar to calloc():
 * - Allocates num * size bytes
 * - Zeroes the memory before returning
 * - Returns NULL on allocation failure or overflow
 *
 * @param app_context The SWF application context containing heap state
 * @param num Number of elements
 * @param size Size of each element
 * @return Pointer to allocated zero-initialized memory, or NULL on failure
 */
void* heap_calloc(SWFAppContext* app_context, size_t num, size_t size);

/**
 * Free memory allocated by heap_alloc() or heap_calloc()
 *
 * Semantics similar to free():
 * - Passing NULL is a no-op
 * - Pointer must have been returned by heap_alloc() or heap_calloc()
 *
 * @param app_context The SWF application context containing heap state
 * @param ptr Pointer to memory to free
 */
void heap_free(SWFAppContext* app_context, void* ptr);

/**
 * Get heap statistics
 *
 * Prints detailed statistics about heap usage including:
 * - Number of heaps
 * - Size of each heap
 * - Allocated memory
 * - Peak allocation
 * - OOM count
 *
 * @param app_context The SWF application context containing heap state
 */
void heap_stats(SWFAppContext* app_context);

/**
 * Currently allocated bytes in the heap arena (o1heap diagnostics.allocated,
 * fragment-rounded). Deterministic for a deterministic allocation sequence.
 * Returns 0 when the backend cannot report (HEAP_PASSTHROUGH) — callers must
 * treat 0 as "unknown", not "empty".
 *
 * @param app_context The SWF application context containing heap state
 */
size_t heap_allocated_bytes(SWFAppContext* app_context);

/**
 * Total heap arena capacity in bytes; 0 when unknown (HEAP_PASSTHROUGH).
 *
 * @param app_context The SWF application context containing heap state
 */
size_t heap_capacity_bytes(SWFAppContext* app_context);

/**
 * Arena geometry, for allocators that want an O(1) address→cell mapping over
 * the heap (the AVM2 collector's census-membership bitmap: 1 bit per
 * allocation-alignment cell, set on enroll, cleared on sweep, replacing a
 * per-collect sorted snapshot of every object address).
 *
 * heap_arena_base()      — first address of the arena; NULL when unknown
 *                          (HEAP_PASSTHROUGH routes to system malloc, whose
 *                          addresses are not confined to an arena).
 * heap_arena_span()      — bytes of address space the arena covers; 0 unknown.
 * heap_arena_alignment() — allocation alignment (O1HEAP_ALIGNMENT); every
 *                          pointer heap_alloc returns is a multiple of it, so
 *                          distinct live allocations never share a cell.
 *
 * Callers MUST treat base == NULL / span == 0 as "no arena" and fall back.
 *
 * @param app_context The SWF application context containing heap state
 */
void* heap_arena_base(SWFAppContext* app_context);
size_t heap_arena_span(SWFAppContext* app_context);
size_t heap_arena_alignment(void);

/**
 * Worst-single-tick transient instrumentation.
 *
 * The AVM2 GC only collects between ticks, so the arena floor is set not by
 * steady live set but by the largest transient a *single* tick allocates
 * before the next collection boundary (e.g. FlxTilemap.arrayToCSV building a
 * multi-hundred-MB string in one boot tick). The run-wide o1heap
 * peak_allocated cannot isolate this — it is monotonic across the whole run.
 *
 * heap_tick_mark()                — rebase the resettable high-water to the
 *                                   current allocated level; call once at each
 *                                   tick boundary (after the collect).
 * heap_peak_since_mark_bytes()    — highest allocated level seen since the
 *                                   last mark. The window's gross transient is
 *                                   this minus heap_allocated_bytes() sampled
 *                                   at the mark. Returns 0 when unknown
 *                                   (HEAP_PASSTHROUGH) — treat as "unknown".
 */
void heap_tick_mark(SWFAppContext* app_context);
size_t heap_peak_since_mark_bytes(SWFAppContext* app_context);

/**
 * Shutdown the heap system
 *
 * Frees all heap arenas. Should be called at program exit.
 * After calling this, heap_alloc() will fail until heap_init() is called again.
 *
 * @param app_context The SWF application context containing heap state
 */
void heap_shutdown(SWFAppContext* app_context);

#endif // HEAP_H
