// Array builtin class + methods (port of Ruffle globals/array.rs semantics
// where they matter for the trace tests: hole handling, avmplus sort
// flags, splice edge cases).

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <avm2/avm2_class.h>
#include <avm2/avm2_error.h>
#include <avm2/avm2_globals.h>
#include <avm2/avm2_main.h>
#include <avm2/avm2_object.h>
#include <avm2/avm2_ops.h>
#include <memory/heap.h>

static Avm2Object* this_array(Avm2Activation* act)
{
	if (act->this_val.kind == AVM2_VALUE_OBJECT
	    && act->this_val.u.obj->kind == AVM2_OBJ_ARRAY)
	{
		return act->this_val.u.obj;
	}
	return NULL;
}

static Avm2Value arg_or_undef(Avm2Activation* act, uint32_t i)
{
	return i < act->argc ? act->args[i] : avm2_undefined();
}

// --- sealed Array subclasses (avmplus bugzilla 654807) ----------------------
//
// Below SWF 13 a sealed `extends Array` subclass keeps element storage, so
// this_array() finds it and the dense-path methods (push/pop/shift/unshift/
// reverse/concat/slice, splice with delCount 0) work exactly as on an Array.
// The rest of avmplus's Array methods walk the receiver with a generic
// get-property loop instead, and index properties are NOT reachable on a
// sealed instance — so the first element read throws #1069. That split is the
// whole "semisealed" column of regress/bug_654807_swf12.
//
// From SWF 13 the subclass gets no storage at all (this_array() is NULL) and
// the same generic paths apply to writes too, which cannot create a property
// on a sealed object: #1056. That is the "sealed" column of _swf13.

// A sealed `extends Array` receiver, and only that: an unrelated sealed
// builtin borrowing an Array method through .call() keeps the pre-existing
// neutral-return behaviour (as3/Array/length_mods runs
// `Array.prototype.public::pop.call(byteArray)`).
static int sealed_receiver(Avm2Activation* act)
{
	Avm2Value t = act->this_val;
	return t.kind == AVM2_VALUE_OBJECT && t.u.obj != NULL
	       && t.u.obj->cls != NULL
	       && t.u.obj->cls->instance_kind == AVM2_OBJ_ARRAY
	       && !avm2_object_is_dynamic(t.u.obj);
}

static void receiver_class_name(Avm2Activation* act, char* buf, int size)
{
	Avm2Value t = act->this_val;
	Avm2Class* cls = (t.kind == AVM2_VALUE_OBJECT && t.u.obj != NULL)
		? t.u.obj->cls : NULL;
	if (cls != NULL) avm2_class_qname_buf(cls, buf, size);
	else snprintf(buf, (size_t) size, "Object");
}

// Generic element READ against a sealed receiver: #1069. `count` is how many
// elements the method would have read — zero means it never touches one.
static void sealed_generic_read(Avm2Activation* act, Avm2Object* arr, uint32_t count)
{
	if (arr == NULL || count == 0 || !sealed_receiver(act)) return;
	char cn[160];
	receiver_class_name(act, cn, sizeof(cn));
	avm2_throw_error(act->ctx, act->ctx->builtins.reference_error_class,
	                 "Error #1069: Property 0 not found on %s and there is "
	                 "no default value.", cn);
}

// Generic element WRITE against a sealed receiver that has no storage: #1056.
static void sealed_generic_write(Avm2Activation* act)
{
	if (!sealed_receiver(act)) return;
	char cn[160];
	receiver_class_name(act, cn, sizeof(cn));
	avm2_throw_error(act->ctx, act->ctx->builtins.reference_error_class,
	                 "Error #1056: Cannot create property 0 on %s.", cn);
}

// Resolve holes to undefined for callback-visible values.
static Avm2Value elem_or_undef(Avm2Object* arr, uint32_t i)
{
	Avm2Value v = avm2_array_get(arr, i);
	return v.kind == AVM2_VALUE_HOLE ? avm2_undefined() : v;
}

// Wrapping start index for slice/splice/indexOf.
static uint32_t wrap_index(double i, uint32_t len)
{
	if (isnan(i)) return 0;
	if (i < 0.0)
	{
		double w = (double) len + trunc(i);
		return w < 0.0 ? 0 : (uint32_t) w;
	}
	double v = trunc(i);
	return v > (double) len ? len : (uint32_t) v;
}


// Look index `i` up along `arr`'s prototype chain; true if a prototype
// supplies a value. Two places can hold one: element storage (Array.prototype
// is ITSELF an Array, so `Array.prototype[3] = x` lands there) and an ordinary
// dynamic prop named "3" (any non-Array prototype).
bool avm2_array_proto_index(Avm2Object* arr, uint32_t i, Avm2Value* out)
{
	char nb[16];
	int nl = snprintf(nb, sizeof(nb), "%u", i);
	for (Avm2Object* p = arr->proto; p != NULL; p = p->proto)
	{
		if (p->kind == AVM2_OBJ_ARRAY)
		{
			Avm2Value pv = avm2_array_get(p, i);
			if (pv.kind != AVM2_VALUE_HOLE) { *out = pv; return true; }
		}
		Avm2Value* dv = avm2_object_find_dynamic(p, nb, (uint32_t) nl);
		if (dv != NULL) { *out = *dv; return true; }
	}
	return false;
}

// Holes resolve through the prototype chain by index name (Ruffle
// globals/array.rs resolve_array_hole).
static Avm2Value resolve_hole(Avm2Object* arr, uint32_t i, Avm2Value v)
{
	if (v.kind != AVM2_VALUE_HOLE) return v;
	Avm2Value out;
	return avm2_array_proto_index(arr, i, &out) ? out : avm2_undefined();
}

// ---------------------------------------------------------------------------
// Basic methods
// ---------------------------------------------------------------------------

static Avm2Value array_get_length(Avm2Activation* act)
{
	Avm2Object* arr = this_array(act);
	return avm2_uint_value(arr != NULL ? avm2_array_ext(arr)->length : 0);
}

static Avm2Value array_set_length(Avm2Activation* act)
{
	Avm2Object* arr = this_array(act);
	// avmplus's set_length goes through the property path, so on a sealed
	// subclass the write is silently dropped -- bug_654807_swf12's
	// "semisealed set_length" still reads 5 after `b.length = 3`.
	if (arr != NULL && act->argc > 0 && !sealed_receiver(act))
	{
		avm2_array_set_length(act->ctx, arr, avm2_coerce_to_u32(act->ctx, act->args[0]));
	}
	return avm2_undefined();
}

