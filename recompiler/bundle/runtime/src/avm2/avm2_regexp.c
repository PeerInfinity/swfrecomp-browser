// RegExp builtin + the String regex paths (match/replace/search/split) —
// port of Ruffle core/src/avm2/regexp.rs, globals/reg_exp.rs, RegExp.as,
// and globals/string.rs, backed by QuickJS's libregexp
// (SWFModernRuntime/third_party/quickjs-libregexp, MIT).
//
// Subject strings are converted to UTF-16 (libregexp cbuf_type 1) so
// lastIndex/index/split offsets are UTF-16 code units, matching avmplus.
// AVM2 patterns are PCRE-flavored; a preprocessor converts the PCRE-isms
// the corpus uses ((?#...) comments, (?P<name>) named groups, /x extended
// whitespace) into ES syntax before lre_compile. A pattern that still
// fails to compile never matches (Ruffle's cached_regex = Err behavior).

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <avm2/avm2_class.h>
#include <avm2/avm2_error.h>
#include <avm2/avm2_globals.h>
#include <avm2/avm2_main.h>
#include <avm2/avm2_object.h>
#include <memory/heap.h>
#include <avm2/avm2_ops.h>

#include "libregexp.h"

// libregexp embedder hooks. Plain libc realloc: regex bytecode and interp
// state are short-lived and small; the o1heap allocator has no realloc.
void* lre_realloc(void* opaque, void* ptr, size_t size)
{
	(void) opaque;
	if (size == 0)
	{
		free(ptr);
		return NULL;
	}
	return realloc(ptr, size);
}

int lre_check_stack_overflow(void* opaque, size_t alloca_size)
{
	(void) opaque;
	(void) alloca_size;
	return 0;
}

int lre_check_timeout(void* opaque)
{
	(void) opaque;
	return 0;
}

// AVM2 flag bits (Ruffle RegExpFlags).
enum
{
	AVM2_RE_GLOBAL = 1 << 0,
	AVM2_RE_IGNORECASE = 1 << 1,
	AVM2_RE_MULTILINE = 1 << 2,
	AVM2_RE_DOTALL = 1 << 3,
	AVM2_RE_EXTENDED = 1 << 4,
};

typedef struct Avm2RegExpExt
{
	const Avm2String* source;
	uint32_t flags;
	uint32_t last_index;   // UTF-16 units
	uint8_t* bytecode;     // compiled libregexp program (NULL until compiled)
	uint8_t compile_tried;
	uint8_t compile_failed;
} Avm2RegExpExt;

