#pragma once

// actiondate.h — public API for the Date subsystem.
//
// Named "actiondate" (not "date") for symmetry with actionmath.h and
// to avoid any chance of shadowing system headers on the
// include/actionmodern/ search path.
//
// Carved out of action.c per
// SWFRecompDocs/plans/split-action-c-math-first.md (Date is the "what
// comes next" entry after Math). All Date state lives in date.c; this
// header exposes only the accessors action.c needs.

#include <actionmodern/action_internal.h>  // ASFunction, ActionVar, SWFAppContext

// Ensure Date prototype + constructor are initialized (idempotent).
// Accessors below call this internally; call directly only if you need
// to force construction before the first accessor use.
void initDatePrototype(SWFAppContext* app_context);

// Construct a fresh Date object (equivalent to `new Date(args...)`).
// Returns an ASObject with native_type = NATIVE_DATE and __proto__
// set to Date.prototype.
ASObject* actionDateConstruct(SWFAppContext* app_context, ActionVar* args, u32 arg_count);

// Invoke Date.prototype.toString on a Date-backed ASObject. Used by
// the top-level `Date()` builtin (called without `new`), which returns
// the current time formatted as a string.
ActionVar actionDateToString(SWFAppContext* app_context, ASObject* date_obj);

// Accessor for the Date constructor ASFunction. Used for global
// registration, secondary-global copies, and enumeration tables.
// Calls initDatePrototype internally.
ASFunction* actionDateGetConstructor(SWFAppContext* app_context);

// Look up a Date method by ASnative index (class 103). Returns NULL
// for unmapped indices. Index scheme follows Ruffle's date.rs method
// table (see date.c for the full mapping).
ASFunction* actionDateGetASnativeMethod(SWFAppContext* app_context, u16 index);
