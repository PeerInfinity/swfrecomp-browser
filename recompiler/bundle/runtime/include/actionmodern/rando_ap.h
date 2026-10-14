#pragma once

// rando_ap.h — pure-POD C bridge to APCpp (Archipelago client).
//
// APCpp's own header (Archipelago.h) is NOT includable from C: although its
// API is wrapped in `extern "C"`, the signatures use C++ types (std::string,
// std::vector, Json::Value) and the header #includes <string>/<json/json.h>.
// This header exposes only the POD subset the C runtime (rando.c) needs; the
// translation to APCpp's C++-typed API lives in rando_ap.cpp (compiled g++).
//
// See SWFRecompDocs/plans/archipelago-randomizer-integration.md (§2.1, §7).
//
// The whole bridge is opt-in: rando.c only references it under WITH_AP. When
// WITH_AP is undefined these symbols are never linked.

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle wrapping an APCpp AP_State* plus bridge bookkeeping.
typedef struct RandoAP RandoAP;

// Allocate a client and call AP_Init. `host` + `port` are combined into a
// single "host:port" URI (port may be NULL/empty). `game`, `slot`, `password`
// are passed through (NULL → ""). Returns NULL on allocation failure.
// Does NOT start networking — call rando_ap_connect() for that.
RandoAP* rando_ap_new(const char* host, const char* port, const char* game,
                      const char* slot, const char* password);

// Stop networking (if started) and free the handle. Safe on NULL.
void rando_ap_free(RandoAP* r);

// Begin connecting (AP_Start): spawns APCpp's background network thread.
void rando_ap_connect(RandoAP* r);

// 1 if authenticated/connected, else 0.
int rando_ap_is_connected(RandoAP* r);

// Number of items the server has granted so far (monotonic; thread-safe).
size_t rando_ap_received_items_size(RandoAP* r);

// Item id at index, or -1 if out of range.
int64_t rando_ap_received_item(RandoAP* r, size_t idx);

// 1 if `item_id` appears anywhere in the received-items list, else 0.
int rando_ap_has_item(RandoAP* r, int64_t item_id);

// 1 if the given location id has been checked, else 0.
int rando_ap_location_is_checked(RandoAP* r, int64_t location_id);

// Report a location as checked (AP_SendItem).
void rando_ap_send_location(RandoAP* r, int64_t location_id);

// Notify the server the game's goal is complete (AP_StoryComplete).
void rando_ap_story_complete(RandoAP* r);

#ifdef __cplusplus
}
#endif