static Avm2RegExpExt* regexp_ext(Avm2Value v)
{
	if (v.kind == AVM2_VALUE_OBJECT && v.u.obj != NULL
	    && v.u.obj->cls != NULL
	    && v.u.obj->cls == avm2_get_context()->builtins.regexp_class)
	{
		return (Avm2RegExpExt*) v.u.obj->native_ext;
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// UTF-8 ↔ UTF-16 subject text
// ---------------------------------------------------------------------------

typedef struct ReText
{
	uint32_t n16;
	uint16_t* units;
	uint32_t* u8off;  // [n16 + 1]: UTF-8 byte offset where each unit starts
	const Avm2String* src;
} ReText;

static void retext_build(Avm2Context* ctx, const Avm2String* s, ReText* t)
{
	t->src = s;
	t->units = avm2_alloc(ctx, (s->len + 1) * 2 * sizeof(uint16_t));
	t->u8off = avm2_alloc(ctx, (s->len * 2 + 2) * sizeof(uint32_t));
	uint32_t n = 0;
	uint32_t i = 0;
	const unsigned char* b = (const unsigned char*) s->utf8;
	while (i < s->len)
	{
		uint32_t cp = 0xFFFD;
		uint32_t start = i;
		unsigned char c = b[i];
		if (c < 0x80)
		{
			cp = c;
			i++;
		}
		else if ((c & 0xE0) == 0xC0 && i + 1 < s->len)
		{
			cp = ((uint32_t) (c & 0x1F) << 6) | (b[i + 1] & 0x3F);
			i += 2;
		}
		else if ((c & 0xF0) == 0xE0 && i + 2 < s->len)
		{
			cp = ((uint32_t) (c & 0x0F) << 12) | ((uint32_t) (b[i + 1] & 0x3F) << 6)
			     | (b[i + 2] & 0x3F);
			i += 3;
		}
		else if ((c & 0xF8) == 0xF0 && i + 3 < s->len)
		{
			cp = ((uint32_t) (c & 0x07) << 18) | ((uint32_t) (b[i + 1] & 0x3F) << 12)
			     | ((uint32_t) (b[i + 2] & 0x3F) << 6) | (b[i + 3] & 0x3F);
			i += 4;
		}
		else
		{
			i++;
		}
		if (cp >= 0x10000)
		{
			t->u8off[n] = start;
			t->units[n++] = (uint16_t) (0xD800 + ((cp - 0x10000) >> 10));
			t->u8off[n] = start;
			t->units[n++] = (uint16_t) (0xDC00 + ((cp - 0x10000) & 0x3FF));
		}
		else
		{
			t->u8off[n] = start;
			t->units[n++] = (uint16_t) cp;
		}
	}
	t->u8off[n] = s->len;
	t->n16 = n;
}

// The UTF-16 view is per-call scratch: every caller releases it (RT_RETURN)
// on its normal exits. It used to leak twice the subject's length per
// exec/test/match/search/replace/split call.
static void retext_free(Avm2Context* ctx, ReText* t)
{
	if (t->units != NULL) heap_free(ctx->app, t->units);
	if (t->u8off != NULL) heap_free(ctx->app, t->u8off);
	t->units = NULL;
	t->u8off = NULL;
}
#define RT_RETURN(v) do { Avm2Value rt_rv_ = (v); retext_free(ctx, &t); return rt_rv_; } while (0)

// Substring by UTF-16 range (clamped).
static const Avm2String* retext_sub(Avm2Context* ctx, const ReText* t,
                                    uint32_t start, uint32_t end)
{
	if (start > t->n16) start = t->n16;
	if (end > t->n16) end = t->n16;
	if (end < start) end = start;
	uint32_t b0 = t->u8off[start];
	uint32_t b1 = t->u8off[end];
	return avm2_string_new(ctx, t->src->utf8 + b0, b1 - b0);
}

// ---------------------------------------------------------------------------
// Compilation (with the PCRE-ism preprocessor)
// ---------------------------------------------------------------------------

static int is_x_space(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'
	       || c == '\v';
}

// Skip a character class starting at the '[' in s[i]; returns the index just
// past the closing ']' (or len).
static uint32_t re_skip_class(const char* s, uint32_t len, uint32_t i)
{
	i++;  // '['
	while (i < len && s[i] != ']')
	{
		if (s[i] == '\\' && i + 1 < len) i++;
		i++;
	}
	return (i < len) ? i + 1 : i;
}

// Count capturing groups and spot backreferences, using the same escape /
// character-class skipping as the emit loop below.
static uint32_t re_count_captures(const char* s, uint32_t len, int* has_backref)
{
	uint32_t caps = 0;
	*has_backref = 0;
	for (uint32_t i = 0; i < len; )
	{
		char c = s[i];
		if (c == '\\')
		{
			if (i + 1 < len)
			{
				char e = s[i + 1];
				if ((e >= '1' && e <= '9') || e == 'k') *has_backref = 1;
			}
			i += 2;
			continue;
		}
		if (c == '[')
		{
			i = re_skip_class(s, len, i);
			continue;
		}
		if (c != '(')
		{
			i++;
			continue;
		}
		i++;
		if (i >= len || s[i] != '?')
		{
			caps++;  // plain capturing group
			continue;
		}
		// Of the (?...) forms only the named-group ones capture. `(?<` is a
		// named group unless it is the `(?<=` / `(?<!` lookbehind.
		if (i + 2 < len && s[i + 1] == '<' && s[i + 2] != '=' && s[i + 2] != '!')
		{
			caps++;
		}
		else if (i + 2 < len && s[i + 1] == 'P' && s[i + 2] == '<')
		{
			caps++;
		}
	}
	return caps;
}

// ---------------------------------------------------------------------------
// Fixed-length lookbehind (Tamarin/PCRE) validation
//
// avmplus's PCRE requires a lookbehind to have a fixed length and reports a
// compile error otherwise, which surfaces to AS3 as a regex that never
// matches. libregexp implements ES2018 variable-length lookbehind, so we
// matched where avmplus does not. Reject exactly what PCRE rejects: every
// branch of the lookbehind must be fixed-length, but the lookbehind's OWN
// top-level branches may differ from one another; a *nested* group whose
// branches differ in length is an error, as is any inexact quantifier
// (`?`, `*`, `+`, `{n,}`, `{n,m}` with n != m).
//
// Pinned row by row from from_avmplus/recursion/pcre_find_fixedlength:
// `(?<=a{3}|b{2})` is legal (top-level branches 3 and 2), `(?<=(a{3}|b{2}))`
// is not, `(?<=(a{3}|b{2}b))` and `(?<=(x(a|b)c|bbb))` are (equal-length
// nested branches). Anything this scanner cannot model returns UNKNOWN and
// is left alone — under-rejecting only costs yield, over-rejecting breaks
// working patterns.
// ---------------------------------------------------------------------------

enum
{
	RE_LEN_VARIABLE = -2,  // proven non-fixed: PCRE would reject
	RE_LEN_UNKNOWN = -1,   // not modelled: leave the pattern alone
};

// Nesting depth past which we stop analysing (recursion budget; the WASM
// stack is small). Over-deep groups come back UNKNOWN.
#define RE_LB_MAX_DEPTH 200

typedef struct ReLbScan
{
	const char* s;
	uint32_t len;
	uint32_t i;
	int depth;
} ReLbScan;

static int re_lb_alt(ReLbScan* sc, int top);

// Consume through the ')' closing the group we are already inside, leaving i
// ON that ')'.
static void re_lb_skip_group(ReLbScan* sc)
{
	int depth = 1;
	while (sc->i < sc->len)
	{
		char c = sc->s[sc->i];
		if (c == '\\') { sc->i += 2; continue; }
		if (c == '[') { sc->i = re_skip_class(sc->s, sc->len, sc->i); continue; }
		if (c == '(') depth++;
		else if (c == ')' && --depth == 0) return;
		sc->i++;
	}
}

static int re_lb_hex(const char* s, uint32_t len, uint32_t i, uint32_t n)
{
	uint32_t k = 0;
	while (k < n && i + k < len)
	{
		char c = s[i + k];
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')
		      || (c >= 'A' && c <= 'F')))
		{
			break;
		}
		k++;
	}
	return (int) k;
}