static const Avm2String* array_join_inner(Avm2Context* ctx, Avm2Object* arr,
                                          const Avm2String* sep)
{
	Avm2ArrayExt* ext = avm2_array_ext(arr);
	const Avm2String* out = avm2_string_from_literal(ctx, "");
	for (uint32_t i = 0; i < ext->dense_len; i++)
	{
		if (i > 0) out = avm2_string_concat(ctx, out, sep);
		Avm2Value v = resolve_hole(arr, i, ext->elems[i]);
		if (v.kind == AVM2_VALUE_UNDEFINED || v.kind == AVM2_VALUE_NULL)
		{
			continue;
		}
		out = avm2_string_concat(ctx, out, avm2_coerce_to_string(ctx, v));
	}
	return out;
}

static Avm2Value array_join(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Object* arr = this_array(act);
	if (arr == NULL) return avm2_string(avm2_string_from_literal(ctx, ""));
	sealed_generic_read(act, arr, avm2_array_ext(arr)->length);
	const Avm2String* sep;
	if (act->argc > 0 && act->args[0].kind != AVM2_VALUE_UNDEFINED)
	{
		sep = avm2_coerce_to_string(ctx, act->args[0]);
	}
	else
	{
		sep = avm2_string_from_literal(ctx, ",");
	}
	return avm2_string(array_join_inner(ctx, arr, sep));
}

static Avm2Value array_to_locale_string(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Object* arr = this_array(act);
	if (arr == NULL) return avm2_string(avm2_string_from_literal(ctx, ""));
	Avm2ArrayExt* ext = avm2_array_ext(arr);
	sealed_generic_read(act, arr, ext->length);
	const Avm2String* sep = avm2_string_from_literal(ctx, ",");
	const Avm2String* out = avm2_string_from_literal(ctx, "");
	for (uint32_t i = 0; i < ext->dense_len; i++)
	{
		if (i > 0) out = avm2_string_concat(ctx, out, sep);
		Avm2Value v = resolve_hole(arr, i, ext->elems[i]);
		if (v.kind == AVM2_VALUE_UNDEFINED || v.kind == AVM2_VALUE_NULL)
		{
			continue;
		}
		Avm2Value s = avm2_call_public_property(ctx, v, "toLocaleString", 14, NULL, 0);
		out = avm2_string_concat(ctx, out, avm2_coerce_to_string(ctx, s));
	}
	return avm2_string(out);
}

static Avm2Value array_to_string(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Object* arr = this_array(act);
	if (arr == NULL) return avm2_string(avm2_string_from_literal(ctx, ""));
	sealed_generic_read(act, arr, avm2_array_ext(arr)->length);
	return avm2_string(array_join_inner(ctx, arr, avm2_string_from_literal(ctx, ",")));
}

static Avm2Value array_push(Avm2Activation* act)
{
	Avm2Object* arr = this_array(act);
	if (arr == NULL)
	{
		if (act->argc > 0) sealed_generic_write(act);
		return avm2_integer(0);
	}
	for (uint32_t i = 0; i < act->argc; i++)
	{
		avm2_array_push(act->ctx, arr, act->args[i]);
	}
	return avm2_uint_value(avm2_array_ext(arr)->length);
}

static Avm2Value array_pop(Avm2Activation* act)
{
	Avm2Object* arr = this_array(act);
	if (arr == NULL) return avm2_undefined();
	Avm2ArrayExt* ext = avm2_array_ext(arr);
	if (ext->length == 0) return avm2_undefined();
	// avmplus pop removes the LAST NON-HOLE element's slot entirely
	// (Ruffle array.rs pop); only when everything is a hole does it drop
	// one trailing hole and return undefined.
	if (ext->sparse != NULL)
	{
		// Highest sparse entry is the last non-hole.
		Avm2SparseElem** link = &ext->sparse;
		while ((*link)->next != NULL) link = &(*link)->next;
		Avm2Value v = (*link)->v;
		if (ext->length > 0) ext->length--;
		*link = NULL;
		return v;
	}
	for (int64_t i = (int64_t) ext->dense_len - 1; i >= 0; i--)
	{
		if (ext->elems[i].kind != AVM2_VALUE_HOLE)
		{
			Avm2Value v = ext->elems[i];
			memmove(ext->elems + i, ext->elems + i + 1,
			        (ext->dense_len - (uint32_t) i - 1) * sizeof(Avm2Value));
			ext->dense_len--;
			ext->length--;
			return v;
		}
	}
	if (ext->dense_len > 0) ext->dense_len--;
	ext->length--;
	return avm2_undefined();
}

static Avm2Value array_shift(Avm2Activation* act)
{
	Avm2Object* arr = this_array(act);
	if (arr == NULL) return avm2_undefined();
	Avm2ArrayExt* ext = avm2_array_ext(arr);
	if (ext->length == 0) return avm2_undefined();
	Avm2Value v = { AVM2_VALUE_HOLE, 0, { 0 } };
	if (ext->dense_len > 0)
	{
		v = ext->elems[0];
		memmove(ext->elems, ext->elems + 1, (ext->dense_len - 1) * sizeof(Avm2Value));
		ext->dense_len--;
	}
	ext->length--;
	return v.kind == AVM2_VALUE_HOLE ? avm2_undefined() : v;
}

static Avm2Value array_unshift(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Object* arr = this_array(act);
	if (arr == NULL)
	{
		if (act->argc > 0) sealed_generic_write(act);
		return avm2_integer(0);
	}
	Avm2ArrayExt* ext = avm2_array_ext(arr);
	uint32_t n = act->argc;
	if (n > 0)
	{
		// Grow the dense region directly (bypasses the sparse gap rule).
		uint32_t need = ext->dense_len + n;
		if (need > ext->cap)
		{
			uint32_t new_cap = ext->cap == 0 ? 8 : ext->cap;
			while (new_cap < need) new_cap *= 2;
			Avm2Value* grown = avm2_alloc(ctx, new_cap * sizeof(Avm2Value));
			if (ext->dense_len > 0)
			{
				memcpy(grown, ext->elems, ext->dense_len * sizeof(Avm2Value));
			}
			if (ext->elems != NULL) heap_free(ctx->app, ext->elems);  // sole owner (see array_reserve)
			ext->elems = grown;
			ext->cap = new_cap;
		}
		memmove(ext->elems + n, ext->elems, ext->dense_len * sizeof(Avm2Value));
		for (uint32_t i = 0; i < n; i++)
		{
			ext->elems[i] = act->args[i];
		}
		ext->dense_len += n;
		ext->length += n;
	}
	return avm2_uint_value(ext->length);
}

