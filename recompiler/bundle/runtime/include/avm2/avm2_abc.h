#ifndef AVM2_ABC_H
#define AVM2_ABC_H

// Static ABC data contract between the recompiler's C emitter
// (SWFRecomp/src/abc/abc_emit.cpp → RecompiledABC/) and the AVM2 runtime.
//
// The emitter lowers the verified IR's pool/table indices
// (SWFRecomp/include/abc/abc_ir.hpp) into these const tables; every index
// here is a direct index into the owning Avm2AbcFileData's arrays,
// INCLUDING the synthetic entry 0 ("any"/no value) that the parser gives
// every constant pool. Runtime-mutable state (script globals, init flags,
// realized classes) lives in the parallel Avm2AbcFileRt.

#include <stddef.h>
#include <stdint.h>

#include <avm2/avm2_value.h>

typedef struct Avm2AbcFileRt Avm2AbcFileRt;
typedef struct Avm2Class Avm2Class;
typedef struct Avm2ScopeChain Avm2ScopeChain;

// Namespace kinds: raw ABC values (abc_types.hpp NamespaceKind).
// Package (0x16) and Namespace (0x08) fold together as "public" during
// name matching, mirroring Ruffle namespace.rs is_public.
typedef struct Avm2AbcNamespace
{
	uint8_t kind;
	uint32_t name;  // string index
} Avm2AbcNamespace;

typedef struct Avm2AbcNsSet
{
	uint32_t count;
	const uint32_t* ns_indices;  // namespace indices
} Avm2AbcNsSet;

// Multiname kinds: raw ABC values (abc_types.hpp MultinameKind).
typedef struct Avm2AbcMultiname
{
	uint8_t kind;
	uint32_t ns;      // namespace index (QName/QNameA)
	uint32_t name;    // string index
	uint32_t ns_set;  // ns-set index (Multiname*/MultinameL*)
	// TypeName (0x1d): parameterized type, e.g. Vector.<int>.
	uint32_t base_type;            // multiname index of the base (Vector)
	uint32_t type_param_count;
	const uint32_t* type_params;   // multiname indices (0 = "any" → Vector.<*>)
} Avm2AbcMultiname;

// Slot/optional-parameter default value: raw ABC DefaultValueKind + a
// pool index whose pool depends on the kind (abc_types.hpp).
typedef struct Avm2AbcDefault
{
	uint8_t has_value;
	uint8_t kind;
	uint32_t index;
} Avm2AbcDefault;

// Trait METADATA — an ABC `[Name(key="value", ...)]` annotation. Both
// indices are into the STRING pool; index 0 is the empty string, which is
// what a bare `[Foo("bar")]` item uses as its key.
typedef struct Avm2AbcMetadataItem
{
	uint32_t key;
	uint32_t value;
} Avm2AbcMetadataItem;

typedef struct Avm2AbcMetadata
{
	uint32_t name;        // string index
	uint32_t item_count;
	const Avm2AbcMetadataItem* items;
} Avm2AbcMetadata;

// Trait kinds: raw ABC values (abc_types.hpp TraitKindType):
// 0 Slot, 1 Method, 2 Getter, 3 Setter, 4 Class, 5 Function, 6 Const.
//
// metadata_count/metadata are APPENDED members: the generated tables use
// positional initializers, and C zero-fills missing trailing members, so a
// stale RecompiledABC still compiles (it just reports no metadata).
typedef struct Avm2AbcTrait
{
	uint8_t kind;
	uint32_t name_mn;         // multiname index (QName)
	uint32_t slot_or_disp_id; // 0 = auto-assign
	uint32_t type_mn;         // multiname index (Slot/Const)
	uint32_t method_or_class; // method index (Method/Getter/Setter/Function) or class index (Class)
	Avm2AbcDefault value;     // Slot/Const initial value
	uint32_t metadata_count;
	const Avm2AbcMetadata* metadata;
} Avm2AbcTrait;

struct Avm2Activation;
typedef Avm2Value (*Avm2MethodFn)(struct Avm2Activation* act);

// Exception table entry, offsets resolved to op indices. Dispatch matches
// Ruffle activation.rs handle_err: op in [from_op, to_op), typed match by
// is_of_type (type_mn 0 = catch-all).
typedef struct Avm2AbcException
{
	uint32_t from_op;
	uint32_t to_op;
	uint32_t target_op;
	uint32_t type_mn;
	uint32_t variable_mn;
	uint8_t active;
} Avm2AbcException;

// Method flags: raw ABC values (abc_types.hpp MethodFlags).
enum
{
	AVM2_METHOD_NEED_ARGUMENTS = 1 << 0,
	AVM2_METHOD_NEED_ACTIVATION = 1 << 1,
	AVM2_METHOD_NEED_REST = 1 << 2,
	AVM2_METHOD_HAS_OPTIONAL = 1 << 3,
	AVM2_METHOD_IGNORE_REST = 1 << 4,
	AVM2_METHOD_NATIVE = 1 << 5,
	// SET_DXNS: the method body contains a dxns/dxnslate op, so it must start
	// with a FRESH default XML namespace instead of inheriting the caller's
	// (avmplus MethodInfo::setsDxns, Ruffle activation.rs).
	AVM2_METHOD_SET_DXNS = 1 << 6,
};