// One atom plus its quantifier.
static int re_lb_atom(ReLbScan* sc)
{
	const char* s = sc->s;
	uint32_t len = sc->len;
	char c = s[sc->i];
	int alen;
	int quantifiable = 1;

	if (c == '(')
	{
		sc->i++;
		int assertion = 0;
		if (sc->i < len && s[sc->i] == '?')
		{
			if (sc->i + 1 < len && (s[sc->i + 1] == '=' || s[sc->i + 1] == '!'))
			{
				assertion = 1;
				sc->i += 2;
			}
			else if (sc->i + 2 < len && s[sc->i + 1] == '<'
			         && (s[sc->i + 2] == '=' || s[sc->i + 2] == '!'))
			{
				assertion = 1;
				sc->i += 3;
			}
			else if (sc->i + 1 < len && s[sc->i + 1] == ':')
			{
				sc->i += 2;
			}
			else if (sc->i + 1 < len && s[sc->i + 1] == '<')
			{
				sc->i += 2;  // named group (?<name>
				while (sc->i < len && s[sc->i] != '>') sc->i++;
				if (sc->i < len) sc->i++;
			}
			else
			{
				// (?i) and friends — not modelled.
				re_lb_skip_group(sc);
				if (sc->i < len) sc->i++;
				return RE_LEN_UNKNOWN;
			}
		}
		if (sc->depth >= RE_LB_MAX_DEPTH)
		{
			re_lb_skip_group(sc);
			if (sc->i < len) sc->i++;
			alen = RE_LEN_UNKNOWN;
		}
		else
		{
			sc->depth++;
			int inner = re_lb_alt(sc, 0);
			sc->depth--;
			if (sc->i < len && s[sc->i] == ')') sc->i++;
			// A nested assertion consumes nothing, so its content's length
			// never constrains ours.
			alen = assertion ? 0 : inner;
		}
	}
	else if (c == '[')
	{
		sc->i = re_skip_class(s, len, sc->i);
		alen = 1;
	}
	else if (c == '\\')
	{
		sc->i++;
		if (sc->i >= len)
		{
			alen = 1;
		}
		else
		{
			char e = s[sc->i++];
			if (e == 'b' || e == 'B')
			{
				alen = 0;
				quantifiable = 0;
			}
			else if (e >= '1' && e <= '9')
			{
				while (sc->i < len && s[sc->i] >= '0' && s[sc->i] <= '9') sc->i++;
				alen = RE_LEN_UNKNOWN;  // backreference: length unknowable
			}
			else if (e == 'k')
			{
				if (sc->i < len && s[sc->i] == '<')
				{
					while (sc->i < len && s[sc->i] != '>') sc->i++;
					if (sc->i < len) sc->i++;
				}
				alen = RE_LEN_UNKNOWN;
			}
			else if ((e == 'p' || e == 'P') && sc->i < len && s[sc->i] == '{')
			{
				while (sc->i < len && s[sc->i] != '}') sc->i++;
				if (sc->i < len) sc->i++;
				alen = 1;
			}
			else if (e == 'x')
			{
				sc->i += (uint32_t) re_lb_hex(s, len, sc->i, 2);
				alen = 1;
			}
			else if (e == 'u')
			{
				if (sc->i < len && s[sc->i] == '{')
				{
					while (sc->i < len && s[sc->i] != '}') sc->i++;
					if (sc->i < len) sc->i++;
				}
				else
				{
					sc->i += (uint32_t) re_lb_hex(s, len, sc->i, 4);
				}
				alen = 1;
			}
			else if (e == 'c')
			{
				if (sc->i < len) sc->i++;
				alen = 1;
			}
			else
			{
				alen = 1;
			}
		}
	}
	else if (c == '^' || c == '$')
	{
		sc->i++;
		alen = 0;
		quantifiable = 0;
	}
	else
	{
		sc->i++;
		alen = 1;
	}

	if (!quantifiable || sc->i >= len) return alen;

	int qmin = -1;
	int qmax = -1;
	char q = s[sc->i];
	if (q == '*') { sc->i++; qmin = 0; qmax = -1; }
	else if (q == '+') { sc->i++; qmin = 1; qmax = -1; }
	else if (q == '?') { sc->i++; qmin = 0; qmax = 1; }
	else if (q == '{')
	{
		uint32_t j = sc->i + 1;
		uint32_t lo = 0;
		int have_lo = 0;
		while (j < len && s[j] >= '0' && s[j] <= '9') { lo = lo * 10 + (uint32_t) (s[j] - '0'); j++; have_lo = 1; }
		if (have_lo && j < len && s[j] == '}')
		{
			qmin = qmax = (int) lo;
			sc->i = j + 1;
		}
		else if (have_lo && j < len && s[j] == ',')
		{
			j++;
			uint32_t hi = 0;
			int have_hi = 0;
			while (j < len && s[j] >= '0' && s[j] <= '9') { hi = hi * 10 + (uint32_t) (s[j] - '0'); j++; have_hi = 1; }
			if (j < len && s[j] == '}')
			{
				qmin = (int) lo;
				qmax = have_hi ? (int) hi : -1;
				sc->i = j + 1;
			}
		}
		// Otherwise it is a literal '{' — the next atom will eat it.
	}
	if (qmin < 0) return alen;
	if (sc->i < len && (s[sc->i] == '?' || s[sc->i] == '+')) sc->i++;  // lazy/possessive
	if (qmin != qmax) return (alen == 0) ? 0 : RE_LEN_VARIABLE;
	if (alen < 0) return alen;
	return alen * qmin;
}

static int re_lb_branch(ReLbScan* sc)
{
	int total = 0;
	while (sc->i < sc->len && sc->s[sc->i] != '|' && sc->s[sc->i] != ')')
	{
		int a = re_lb_atom(sc);
		if (a == RE_LEN_VARIABLE) total = RE_LEN_VARIABLE;
		else if (a == RE_LEN_UNKNOWN) { if (total != RE_LEN_VARIABLE) total = RE_LEN_UNKNOWN; }
		else if (total >= 0) total += a;
	}
	return total;
}

// `top` = the lookbehind's own alternation, where PCRE lets branches differ.
static int re_lb_alt(ReLbScan* sc, int top)
{
	int total = re_lb_branch(sc);
	while (sc->i < sc->len && sc->s[sc->i] == '|')
	{
		sc->i++;
		int b = re_lb_branch(sc);
		if (total == RE_LEN_VARIABLE || b == RE_LEN_VARIABLE) total = RE_LEN_VARIABLE;
		else if (total == RE_LEN_UNKNOWN || b == RE_LEN_UNKNOWN) total = RE_LEN_UNKNOWN;
		else if (b != total) total = top ? RE_LEN_UNKNOWN : RE_LEN_VARIABLE;
	}
	return total;
}

// 0 = some lookbehind in the pattern is one PCRE would refuse to compile.
static int re_lookbehind_ok(const char* s, uint32_t len)
{
	for (uint32_t i = 0; i < len; )
	{
		char c = s[i];
		if (c == '\\') { i += 2; continue; }
		if (c == '[') { i = re_skip_class(s, len, i); continue; }
		if (c == '(' && i + 3 < len && s[i + 1] == '?' && s[i + 2] == '<'
		    && (s[i + 3] == '=' || s[i + 3] == '!'))
		{
			ReLbScan sc = { s, len, i + 4, 0 };
			if (re_lb_alt(&sc, 1) == RE_LEN_VARIABLE) return 0;
			// Fall through by one char so nested lookbehinds get their own
			// pass.
		}
		i++;
	}
	return 1;
}

