#include <sprite_frame_scripts.h>
#include <actionmodern/action_queue.h>
#include <libswf/swf.h>

#include <stdint.h>

// Sprite context globals captured at push time. Defined elsewhere; we
// only read them here.
extern MovieClip* g_current_context;
extern DisplayObject* g_current_sprite_obj;
extern MovieClip* actionGetBaseClip(void);

static SpriteFrameScriptEntry* g_table = NULL;
static size_t g_count = 0;

void tagInitSpriteFrameScripts(SpriteFrameScriptEntry* table, size_t count)
{
	g_table = table;
	g_count = count;
}

SpriteFrameScriptFn actionGetSpriteFrameScript(size_t char_id, size_t frame_idx)
{
	if (g_table == NULL) return NULL;
	for (size_t i = 0; i < g_count; i++)
	{
		if (g_table[i].char_id == char_id && g_table[i].frame_idx == frame_idx)
			return g_table[i].fn;
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// Phase B — deferred sprite-script queue
// ---------------------------------------------------------------------------

#define MAX_PENDING_SPRITE_SCRIPTS 16

static PendingSpriteScriptEntry g_pending[MAX_PENDING_SPRITE_SCRIPTS];
static size_t g_pending_count = 0;

int actionQueuePendingSpriteScript(size_t char_id, size_t frame_idx,
                                   SpriteFrameScriptFn fn)
{
	if (g_pending_count >= MAX_PENDING_SPRITE_SCRIPTS) return 0;
	g_pending[g_pending_count].fn = fn;
	g_pending[g_pending_count].char_id = char_id;
	g_pending[g_pending_count].frame_idx = frame_idx;
	// Capture sprite context at push time so Phase C's drain hook can
	// dispatch the script under the sprite that issued the goto, rather
	// than whatever context is active at outer-drain time (root level).
	g_pending[g_pending_count].ctx_mc = (void*)g_current_context;
	g_pending[g_pending_count].ctx_base = (void*)actionGetBaseClip();
	g_pending[g_pending_count].ctx_sprite_obj = (void*)g_current_sprite_obj;
	g_pending_count++;
	return 1;
}

size_t actionPendingSpriteScriptCount(void) { return g_pending_count; }

const PendingSpriteScriptEntry* actionPendingSpriteScriptAt(size_t i)
{
	if (i >= g_pending_count) return NULL;
	return &g_pending[i];
}

void actionResetPendingSpriteScriptQueue(void) { g_pending_count = 0; }

void actionPendingSpriteScriptsRebaseSpriteObj(void* old_base,
                                               size_t old_bytes,
                                               void* new_base)
{
	uintptr_t lo = (uintptr_t)old_base;
	uintptr_t hi = lo + old_bytes;
	for (size_t i = 0; i < g_pending_count; i++) {
		uintptr_t v = (uintptr_t)g_pending[i].ctx_sprite_obj;
		if (v >= lo && v < hi)
			g_pending[i].ctx_sprite_obj = new_base
				? (char*)new_base + (v - lo)
				: NULL;  // new_base==NULL: buffer being FREED, not moved
	}
}

size_t actionFlushPendingSpriteScriptsToScriptQueue(SWFAppContext* app_context)
{
	size_t flushed = 0;
	for (size_t i = 0; i < g_pending_count; i++) {
		PendingSpriteScriptEntry* e = &g_pending[i];
		if (e->fn == NULL) continue;
		actionQueueSpriteScriptCaptured(app_context, e->fn,
		                                 e->ctx_mc, e->ctx_base,
		                                 e->ctx_sprite_obj);
		flushed++;
	}
	g_pending_count = 0;
	return flushed;
}