typedef struct Avm2AbcMethodData
{
	Avm2MethodFn fn;         // NULL = no body (native / interface method)
	const char* debug_name;  // pool name, for diagnostics
	uint8_t flags;
	// Referenced by NewFunction / a Function trait (Ruffle is_function):
	// with an all-untyped default-free signature the method is "unchecked"
	// (missing args become undefined, extra args allowed).
	uint8_t is_function;
	uint32_t param_count;
	const uint32_t* param_types;      // param_count multiname indices (NULL if 0)
	const Avm2AbcDefault* optionals;  // param_count entries, trailing ones present (NULL if none)
	uint32_t return_type_mn;
	uint32_t body_trait_count;        // activation-object traits (method body traits)
	const Avm2AbcTrait* body_traits;
	uint32_t exception_count;
	const Avm2AbcException* exceptions;
} Avm2AbcMethodData;

enum
{
	AVM2_CLASS_FLAG_SEALED = 1 << 0,
	AVM2_CLASS_FLAG_FINAL = 1 << 1,
	AVM2_CLASS_FLAG_INTERFACE = 1 << 2,
	// Synthetic per-newcatch class (avm2_op_newcatch): owned exclusively by
	// its catch-scope object, freed when that object is swept.
	AVM2_CLASS_FLAG_SYNTH_CATCH = 1 << 7,
};

typedef struct Avm2AbcClassData
{
	uint32_t name_mn;       // multiname index (QName)
	uint32_t super_mn;      // multiname index (0 = none: Object/interface)
	uint8_t flags;
	uint8_t has_protected_ns;
	uint32_t protected_ns;  // namespace index
	uint32_t interface_count;
	const uint32_t* interface_mns;  // multiname indices
	uint32_t instance_init; // method index
	uint32_t class_init;    // method index (static initializer)
	uint32_t instance_trait_count;
	const Avm2AbcTrait* instance_traits;
	uint32_t class_trait_count;
	const Avm2AbcTrait* class_traits;
	// Non-zero = this class matched a known native intrinsic implementation
	// (pool-normalized fingerprint match at recompile time). 0 = no match.
	// Appended at the END so existing positional initializers stay valid.
	uint32_t intrinsic_id;
} Avm2AbcClassData;

typedef struct Avm2AbcScriptData
{
	uint32_t init_method;  // method index
	uint32_t trait_count;
	const Avm2AbcTrait* traits;
} Avm2AbcScriptData;

typedef struct Avm2AbcFileData
{
	uint32_t int_count;
	const int32_t* ints;
	uint32_t uint_count;
	const uint32_t* uints;
	uint32_t double_count;
	const double* doubles;
	uint32_t string_count;
	const Avm2String* strings;
	uint32_t namespace_count;
	const Avm2AbcNamespace* namespaces;
	uint32_t ns_set_count;
	const Avm2AbcNsSet* ns_sets;
	uint32_t multiname_count;
	const Avm2AbcMultiname* multinames;
	uint32_t method_count;
	const Avm2AbcMethodData* methods;
	uint32_t class_count;
	const Avm2AbcClassData* classes;
	uint32_t script_count;
	const Avm2AbcScriptData* scripts;
} Avm2AbcFileData;

// SymbolClass (tag 76): char_id 0 = the root/main timeline class.
typedef struct Avm2SymbolClassBinding
{
	uint16_t char_id;
	const char* class_name;  // dotted ("test_fla.MainTimeline")
} Avm2SymbolClassBinding;

// Provided by the generated RecompiledABC/abc_registry.c. The in-browser
// recompiler's graphics HOST (-DDYNAMIC_HOST) links no generated code: it
// declares every table as a POINTER (and every count as a plain scalar) that
// the loader fills from the guest's tables (wasm_wrappers/host_main_graphics.c,
// setAvm2Tables). Indexing syntax is identical, so no call site changes; the
// runtime never takes sizeof() of these.
#ifdef DYNAMIC_HOST
extern const Avm2AbcFileData* const* avm2_generated_abc_files;
extern uint32_t avm2_generated_abc_file_count;
extern const Avm2SymbolClassBinding* avm2_generated_symbol_classes;
extern uint32_t avm2_generated_symbol_class_count;
extern uint8_t avm2_generated_swf_version;
#else
extern const Avm2AbcFileData* const avm2_generated_abc_files[];
extern const uint32_t avm2_generated_abc_file_count;
extern const Avm2SymbolClassBinding avm2_generated_symbol_classes[];
extern const uint32_t avm2_generated_symbol_class_count;
extern const uint8_t avm2_generated_swf_version;
#endif
// Frame scoping for the two tag classes Ruffle runs per-frame
// (movie_clip.rs::run_abc_and_symbol_tags). Parallel to the arrays above:
// 0-based frame index of the DoABC tag / SymbolClass row, plus the DoABC2
// LAZY_INITIALIZE bit. All-zero for every movie whose AVM2 tags live in
// frame 1, which is 4934 of the 4943 corpus SWFs.
#ifdef DYNAMIC_HOST
extern const uint32_t* avm2_generated_abc_frames;
extern const uint8_t* avm2_generated_abc_lazy;
extern const uint32_t* avm2_generated_symbol_class_frames;
#else
extern const uint32_t avm2_generated_abc_frames[];
extern const uint8_t avm2_generated_abc_lazy[];
extern const uint32_t avm2_generated_symbol_class_frames[];
#endif