static char* re_preprocess(Avm2Context* ctx, const Avm2String* src,
                           uint32_t flags, uint32_t* out_len)
{
	const char* s = src->utf8;
	uint32_t len = src->len;
	// Tamarin's PCRE has no capture ceiling; libregexp caps at
	// CAPTURE_COUNT_MAX (255) and a pattern over it fails to compile, so it
	// never matches at all. Such a pattern only needs its grouping unless
	// something reads the groups back, so when there are no backreferences
	// to renumber, demote plain `(` to `(?:`. Strictly monotone — the
	// affected patterns match nothing today. (from_avmplus recursion/pcre_*)
	int has_backref = 0;
	int demote = (re_count_captures(s, len, &has_backref) >= 255) && !has_backref;
	char* out = avm2_alloc(ctx, (demote ? len * 3 : len) + 1);
	uint32_t n = 0;
	int in_class = 0;
	uint32_t i = 0;
	while (i < len)
	{
		char c = s[i];
		if (c == '\\' && i + 1 < len)
		{
			out[n++] = c;
			out[n++] = s[i + 1];
			i += 2;
			continue;
		}
		if (in_class)
		{
			if (c == ']') in_class = 0;
			out[n++] = c;
			i++;
			continue;
		}
		if (c == '[')
		{
			in_class = 1;
			out[n++] = c;
			i++;
			continue;
		}
		if (c == '(' && i + 2 < len && s[i + 1] == '?' && s[i + 2] == '#')
		{
			// PCRE comment group: (?#...) up to the first ')'.
			i += 3;
			while (i < len && s[i] != ')') i++;
			if (i < len) i++;
			continue;
		}
		if (c == '(' && i + 3 < len && s[i + 1] == '?' && s[i + 2] == 'P'
		    && s[i + 3] == '<')
		{
			// Python/PCRE named group (?P<name> → ES (?<name>.
			out[n++] = '(';
			out[n++] = '?';
			i += 3;
			continue;
		}
		if (demote && c == '(' && (i + 1 >= len || s[i + 1] != '?'))
		{
			out[n++] = '(';
			out[n++] = '?';
			out[n++] = ':';
			i++;
			continue;
		}
		if (flags & AVM2_RE_EXTENDED)
		{
			if (is_x_space(c))
			{
				i++;
				continue;
			}
			if (c == '#')
			{
				while (i < len && s[i] != '\n') i++;
				continue;
			}
		}
		out[n++] = c;
		i++;
	}
	out[n] = '\0';
	*out_len = n;
	return out;
}

// Compile (once); returns 0 if the pattern doesn't compile.
static int re_ensure_compiled(Avm2Context* ctx, Avm2RegExpExt* ext)
{
	if (ext->compile_tried) return !ext->compile_failed;
	ext->compile_tried = 1;
	uint32_t plen = 0;
	char* pat = re_preprocess(ctx, ext->source, ext->flags, &plen);
	if (!re_lookbehind_ok(pat, plen))
	{
		// PCRE (so avmplus) refuses a variable-length lookbehind; libregexp
		// would happily compile one. Join the never-match path.
		ext->compile_failed = 1;
		return 0;
	}
	int re_flags = 0;
	if (ext->flags & AVM2_RE_IGNORECASE) re_flags |= LRE_FLAG_IGNORECASE;
	if (ext->flags & AVM2_RE_MULTILINE) re_flags |= LRE_FLAG_MULTILINE;
	if (ext->flags & AVM2_RE_DOTALL) re_flags |= LRE_FLAG_DOTALL;
	char err[128];
	int bc_len = 0;
	uint8_t* bc = lre_compile(&bc_len, err, sizeof(err), pat, plen, re_flags, ctx);
	if (bc == NULL)
	{
		ext->compile_failed = 1;
		return 0;
	}
	// Copy the bytecode into the immortal heap so libc can free its buffer.
	ext->bytecode = avm2_alloc(ctx, (uint32_t) bc_len);
	memcpy(ext->bytecode, bc, (size_t) bc_len);
	free(bc);
	return 1;
}

// ---------------------------------------------------------------------------
// Matching
// ---------------------------------------------------------------------------

#define RE_MAX_CAPTURES 255

typedef struct ReMatch
{
	uint32_t ngroups;  // capture count INCLUDING the whole match (index 0)
	// UTF-16 start/end per group; -1 = unmatched.
	int64_t g[RE_MAX_CAPTURES * 2];
	const char* group_names;  // packed (libregexp) or NULL
} ReMatch;

// Find the first match at or after start16. Returns 1 on match.
static int re_find(Avm2Context* ctx, Avm2RegExpExt* ext, const ReText* t,
                   uint32_t start16, ReMatch* m)
{
	if (!re_ensure_compiled(ctx, ext)) return 0;
	if (start16 > t->n16) return 0;
	int cc = lre_get_capture_count(ext->bytecode);
	if (cc > RE_MAX_CAPTURES) cc = RE_MAX_CAPTURES;
	uint8_t* capture[RE_MAX_CAPTURES * 2];
	int r = lre_exec(capture, ext->bytecode, (const uint8_t*) t->units,
	                 (int) start16, (int) t->n16, 1, ctx);
	if (r != 1) return 0;
	m->ngroups = (uint32_t) cc;
	for (int i = 0; i < cc; i++)
	{
		if (capture[i * 2] == NULL || capture[i * 2 + 1] == NULL)
		{
			m->g[i * 2] = -1;
			m->g[i * 2 + 1] = -1;
		}
		else
		{
			m->g[i * 2] = (capture[i * 2] - (const uint8_t*) t->units) / 2;
			m->g[i * 2 + 1] = (capture[i * 2 + 1] - (const uint8_t*) t->units) / 2;
		}
	}
	m->group_names = lre_get_groupnames(ext->bytecode);
	return 1;
}

// exec core (Ruffle RegExp::exec): global regexes scan from lastIndex and
// update it to the match end.
static int re_exec(Avm2Context* ctx, Avm2RegExpExt* ext, const ReText* t, ReMatch* m)
{
	uint32_t start = (ext->flags & AVM2_RE_GLOBAL) ? ext->last_index : 0;
	if (!re_find(ctx, ext, t, start, m)) return 0;
	if (ext->flags & AVM2_RE_GLOBAL)
	{
		ext->last_index = (uint32_t) m->g[1];
	}
	return 1;
}

// Build the exec-result array: groups as elements (undefined when
// unmatched), named groups as dynamic props ("" when unmatched), plus
// index/input (Ruffle globals/reg_exp.rs exec).
static Avm2Value re_match_array(Avm2Context* ctx, const ReText* t, const ReMatch* m)
{
	Avm2Object* arr = avm2_array_new(ctx, 0);
	for (uint32_t i = 0; i < m->ngroups; i++)
	{
		if (m->g[i * 2] < 0)
		{
			avm2_array_push(ctx, arr, avm2_undefined());
		}
		else
		{
			avm2_array_push(ctx, arr, avm2_string(
				retext_sub(ctx, t, (uint32_t) m->g[i * 2], (uint32_t) m->g[i * 2 + 1])));
		}
	}
	if (m->group_names != NULL)
	{
		const char* p = m->group_names;
		for (uint32_t i = 1; i < m->ngroups; i++)
		{
			if (*p != '\0')
			{
				Avm2Value v;
				if (m->g[i * 2] < 0)
				{
					v = avm2_string(avm2_string_from_literal(ctx, ""));
				}
				else
				{
					v = avm2_string(retext_sub(ctx, t, (uint32_t) m->g[i * 2],
					                           (uint32_t) m->g[i * 2 + 1]));
				}
				avm2_object_set_dynamic(ctx, arr, p, (uint32_t) strlen(p), v);
			}
			p += strlen(p) + LRE_GROUP_NAME_TRAILER_LEN;
		}
	}
	avm2_object_set_dynamic(ctx, arr, "index", 5, avm2_number((double) m->g[0]));
	avm2_object_set_dynamic(ctx, arr, "input", 5, avm2_string(t->src));
	return avm2_object_value(arr);
}

