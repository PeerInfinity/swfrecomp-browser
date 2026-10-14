#pragma once

// actionrando.h — public API for the Rando subsystem (Archipelago client
// exposed to AVM1 as the `Rando` class). Named "actionrando" for symmetry
// with actionmath.h / actiondate.h.
//
// The `Rando` class is the AVM1 ↔ Archipelago bridge: ActionScript constructs
// `new Rando(host, port, game, slot, password)` and polls it each frame
// (isConnected / receivedItemsSize / receivedItem / hasItem /
// locationIsChecked / sendLocation / storyComplete). The native methods call
// the pure-POD rando_ap.h bridge, which is backed by APCpp (rando_ap.cpp).
//
// Build gating: the whole class is compiled in only when WITH_AP is defined.
// When WITH_AP is undefined, initRandoPrototype() is a no-op (defined in
// rando.c under #else) and `Rando` is simply absent from the global scope.
//
// See SWFRecompDocs/plans/archipelago-randomizer-integration.md (Phase 1).

#include <actionmodern/action_internal.h>  // ASFunction, ActionVar, SWFAppContext

// Ensure the Rando constructor + prototype are initialized (idempotent).
// No-op when built without WITH_AP.
void initRandoPrototype(SWFAppContext* app_context);

// Accessor for the Rando constructor ASFunction, for global registration.
// Calls initRandoPrototype internally. Returns NULL when built without WITH_AP.
ASFunction* actionRandoGetConstructor(SWFAppContext* app_context);