// Runs frame `frame_idx`'s (0-based) held-back DoABC + SymbolClass tags once.
// No-op for frame 0 (the boot path already ran it) and for movies with no
// deferred tags at all. Defined in avm2_main.c.
void avm2_run_frame_tags(Avm2Context* ctx, uint32_t frame_idx);

// ---------------------------------------------------------------------------
// Static timeline tables (Stage 5): emitted by the recompiler's
// abc_timeline.cpp second pass over the tag stream (AVM2 SWFs only, into
// RecompiledABC/abc_timeline.c). Mirrors Ruffle's static movie data; the
// AVM2 display tree consumes these directly — zero entanglement with the
// AVM1 tag.c runtime.
// ---------------------------------------------------------------------------

enum
{
	AVM2_TLOP_PLACE = 0,   // PlaceObject/2/3 (new character or modify/move)
	AVM2_TLOP_REMOVE = 1,  // RemoveObject/2
};

enum
{
	AVM2_TLF_HAS_CHAR = 1 << 0,
	AVM2_TLF_MOVE = 1 << 1,        // PlaceObject2 move flag
	AVM2_TLF_HAS_MATRIX = 1 << 2,
	AVM2_TLF_HAS_NAME = 1 << 3,
	AVM2_TLF_HAS_CLIP_DEPTH = 1 << 4,
	AVM2_TLF_HAS_RATIO = 1 << 5,
	AVM2_TLF_HAS_VISIBLE = 1 << 6,  // PlaceObject3 visible flag present
	AVM2_TLF_HAS_FILTERS = 1 << 7,  // PlaceObject3 SurfaceFilterList present
	AVM2_TLF_HAS_CXFORM = 1 << 8,   // CXFORM(WITHALPHA) present
	AVM2_TLF_HAS_BLEND = 1 << 9,    // PlaceObject3 BlendMode present
	AVM2_TLF_HAS_CACHE = 1 << 10,   // PlaceObject3 BitmapCache present
};

// One PlaceObject3 SurfaceFilterList entry, kept in the SWF's own fixed-point
// encoding so the runtime's AS-facing round trip does the exact same integer
// math as an AS-authored filter (see avm2_filters.c). `kind` matches
// Avm2FilterKind 1..8 — a tag list can never carry a displacement-map or
// shader filter.
typedef struct Avm2TagFilter
{
	uint8_t kind;
	int32_t blur_x, blur_y;    // Fixed16 bits
	int32_t angle, distance;   // Fixed16 bits (radians / pixels)
	int16_t strength;          // Fixed8 bits
	uint8_t quality;           // passes (0..15)
	uint8_t inner, knockout, on_top, composite_source;
	uint32_t color;  uint8_t alpha;    // shadow / glow / convolution default
	uint32_t color2; uint8_t alpha2;   // bevel highlight
	const float* cm;                   // 20 entries, ColorMatrix only
	uint8_t conv_cols, conv_rows;
	const float* conv_matrix; uint32_t conv_len;
	float divisor, bias;
	uint8_t preserve_alpha, clamp;
	uint8_t grad_count;
	const uint32_t* grad_colors;
	const uint8_t* grad_alphas;
	const uint8_t* grad_ratios;
} Avm2TagFilter;

typedef struct Avm2TimelineOp
{
	uint8_t kind;    // AVM2_TLOP_*
	uint16_t flags;  // AVM2_TLF_*
	uint8_t visible; // valid when HAS_VISIBLE
	uint16_t char_id;
	uint16_t depth;
	uint16_t clip_depth;
	uint16_t ratio;
	const char* name;  // NULL unless HAS_NAME
	// Matrix (valid when HAS_MATRIX): scale/rot as f32, translate twips.
	float mtx_a, mtx_b, mtx_c, mtx_d;
	int32_t mtx_tx, mtx_ty;
	// SurfaceFilterList (valid when HAS_FILTERS; an empty list still sets the
	// flag, which is how a PlaceObject3 CLEARS a depth's filters).
	uint16_t filter_count;
	const Avm2TagFilter* filters;
	// CXFORM (valid when HAS_CXFORM), in Ruffle's ColorTransform layout:
	// Fixed8 multipliers (256 = 1.0) and i16 addends, R/G/B/A order.
	int16_t cx_mult[4];
	int16_t cx_add[4];
	uint8_t blend_mode;    // SWF numeric blend id (valid when HAS_BLEND)
	uint8_t bitmap_cache;  // valid when HAS_CACHE
} Avm2TimelineOp;

