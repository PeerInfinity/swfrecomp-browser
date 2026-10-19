#ifndef AVM2_EXTERNAL_H
#define AVM2_EXTERNAL_H

// flash.external.ExternalInterface — test-provider surface (avm2_external.c).
//
// Mirrors Ruffle's ExternalInterfaceProvider + external::Value
// (core/src/external.rs) for per-test C harnesses (verify_output.py compiles a
// test dir's test_harness.c into the build): a harness installs a provider,
// which makes ExternalInterface.available true and receives every
// ExternalInterface.call(); host → AS3 calls go through
// avm2_ei_call_internal (Ruffle's Player::call_internal_interface). Values
// cross the boundary as a C-owned tree, never as GC-managed AVM2 values.
// Builds that install no provider are unaffected.

#include <stdbool.h>
#include <stdint.h>

typedef enum Avm2EiKind
{
	AVM2_EI_UNDEFINED = 0,
	AVM2_EI_NULL,
	AVM2_EI_BOOL,
	AVM2_EI_NUMBER,
	AVM2_EI_STRING,
	AVM2_EI_LIST,
	AVM2_EI_OBJECT,
} Avm2EiKind;

typedef struct Avm2EiValue
{
	Avm2EiKind kind;
	bool b;                       // BOOL
	double d;                     // NUMBER
	char* str;                    // STRING: malloc'd UTF-8
	uint32_t count;               // LIST / OBJECT entry count
	struct Avm2EiValue* items;    // LIST elements / OBJECT values (malloc'd)
	char** keys;                  // OBJECT keys, sorted by byte order (BTreeMap)
} Avm2EiValue;

// Provider: receives call(name, ...args) with args already converted from AS3
// (Ruffle Value::from_avm2); the returned tree is converted back with
// into_avm2 and then freed by the runtime. Must not keep `args`.
typedef Avm2EiValue (*Avm2EiProvider)(const char* name, const Avm2EiValue* args,
                                      uint32_t argc);

void avm2_ei_set_provider(Avm2EiProvider provider);

// Host → AS3: invoke the callback registered under `name` with `args`
// (converted with into_avm2). Returns the callback's result converted with
// from_avm2; NULL for an unknown name or a callback that threw (reported as an
// uncaught error). The caller owns the result (avm2_ei_value_free).
Avm2EiValue avm2_ei_call_internal(const char* name, const Avm2EiValue* args,
                                  uint32_t argc);

// Constructors / destructor for harness-built trees. avm2_ei_object takes
// ownership of `keys`' strings and `values` (both malloc'd arrays of n).
Avm2EiValue avm2_ei_undefined(void);
Avm2EiValue avm2_ei_null(void);
Avm2EiValue avm2_ei_bool(bool b);
Avm2EiValue avm2_ei_number(double d);
Avm2EiValue avm2_ei_string(const char* s);
Avm2EiValue avm2_ei_list(const Avm2EiValue* items, uint32_t n);    // copies the array
Avm2EiValue avm2_ei_object(char** keys, Avm2EiValue* values, uint32_t n);
void avm2_ei_value_free(Avm2EiValue* v);

#endif // AVM2_EXTERNAL_H