// ---------------------------------------------------------------------------
// RegExp class
// ---------------------------------------------------------------------------

static void regexp_init_from_args(Avm2Context* ctx, Avm2RegExpExt* ext,
                                  const Avm2Value* args, uint32_t argc)
{
	ext->last_index = 0;
	Avm2Value re_arg = argc > 0 ? args[0] : avm2_undefined();
	Avm2RegExpExt* other = regexp_ext(re_arg);
	if (other != NULL)
	{
		if (argc > 1 && args[1].kind != AVM2_VALUE_UNDEFINED)
		{
			avm2_throw_error(ctx, ctx->builtins.type_error_class,
			                 "Error #1100: Cannot supply flags when constructing "
			                 "one RegExp from another.");
		}
		ext->source = other->source;
		ext->flags = other->flags;
		return;
	}
	ext->source = (re_arg.kind == AVM2_VALUE_UNDEFINED)
		? avm2_string_from_literal(ctx, "")
		: avm2_coerce_to_string(ctx, re_arg);
	ext->flags = 0;
	if (argc > 1 && args[1].kind != AVM2_VALUE_UNDEFINED)
	{
		const Avm2String* fs = avm2_coerce_to_string(ctx, args[1]);
		for (uint32_t i = 0; i < fs->len; i++)
		{
			switch (fs->utf8[i])
			{
				case 's': ext->flags |= AVM2_RE_DOTALL; break;
				case 'x': ext->flags |= AVM2_RE_EXTENDED; break;
				case 'g': ext->flags |= AVM2_RE_GLOBAL; break;
				case 'i': ext->flags |= AVM2_RE_IGNORECASE; break;
				case 'm': ext->flags |= AVM2_RE_MULTILINE; break;
				default: break;
			}
		}
	}
}

static Avm2Value regexp_construct(Avm2Context* ctx, Avm2Class* cls,
                                  const Avm2Value* args, uint32_t argc)
{
	Avm2Object* obj = avm2_object_alloc(ctx, AVM2_OBJ_SCRIPT, 1);
	obj->cls = cls;
	obj->vtable = &cls->ivtable;
	obj->proto = cls->prototype_obj;
	Avm2RegExpExt* ext = avm2_alloc(ctx, sizeof(Avm2RegExpExt));
	memset(ext, 0, sizeof(Avm2RegExpExt));
	obj->native_ext = ext;
	obj->native_ext_size = sizeof(Avm2RegExpExt);  // GC conservative-scan span
	regexp_init_from_args(ctx, ext, args, argc);
	return avm2_object_value(obj);
}

// RegExp(x): identity when x is already a RegExp, else construct
// (Ruffle reg_exp.rs call_handler).
static Avm2Value regexp_call(Avm2Context* ctx, Avm2Class* cls,
                             const Avm2Value* args, uint32_t argc)
{
	if (argc == 1 && regexp_ext(args[0]) != NULL)
	{
		return args[0];
	}
	return regexp_construct(ctx, cls, args, argc);
}

static Avm2RegExpExt* this_regexp(Avm2Activation* act)
{
	return regexp_ext(act->this_val);
}

#define RE_FLAG_GETTER(fname, bit) \
	static Avm2Value fname(Avm2Activation* act) \
	{ \
		Avm2RegExpExt* ext = this_regexp(act); \
		if (ext == NULL) return avm2_undefined(); \
		return avm2_bool((ext->flags & (bit)) != 0); \
	}
RE_FLAG_GETTER(regexp_get_dotall, AVM2_RE_DOTALL)
RE_FLAG_GETTER(regexp_get_extended, AVM2_RE_EXTENDED)
RE_FLAG_GETTER(regexp_get_global, AVM2_RE_GLOBAL)
RE_FLAG_GETTER(regexp_get_ignore_case, AVM2_RE_IGNORECASE)
RE_FLAG_GETTER(regexp_get_multiline, AVM2_RE_MULTILINE)
#undef RE_FLAG_GETTER

static Avm2Value regexp_get_source(Avm2Activation* act)
{
	Avm2RegExpExt* ext = this_regexp(act);
	if (ext == NULL) return avm2_undefined();
	return avm2_string(ext->source);
}

static Avm2Value regexp_get_last_index(Avm2Activation* act)
{
	Avm2RegExpExt* ext = this_regexp(act);
	if (ext == NULL) return avm2_undefined();
	return avm2_uint_value(ext->last_index);
}

static Avm2Value regexp_set_last_index(Avm2Activation* act)
{
	Avm2RegExpExt* ext = this_regexp(act);
	if (ext != NULL && act->argc > 0)
	{
		int32_t i = avm2_coerce_to_i32(act->ctx, act->args[0]);
		ext->last_index = (uint32_t) i;
	}
	return avm2_undefined();
}

static const Avm2String* arg_string_or_empty(Avm2Activation* act, uint32_t i)
{
	if (i >= act->argc || act->args[i].kind == AVM2_VALUE_UNDEFINED)
	{
		return avm2_string_from_literal(act->ctx, "");
	}
	return avm2_coerce_to_string(act->ctx, act->args[i]);
}

static Avm2Value regexp_exec(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2RegExpExt* ext = this_regexp(act);
	if (ext == NULL) return avm2_undefined();
	const Avm2String* text = arg_string_or_empty(act, 0);
	ReText t;
	retext_build(ctx, text, &t);
	ReMatch m;
	if (!re_exec(ctx, ext, &t, &m)) RT_RETURN(avm2_null());
	RT_RETURN(re_match_array(ctx, &t, &m));
}