typedef struct Avm2FrameLabelData
{
	uint32_t frame;      // 0-based frame index
	const char* label;
} Avm2FrameLabelData;

typedef struct Avm2TimelineData
{
	uint16_t char_id;          // 0 = root/main timeline
	uint32_t frame_count;      // ShowFrame count (incl. a trailing partial frame)
	uint32_t declared_frames;  // header/DefineSprite frame count field
	const Avm2TimelineOp* ops;
	const uint32_t* frame_op_starts;  // frame_count + 1 entries into ops[]
	uint32_t label_count;
	const Avm2FrameLabelData* labels;
	// 1 when the tag stream ended WITHOUT an End tag (Ruffle
	// preload_progress.has_end_tag == false): such a clip does not loop at its
	// last frame (determine_next_frame -> Same). Inverted sense on purpose so
	// the zero default -- and every timeline the recompiler emits without this
	// trailing field -- keeps the ordinary looping behaviour.
	uint8_t no_end_tag;
} Avm2TimelineData;

// Character dictionary entry (Define* tags): classification + bounds.
enum
{
	AVM2_CHAR_SHAPE = 0,
	AVM2_CHAR_SPRITE = 1,
	AVM2_CHAR_BUTTON = 2,
	AVM2_CHAR_TEXT = 3,      // DefineText/2 (static text)
	AVM2_CHAR_EDITTEXT = 4,  // DefineEditText
	AVM2_CHAR_BITMAP = 5,
	AVM2_CHAR_MORPHSHAPE = 6,
	AVM2_CHAR_VIDEO = 7,
	AVM2_CHAR_OTHER = 8,     // fonts, sounds, ... (not placeable)
};

typedef struct Avm2CharInfo
{
	uint16_t char_id;
	uint8_t kind;  // AVM2_CHAR_*
	// Self bounds in twips (shapes/texts; zero for sprites — computed from
	// children at runtime).
	int32_t xmin, xmax, ymin, ymax;
	// DefineEditText initial text (NULL when absent / not an EditText).
	const char* init_text;
	// Stroke-EXCLUSIVE self bounds in twips: DefineShape4's `EdgeBounds` and
	// DefineMorphShape2's `StartEdgeBounds` (SWF19). AVM2
	// `DisplayObject.getRect` reports this box where `getBounds` reports the
	// shape bounds above (Ruffle `BoundsMode::ScriptWithoutStrokes`).
	// `has_edge == 0` means the tag carried no EdgeBounds (DefineShape1-3,
	// DefineMorphShape1) and the shape bounds stand in, matching Ruffle's
	// reader. Trailing fields: older generated tables use a positional
	// initializer that stops at `init_text` and these zero out.
	int32_t exmin, exmax, eymin, eymax;
	uint8_t has_edge;
} Avm2CharInfo;

// Per-DefineShape geometry: a (vert_offset, vert_count) range into the
// resident shape_data[] vertex table (draws.c), keyed by char_id, so the
// AVM2 render walk can issue renderer_draw_shape without the AVM1 Character
// dictionary (which is not linked into the AVM2 runtime). Offsets are in
// VERTICES (each shape_data row is one 4*u32 vertex), matching
// render_webgpu_draw_shape's (offset, num_verts) arguments.
typedef struct Avm2ShapeGeom
{
	uint16_t char_id;
	// 1 iff every triangle of this shape uses a fill class the AVM2 walk +
	// WGSL shader render today: solid (T1), stroke (T2), gradient (T3). The
	// render walk skips shapes with renderable == 0 — after T3 the only such
	// class is BITMAP fills (0x40-0x43), deferred to a later tranche.
	uint8_t  renderable;
	uint32_t vert_offset;
	uint32_t vert_count;
	// T6 (DefineMorphShape): start index into morph_end_shape_data for this
	// shape's END vertices, paired 1:1 with the shape_data range above (both
	// appended in lockstep by the recompiler). is_morph == 1 routes the node
	// through the ratio-lerp runtime-tris path instead of renderer_draw_shape.
	uint32_t morph_end_offset;
	uint8_t  is_morph;
} Avm2ShapeGeom;