static Avm2Value array_reverse(Avm2Activation* act)
{
	Avm2Object* arr = this_array(act);
	if (arr == NULL) return act->this_val;
	Avm2ArrayExt* ext = avm2_array_ext(arr);
	// Reverse only up to the last non-hole; trailing holes stay trailing
	// (Ruffle globals/array.rs reverse).
	uint32_t last_non_hole = 0;
	int found = 0;
	for (uint32_t i = 0; i < ext->dense_len; i++)
	{
		if (ext->elems[i].kind != AVM2_VALUE_HOLE)
		{
			last_non_hole = i + 1;
			found = 1;
		}
	}
	uint32_t span = found ? last_non_hole
	                      : (ext->dense_len > 0 ? ext->dense_len - 1 : 0);
	for (uint32_t i = 0; i < span / 2; i++)
	{
		Avm2Value t = ext->elems[i];
		ext->elems[i] = ext->elems[span - 1 - i];
		ext->elems[span - 1 - i] = t;
	}
	return act->this_val;
}

static Avm2Value array_concat(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Object* out = avm2_array_new(ctx, 0);
	Avm2Object* arr = this_array(act);
	if (arr != NULL)
	{
		Avm2ArrayExt* ext = avm2_array_ext(arr);
		for (uint32_t i = 0; i < ext->dense_len; i++)
		{
			avm2_array_push(ctx, out, ext->elems[i]);
		}
	}
	for (uint32_t a = 0; a < act->argc; a++)
	{
		Avm2Value v = act->args[a];
		Avm2ArrayExt* aext = (v.kind == AVM2_VALUE_OBJECT)
			? avm2_array_ext(v.u.obj) : NULL;
		if (aext != NULL)
		{
			for (uint32_t i = 0; i < aext->dense_len; i++)
			{
				avm2_array_push(ctx, out, aext->elems[i]);
			}
		}
		else
		{
			avm2_array_push(ctx, out, v);
		}
	}
	return avm2_object_value(out);
}

static Avm2Value array_slice(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Object* arr = this_array(act);
	Avm2Object* out = avm2_array_new(ctx, 0);
	if (arr == NULL) return avm2_object_value(out);
	Avm2ArrayExt* ext = avm2_array_ext(arr);
	double s = act->argc > 0 ? avm2_coerce_to_number(ctx, act->args[0]) : 0.0;
	double e = (act->argc > 1 && act->args[1].kind != AVM2_VALUE_UNDEFINED)
		? avm2_coerce_to_number(ctx, act->args[1]) : (double) 0xFFFFFFFFu;
	uint32_t start = wrap_index(s, ext->length);
	uint32_t end = wrap_index(e, ext->length);
	if (end > ext->dense_len) end = ext->dense_len;
	for (uint32_t i = start; i < end; i++)
	{
		avm2_array_push(ctx, out, resolve_hole(arr, i, ext->elems[i]));
	}
	return avm2_object_value(out);
}

static Avm2Value array_splice(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Object* arr = this_array(act);
	Avm2Object* removed = avm2_array_new(ctx, 0);
	if (arr == NULL)
	{
		if (act->argc > 0) sealed_generic_write(act);
		return avm2_undefined();
	}
	if (act->argc == 0) return avm2_undefined();
	Avm2ArrayExt* ext = avm2_array_ext(arr);
	uint32_t len = ext->dense_len;
	uint32_t start = wrap_index(avm2_coerce_to_number(ctx, act->args[0]), len);
	uint32_t del;
	if (act->argc > 1)
	{
		double d = avm2_coerce_to_number(ctx, act->args[1]);
		if (isnan(d) || d < 0.0) del = 0;
		else if ((double) start + d > (double) len) del = len - start;
		else del = (uint32_t) d;
	}
	else
	{
		del = len - start;
	}
	uint32_t ins = act->argc > 2 ? act->argc - 2 : 0;
	// Removing elements means reading them back for the result array, which
	// is the generic path; inserting only (delCount 0) is not.
	sealed_generic_read(act, arr, del);

	// Resolve holes across the tail before mutating (Ruffle splice).
	for (uint32_t i = start; i < len; i++)
	{
		ext->elems[i] = resolve_hole(arr, i, ext->elems[i]);
	}
	for (uint32_t i = 0; i < del; i++)
	{
		avm2_array_push(ctx, removed, ext->elems[start + i]);
	}
	uint32_t new_len = len - del + ins;
	if (new_len > ext->cap)
	{
		uint32_t new_cap = ext->cap == 0 ? 8 : ext->cap;
		while (new_cap < new_len) new_cap *= 2;
		Avm2Value* grown = avm2_alloc(ctx, new_cap * sizeof(Avm2Value));
		if (ext->dense_len > 0)
		{
			memcpy(grown, ext->elems, ext->dense_len * sizeof(Avm2Value));
		}
		if (ext->elems != NULL) heap_free(ctx->app, ext->elems);  // sole owner (see array_reserve)
		ext->elems = grown;
		ext->cap = new_cap;
	}
	memmove(ext->elems + start + ins, ext->elems + start + del,
	        (len - start - del) * sizeof(Avm2Value));
	for (uint32_t i = 0; i < ins; i++)
	{
		ext->elems[start + i] = act->args[2 + i];
	}
	ext->dense_len = new_len;
	ext->length = ext->length - del + ins;
	return avm2_object_value(removed);
}

static Avm2Value array_index_of(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Object* arr = this_array(act);
	if (arr == NULL || act->argc == 0) return avm2_integer(-1);
	Avm2ArrayExt* ext = avm2_array_ext(arr);
	sealed_generic_read(act, arr, ext->length);
	double fromf = act->argc > 1 ? avm2_coerce_to_number(ctx, act->args[1]) : 0.0;
	uint32_t from = wrap_index(fromf, ext->length);
	for (uint32_t i = from; i < ext->dense_len; i++)
	{
		Avm2Value v = resolve_hole(arr, i, ext->elems[i]);
		if (avm2_strict_eq(v, act->args[0]))
		{
			return avm2_integer((int32_t) i);
		}
	}
	for (Avm2SparseElem* sp = ext->sparse; sp != NULL; sp = sp->next)
	{
		if (sp->idx >= from && avm2_strict_eq(sp->v, act->args[0]))
		{
			return avm2_uint_value(sp->idx);
		}
	}
	return avm2_integer(-1);
}