static Avm2Value regexp_test(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2RegExpExt* ext = this_regexp(act);
	if (ext == NULL) return avm2_undefined();
	const Avm2String* text = arg_string_or_empty(act, 0);
	ReText t;
	retext_build(ctx, text, &t);
	ReMatch m;
	RT_RETURN(avm2_bool(re_exec(ctx, ext, &t, &m) != 0));
}

// prototype.toString: "/source/gimsx"; throws 1034 for non-RegExp
// receivers (`var regexp:RegExp = this` in RegExp.as).
static Avm2Value regexp_to_string(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2RegExpExt* ext = this_regexp(act);
	if (ext == NULL)
	{
		char dbg[128];
		if (act->this_val.kind == AVM2_VALUE_OBJECT)
		{
			char vq[96];
			avm2_class_qname_buf(avm2_value_class(ctx, act->this_val), vq, sizeof(vq));
			snprintf(dbg, sizeof(dbg), "%s@00000000000", vq);
		}
		else
		{
			const Avm2String* s = avm2_coerce_to_string(ctx, act->this_val);
			snprintf(dbg, sizeof(dbg), "%.*s", s->len > 100 ? 100 : (int) s->len,
			         s->utf8);
		}
		avm2_throw_error(ctx, ctx->builtins.type_error_class,
		                 "Error #1034: Type Coercion failed: cannot convert %s "
		                 "to RegExp.", dbg);
	}
	char* buf = avm2_alloc(ctx, ext->source->len + 8);
	uint32_t n = 0;
	buf[n++] = '/';
	memcpy(buf + n, ext->source->utf8, ext->source->len);
	n += ext->source->len;
	buf[n++] = '/';
	if (ext->flags & AVM2_RE_GLOBAL) buf[n++] = 'g';
	if (ext->flags & AVM2_RE_IGNORECASE) buf[n++] = 'i';
	if (ext->flags & AVM2_RE_MULTILINE) buf[n++] = 'm';
	if (ext->flags & AVM2_RE_DOTALL) buf[n++] = 's';
	if (ext->flags & AVM2_RE_EXTENDED) buf[n++] = 'x';
	{ const Avm2String* r_ = avm2_string_new(ctx, buf, n); heap_free(ctx->app, buf); return avm2_string(r_); }  // copied: the buffer is scratch
}

// ---------------------------------------------------------------------------
// String regex paths (Ruffle globals/string.rs)
// ---------------------------------------------------------------------------

// Coerce the pattern arg into a RegExp ext: the arg itself if it's a
// RegExp, else a fresh flagless RegExp over the coerced string.
static Avm2RegExpExt* pattern_to_regexp(Avm2Context* ctx, Avm2Value pattern)
{
	Avm2RegExpExt* ext = regexp_ext(pattern);
	if (ext != NULL) return ext;
	// Coerce to a STRING first (Ruffle search / match_internal both call
	// coerce_to_string before constructing). The RegExp constructor maps
	// undefined to the EMPTY pattern per ECMA-262 §15.10.4.1, which matches
	// at index 0; going through the string makes the pattern the literal
	// "undefined", which never matches — so search() → -1, match() → null.
	Avm2Value args[1];
	args[0] = avm2_string(avm2_coerce_to_string(ctx, pattern));
	Avm2Value re = regexp_construct(ctx, ctx->builtins.regexp_class, args, 1);
	return regexp_ext(re);
}

static Avm2Value string_match_regex(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	const Avm2String* text = avm2_coerce_to_string(ctx, act->this_val);
	Avm2Value pattern = act->argc > 0 ? act->args[0] : avm2_undefined();
	Avm2RegExpExt* re = pattern_to_regexp(ctx, pattern);
	ReText t;
	retext_build(ctx, text, &t);
	if (re->flags & AVM2_RE_GLOBAL)
	{
		Avm2Object* arr = avm2_array_new(ctx, 0);
		uint32_t last = 0;
		uint32_t old_last_index = re->last_index;
		re->last_index = 0;
		ReMatch m;
		while (re_exec(ctx, re, &t, &m))
		{
			if (re->last_index == last) break;
			avm2_array_push(ctx, arr, avm2_string(
				retext_sub(ctx, &t, (uint32_t) m.g[0], (uint32_t) m.g[1])));
			last = re->last_index;
		}
		re->last_index = 0;
		if (old_last_index == re->last_index)
		{
			re->last_index = 1;  // avmplus quirk (Ruffle match_internal)
		}
		RT_RETURN(avm2_object_value(arr));
	}
	uint32_t old = re->last_index;
	re->last_index = 0;
	ReMatch m;
	if (re_exec(ctx, re, &t, &m))
	{
		re->last_index = old;
		RT_RETURN(re_match_array(ctx, &t, &m));
	}
	re->last_index = old;
	RT_RETURN(avm2_null());
}

static Avm2Value string_search_regex(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	const Avm2String* text = avm2_coerce_to_string(ctx, act->this_val);
	Avm2Value pattern = act->argc > 0 ? act->args[0] : avm2_undefined();
	Avm2RegExpExt* re = pattern_to_regexp(ctx, pattern);
	ReText t;
	retext_build(ctx, text, &t);
	uint32_t old = re->last_index;
	re->last_index = 0;
	ReMatch m;
	int hit = re_exec(ctx, re, &t, &m);
	re->last_index = old;
	if (!hit) RT_RETURN(avm2_integer(-1));
	RT_RETURN(avm2_integer((int32_t) m.g[0]));
}