// DefineEditText static data (Stage 6). Flags mirror the tag bit-for-bit
// where possible; raw_text is the tag's InitialText verbatim (HTML markup
// preserved when the html flag is set).
enum
{
	AVM2_ETF_WORD_WRAP = 1 << 0,
	AVM2_ETF_MULTILINE = 1 << 1,
	AVM2_ETF_PASSWORD = 1 << 2,
	AVM2_ETF_READ_ONLY = 1 << 3,
	AVM2_ETF_AUTO_SIZE = 1 << 4,
	AVM2_ETF_NO_SELECT = 1 << 5,
	AVM2_ETF_BORDER = 1 << 6,
	AVM2_ETF_WAS_STATIC = 1 << 7,
	AVM2_ETF_HTML = 1 << 8,
	AVM2_ETF_USE_OUTLINES = 1 << 9,
	AVM2_ETF_HAS_FONT = 1 << 10,
	AVM2_ETF_HAS_FONT_CLASS = 1 << 11,
	AVM2_ETF_HAS_TEXT_COLOR = 1 << 12,
	AVM2_ETF_HAS_MAX_LENGTH = 1 << 13,
	AVM2_ETF_HAS_LAYOUT = 1 << 14,
	AVM2_ETF_HAS_TEXT = 1 << 15,
};

typedef struct Avm2EditTextData
{
	uint16_t char_id;
	uint16_t flags;          // AVM2_ETF_*
	uint16_t font_id;        // meaningful when HAS_FONT
	const char* font_class;  // NULL unless HAS_FONT_CLASS
	uint16_t font_height;    // twips (HAS_FONT)
	uint32_t color_rgba;     // 0xRRGGBBAA (HAS_TEXT_COLOR)
	uint16_t max_length;     // HAS_MAX_LENGTH
	uint8_t align;           // 0 left, 1 right, 2 center, 3 justify (HAS_LAYOUT)
	uint16_t left_margin;    // twips (HAS_LAYOUT)
	uint16_t right_margin;   // twips (HAS_LAYOUT)
	int16_t indent;          // twips (HAS_LAYOUT) -- SIGNED (see abc_timeline.cpp)
	int16_t leading;         // twips (HAS_LAYOUT)
	const char* variable_name;  // NULL when empty
	const char* raw_text;    // NULL unless HAS_TEXT
	// CSMTextSettings (tag 74) for this character, if present.
	uint8_t has_render_settings;
	uint8_t aa_advanced;     // UseFlashType != 0
	uint8_t grid_fit;        // 0 none, 1 pixel, 2 subpixel
	float cs_thickness, cs_sharpness;
} Avm2EditTextData;

// DefineFont2/3 data (Stage 6 + RWK-2): layout measurement (advances + code
// table + vertical metrics) plus flattened glyph outlines for the CPU
// rasterizer behind BitmapData.draw(TextField). Units are the font's EM
// square (1024 for DefineFont2, 20480 for DefineFont3).
typedef struct Avm2FontData
{
	uint16_t font_id;
	const char* name;
	uint8_t bold, italic;
	uint8_t has_layout;
	uint16_t em_square;      // 1024 (DefineFont2) or 20480 (DefineFont3)
	int32_t ascent, descent, leading;  // font units (has_layout)
	uint32_t glyph_count;
	const uint16_t* codes;   // glyph index -> character code
	// glyph index -> advance, font units (has_layout). Widened past int16
	// because the SWF ADVANCETABLE is UNSIGNED 16-bit (Ruffle read.rs:1079)
	// and a DefineFont3 face can legitimately store 32768.
	const int32_t* advances;
	// Glyph outlines: contour polylines in font units, curves pre-flattened
	// at recompile time. All NULL when outlines are unavailable (device
	// fallback font, older generated tables). Contours of glyph g are
	// contour indices [glyph_contour_start[g], glyph_contour_start[g+1]);
	// contour k's points are pair indices
	// [k == glyph_contour_start[g] ? glyph_pt_start[g] : glyph_contour_ends[k-1],
	//  glyph_contour_ends[k]) into glyph_pts. Contours are implicitly closed.
	const int32_t* glyph_pts;             // x,y pairs
	const uint32_t* glyph_pt_start;       // glyph_count+1 entries
	const uint32_t* glyph_contour_ends;   // absolute pair indices
	const uint32_t* glyph_contour_start;  // glyph_count+1 entries
	// Kerning pairs from the TTF `kern` table, in character codes (B9). Only
	// device faces declared in test.toml carry these; SWF-embedded faces leave
	// them NULL/0. Ruffle reads `kern` ONLY, never GPOS (font.rs:261-269).
	const uint16_t* kern_left;
	const uint16_t* kern_right;
	const int16_t* kern_value;   // font units
	uint32_t kern_count;
	// Per-glyph fallback chain (test.toml [font_sorts]): indices into
	// avm2_generated_device_fonts, in order, main face FIRST. Ruffle
	// FontSet::from_fonts (font.rs:1492) — metrics and kerning always come
	// from the main face, only the glyph lookup walks the chain.
	const uint32_t* fallback;
	uint32_t fallback_count;
} Avm2FontData;

