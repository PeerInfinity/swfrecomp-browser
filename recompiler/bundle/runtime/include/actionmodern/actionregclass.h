#pragma once

// actionregclass.h — public API for the Object.registerClass subsystem.
//
// Carved out of action.c per
// SWFRecompDocs/plans/split-action-c-math-first.md (third subsystem,
// after Math and Date). Registry data and the timeline-placement +
// attachMovie invocation paths all live in registered_class.c.
//
// Callsites:
//   - action.c Object.registerClass builtin (register/unregister)
//   - action.c attachMovie paths (lookup + invoke)
//   - libswf/tag.c tagPlaceObject2 sprite-init (setup + invoke)
//   - libswf/tag.c catch-up child construction (invoke)

#include <actionmodern/action_internal.h>  // ASFunction, SWFAppContext, MovieClip

// ------------------------------------------------------------------
// Registry — Ruffle-style dual (case-sensitive for SWF7+,
// case-insensitive for SWF<=6).
// ------------------------------------------------------------------

// Register (ctor != NULL) or unregister (ctor == NULL) a class for a
// symbol name. Uses the current g_swf_version to choose the registry.
void actionRegisterClassForSymbol(const char* symbol_name, void* constructor);

// Look up a registered class by symbol name. Uses g_swf_version to
// choose the registry. Returns ASFunction* (as void*) or NULL.
void* lookupRegisteredClass(const char* symbol_name);

// Reverse lookup for AMF0 typed-object serialization: the alias a
// constructor is currently registered under, or NULL. Latest registration
// wins when one constructor has several aliases; the returned string keeps
// the casing of that registration (the SWF<=6 registry lowercases its key
// but not this). Registry choice follows g_swf_version.
const char* actionLookupClassAlias(void* constructor);

// Version-explicit variant: swf_version is typically the defining SWF
// version of the export (for timeline sprites) or the attaching
// clip's movie version (for attachMovie).
void* lookupRegisteredClassVersion(const char* symbol_name, int swf_version);

// Look up a registered class by character id: iterates all export
// names for the char_id (via ng_forEachExportName) and returns the
// first registered constructor. Writes the matching export name to
// *out_export_name on success. Handles sprites with multiple export
// names where registerClass was called with one and attachMovie with
// another.
void* lookupRegisteredClassByCharId(size_t char_id, int swf_version, const char** out_export_name);

// ------------------------------------------------------------------
// Constructor application.
// ------------------------------------------------------------------

// Set __proto__ (= ctor.prototype) and __constructor__ on the MC's
// dynamic_props, WITHOUT invoking the constructor. Called before
// on(construct) clip events fire so prototype properties are visible
// inside the clip events.
void actionSetupRegisteredClassPrototype(SWFAppContext* app_context, const char* export_name, MovieClip* mc);

// Run the registered-class constructor against the given MovieClip
// as 'this' (MOVIECLIP receiver, not OBJECT). Pushes super context,
// switches current context to the MC, dispatches the ctor, restores.
// Calls actionSetupRegisteredClassPrototype internally so it is safe
// to call directly if setup has not already run.
void actionInvokeRegisteredClassConstructor(SWFAppContext* app_context, const char* export_name, MovieClip* mc);