static Avm2Value array_last_index_of(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Object* arr = this_array(act);
	if (arr == NULL || act->argc == 0) return avm2_integer(-1);
	Avm2ArrayExt* ext = avm2_array_ext(arr);
	if (ext->length == 0) return avm2_integer(-1);
	sealed_generic_read(act, arr, ext->length);
	uint32_t from = ext->length - 1;
	if (act->argc > 1)
	{
		double f = avm2_coerce_to_number(ctx, act->args[1]);
		if (f < 0.0)
		{
			// Ruffle: length.saturating_sub(-from) — clamps to 0.
			double w = (double) ext->length + trunc(f);
			from = w < 0.0 ? 0 : (uint32_t) w;
		}
		else if (trunc(f) < (double) ext->length)
		{
			from = (uint32_t) trunc(f);
		}
	}
	// Sparse hits above the dense region win (higher indices first).
	{
		Avm2SparseElem* best = NULL;
		for (Avm2SparseElem* sp = ext->sparse; sp != NULL; sp = sp->next)
		{
			if (sp->idx <= from && avm2_strict_eq(sp->v, act->args[0])) best = sp;
		}
		if (best != NULL) return avm2_uint_value(best->idx);
	}
	int64_t dense_from = from < ext->dense_len ? (int64_t) from
	                                           : (int64_t) ext->dense_len - 1;
	for (int64_t i = dense_from; i >= 0; i--)
	{
		Avm2Value v = resolve_hole(arr, (uint32_t) i, ext->elems[i]);
		if (avm2_strict_eq(v, act->args[0]))
		{
			return avm2_integer((int32_t) i);
		}
	}
	return avm2_integer(-1);
}


// FP19+ mutation helpers.
//
// insertAt/removeAt shift the dense storage, which moves every hole in the
// tail to a new index. A hole reads through to Array.prototype[index], so the
// shift must not carry it along: bake each tail hole down to the value it has
// NOW, before the memmove. array_splice has had this pass since it was
// written; these two never did.
static void resolve_tail_holes(Avm2Object* arr, Avm2ArrayExt* ext, uint32_t from)
{
	for (uint32_t i = from; i < ext->dense_len; i++)
	{
		ext->elems[i] = resolve_hole(arr, i, ext->elems[i]);
	}
}

static Avm2Value array_insert_at(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Object* arr = this_array(act);
	if (arr == NULL || act->argc == 0) return avm2_undefined();
	Avm2ArrayExt* ext = avm2_array_ext(arr);
	double idxf = avm2_coerce_to_number(ctx, act->args[0]);
	uint32_t idx = wrap_index(idxf, ext->dense_len);
	Avm2Value v = act->argc > 1 ? act->args[1] : avm2_undefined();
	// Resolve holes across the shifted tail before mutating, exactly as
	// array_splice does: after the memmove a hole would fall through to the
	// prototype at its NEW index, so `insertAt` and `splice` would disagree
	// on a sparse array (from_avmplus/as3/Array/insertremove compares the
	// two element by element).
	resolve_tail_holes(arr, ext, idx);
	uint32_t need = ext->dense_len + 1;
	if (need > ext->cap)
	{
		uint32_t new_cap = ext->cap == 0 ? 8 : ext->cap;
		while (new_cap < need) new_cap *= 2;
		Avm2Value* grown = avm2_alloc(ctx, new_cap * sizeof(Avm2Value));
		if (ext->dense_len > 0)
		{
			memcpy(grown, ext->elems, ext->dense_len * sizeof(Avm2Value));
		}
		if (ext->elems != NULL) heap_free(ctx->app, ext->elems);  // sole owner (see array_reserve)
		ext->elems = grown;
		ext->cap = new_cap;
	}
	memmove(ext->elems + idx + 1, ext->elems + idx,
	        (ext->dense_len - idx) * sizeof(Avm2Value));
	ext->elems[idx] = v;
	ext->dense_len++;
	ext->length++;
	return avm2_undefined();
}

static Avm2Value array_remove_at(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Object* arr = this_array(act);
	if (arr == NULL || act->argc == 0) return avm2_undefined();
	Avm2ArrayExt* ext = avm2_array_ext(arr);
	double idxf = avm2_coerce_to_number(ctx, act->args[0]);
	if (idxf >= (double) ext->dense_len) return avm2_undefined();
	uint32_t idx = wrap_index(idxf, ext->dense_len);
	if (idx >= ext->dense_len) return avm2_undefined();
	// Same hole-resolution pass as insertAt/splice (see above).
	resolve_tail_holes(arr, ext, idx);
	Avm2Value v = ext->elems[idx];
	memmove(ext->elems + idx, ext->elems + idx + 1,
	        (ext->dense_len - idx - 1) * sizeof(Avm2Value));
	ext->dense_len--;
	if (ext->length > 0) ext->length--;
	return v.kind == AVM2_VALUE_HOLE ? avm2_undefined() : v;
}

// ---------------------------------------------------------------------------
// Iteration methods
// ---------------------------------------------------------------------------

// callback:Function parameter (Ruffle try_get_function): null/undefined →
// no callback (each method returns its neutral value), non-callable → 1034.
static int array_callback_arg(Avm2Context* ctx, Avm2Value v, Avm2Value* out)
{
	if (v.kind == AVM2_VALUE_NULL || v.kind == AVM2_VALUE_UNDEFINED) return 0;
	if (v.kind == AVM2_VALUE_OBJECT && v.u.obj->kind == AVM2_OBJ_FUNCTION)
	{
		*out = v;
		return 1;
	}
	const Avm2String* s = avm2_coerce_to_string(ctx, v);
	avm2_throw_error(ctx, ctx->builtins.type_error_class,
	                 "Error #1034: Type Coercion failed: cannot convert %.*s to "
	                 "Function.", s->len > 100 ? 100 : (int) s->len, s->utf8);
}

static Avm2Value call_cb(Avm2Activation* act, Avm2Value cb, Avm2Value recv,
                         Avm2Value item, uint32_t index)
{
	Avm2Value args[3];
	args[0] = item;
	args[1] = avm2_uint_value(index);
	args[2] = act->this_val;
	return avm2_call_value(act->ctx, cb, recv, args, 3);
}

static Avm2Value array_for_each(Avm2Activation* act)
{
	Avm2Object* arr = this_array(act);
	if (arr == NULL) return avm2_undefined();
	sealed_generic_read(act, arr, avm2_array_ext(arr)->length);
	Avm2Value cb;
	if (!array_callback_arg(act->ctx, arg_or_undef(act, 0), &cb))
	{
		return avm2_undefined();
	}
	Avm2Value recv = arg_or_undef(act, 1);
	for (uint32_t i = 0; i < avm2_array_ext(arr)->dense_len; i++)
	{
		call_cb(act, cb, recv, elem_or_undef(arr, i), i);
	}
	return avm2_undefined();
}