// One rendered glyph of a TextField, as collected by
// avm2_edittext_collect_glyphs (avm2_text.c) for BitmapData.draw: placement
// is field-local twips (layout_to_local applied), color is the span's
// 0xRRGGBB, scale converts font units to twips at the span's point size.
typedef struct Avm2GlyphPlacement
{
	const Avm2FontData* font;
	uint32_t glyph;    // glyph index into the font tables
	int32_t x_twips;   // pen x (left edge of the glyph cell)
	int32_t y_twips;   // baseline y
	float scale;       // twips per font unit
	uint32_t color;    // straight 0xRRGGBB
} Avm2GlyphPlacement;

// Static text (DefineText/2, the StaticText display object). One placed glyph
// of a DefineText character, parsed from the tag's GLYPHENTRY runs at recompile
// time: font_id resolves to avm2_generated_fonts at place-time; placement is
// field-local twips (the record matrix translation + running advance baked in),
// scale converts font units to twips at the record's text height, color is the
// span's straight 0xRRGGBB. Fed through the SAME glyph raster/tessellation as
// EditText (avm2_statictext_collect_glyphs -> Avm2GlyphPlacement).
typedef struct Avm2StaticGlyph
{
	uint16_t font_id;
	uint32_t glyph;    // glyph index into the font tables
	int32_t x_twips;   // pen x (left edge of the glyph cell), field-local
	int32_t y_twips;   // baseline y, field-local
	float scale;       // twips per font unit (text_height / em_square)
	uint32_t color;    // straight 0xRRGGBB
} Avm2StaticGlyph;

// One DefineText/2 character: a contiguous run of placed glyphs in the flat
// avm2_generated_static_glyphs table (mirrors Avm2ShapeGeom's range model).
typedef struct Avm2StaticTextData
{
	uint16_t char_id;
	uint32_t glyph_start;   // index into avm2_generated_static_glyphs
	uint32_t glyph_count;
} Avm2StaticTextData;

// DefineSceneAndFrameLabelData (root timeline only).
typedef struct Avm2SceneData
{
	const char* name;
	uint32_t offset;  // 0-based start frame
} Avm2SceneData;

// DefineButton/2 state records (Stage-5 tranche 5).
typedef struct Avm2ButtonRecordData
{
	uint16_t char_id;
	uint16_t depth;
	uint8_t state_flags;  // bit0 up, bit1 over, bit2 down, bit3 hitTest
	uint8_t has_matrix;
	float mtx_a, mtx_b, mtx_c, mtx_d;
	int32_t mtx_tx, mtx_ty;
} Avm2ButtonRecordData;

typedef struct Avm2ButtonData
{
	uint16_t char_id;
	uint32_t record_count;
	const Avm2ButtonRecordData* records;
} Avm2ButtonData;

// Embedded bitmap (DefineBitsLossless/2), decoded at recompile time to
// STRAIGHT (non-premultiplied) RGBA — byte order R,G,B,A per pixel, row
// major, width*height*4 bytes (matches Ruffle decode_define_bits_lossless
// out_data). `transparency` = the DefineBitsLossless2 alpha flag; when 0,
// alpha bytes are all 255. The BitmapData runtime premultiplies on ingest.
typedef struct Avm2BitmapData
{
	uint16_t char_id;
	// 32-bit: a PNG smuggled through DefineBitsJPEG2/3 (SWF8+) carries u32
	// dimensions and can exceed 65535 (see visual/define_bits_jpeg2_huge).
	// DefineBitsLossless is u16-limited, but the field is shared.
	uint32_t width;
	uint32_t height;
	uint8_t transparency;
	// Straight RGBA, width*height*4 bytes (NULL if decode failed). When
	// z_len != 0 the pointer is instead a zlib DEFLATE stream of length z_len
	// that inflates to width*height*4 straight-RGBA bytes (recompile-time
	// compression — the raw tables are ~46 MB for a real game); z_len == 0
	// means `rgba` is the uncompressed pixels.
	const uint8_t* rgba;
	uint32_t z_len;
} Avm2BitmapData;

// Embedded binary (DefineBinaryData, tag 87): raw bytes, ByteArray seed.
typedef struct Avm2BinaryData
{
	uint16_t char_id;
	uint32_t len;
	const uint8_t* bytes;  // NULL when len == 0
} Avm2BinaryData;

// Embedded sound (DefineSound, tag 14). Fields mirror the tag bitfields.
typedef struct Avm2SoundData
{
	uint16_t char_id;
	uint8_t format;        // SoundFormat (0 uncompressed-native-endian, 2 MP3, ...)
	uint8_t rate;          // 0=5512Hz 1=11025 2=22050 3=44100
	uint8_t sample_size;   // 0=8-bit 1=16-bit
	uint8_t stereo;        // 0=mono 1=stereo
	uint32_t sample_count;
	uint32_t data_size;    // bytesTotal: compressed payload bytes, minus the
	                       // 2-byte MP3 seek prefix (Ruffle SoundInstance.size)
	// Full decodable payload — ALL remaining tag bytes, INCLUDING the 2-byte
	// MP3 seek-samples prefix (what audio_define_sound expects). data_size
	// above keeps its bytesTotal semantics; this pair feeds the mixer.
	const uint8_t* data;   // NULL when data_len == 0
	uint32_t data_len;
} Avm2SoundData;

