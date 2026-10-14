#pragma once

#include <stackvalue.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct
{
	ActionStackValueType type;
	u32 str_size;       // For strings: UTF-16 code unit count
	u32 string_id;      // String ID for constant strings (0 for dynamic strings)
	union {
		u64 numeric_value;
		struct {
			uint16_t* heap_ptr;   // UTF-16 string data
			bool owns_memory;
		} string_data;
	} data;
} ActionVar;

void initMap();
void freeMap();

// Array-based variable storage for constant string IDs
extern ActionVar** var_array;
extern size_t var_array_size;

void initVarArray(size_t max_string_id);
ActionVar* getVariableById(u32 string_id);
int variableIsArrayOwned(ActionVar* var);

ActionVar* getVariable(char* var_name, size_t key_size);
bool hasVariable(char* var_name, size_t key_size);
void setVariableWithValue(ActionVar* var, char* stack, u32 sp);