// Evaluate $-sequences of a string replacement (Ruffle
// effective_replacement).
static const Avm2String* effective_replacement(Avm2Context* ctx,
                                               const Avm2String* replacement,
                                               const ReText* t, const ReMatch* m)
{
	if (memchr(replacement->utf8, '$', replacement->len) == NULL)
	{
		return replacement;
	}
	const Avm2String* ret = avm2_string_from_literal(ctx, "");
	const char* s = replacement->utf8;
	uint32_t len = replacement->len;
	uint32_t i = 0;
	uint32_t ncaps = m->ngroups - 1;
	while (i < len)
	{
		char c = s[i];
		if (c != '$')
		{
			// Copy the run up to the next '$'.
			uint32_t j = i;
			while (j < len && s[j] != '$') j++;
			ret = avm2_string_concat(ctx, ret, avm2_string_new(ctx, s + i, j - i));
			i = j;
			continue;
		}
		i++;
		if (i >= len)
		{
			ret = avm2_string_concat(ctx, ret, avm2_string_from_literal(ctx, "$"));
			break;
		}
		char n = s[i];
		if (n == '$')
		{
			ret = avm2_string_concat(ctx, ret, avm2_string_from_literal(ctx, "$"));
			i++;
		}
		else if (n == '&')
		{
			ret = avm2_string_concat(ctx, ret,
				retext_sub(ctx, t, (uint32_t) m->g[0], (uint32_t) m->g[1]));
			i++;
		}
		else if (n == '`')
		{
			ret = avm2_string_concat(ctx, ret, retext_sub(ctx, t, 0, (uint32_t) m->g[0]));
			i++;
		}
		else if (n == '\'')
		{
			ret = avm2_string_concat(ctx, ret,
				retext_sub(ctx, t, (uint32_t) m->g[1], t->n16));
			i++;
		}
		else if (n >= '0' && n <= '9')
		{
			uint32_t d = (uint32_t) (n - '0');
			if (d > ncaps)
			{
				char two[3] = { '$', n, 0 };
				ret = avm2_string_concat(ctx, ret, avm2_string_new(ctx, two, 2));
				i++;
				continue;
			}
			uint32_t grp = d;
			int has_second = 0;
			char second = 0;
			if (i + 1 < len && s[i + 1] >= '0' && s[i + 1] <= '9')
			{
				uint32_t two_digit = d * 10 + (uint32_t) (s[i + 1] - '0');
				if (two_digit <= ncaps && two_digit != 0)
				{
					grp = two_digit;
					second = s[i + 1];
					has_second = 1;
					i++;
				}
			}
			if (grp == 0)
			{
				char two[3] = { '$', n, 0 };
				ret = avm2_string_concat(ctx, ret, avm2_string_new(ctx, two, 2));
				i++;
				continue;
			}
			if (m->g[grp * 2] >= 0)
			{
				ret = avm2_string_concat(ctx, ret,
					retext_sub(ctx, t, (uint32_t) m->g[grp * 2],
					           (uint32_t) m->g[grp * 2 + 1]));
			}
			// Leading-zero two-digit refs append the second digit after
			// the replacement (Ruffle quirk).
			if (has_second && d == 0)
			{
				ret = avm2_string_concat(ctx, ret, avm2_string_new(ctx, &second, 1));
			}
			i++;
		}
		else
		{
			char two[3] = { '$', n, 0 };
			ret = avm2_string_concat(ctx, ret, avm2_string_new(ctx, two, 2));
			i++;
		}
	}
	return ret;
}

static Avm2Value string_replace_regex(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	const Avm2String* text = avm2_coerce_to_string(ctx, act->this_val);
	Avm2Value pattern = act->argc > 0 ? act->args[0] : avm2_undefined();
	Avm2Value replacement = act->argc > 1 ? act->args[1] : avm2_undefined();

	Avm2RegExpExt* re = regexp_ext(pattern);
	if (re == NULL)
	{
		// String pattern: replace the FIRST occurrence only.
		const Avm2String* pat = avm2_coerce_to_string(ctx, pattern);
		int64_t pos = -1;
		if (pat->len <= text->len)
		{
			for (uint32_t i = 0; i + pat->len <= text->len; i++)
			{
				if (memcmp(text->utf8 + i, pat->utf8, pat->len) == 0)
				{
					pos = (int64_t) i;
					break;
				}
			}
		}
		if (pos < 0) return avm2_string(text);
		const Avm2String* ret = avm2_string_new(ctx, text->utf8, (uint32_t) pos);
		if (replacement.kind == AVM2_VALUE_OBJECT
		    && replacement.u.obj->kind == AVM2_OBJ_FUNCTION)
		{
			// UTF-16 position for the callback (Ruffle passes WStr offset).
			ReText t;
			retext_build(ctx, text, &t);
			uint32_t pos16 = 0;
			while (pos16 < t.n16 && t.u8off[pos16] < (uint32_t) pos) pos16++;
			Avm2Value cargs[3];
			cargs[0] = avm2_string(pat);
			cargs[1] = avm2_uint_value(pos16);
			cargs[2] = avm2_string(text);
			retext_free(ctx, &t);
			Avm2Value r = avm2_call_value(ctx, replacement, avm2_null(), cargs, 3);
			ret = avm2_string_concat(ctx, ret, avm2_coerce_to_string(ctx, r));
		}
		else
		{
			ret = avm2_string_concat(ctx, ret, avm2_coerce_to_string(ctx, replacement));
		}
		ret = avm2_string_concat(ctx, ret,
			avm2_string_new(ctx, text->utf8 + pos + pat->len,
			                text->len - (uint32_t) pos - pat->len));
		return avm2_string(ret);
	}

	int is_fn = replacement.kind == AVM2_VALUE_OBJECT
	            && replacement.u.obj->kind == AVM2_OBJ_FUNCTION;
	const Avm2String* repl_str = NULL;
	if (!is_fn)
	{
		repl_str = avm2_coerce_to_string(ctx, replacement);
	}

	ReText t;
	retext_build(ctx, text, &t);
	int global = (re->flags & AVM2_RE_GLOBAL) != 0;
	uint32_t start = 0;
	ReMatch m;
	if (!re_find(ctx, re, &t, start, &m))
	{
		RT_RETURN(avm2_string(text));  // lastIndex untouched (Ruffle)
	}
	const Avm2String* ret = avm2_string_from_literal(ctx, "");
	int have = 1;
	while (have)
	{
		ret = avm2_string_concat(ctx, ret, retext_sub(ctx, &t, start, (uint32_t) m.g[0]));
		if (is_fn)
		{
			// (match, capture1.., index, whole string); unmatched → "".
			uint32_t nargs = m.ngroups + 2;
			Avm2Value cargs[RE_MAX_CAPTURES + 2];
			for (uint32_t i = 0; i < m.ngroups; i++)
			{
				cargs[i] = (m.g[i * 2] < 0)
					? avm2_string(avm2_string_from_literal(ctx, ""))
					: avm2_string(retext_sub(ctx, &t, (uint32_t) m.g[i * 2],
					                         (uint32_t) m.g[i * 2 + 1]));
			}
			cargs[m.ngroups] = avm2_uint_value((uint32_t) m.g[0]);
			cargs[m.ngroups + 1] = avm2_string(text);
			Avm2Value r = avm2_call_value(ctx, replacement, avm2_null(), cargs, nargs);
			ret = avm2_string_concat(ctx, ret, avm2_coerce_to_string(ctx, r));
		}
		else
		{
			ret = avm2_string_concat(ctx, ret,
				effective_replacement(ctx, repl_str, &t, &m));
		}
		start = (uint32_t) m.g[1];
		if (m.g[0] == m.g[1])
		{
			if (start == t.n16) break;
			ret = avm2_string_concat(ctx, ret, retext_sub(ctx, &t, start, start + 1));
			start += 1;
		}
		if (!global) break;
		have = re_find(ctx, re, &t, start, &m);
	}
	ret = avm2_string_concat(ctx, ret, retext_sub(ctx, &t, start, t.n16));
	RT_RETURN(avm2_string(ret));
}

