#pragma once

#include <common.h>

#include <stddef.h>

#define ENSURE_SIZE(ptr, new_size, capac, elem_size) \
	while (new_size >= capac) \
	{ \
		grow_ptr(app_context, (char**) &ptr, &capac, elem_size); \
	}

void grow_ptr(SWFAppContext* app_context, char** ptr, size_t* capacity_ptr, size_t elem_size);

u32 get_elapsed_ms();
int getpagesize();

char* vmem_reserve(size_t size);
void vmem_release(char* addr, size_t size);

// ---------------------------------------------------------------------------
// Ruffle test-navigator request log
// ---------------------------------------------------------------------------
//
// Ruffle's test harness swaps in a TestNavigatorBackend that traces every
// request the player would have made
// (tests/framework/src/backends/navigator.rs). ~24 corpus tests set
// `log_fetch = true` in test.toml and grade that log interleaved with their own
// traces; `verify_output.py` turns the flag into -DLOG_FETCH on the gcc line, so
// every other build compiles these to nothing and stays byte-identical.
//
// The two logs differ in TIMING, which is what the interleaving grades:
//   * navigate_to_url is a plain synchronous backend call -> printed at once.
//   * fetch happens inside the spawned load future, which Ruffle's harness only
//     polls once the frame is over -> queued here and flushed by the VM at its
//     load-drain point, AFTER the calling frame's traces.
// Both VMs share this because AVM1 (getURL/loadVariables) and AVM2
// (Loader/URLLoader/navigateToURL) must emit the identical format.
#ifdef LOG_FETCH
#define SWF_LOG_FETCH_ENABLED 1
#else
#define SWF_LOG_FETCH_ENABLED 0
#endif

// One header or one navigate param. Lengths are explicit: AVM strings are not
// NUL-terminated.
typedef struct SwfLogPair
{
	const char* name;
	size_t name_len;
	const char* value;
	size_t value_len;
} SwfLogPair;

// Queue one "Navigator::fetch:" block. `has_body` distinguishes "no body"
// (Ruffle's `None`, which also forces the method to GET at the call site) from
// an empty one. `body_is_form` selects Ruffle's two body renderings: the
// verbatim text used for application/x-www-form-urlencoded, or Rust's
// `{:02X?}` slice debug ("[41, 3D, 62]") for anything else.
void swf_log_fetch_queue(const char* url, size_t url_len,
                         const char* method, size_t method_len,
                         const SwfLogPair* headers, size_t header_count,
                         const char* mime, size_t mime_len,
                         const unsigned char* body, size_t body_len,
                         int has_body, int body_is_form);
int swf_log_fetch_pending(void);
void swf_log_fetch_flush(void);

// Print one "Navigator::navigate_to_url:" block immediately. `method` NULL
// means the call carried no vars_method, so neither Method nor Param lines
// are printed. The target is normalized here rather than by the caller because
// Ruffle normalizes in navigate_to_url_normalized, i.e. at this same sink, for
// both VMs.
void swf_log_navigate(const char* url, size_t url_len,
                      const char* target, size_t target_len,
                      const char* method, size_t method_len,
                      const SwfLogPair* params, size_t param_count);

// Print one "Navigator::connect_socket" block immediately (4-space indent on
// the Host/Port line — avm1/xml_socket_connect_null grades that literally).
// Called from swf_socket_connect on every connect attempt, script or not.
void swf_log_connect_socket(const char* host, size_t host_len, int port);

// ---------------------------------------------------------------------------
// Legacy charset decoding (AVM1 form loader)
// ---------------------------------------------------------------------------
//
// `loadVariables` / `loadVariablesNum` do NOT read their payload as UTF-8
// unconditionally. Ruffle's `load_form_into_object`
// (core/src/loader.rs:1004-1022) picks the encoding like this:
//
//   if System.useCodepage        -> sniff the bytes (chardetng)
//   else if root SWF version <=5 -> windows-1252
//   else                         -> verbatim (bytes are handed on untouched
//                                   and only later lossily read as UTF-8)
//
// and only then runs form-urlencoded parsing. Every other loader
// (LoadVars.load, XML.load, StyleSheet.load) is unconditionally UTF-8 in
// Ruffle, so this API must stay wired to the form-loader path alone.
typedef enum
{
	LEGACY_CS_UTF8 = 0,        // verbatim passthrough — never re-encoded
	LEGACY_CS_WINDOWS_1252,
	LEGACY_CS_SHIFT_JIS
} LegacyCharset;

// Stand-in for chardetng. Strict-UTF-8 wins; otherwise a stream that is valid
// Shift-JIS *and* contains at least one real double-byte sequence is SJIS;
// everything else is windows-1252.
LegacyCharset legacy_charset_detect(const unsigned char* body, int len);

// Decode `len` bytes to a malloc'd, NUL-terminated UTF-8 buffer (caller frees;
// NULL on allocation failure). `out_len` is optional and receives the decoded
// byte count. LEGACY_CS_UTF8 is a verbatim memcpy — deliberately not validated
// or re-encoded, so the default form-loader path stays byte-identical.
char* legacy_charset_to_utf8(const unsigned char* body, int len,
                             LegacyCharset cs, int* out_len);