static Avm2Value array_map(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Object* arr = this_array(act);
	Avm2Object* out = avm2_array_new(ctx, 0);
	if (arr == NULL) return avm2_object_value(out);
	sealed_generic_read(act, arr, avm2_array_ext(arr)->length);
	Avm2Value cb;
	if (!array_callback_arg(ctx, arg_or_undef(act, 0), &cb))
	{
		return avm2_object_value(out);
	}
	Avm2Value recv = arg_or_undef(act, 1);
	for (uint32_t i = 0; i < avm2_array_ext(arr)->dense_len; i++)
	{
		avm2_array_push(ctx, out, call_cb(act, cb, recv, elem_or_undef(arr, i), i));
	}
	return avm2_object_value(out);
}

static Avm2Value array_filter(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Object* arr = this_array(act);
	Avm2Object* out = avm2_array_new(ctx, 0);
	if (arr == NULL) return avm2_object_value(out);
	sealed_generic_read(act, arr, avm2_array_ext(arr)->length);
	Avm2Value cb;
	if (!array_callback_arg(ctx, arg_or_undef(act, 0), &cb))
	{
		return avm2_object_value(out);
	}
	Avm2Value recv = arg_or_undef(act, 1);
	for (uint32_t i = 0; i < avm2_array_ext(arr)->dense_len; i++)
	{
		Avm2Value item = elem_or_undef(arr, i);
		if (avm2_coerce_to_boolean(call_cb(act, cb, recv, item, i)))
		{
			avm2_array_push(ctx, out, item);
		}
	}
	return avm2_object_value(out);
}

static Avm2Value array_every(Avm2Activation* act)
{
	Avm2Object* arr = this_array(act);
	if (arr == NULL) return avm2_bool(true);
	sealed_generic_read(act, arr, avm2_array_ext(arr)->length);
	Avm2Value cb;
	if (!array_callback_arg(act->ctx, arg_or_undef(act, 0), &cb))
	{
		return avm2_bool(true);
	}
	Avm2Value recv = arg_or_undef(act, 1);
	for (uint32_t i = 0; i < avm2_array_ext(arr)->dense_len; i++)
	{
		if (!avm2_coerce_to_boolean(call_cb(act, cb, recv, elem_or_undef(arr, i), i)))
		{
			return avm2_bool(false);
		}
	}
	return avm2_bool(true);
}

static Avm2Value array_some(Avm2Activation* act)
{
	Avm2Object* arr = this_array(act);
	if (arr == NULL) return avm2_bool(false);
	sealed_generic_read(act, arr, avm2_array_ext(arr)->length);
	Avm2Value cb;
	if (!array_callback_arg(act->ctx, arg_or_undef(act, 0), &cb))
	{
		return avm2_bool(false);
	}
	Avm2Value recv = arg_or_undef(act, 1);
	for (uint32_t i = 0; i < avm2_array_ext(arr)->dense_len; i++)
	{
		if (avm2_coerce_to_boolean(call_cb(act, cb, recv, elem_or_undef(arr, i), i)))
		{
			return avm2_bool(true);
		}
	}
	return avm2_bool(false);
}

// ---------------------------------------------------------------------------
// Sorting — exact port of Ruffle globals/array.rs (avmplus qsort, hole
// resolution, undefined-last, UNIQUESORT/RETURNINDEXEDARRAY postprocess,
// SWF-version compare quirks)
// ---------------------------------------------------------------------------

typedef Avm2SortItem SortItem;

typedef struct SortCtx
{
	Avm2Activation* act;
	Avm2Value compare_fn;       // undefined = default compare
	uint32_t options;           // flags for the single-comparator path
	// sortOn fields:
	const Avm2String** fields;
	uint32_t* field_opts;
	uint32_t field_count;
	int unique_satisfied;
} SortCtx;

static int cmp_string_values(Avm2Context* ctx, Avm2Value a, Avm2Value b, int case_insensitive)
{
	const Avm2String* x = avm2_coerce_to_string(ctx, a);
	const Avm2String* y = avm2_coerce_to_string(ctx, b);
	uint32_t n = x->len < y->len ? x->len : y->len;
	if (!case_insensitive)
	{
		int c = memcmp(x->utf8, y->utf8, n);
		if (c != 0) return c < 0 ? -1 : 1;
	}
	else
	{
		for (uint32_t i = 0; i < n; i++)
		{
			char cx = x->utf8[i];
			char cy = y->utf8[i];
			if (cx >= 'A' && cx <= 'Z') cx += 32;
			if (cy >= 'A' && cy <= 'Z') cy += 32;
			if (cx != cy) return cx < cy ? -1 : 1;
		}
	}
	return x->len < y->len ? -1 : (x->len > y->len ? 1 : 0);
}

// Ruffle Value::normalize: canonicalize to Integer only when the value
// fits avmplus's 29-bit atom integer range (fits_in_value_integer_i32).
static int normalize_to_i32(Avm2Value v, int32_t* out)
{
	if (v.kind == AVM2_VALUE_INTEGER)
	{
		if (v.u.i < (1 << 28) && v.u.i >= -(1 << 28))
		{
			*out = v.u.i;
			return 1;
		}
		return 0;
	}
	if (v.kind == AVM2_VALUE_NUMBER)
	{
		int32_t i = (int32_t) v.u.d;
		if (v.u.d == (double) i && !(v.u.d == 0.0 && signbit(v.u.d))
		    && i < (1 << 28) && i >= -(1 << 28))
		{
			*out = i;
			return 1;
		}
	}
	return 0;
}

// compare_numeric with the SWF<11 avmplus atom-overflow quirk.
static int cmp_numeric_values(Avm2Context* ctx, Avm2Value a, Avm2Value b, int compat)
{
	int32_t ia;
	int32_t ib;
	if (compat && normalize_to_i32(a, &ia) && normalize_to_i32(b, &ib))
	{
		int32_t x = (int32_t) (((uint32_t) ia << 3) | 6u);
		int32_t y = (int32_t) (((uint32_t) ib << 3) | 6u);
		int32_t diff = (int32_t) ((uint32_t) x - (uint32_t) y);
		return diff < 0 ? -1 : (diff > 0 ? 1 : 0);
	}
	double x = avm2_coerce_to_number(ctx, a);
	double y = avm2_coerce_to_number(ctx, b);
	if (isnan(x) && isnan(y)) return 0;
	if (isnan(x)) return 1;
	if (isnan(y)) return -1;
	return x < y ? -1 : (x > y ? 1 : 0);
}