static Avm2Value string_split_regex(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Value delim = act->argc > 0 ? act->args[0] : avm2_undefined();
	Avm2RegExpExt* re = regexp_ext(delim);
	if (re == NULL)
	{
		return avm2_string_split_plain(act);
	}
	const Avm2String* text = avm2_coerce_to_string(ctx, act->this_val);
	uint32_t limit = 0xFFFFFFFFu;
	if (act->argc > 1 && act->args[1].kind != AVM2_VALUE_UNDEFINED)
	{
		limit = avm2_coerce_to_u32(ctx, act->args[1]);
	}
	Avm2Object* arr = avm2_array_new(ctx, 0);
	if (limit == 0) return avm2_object_value(arr);

	ReText t;
	retext_build(ctx, text, &t);

	// The empty regex splits into characters (UTF-16 units grouped per
	// code point — Ruffle text.chars()).
	if (re->source->len == 0)
	{
		uint32_t i = 0;
		uint32_t count = 0;
		while (i < t.n16 && count < limit)
		{
			uint32_t next = i + 1;
			if (t.units[i] >= 0xD800 && t.units[i] < 0xDC00 && next < t.n16
			    && t.units[next] >= 0xDC00 && t.units[next] < 0xE000)
			{
				next++;
			}
			avm2_array_push(ctx, arr, avm2_string(retext_sub(ctx, &t, i, next)));
			count++;
			i = next;
		}
		RT_RETURN(avm2_object_value(arr));
	}

	uint32_t start = 0;
	uint32_t count = 0;
	ReMatch m;
	while (re_find(ctx, re, &t, start, &m))
	{
		if ((uint32_t) m.g[1] == start) break;
		avm2_array_push(ctx, arr, avm2_string(
			retext_sub(ctx, &t, start, (uint32_t) m.g[0])));
		count++;
		if (count >= limit) break;
		for (uint32_t i = 1; i < m.ngroups; i++)
		{
			if (m.g[i * 2] < 0) continue;
			avm2_array_push(ctx, arr, avm2_string(
				retext_sub(ctx, &t, (uint32_t) m.g[i * 2], (uint32_t) m.g[i * 2 + 1])));
			count++;
			if (count >= limit)
			{
				// Intentional bug to match Flash: only the CAPTURE loop
				// stops — the outer scan continues and adds parts past the
				// limit (Ruffle regexp.rs split).
				break;
			}
		}
		start = (uint32_t) m.g[1];
	}
	if (count < limit)
	{
		avm2_array_push(ctx, arr, avm2_string(retext_sub(ctx, &t, start, t.n16)));
	}
	RT_RETURN(avm2_object_value(arr));
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void avm2_register_regexp(Avm2Context* ctx)
{
	Avm2Builtins* b = &ctx->builtins;
	Avm2Class* cls = avm2_builtin_class(ctx, "", "RegExp", b->object_class);
	b->regexp_class = cls;
	cls->native_construct = regexp_construct;
	cls->native_call = regexp_call;

	avm2_builtin_add_getter(ctx, cls, "dotall", regexp_get_dotall);
	avm2_builtin_add_getter(ctx, cls, "extended", regexp_get_extended);
	avm2_builtin_add_getter(ctx, cls, "global", regexp_get_global);
	avm2_builtin_add_getter(ctx, cls, "ignoreCase", regexp_get_ignore_case);
	avm2_builtin_add_getter(ctx, cls, "multiline", regexp_get_multiline);
	avm2_builtin_add_getter(ctx, cls, "source", regexp_get_source);
	{
		Avm2PropEntry e;
		memset(&e, 0, sizeof(e));
		e.key = avm2_public_key("lastIndex", 9);
		e.kind = AVM2_PROP_GETSET;
		e.method.fn = regexp_get_last_index;
		e.method.debug_name = "get lastIndex";
		e.setter.fn = regexp_set_last_index;
		e.setter.debug_name = "set lastIndex";
		e.defining_class = cls;
		avm2_vtable_append(ctx, &cls->ivtable, &e);
	}
	avm2_builtin_add_method(ctx, cls, "exec", regexp_exec);
	avm2_builtin_add_method(ctx, cls, "test", regexp_test);

	Avm2Object* proto = cls->prototype_obj;
	avm2_proto_add_function(ctx, proto, "exec", regexp_exec);
	avm2_proto_add_function(ctx, proto, "test", regexp_test);
	avm2_proto_add_function(ctx, proto, "toString", regexp_to_string);

	// String regex paths: add match/replace/search, make split regex-aware.
	Avm2Class* sc = b->string_class;
	avm2_builtin_add_method(ctx, sc, "match", string_match_regex);
	avm2_builtin_add_method(ctx, sc, "replace", string_replace_regex);
	avm2_builtin_add_method(ctx, sc, "search", string_search_regex);
	{
		Avm2PropKey key = avm2_public_key("split", 5);
		Avm2PropEntry* e = (Avm2PropEntry*) avm2_vtable_find(&sc->ivtable, &key);
		if (e != NULL)
		{
			e->method.fn = string_split_regex;
		}
	}
	// Same three on String.prototype for the ES3-compat layer, and re-point
	// prototype.split at the regex-aware impl now that it exists (String
	// registration seeded it with the plain one).
	Avm2Object* sproto = sc->prototype_obj;
	avm2_proto_add_function_n(ctx, sproto, "match", string_match_regex, 1);
	avm2_proto_add_function_n(ctx, sproto, "replace", string_replace_regex, 2);
	avm2_proto_add_function_n(ctx, sproto, "search", string_search_regex, 1);
	avm2_proto_add_function_n(ctx, sproto, "split", string_split_regex, 2);
}
