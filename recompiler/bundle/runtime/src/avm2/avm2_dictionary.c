// flash.utils.Dictionary — port of Ruffle
// core/src/avm2/object/dictionary_object.rs + globals/flash/utils/Dictionary.as.
//
// Dictionary instances are plain dynamic ScriptObjects whose expando list
// additionally accepts OBJECT keys (identity semantics, no namespace —
// Avm2DynProp.key_obj). Primitive keys coerce to strings exactly like a
// plain Object. Object and string keys share one insertion-ordered list,
// so for-in interleaves them in insertion order. Stringified properties
// are always enumerable (setPropertyIsEnumerable is a no-op).
//
// Weak keys are accepted but behave like strong keys: AVM2 allocations
// are census-invisible/immortal (avm2_object.c GC note), so collection is
// unobservable anyway (dictionary_weak_keys is upstream-ignored).

#include <string.h>

#include <avm2/avm2_class.h>
#include <avm2/avm2_globals.h>
#include <avm2/avm2_main.h>
#include <avm2/avm2_object.h>
#include <avm2/avm2_ops.h>

int avm2_is_dictionary(Avm2Object* obj)
{
	Avm2Context* ctx = avm2_get_context();
	if (ctx->builtins.dictionary_class == NULL) return 0;
	for (Avm2Class* c = obj->cls; c != NULL; c = c->super_class)
	{
		if (c == ctx->builtins.dictionary_class) return 1;
	}
	return 0;
}

static Avm2Value dictionary_proto_to_json(Avm2Activation* act)
{
	return avm2_string(avm2_string_from_literal(act->ctx, "Dictionary"));
}

void avm2_register_dictionary(Avm2Context* ctx)
{
	Avm2Builtins* b = &ctx->builtins;
	Avm2Class* cls = avm2_builtin_class(ctx, "flash.utils", "Dictionary",
	                                    b->object_class);
	b->dictionary_class = cls;
	// dynamic class; the constructor ignores weakKeys (see header note).
	avm2_proto_add_function(ctx, cls->prototype_obj, "toJSON",
	                        dictionary_proto_to_json);
}
