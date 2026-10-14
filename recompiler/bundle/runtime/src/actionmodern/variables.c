#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <string.h>

#include <map.h>
#include <common.h>
#include <variables.h>
#include <actionmodern/action.h>
#include <actionmodern/object.h>  // swfGcMarkVar (Stage 3 collector root marker)
#include <ctype.h>

#define VAL(type, x) *((type*) x)

// For SWF <= 6, fold a variable name to lowercase (ASCII only since hashmap keys are typically ASCII)
// Returns a stack-allocated buffer valid until next call
static void fold_key_lower(const char* src, size_t len, char* dst)
{
	for (size_t i = 0; i < len; i++) {
		dst[i] = (src[i] >= 'A' && src[i] <= 'Z') ? (src[i] + 32) : src[i];
	}
	dst[len] = '\0';
}

hashmap* var_map = NULL;
ActionVar** var_array = NULL;
size_t var_array_size = 0;

void initMap()
{
	var_map = hashmap_create();
}

void initVarArray(size_t max_string_id)
{
	if (var_array != NULL) {
		// Already initialized (e.g., child SWF calling initVarArray after parent).
		// Only grow if the child needs more slots; never shrink or reinitialize.
		if (max_string_id > var_array_size) {
			ActionVar** new_array = (ActionVar**) calloc(max_string_id, sizeof(ActionVar*));
			if (!new_array) {
				EXC("Failed to reallocate variable array\n");
				exit(1);
			}
			for (size_t i = 0; i < var_array_size; i++) {
				new_array[i] = var_array[i];
			}
			free(var_array);
			var_array = new_array;
			var_array_size = max_string_id;
		}
		return;
	}
	var_array_size = max_string_id;
	var_array = (ActionVar**) calloc(var_array_size, sizeof(ActionVar*));

	if (!var_array)
	{
		EXC("Failed to allocate variable array\n");
		exit(1);
	}
}

static int free_variable_callback(const void *key, size_t ksize, uintptr_t value, void *usr)
{
	ActionVar* var = (ActionVar*) value;

	// Skip entries that are shared with var_array (will be freed by var_array loop)
	for (size_t i = 0; i < var_array_size; i++) {
		if (var_array[i] == var) return 0;
	}

	// Free heap-allocated strings
	if (var->type == ACTION_STACK_VALUE_STRING && var->data.string_data.owns_memory)
	{
		free(var->data.string_data.heap_ptr);
	}

	free(var);
	return 0;
}

void freeMap()
{
	if (var_map)
	{
		hashmap_iterate(var_map, free_variable_callback, NULL);
		hashmap_free(var_map);
		var_map = NULL;
	}

	// Free array-based variables
	if (var_array)
	{
		for (size_t i = 0; i < var_array_size; i++)
		{
			if (var_array[i])
			{
				// Free heap-allocated strings
				if (var_array[i]->type == ACTION_STACK_VALUE_STRING &&
				    var_array[i]->data.string_data.owns_memory)
				{
					free(var_array[i]->data.string_data.heap_ptr);
				}
				free(var_array[i]);
			}
		}
		free(var_array);
		var_array = NULL;
		var_array_size = 0;
	}
}

// Returns 1 if `var` is currently owned by var_array (the primary owner that
// freeMap's var_array loop will free). The var_map hashmap is a secondary index
// that may alias the same ActionVar — callers that replace/free a hashmap entry
// must NOT free it when it is array-owned, or freeMap later double-frees /
// reads a dangling var_array slot. This mirrors free_variable_callback's own
// "skip entries shared with var_array" ownership rule. The collision arises for
// SWF<=6 case-folded keys: e.g. `_LEVEL0` and `_level0` get distinct var_array
// slots but fold to one hashmap key (see actionSetVariable's hashmap sync).
int variableIsArrayOwned(ActionVar* var)
{
	if (!var || !var_array) return 0;
	for (size_t i = 0; i < var_array_size; i++)
		if (var_array[i] == var) return 1;
	return 0;
}

ActionVar* getVariableById(u32 string_id)
{
	if (string_id == 0 || string_id >= var_array_size)
	{
		// Invalid ID or dynamic string (ID = 0)
		return NULL;
	}

	// Lazy allocation
	if (!var_array[string_id])
	{
		ActionVar* var = (ActionVar*) malloc(sizeof(ActionVar));
		if (!var)
		{
			EXC("Failed to allocate variable\n");
			return NULL;
		}

		// Initialize with unset type (empty string, UTF-16 NULL with 0 length)
		var->type = ACTION_STACK_VALUE_STRING;
		var->str_size = 0;
		var->string_id = 0;
		var->data.string_data.heap_ptr = NULL;
		var->data.string_data.owns_memory = false;
		var->data.numeric_value = 0;

		var_array[string_id] = var;
	}

	return var_array[string_id];
}

ActionVar* getVariable(char* var_name, size_t key_size)
{
	ActionVar* var;

	// For SWF <= 6, fold keys to lowercase for case-insensitive matching
	char folded_buf[512];
	char* lookup_key = var_name;
	size_t lookup_size = key_size;
	if (g_swf_version <= 6 && key_size < sizeof(folded_buf)) {
		fold_key_lower(var_name, key_size, folded_buf);
		lookup_key = folded_buf;
	}

	if (hashmap_get(var_map, lookup_key, lookup_size, (uintptr_t*) &var))
	{
		return var;
	}

	var = (ActionVar*) malloc(sizeof(ActionVar));
	if (var == NULL) return NULL;

	// Initialize with unset type (empty string, UTF-16 NULL with 0 length)
	var->type = ACTION_STACK_VALUE_STRING;
	var->str_size = 0;
	var->string_id = 0;
	var->data.string_data.heap_ptr = NULL;
	var->data.string_data.owns_memory = false;
	var->data.numeric_value = 0;

	// The hashmap stores the key pointer directly (not a copy), so we must
	// heap-allocate the key to ensure it outlives the caller's stack frame.
	char* key_copy = (char*) malloc(lookup_size + 1);
	if (key_copy == NULL) { free(var); return NULL; }
	memcpy(key_copy, lookup_key, lookup_size);
	key_copy[lookup_size] = '\0';

	hashmap_set(var_map, key_copy, lookup_size, (uintptr_t) var);

	return var;
}