// Provided by the generated RecompiledABC/abc_timeline.c (pointer/scalar form
// under DYNAMIC_HOST, see above):
#ifdef DYNAMIC_HOST
extern const Avm2TimelineData* avm2_generated_timelines;
extern uint32_t avm2_generated_timeline_count;
extern const Avm2CharInfo* avm2_generated_chars;
extern uint32_t avm2_generated_char_count;
extern const Avm2ShapeGeom* avm2_generated_shape_geom;
extern uint32_t avm2_generated_shape_geom_count;
extern const Avm2SceneData* avm2_generated_scenes;
extern uint32_t avm2_generated_scene_count;
extern const Avm2ButtonData* avm2_generated_buttons;
extern uint32_t avm2_generated_button_count;
extern const Avm2EditTextData* avm2_generated_edittexts;
extern uint32_t avm2_generated_edittext_count;
extern const Avm2FontData* avm2_generated_fonts;
extern uint32_t avm2_generated_font_count;
#else
extern const Avm2TimelineData avm2_generated_timelines[];
extern const uint32_t avm2_generated_timeline_count;
extern const Avm2CharInfo avm2_generated_chars[];
extern const uint32_t avm2_generated_char_count;
extern const Avm2ShapeGeom avm2_generated_shape_geom[];
extern const uint32_t avm2_generated_shape_geom_count;
extern const Avm2SceneData avm2_generated_scenes[];
extern const uint32_t avm2_generated_scene_count;
extern const Avm2ButtonData avm2_generated_buttons[];
extern const uint32_t avm2_generated_button_count;
extern const Avm2EditTextData avm2_generated_edittexts[];
extern const uint32_t avm2_generated_edittext_count;
extern const Avm2FontData avm2_generated_fonts[];
extern const uint32_t avm2_generated_font_count;
#endif
// Device faces declared in the test harness's test.toml (B9). Player-level
// state, so only the main movie's table exists (never symbol-prefixed).
#ifdef DYNAMIC_HOST
extern const Avm2FontData* avm2_generated_device_fonts;
extern uint32_t avm2_generated_device_font_count;
extern const Avm2StaticGlyph* avm2_generated_static_glyphs;
extern uint32_t avm2_generated_static_glyph_count;
extern const Avm2StaticTextData* avm2_generated_statictexts;
extern uint32_t avm2_generated_statictext_count;
extern const Avm2BitmapData* avm2_generated_bitmaps;
extern uint32_t avm2_generated_bitmap_count;
extern const Avm2BinaryData* avm2_generated_binaries;
extern uint32_t avm2_generated_binary_count;
extern const Avm2SoundData* avm2_generated_sounds;
extern uint32_t avm2_generated_sound_count;
extern const int32_t* avm2_generated_stage_rect;   // xmin xmax ymin ymax twips
extern uint16_t avm2_generated_frame_rate;     // 8.8 fixed
extern uint16_t avm2_generated_header_frames;  // header frame count
extern uint32_t avm2_generated_bg_color;       // 0xRRGGBB (SetBackgroundColor)
#else
extern const Avm2FontData avm2_generated_device_fonts[];
extern const uint32_t avm2_generated_device_font_count;
extern const Avm2StaticGlyph avm2_generated_static_glyphs[];
extern const uint32_t avm2_generated_static_glyph_count;
extern const Avm2StaticTextData avm2_generated_statictexts[];
extern const uint32_t avm2_generated_statictext_count;
extern const Avm2BitmapData avm2_generated_bitmaps[];
extern const uint32_t avm2_generated_bitmap_count;
extern const Avm2BinaryData avm2_generated_binaries[];
extern const uint32_t avm2_generated_binary_count;
extern const Avm2SoundData avm2_generated_sounds[];
extern const uint32_t avm2_generated_sound_count;
extern const int32_t avm2_generated_stage_rect[4];   // xmin xmax ymin ymax twips
extern const uint16_t avm2_generated_frame_rate;     // 8.8 fixed
extern const uint16_t avm2_generated_header_frames;  // header frame count
extern const uint32_t avm2_generated_bg_color;       // 0xRRGGBB (SetBackgroundColor)
#endif