// The wrapped comparator (Ruffle sort_inner's closure).
static int sort_cmp(void* ud, const SortItem* pa, const SortItem* pb)
{
	SortCtx* sc = ud;
	Avm2Context* ctx = sc->act->ctx;
	Avm2Value a = pa->v;
	Avm2Value b = pb->v;
	int au = a.kind == AVM2_VALUE_UNDEFINED;
	int bu = b.kind == AVM2_VALUE_UNDEFINED;
	if (au && bu)
	{
		sc->unique_satisfied = 0;
		return 0;
	}
	if (au) return 1;
	if (bu) return -1;

	int cmp;
	if (sc->field_count > 0)
	{
		// sortOn: fields in order; per-field DESCENDING applies inside.
		cmp = 0;
		for (uint32_t f = 0; f < sc->field_count; f++)
		{
			Avm2Value fa = avm2_get_public_property(ctx, a, sc->fields[f]->utf8,
			                                        sc->fields[f]->len, NULL);
			Avm2Value fb = avm2_get_public_property(ctx, b, sc->fields[f]->utf8,
			                                        sc->fields[f]->len, NULL);
			uint32_t fo = sc->field_opts[f];
			int c;
			if (fo & 16)
			{
				c = cmp_numeric_values(ctx, fa, fb, ctx->swf_version < 11);
			}
			else if (fo & 1)
			{
				c = cmp_string_values(ctx, fa, fb, 1);
			}
			else
			{
				c = cmp_string_values(ctx, fa, fb, 0);
			}
			if (c == 0) continue;
			return (fo & 2) ? -c : c;
		}
		sc->unique_satisfied = 0;
		return 0;
	}
	if (sc->compare_fn.kind != AVM2_VALUE_UNDEFINED)
	{
		Avm2Value args[2] = { a, b };
		Avm2Value r = avm2_call_value(ctx, sc->compare_fn, avm2_null(), args, 2);
		if (ctx->swf_version < 13)
		{
			// avmplus bug 532454: result coerced to int for SWF<13.
			int32_t order = avm2_coerce_to_i32(ctx, r);
			cmp = order < 0 ? -1 : (order > 0 ? 1 : 0);
		}
		else
		{
			double order = avm2_coerce_to_number(ctx, r);
			cmp = order < 0.0 ? -1 : (order > 0.0 ? 1 : 0);
		}
	}
	else if (sc->options & 16)
	{
		cmp = cmp_numeric_values(ctx, a, b, ctx->swf_version < 11);
	}
	else if (sc->options & 1)
	{
		cmp = cmp_string_values(ctx, a, b, 1);
	}
	else
	{
		cmp = cmp_string_values(ctx, a, b, 0);
	}

	if (cmp == 0)
	{
		sc->unique_satisfied = 0;
		return 0;
	}
	if (sc->options & 2) return -cmp;  // DESCENDING
	return cmp;
}

static void sort_swap(SortItem* a, SortItem* b)
{
	SortItem t = *a;
	*a = *b;
	*b = t;
}

// Port of Ruffle's avmplus QuickSort (globals/array.rs qsort). Shared with
// Vector.sort (avm2_vector.c) — Ruffle exports it for exactly that reason.
void avm2_avmplus_qsort(void* ud, Avm2SortCmp cmp, Avm2SortItem* s, uint32_t n)
{
	if (n < 2) return;
	if (n == 2)
	{
		if (cmp(ud, &s[0], &s[1]) > 0) sort_swap(&s[0], &s[1]);
		return;
	}
	if (n == 3)
	{
		if (cmp(ud, &s[0], &s[1]) > 0) sort_swap(&s[0], &s[1]);
		if (cmp(ud, &s[1], &s[2]) > 0)
		{
			sort_swap(&s[1], &s[2]);
			if (cmp(ud, &s[0], &s[1]) > 0) sort_swap(&s[0], &s[1]);
		}
		return;
	}

	sort_swap(&s[0], &s[n / 2]);
	uint32_t left = 0;
	uint32_t right = n;
	for (;;)
	{
		do
		{
			left++;
		} while (!(left >= n || cmp(ud, &s[left], &s[0]) > 0));
		do
		{
			right--;
		} while (!(right == 0 || cmp(ud, &s[right], &s[0]) < 0));
		if (right < left) break;
		sort_swap(&s[left], &s[right]);
	}
	sort_swap(&s[0], &s[right]);

	// [..right] lower, [right..left] equal-to-pivot, [left..] higher.
	SortItem* fst = s;
	uint32_t fst_n = right;
	SortItem* snd = s + left;
	uint32_t snd_n = n - left;
	if (fst_n >= snd_n)
	{
		SortItem* t = fst; fst = snd; snd = t;
		uint32_t tn = fst_n; fst_n = snd_n; snd_n = tn;
	}
	avm2_avmplus_qsort(ud, cmp, fst, fst_n);
	avm2_avmplus_qsort(ud, cmp, snd, snd_n);
}

static Avm2Value sort_apply(Avm2Activation* act, SortCtx* sc)
{
	Avm2Context* ctx = act->ctx;
	Avm2Object* arr = this_array(act);
	// avmplus's sort returns the receiver, so `String(b.sort(...))` on a
	// storage-less sealed subclass stringifies an empty array.
	if (arr == NULL) return act->this_val;
	Avm2ArrayExt* ext = avm2_array_ext(arr);
	uint32_t len = ext->dense_len;
	sealed_generic_read(act, arr, ext->length);

	// values = (index, hole-resolved value) — Ruffle extract_array_values.
	SortItem* items = avm2_alloc(ctx, (len + 1) * sizeof(SortItem));
	uint8_t* was_hole = avm2_alloc(ctx, len + 1);
	for (uint32_t i = 0; i < len; i++)
	{
		items[i].idx = i;
		items[i].v = resolve_hole(arr, i, ext->elems[i]);
		was_hole[i] = (ext->elems[i].kind == AVM2_VALUE_HOLE);
	}

	sc->unique_satisfied = 1;
	avm2_avmplus_qsort(sc, sort_cmp, items, len);

	// sort_postprocess.
	if ((sc->options & 4) && !sc->unique_satisfied)
	{
		heap_free(ctx->app, items);
		heap_free(ctx->app, was_hole);
		return avm2_integer(0);
	}
	if (sc->options & 8)  // RETURNINDEXEDARRAY
	{
		Avm2Object* out = avm2_array_new(ctx, 0);
		for (uint32_t i = 0; i < len; i++)
		{
			avm2_array_push(ctx, out, avm2_uint_value(items[i].idx));
		}
		heap_free(ctx->app, items);
		heap_free(ctx->app, was_hole);
		return avm2_object_value(out);
	}
	Avm2Value* new_elems = avm2_alloc(ctx, (len + 1) * sizeof(Avm2Value));
	for (uint32_t i = 0; i < len; i++)
	{
		if (!was_hole[items[i].idx])
		{
			new_elems[i] = ext->elems[items[i].idx];
		}
		else if (items[i].v.kind != AVM2_VALUE_UNDEFINED)
		{
			new_elems[i] = items[i].v;  // proto-resolved hole materializes
		}
		else
		{
			new_elems[i].kind = AVM2_VALUE_HOLE;
			new_elems[i]._pad = 0;
			new_elems[i].u.obj = NULL;
		}
	}
	memcpy(ext->elems, new_elems, len * sizeof(Avm2Value));
	heap_free(ctx->app, items);
	heap_free(ctx->app, was_hole);
	heap_free(ctx->app, new_elems);
	return act->this_val;
}