bool hasVariable(char* var_name, size_t key_size)
{
	ActionVar* var;
	// For SWF <= 6, fold keys to lowercase for case-insensitive matching
	char folded_buf[512];
	char* lookup_key = var_name;
	if (g_swf_version <= 6 && key_size < sizeof(folded_buf)) {
		fold_key_lower(var_name, key_size, folded_buf);
		lookup_key = folded_buf;
	}
	return hashmap_get(var_map, lookup_key, key_size, (uintptr_t*) &var);
}

// Update var_map entry directly, bypassing the local-scope short-circuit.
// Used by callers (e.g. actionSetMember on root MC) that want to keep
// var_map in sync with root.dynamic_props regardless of caller context.
void setGlobalVariableByName(const char* var_name, ActionVar* value)
{
	size_t key_size = strlen(var_name);
	ActionVar* var = getVariable((char*)var_name, key_size);

	if (var == NULL) {
		return;
	}

	// Free old data if it was a heap-allocated string
	if (var->type == ACTION_STACK_VALUE_STRING && var->data.string_data.owns_memory) {
		free(var->data.string_data.heap_ptr);
		var->data.string_data.heap_ptr = NULL;
		var->data.string_data.owns_memory = false;
	}

	// Copy the new value
	var->type = value->type;
	var->str_size = value->str_size;
	var->data = value->data;
}

void setVariableByName(const char* var_name, ActionVar* value)
{
	// If inside a function scope, define on local scope (not global)
	// This prevents DefineFunction parameter binding from leaking to root scope
	extern bool setVariableOnLocalScope(const char* var_name, ActionVar* value);
	if (setVariableOnLocalScope(var_name, value)) {
		return;
	}

	setGlobalVariableByName(var_name, value);
}

void setVariableWithValue(ActionVar* var, char* stack, u32 sp)
{
	ActionStackValueType type = stack[sp];

	if (type == ACTION_STACK_VALUE_STRING)
	{
		// Copy UTF-16 string data to heap (stack data is transient)
		// IMPORTANT: Make the copy BEFORE freeing the old value, because the
		// source pointer on the stack may point to the old heap_ptr (e.g., when
		// DefineLocal re-assigns the same variable with a value derived from it).
		u32 u16_len = VAL(u32, &stack[sp + 8]);  // code unit count
		const uint16_t* src = (const uint16_t*) VAL(u64, &stack[sp + 16]);

		uint16_t* heap_copy = NULL;
		if (u16_len > 0 && src != NULL)
		{
			heap_copy = (uint16_t*) malloc(u16_len * sizeof(uint16_t));
			if (heap_copy)
				memcpy(heap_copy, src, u16_len * sizeof(uint16_t));
		}

		// Note: old string value is intentionally NOT freed here.
		// Without reference counting, the old pointer may still be referenced by
		// array elements, object properties, or other variables. Freeing causes
		// use-after-free when e.g. sort() accesses array elements whose strings
		// were also stored in a variable that was later reassigned.

		var->type = ACTION_STACK_VALUE_STRING;
		var->string_id = VAL(u32, &stack[sp + 12]);

		if (heap_copy != NULL)
		{
			var->str_size = u16_len;
			var->data.string_data.heap_ptr = heap_copy;
			var->data.string_data.owns_memory = true;
		}
		else if (u16_len == 0 || src == NULL)
		{
			// Empty string: use a non-NULL sentinel pointer to distinguish
			// from zero-initialized "unset" variable slots (which have NULL heap_ptr)
			static const uint16_t empty_u16[] = {0};
			var->str_size = 0;
			var->data.string_data.heap_ptr = (uint16_t*) empty_u16;
			var->data.string_data.owns_memory = false;
		}
		else
		{
			// malloc failed
			var->str_size = 0;
			var->data.numeric_value = 0;
		}
	}
	else
	{
		// Note: old string value intentionally not freed (see above comment)

		// Numeric types - store directly
		var->type = type;
		var->str_size = VAL(u32, &stack[sp + 8]);
		var->data.numeric_value = VAL(u64, &stack[sp + 16]);
	}
}
// Stage 3 collector root marker: every timeline variable (global var_map +
// the SWF<=6 case-fold var_array) is a borrowed ActionVar that can hold
// OBJECT/ARRAY/FUNCTION values across frames.
static int gc_mark_variable_callback(const void* key, size_t ksize, uintptr_t value, void* usr)
{
	(void)key; (void)ksize; (void)usr;
	ActionVar* var = (ActionVar*)value;
	if (var != NULL)
		swfGcMarkVar(var);
	return 0;
}

void variablesGcMarkRoots(void)
{
	if (var_map != NULL)
		hashmap_iterate(var_map, gc_mark_variable_callback, NULL);
	for (size_t i = 0; i < var_array_size; i++)
		if (var_array[i] != NULL)
			swfGcMarkVar(var_array[i]);
}