// ---------------------------------------------------------------------------
// Per-movie table aggregate (loader-arc tranche 6)
// ---------------------------------------------------------------------------
//
// The MAIN movie has no Avm2MovieTables — it boots from the globals above,
// which stay exactly as they are. A CHILD movie (a sibling .swf recompiled
// with `symbol_prefix`/`char_id_base` set) emits its whole table set under
// prefixed names plus one `<prefix>avm2_movie_tables` bundling them, and the
// generated movie registry hangs it off MovieEntry.avm2_tables. Loader's SWF
// arm registers the child from that pointer.
//
// Char ids inside a child's tables are already offset by `char_id_base`, so
// they never collide with the parent's and the runtime's char lookups stay
// keyed by a bare id — they just gain a second place to look. The movie's own
// root timeline is the row whose char_id == char_id_base.
typedef struct Avm2MovieTables
{
	const Avm2AbcFileData* const* abc_files;   uint32_t abc_file_count;
	const Avm2SymbolClassBinding* symbol_classes; uint32_t symbol_class_count;
	const Avm2TimelineData* timelines;         uint32_t timeline_count;
	const Avm2CharInfo* chars;                 uint32_t char_count;
	const Avm2ShapeGeom* shape_geom;           uint32_t shape_geom_count;
	const Avm2SceneData* scenes;               uint32_t scene_count;
	const Avm2ButtonData* buttons;             uint32_t button_count;
	const Avm2EditTextData* edittexts;         uint32_t edittext_count;
	const Avm2FontData* fonts;                 uint32_t font_count;
	const Avm2StaticGlyph* static_glyphs;      uint32_t static_glyph_count;
	const Avm2StaticTextData* statictexts;     uint32_t statictext_count;
	const Avm2BitmapData* bitmaps;             uint32_t bitmap_count;
	const Avm2BinaryData* binaries;            uint32_t binary_count;
	const Avm2SoundData* sounds;               uint32_t sound_count;
	const int32_t* stage_rect;                 // xmin xmax ymin ymax twips
	uint16_t frame_rate;                       // 8.8 fixed
	uint16_t header_frames;
	uint32_t bg_color;
	uint32_t char_id_base;
	uint8_t swf_version;
} Avm2MovieTables;

// ---------------------------------------------------------------------------
// Runtime-mutable per-file state
// ---------------------------------------------------------------------------

enum
{
	AVM2_SCRIPT_UNINITIALIZED = 0,
	AVM2_SCRIPT_INITIALIZING = 1,
	AVM2_SCRIPT_INITIALIZED = 2,
};

struct Avm2AbcFileRt
{
	const Avm2AbcFileData* data;
	Avm2Context* ctx;
	// The ApplicationDomain this file's definitions live in and resolve
	// against (loader-arc tranche 8). Every file of a single-movie program
	// shares the root scope; a loaded movie gets the LoaderContext's domain or
	// a fresh child of the loading movie's.
	const struct Avm2DomainScope* scope;
	Avm2Object** script_globals;  // one per script
	uint8_t* script_init_state;
	Avm2Class** classes;          // realized by NewClass; NULL until then
	// (mn_idx -> resolved Class*) memo for avm2_class_for_mn. Sized to
	// data->multiname_count; NULL entry = not-yet-cached. Only NON-NULL
	// resolutions are cached (a type name is transiently unresolvable during
	// its own cinit; the domain is append-only so a non-NULL hit is permanent).
	Avm2Class** coerce_class_memo;
	// (method_index -> synthetic activation class) for NewActivation. avmplus
	// mints ONE sealed+final class per method at verify time (Ruffle
	// verify.rs create_activation_class / Class::for_activation), so the class
	// is per-method, not per-call. Sized to data->method_count; NULL = not
	// minted yet. Deliberately NOT flagged AVM2_CLASS_FLAG_SYNTH_CATCH: that
	// flag makes the GC free the class with the object, and this one is shared
	// across every activation of the method (classes are immortal — see
	// avm2_main.c mark_class_roots; this one has no class_object/prototype/
	// scope so it needs no marking at all).
	Avm2Class** activation_classes;
	// (method_index -> first vtable binding) for CallStatic. avmplus gives a
	// MethodInfo exactly ONE MethodEnv — the one from its FIRST binding — and
	// `callstatic` dispatches through it, so the callee runs on the scope
	// captured where the method was first installed, NOT on the caller's scope
	// (avm2/getouterscope_two_classobjects prints 50 four times because both
	// methods were first bound by the FIRST NewClass). First writer wins.
	Avm2ScopeChain** method_env_scope;
	Avm2Class** method_env_class;
};

// Activation record passed to every emitted method body and native method.
typedef struct Avm2Activation
{
	Avm2Context* ctx;
	Avm2AbcFileRt* file;       // defining ABC file (NULL for pure natives)
	Avm2Class* bound_class;    // defining class (ConstructSuper); NULL for scripts
	Avm2ScopeChain* outer;     // captured outer scope chain (NULL = empty)
	Avm2Value this_val;
	const Avm2Value* args;
	uint32_t argc;
	Avm2Object* callee;        // function object being invoked (arguments.callee); may be NULL
} Avm2Activation;

#endif // AVM2_ABC_H