static Avm2Value array_sort(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	SortCtx sc;
	memset(&sc, 0, sizeof(sc));
	sc.act = act;
	sc.compare_fn = avm2_undefined();
	if (act->argc > 1)
	{
		sc.compare_fn = act->args[0];
		sc.options = avm2_coerce_to_u32(ctx, act->args[1]) & 0xFF;
	}
	else if (act->argc == 1)
	{
		if (act->args[0].kind == AVM2_VALUE_OBJECT
		    && (act->args[0].u.obj->kind == AVM2_OBJ_FUNCTION
		        || act->args[0].u.obj->kind == AVM2_OBJ_CLASS))
		{
			sc.compare_fn = act->args[0];
		}
		else
		{
			sc.options = avm2_coerce_to_u32(ctx, act->args[0]) & 0xFF;
		}
	}
	return sort_apply(act, &sc);
}

static Avm2Value array_sort_on(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	SortCtx sc;
	memset(&sc, 0, sizeof(sc));
	sc.act = act;
	sc.compare_fn = avm2_undefined();
	if (act->argc == 0) return avm2_integer(0);

	// Field name(s).
	const Avm2String* fields_buf[32];
	uint32_t opts_buf[32];
	sc.fields = fields_buf;
	sc.field_opts = opts_buf;

	Avm2ArrayExt* names = (act->args[0].kind == AVM2_VALUE_OBJECT)
		? avm2_array_ext(act->args[0].u.obj) : NULL;
	if (names != NULL)
	{
		for (uint32_t i = 0; i < names->dense_len && i < 32; i++)
		{
			Avm2Value nv = names->elems[i].kind == AVM2_VALUE_HOLE
				? avm2_undefined() : names->elems[i];
			fields_buf[sc.field_count] = avm2_coerce_to_string(ctx, nv);
			opts_buf[sc.field_count] = 0;
			sc.field_count++;
		}
	}
	else
	{
		fields_buf[0] = avm2_coerce_to_string(ctx, act->args[0]);
		opts_buf[0] = 0;
		sc.field_count = 1;
	}

	// Options: single value or per-field array; missing entries repeat the
	// last one (Ruffle resize semantics).
	if (act->argc > 1)
	{
		Avm2ArrayExt* opts = (act->args[1].kind == AVM2_VALUE_OBJECT)
			? avm2_array_ext(act->args[1].u.obj) : NULL;
		if (opts != NULL)
		{
			uint32_t last = 0;
			for (uint32_t i = 0; i < sc.field_count; i++)
			{
				if (i < opts->dense_len && opts->elems[i].kind != AVM2_VALUE_HOLE)
				{
					last = avm2_coerce_to_u32(ctx, opts->elems[i]) & 0xFF;
				}
				opts_buf[i] = last;
			}
		}
		else
		{
			uint32_t o = avm2_coerce_to_u32(ctx, act->args[1]) & 0xFF;
			for (uint32_t i = 0; i < sc.field_count; i++) opts_buf[i] = o;
		}
	}
	// The postprocess flags come from the FIRST option only.
	sc.options = (sc.field_count > 0 ? opts_buf[0] : 0) & (4 | 8);
	return sort_apply(act, &sc);
}

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

// `new Array(...)` semantics applied to an already-allocated array object.
// Shared by array_construct and array_super_init, so a SWF subclass's
// `super(length)` behaves exactly like `new Array(length)`
// (as3/Array/length_mods's Subarray passes its ctor arg straight through).
static void array_init_ctor_args(Avm2Context* ctx, Avm2Object* obj,
                                 const Avm2Value* args, uint32_t argc)
{
	if (argc == 1 && avm2_value_is_number(args[0]))
	{
		double d = avm2_coerce_to_number(ctx, args[0]);
		uint32_t n = (uint32_t) d;
		if ((double) n != d)
		{
			avm2_throw_error(ctx, ctx->builtins.range_error_class,
			                 "Error #1005: Array index is not a positive integer (%s).",
			                 avm2_coerce_to_string(ctx, args[0])->utf8);
		}
		avm2_array_set_length(ctx, obj, n);
		return;
	}
	for (uint32_t i = 0; i < argc; i++)
	{
		avm2_array_set(ctx, obj, i, args[i]);
	}
}

static Avm2Value array_construct(Avm2Context* ctx, Avm2Class* cls,
                                 const Avm2Value* args, uint32_t argc)
{
	(void) cls;
	if (argc == 1 && avm2_value_is_number(args[0]))
	{
		double d = avm2_coerce_to_number(ctx, args[0]);
		uint32_t n = (uint32_t) d;
		if ((double) n != d)
		{
			avm2_throw_error(ctx, ctx->builtins.range_error_class,
			                 "Error #1005: Array index is not a positive integer (%s).",
			                 avm2_coerce_to_string(ctx, args[0])->utf8);
		}
		return avm2_object_value(avm2_array_new(ctx, n));
	}
	return avm2_object_value(avm2_array_from_values(ctx, args, argc));
}

// ConstructSuper into Array from a SWF subclass: the receiver is already
// allocated (with element storage, when the subclass is not sealed), so run
// Array's constructor semantics on it in place. A sealed subclass has no
// storage and avm2_array_ext returns NULL — nothing to initialize.
static void array_super_init(Avm2Context* ctx, Avm2Object* obj,
                             const Avm2Value* args, uint32_t argc)
{
	if (avm2_array_ext(obj) == NULL) return;
	array_init_ctor_args(ctx, obj, args, argc);
}

