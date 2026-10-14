#ifndef SPRITE_FRAME_SCRIPTS_H
#define SPRITE_FRAME_SCRIPTS_H

#include <stddef.h>

struct SWFAppContext;

typedef void (*SpriteFrameScriptFn)(struct SWFAppContext*);

typedef struct {
	size_t char_id;
	size_t frame_idx;
	SpriteFrameScriptFn fn;
} SpriteFrameScriptEntry;

void tagInitSpriteFrameScripts(SpriteFrameScriptEntry* table, size_t count);

SpriteFrameScriptFn actionGetSpriteFrameScript(size_t char_id, size_t frame_idx);

// ---------------------------------------------------------------------------
// Phase B/C (GOTO_FIFO_UNIFICATION_INCREMENTAL): deferred sprite-script queue.
//
// ng_gotoFrameCurrentSprite resolves the target frame's script via
// actionGetSpriteFrameScript and pushes onto this bounded queue. Phase B
// captures the entry but nothing dispatches it. Phase C adds the drain
// hook actionFlushPendingSpriteScriptsToScriptQueue which transfers the
// queued entries into AQ_KIND_SCRIPT so they FIFO-interleave with other
// scripts in actionDrainOnloadAndScript. The hook is only called when the
// runtime flag g_unify_sprite_drain is set (default 0); Phase D flips it.
//
// Queue is bounded (MAX_PENDING_SPRITE_SCRIPTS=16). On overflow, the new
// entry is dropped silently. actionResetPendingSpriteScriptQueue is called
// once per tick to clear stale entries that didn't drain.
//
// Context is captured at push time so the script fires under the sprite's
// MovieClip rather than whatever context is active at drain time (the
// outer drain runs at root level by default). The void* fields below hold
// MovieClip*/DisplayObject* pointers — typed as void* to keep this header
// free of libswf includes.
// ---------------------------------------------------------------------------

typedef struct {
	SpriteFrameScriptFn fn;
	size_t char_id;
	size_t frame_idx;
	void* ctx_mc;          // MovieClip* — sprite at goto-issue time
	void* ctx_base;        // MovieClip* — closure base_clip at goto-issue time
	void* ctx_sprite_obj;  // DisplayObject* — sprite display object
} PendingSpriteScriptEntry;

// Push a deferred sprite-script entry. fn may be NULL (entry is still
// recorded for later sequencing). Captures the live sprite context so
// Phase C's drain hook can dispatch the script in the right MC. Returns
// 1 on success, 0 if dropped (queue full).
int actionQueuePendingSpriteScript(size_t char_id, size_t frame_idx,
                                   SpriteFrameScriptFn fn);

// Number of currently-queued entries.
size_t actionPendingSpriteScriptCount(void);

// Read the i-th pending entry without removing it (i < count). Returns
// NULL if i is out of range.
const PendingSpriteScriptEntry* actionPendingSpriteScriptAt(size_t i);

// Drop all queued entries without dispatching. Called at end of tick
// to prevent leaks across ticks.
void actionResetPendingSpriteScriptQueue(void);

// Phase C: transfer all currently-pending sprite-script entries into the
// AQ_KIND_SCRIPT bucket of the action queue, with their captured sprite
// context. After this returns, the pending queue is empty and the
// transferred entries will dispatch from actionDrainOnloadAndScript in
// FIFO order interleaved with any other queued scripts. Entries with
// fn==NULL are dropped (no script to run). Returns the number of entries
// transferred. Caller owns the gating: this function does NOT consult
// g_unify_sprite_drain — actionDrainOnloadAndScript does.
size_t actionFlushPendingSpriteScriptsToScriptQueue(struct SWFAppContext* app_context);

// Sprite display-list realloc rebase (see swf.h ALIASING RULE): repoint
// pending entries whose captured ctx_sprite_obj falls inside the moved
// buffer [old_base, old_base+old_bytes).
void actionPendingSpriteScriptsRebaseSpriteObj(void* old_base,
                                               size_t old_bytes,
                                               void* new_base);

#endif