void avm2_register_array(Avm2Context* ctx)
{
	Avm2Builtins* b = &ctx->builtins;
	Avm2Class* cls = avm2_builtin_class(ctx, "", "Array", b->object_class);
	b->array_class = cls;
	cls->native_construct = array_construct;
	cls->native_call = array_construct;
	// Subclasses inherit element storage + `super(...)` semantics.
	cls->instance_kind = AVM2_OBJ_ARRAY;
	cls->native_super_init = array_super_init;

	// length is a getter/setter pair.
	{
		Avm2PropEntry e;
		memset(&e, 0, sizeof(e));
		e.key = avm2_public_key("length", 6);
		e.kind = AVM2_PROP_GETSET;
		e.method.fn = array_get_length;
		e.method.debug_name = "get length";
		e.setter.fn = array_set_length;
		e.setter.debug_name = "set length";
		avm2_vtable_append(ctx, &cls->ivtable, &e);
	}
	avm2_builtin_add_method_n(ctx, cls, "join", array_join, 1);
	// No public `toString` trait: in avmplus Array's is AS3-namespaced, with a
	// *public prototype function* alongside it, so the public name is shadowable.
	// ecma3/Array/e15_4_1_1 assigns `arr.toString = Object.prototype.toString`
	// on an instance and e15_4_4 does the same on Array.prototype; both expect
	// the write to land (a public method trait here would throw #1037) and the
	// assigned function to win the subsequent call. The prototype entries added
	// at the bottom of this function keep ordinary `arr.toString()` working.
	avm2_builtin_add_method(ctx, cls, "toLocaleString", array_to_locale_string);
	avm2_builtin_add_method(ctx, cls, "push", array_push);
	avm2_builtin_add_method(ctx, cls, "pop", array_pop);
	avm2_builtin_add_method(ctx, cls, "shift", array_shift);
	avm2_builtin_add_method(ctx, cls, "unshift", array_unshift);
	avm2_builtin_add_method(ctx, cls, "reverse", array_reverse);
	avm2_builtin_add_method(ctx, cls, "concat", array_concat);
	avm2_builtin_add_method_n(ctx, cls, "slice", array_slice, 2);
	avm2_builtin_add_method(ctx, cls, "splice", array_splice);
	avm2_builtin_add_method_n(ctx, cls, "indexOf", array_index_of, 2);
	avm2_builtin_add_method_n(ctx, cls, "lastIndexOf", array_last_index_of, 2);
	avm2_builtin_add_method_n(ctx, cls, "forEach", array_for_each, 2);
	avm2_builtin_add_method_n(ctx, cls, "map", array_map, 2);
	avm2_builtin_add_method_n(ctx, cls, "filter", array_filter, 2);
	avm2_builtin_add_method_n(ctx, cls, "every", array_every, 2);
	avm2_builtin_add_method_n(ctx, cls, "some", array_some, 2);
	avm2_builtin_add_method(ctx, cls, "sort", array_sort);
	avm2_builtin_add_method_n(ctx, cls, "sortOn", array_sort_on, 2);
	avm2_builtin_add_method(ctx, cls, "insertAt", array_insert_at);
	avm2_builtin_add_method(ctx, cls, "removeAt", array_remove_at);

	avm2_builtin_add_static_const(ctx, cls, "CASEINSENSITIVE", avm2_integer(1));
	avm2_builtin_add_static_const(ctx, cls, "DESCENDING", avm2_integer(2));
	avm2_builtin_add_static_const(ctx, cls, "UNIQUESORT", avm2_integer(4));
	avm2_builtin_add_static_const(ctx, cls, "RETURNINDEXEDARRAY", avm2_integer(8));
	avm2_builtin_add_static_const(ctx, cls, "NUMERIC", avm2_integer(16));

	// ES3-compat layer on Array.prototype (Ruffle globals/Array.as). Ruffle
	// omits insertAt/removeAt here — they are AS3-only additions, not ES3.
	Avm2Object* proto = cls->prototype_obj;

	// avmplus builds each builtin's prototype as an instance of that class, so
	// `Array.prototype` is a genuine empty Array: `Array.prototype.length` is 0
	// (not undefined) and Object.prototype.toString.call(it) says
	// "[object Array]" (ecma3/Array/e15_4_4). Retype the object in place -- it
	// is already GC-pinned, already carries `constructor`, and its proto link
	// to Object.prototype stays put.
	proto->kind = AVM2_OBJ_ARRAY;
	proto->cls = cls;
	proto->vtable = &cls->ivtable;
	Avm2ArrayExt* proto_ext = avm2_alloc(ctx, sizeof(Avm2ArrayExt));
	memset(proto_ext, 0, sizeof(Avm2ArrayExt));
	proto->native_ext = proto_ext;

	avm2_proto_add_function_n(ctx, proto, "concat", array_concat, 0);
	avm2_proto_add_function_n(ctx, proto, "every", array_every, 2);
	avm2_proto_add_function_n(ctx, proto, "filter", array_filter, 2);
	avm2_proto_add_function_n(ctx, proto, "forEach", array_for_each, 2);
	avm2_proto_add_function_n(ctx, proto, "indexOf", array_index_of, 2);
	avm2_proto_add_function_n(ctx, proto, "join", array_join, 1);
	avm2_proto_add_function_n(ctx, proto, "lastIndexOf", array_last_index_of, 2);
	avm2_proto_add_function_n(ctx, proto, "map", array_map, 2);
	avm2_proto_add_function_n(ctx, proto, "pop", array_pop, 0);
	avm2_proto_add_function_n(ctx, proto, "push", array_push, 0);
	avm2_proto_add_function_n(ctx, proto, "reverse", array_reverse, 0);
	avm2_proto_add_function_n(ctx, proto, "shift", array_shift, 0);
	avm2_proto_add_function_n(ctx, proto, "slice", array_slice, 2);
	avm2_proto_add_function_n(ctx, proto, "some", array_some, 2);
	avm2_proto_add_function_n(ctx, proto, "sort", array_sort, 0);
	avm2_proto_add_function_n(ctx, proto, "sortOn", array_sort_on, 2);
	avm2_proto_add_function_n(ctx, proto, "splice", array_splice, 0);
	avm2_proto_add_function_n(ctx, proto, "toLocaleString", array_to_locale_string, 0);
	avm2_proto_add_function_n(ctx, proto, "toString", array_to_string, 0);
	avm2_proto_add_function_n(ctx, proto, "unshift", array_unshift, 0);
}
