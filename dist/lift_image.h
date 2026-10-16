// lift_image.h: Lift Image, a baked image format for real-time engines. Specified in docs/format.md.
// Generated from src/ by tools/amalgamate.py, so edit those instead.
// MIT License, Copyright (c) 2026 Nick Klingensmith. The full text is at the end of this file.
//
// In exactly one C or C++ file (C99 or C++11 and later), ideally on its own, since the implementation defines internal names:
//   #define LFI_IMPLEMENTATION    // the decoder
//   #define LFI_ENCODER           // and the encoder, which also needs -lm
//   #define LFI_ENCODER_QUALITY 0 // optional: about 4 KB less encoder and 25% faster lossy encodes, for files about 1.5% larger
//   #include "lift_image.h"
// Everywhere else, include it plainly for the API.

#ifndef LIFT_IMAGE_H
#define LIFT_IMAGE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Lift Image: opponent color + 9/3 integer wavelet + deadzone quantizer + tANS. See docs/format.md.

typedef enum {
	lfi_alpha_auto,   // cutout if alpha is only ever 0 and 255, blend otherwise
	lfi_alpha_blend,  // alpha keeps its gradients
	lfi_alpha_cutout, // alpha is thresholded at 128, then decodes as exactly 0 or 255
} lfi_alpha_e;

typedef struct {
	float       step;          // base quantizer step, <= 1 is lossless
	int32_t     channels;      // 1 gray from R, 2 normal XY from RG, 3 color, 4 color + alpha. 0 picks 3 or 4 by the alpha
	bool        linear;        // stored for the reader, the codec treats sRGB and linear data alike
	lfi_alpha_e alpha;
	bool        premultiplied; // stored for the reader, and decoding keeps RGB <= A. Straight alpha gets the color
	                           // under transparent pixels replaced by a smooth fill, so reduced mips don't fringe
	float       dark;          // finer chroma steps in dark tiles, in quarter octaves per halving of brightness below 64. Luma gets half. Try 4
	int32_t     tile_log2;     // 0 for 512x512 tiles
	int32_t     layers;        // 0 or 1 for one image. Array layers follow one another in the pixels
	bool        cubemap;       // layers are cube faces +X, -X, +Y, -Y, +Z, -Z, six per cube
	bool        data;          // 3 or 4 channels of independent data, like packed material maps: no color transform or
	                           // perceptual tuning, and alpha is just a fourth channel. 8-bit only
} lfi_opt_t;

typedef enum {
	lfi_format_native,   // lfi_info's pixel_size: R8, RG8, RGBA8, or R16F and RGBA16F for HDR
	lfi_format_rg11b10f, // HDR only, 4 bytes, R in the low bits. Gray goes in R
	lfi_format_rgb9e5,   // HDR only, 4 bytes, R in the low bits. Gray goes in R
} lfi_format_e;

typedef enum {
	lfi_ok,
	lfi_error_truncated, // the data ends before what this call needs, as when a prefix is too short for a mip
	lfi_error_format,    // not a Lift Image, or a version or required feature this decoder doesn't know
	lfi_error_corrupt,   // inconsistent header, tables or streams
	lfi_error_checksum,  // the file doesn't match its CRC, only from lfi_verify
	lfi_error_argument,  // a mip or cache_mip out of range
} lfi_result_e;

typedef struct {
	int32_t w, h;
	int32_t layers;     // array layers, six per cube for cubemaps
	int32_t channels;   // coded channels
	int32_t pixel_size; // bytes per decoded pixel: R8, RG8, or RGBA8 for color with or without alpha. HDR is R16F or RGBA16F
	int32_t mips;       // decodable mips, where mip k is (w + 2^k - 1) >> k wide
	size_t  file_size;  // bytes of the whole file, so a reader that has only its first LFI_HEADER_MAX knows how much follows
	bool    linear;
	bool    premultiplied;
	bool    hdr;
	bool    cubemap;
	bool    data;
} lfi_info_t;

#define LFI_HEADER_MAX 93 // the most lfi_info reads from the start of a file

#ifdef __cplusplus
extern "C" {
#endif

// Decoding reads data in place, so files can be mapped straight from disk. Every decoder call validates what it
// reads, and data needs no alignment. A prefix of lfi_mip_bytes decodes to that mip, so streamed files can
// show a small mip before the rest arrives.
uint8_t*     lfi_encode      (const uint8_t* rgba, int32_t w, int32_t h, const lfi_opt_t* opt, size_t* out_size);
uint8_t*     lfi_encode_hdr  (const float* rgba, int32_t w, int32_t h, const lfi_opt_t* opt, size_t* out_size);
lfi_result_e lfi_info       (const uint8_t* data, size_t size, lfi_info_t* out_info);
lfi_result_e lfi_verify      (const uint8_t* data, size_t size);
size_t       lfi_mip_bytes   (const uint8_t* data, size_t size, int32_t mip);
size_t       lfi_scratch_size(const uint8_t* data, size_t size);
size_t       lfi_cache_size  (const uint8_t* data, size_t size, int32_t mip);
lfi_result_e lfi_decode_step (const uint8_t* data, size_t size, const int16_t* opt_cache, int32_t cache_mip, int32_t mip, lfi_format_e format, int16_t* opt_out_cache, uint8_t* opt_out_pixels, void* scratch);

// Inline, so the decoder isn't compiled a second time specialized for these constants
static inline lfi_result_e lfi_decode(const uint8_t* data, size_t size, int32_t mip, lfi_format_e format, uint8_t* out_pixels, void* scratch) {
	return lfi_decode_step(data, size, NULL, 0, mip, format, NULL, out_pixels, scratch);
}

#ifdef __cplusplus
}
#endif

#endif // LIFT_IMAGE_H

#if defined(LFI_IMPLEMENTATION) && !defined(LIFT_IMAGE_IMPLEMENTED)
#define LIFT_IMAGE_IMPLEMENTED

// ---- ent.h

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// The codec compiles as C and C++. C++ has no restrict, and defining that keyword would break MSVC's headers.
#if defined(__cplusplus) || defined(_MSC_VER)
#define LFI_RESTRICT __restrict
#else
#define LFI_RESTRICT restrict
#endif

// SSE2 blocks for interleaving and clamping loops, which MSVC, and clang without AVX2, vectorize only 4 lanes wide.
// With AVX2, clang and GCC do better on the plain loops, as does ARM.
#if (defined(_MSC_VER) && !defined(__clang__) && (defined(_M_X64) || defined(_M_IX86))) || (defined(__SSE2__) && !defined(__AVX2__))
#define LFI_SSE2 1
#include <emmintrin.h>
#else
#define LFI_SSE2 0
#endif

// Static-model tANS with two interleaved states, each with its own bit stream. A symbol's extra
// bits follow its state bits in the same stream, so one shift consumes both. Symbols have
// ENT_SYMBOL_BITS, models are selected per call by context, and contexts may share a table.

#define ENT_PROB_BITS        11 // 12 is 0.1% smaller, but table builds then dominate small images
#define ENT_PROB_SCALE       (1u << ENT_PROB_BITS)
#define ENT_SYMBOL_BITS      9
#define ENT_SYMBOLS          (1 << ENT_SYMBOL_BITS)
#define ENT_TWIN             16 // symbols differing only in this bit may share one listed frequency, half each
#define ENT_INVALID          0xFF // from an ent_extra_bits_fn, for a symbol its table can't hold
#define ENT_CONTEXTS         16
#define ENT_PAD              16 // readers may touch this many bytes past the end: an 8 byte refill up to 7 bytes ahead
#define ENT_SPREAD_STEP      ((ENT_PROB_SCALE >> 1) + (ENT_PROB_SCALE >> 3) + 3)
#define ENT_TABLE_WORDS      (ENT_PROB_SCALE * 3 / 2) // an entry per state, then a u16 symbol per state
#define ENT_TABLES_SIZE      (ENT_CONTEXTS * ENT_TABLE_WORDS * sizeof(uint32_t) + ENT_CONTEXTS) // tables, then each context's table index

// Entry: state + extra bits | state bits << 8 | next state base << 16
#define ENT_ENTRY_BITS(entry)       ((entry) & 0xFF)
#define ENT_ENTRY_STATE_BITS(entry) ((entry) >> 8 & 0xFF)
#define ENT_ENTRY_BASE(entry)       ((entry) >> 16)

typedef struct {
	uint8_t* data;
	size_t   size;
	size_t   capacity;
} buffer_t;

typedef struct {
	buffer_t out;
	uint64_t bits;
	uint32_t count;
} bit_writer_t;

typedef struct {
	uint16_t* symbols;     // (context << ENT_SYMBOL_BITS) | symbol
	uint32_t* extra;       // raw bits after each symbol
	uint8_t*  extra_count;
	size_t    count;
	size_t    capacity;
} ent_encoder_t;

// Streams are read at a bit position, one 8 byte load per symbol, which holds fewer registers than a bit buffer.
// Positions count from the stream's end, so an overrun is a position above 0, checked without another register.
typedef struct {
	const uint32_t* tables; // from ent_open_tables
	const uint8_t*  end[2];
	int64_t         pos[2];
	uint32_t        state[2];
} ent_decoder_t;

typedef uint32_t (*ent_extra_bits_fn)(uint32_t table, uint32_t symbol);

void buffer_push    (buffer_t* ref_buffer, const void* data, size_t size);
void buffer_u32     (buffer_t* ref_buffer, uint32_t value);
void ent_grow       (ent_encoder_t* ref_encoder);
void ent_write      (ent_encoder_t* encoders, int32_t count, buffer_t* out_tables, buffer_t* out_streams);
void ent_free       (ent_encoder_t* ref_encoder);
bool ent_open_tables(uint32_t* tables_mem, const uint8_t* data, size_t size, ent_extra_bits_fn extra_bits);
ent_decoder_t ent_open_stream(const uint32_t* tables, const uint8_t* data, size_t size0, size_t size1);

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
static inline uint32_t bit_length(uint32_t value) { unsigned long top; return _BitScanReverse(&top, value) ? (uint32_t)top + 1 : 0; }
#else
static inline uint32_t bit_length(uint32_t value) { return value ? 32 - (uint32_t)__builtin_clz(value) : 0; }
#endif

static inline uint32_t read_u32(const uint8_t* at) { uint32_t value; memcpy(&value, at, 4); return value; }
static inline uint64_t read_u64(const uint8_t* at) { uint64_t value; memcpy(&value, at, 8); return value; }

// A symbol in a context, then extra_count raw bits
static inline void ent_symbol(ent_encoder_t* ref_encoder, uint32_t context, uint32_t symbol, uint32_t extra, uint32_t extra_count) {
	if (ref_encoder->count == ref_encoder->capacity) ent_grow(ref_encoder);
	ref_encoder->symbols    [ref_encoder->count] = (uint16_t)(context << ENT_SYMBOL_BITS | symbol);
	ref_encoder->extra      [ref_encoder->count] = extra;
	ref_encoder->extra_count[ref_encoder->count] = (uint8_t)extra_count;
	ref_encoder->count++;
}

static inline ent_decoder_t ent_streams(const uint32_t* tables, const uint8_t* end0, const uint8_t* end1, int64_t pos0, int64_t pos1, uint32_t state0, uint32_t state1) {
	ent_decoder_t decoder = { tables, { end0, end1 }, { pos0, pos1 }, { state0, state1 } };
	return decoder;
}

static inline const uint32_t* ent_table(const ent_decoder_t* decoder, uint32_t context) {
	const uint8_t* map = (const uint8_t*)(decoder->tables + ENT_CONTEXTS * ENT_TABLE_WORDS);
	return decoder->tables + map[context] * ENT_TABLE_WORDS;
}

static inline uint32_t ent_table_symbol(const uint32_t* table, uint32_t state) { return ((const uint16_t*)(table + ENT_PROB_SCALE))[state]; }

// One step of a state on its stream, returning the extra bits. State and extra bits take at most
// 11 + 25, within the 57 a load at a bit position covers. Callers check the position stays <= 0.
static inline uint32_t ent_step(uint32_t* ref_state, const uint8_t* end, int64_t* ref_pos, const uint32_t* table) {
	uint64_t window = read_u64(end + (*ref_pos >> 3)) >> (*ref_pos & 7);
	uint32_t entry  = table[*ref_state];
	uint32_t bits   = (uint32_t)(window & ((1ull << ENT_ENTRY_BITS(entry)) - 1));
	*ref_pos   += ENT_ENTRY_BITS(entry);
	*ref_state  = ENT_ENTRY_BASE(entry) + (bits & ((1u << ENT_ENTRY_STATE_BITS(entry)) - 1));
	return bits >> ENT_ENTRY_STATE_BITS(entry);
}

// One symbol from a context without extra bits. Hot loops keep the states and positions in locals instead.
static inline uint32_t ent_decode(ent_decoder_t* ref_decoder, uint32_t context) {
	const uint32_t* table  = ent_table(ref_decoder, context);
	uint32_t        symbol = ent_table_symbol(table, ref_decoder->state[0]);
	ent_step(&ref_decoder->state[0], ref_decoder->end[0], &ref_decoder->pos[0], table);

	const uint8_t* end   = ref_decoder->end[0];   ref_decoder->end[0]   = ref_decoder->end[1];   ref_decoder->end[1]   = end;
	int64_t        pos   = ref_decoder->pos[0];   ref_decoder->pos[0]   = ref_decoder->pos[1];   ref_decoder->pos[1]   = pos;
	uint32_t       state = ref_decoder->state[0]; ref_decoder->state[0] = ref_decoder->state[1]; ref_decoder->state[1] = state;
	return symbol;
}

// ---- color.h

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// Reversible opponent color by lifting: chroma is R - G and B - G, then Y = G + floor(0.3 (R - G)) + floor(0.1 (B - G)),
// about 0.3 R + 0.6 G + 0.1 B. The axes follow the eye's, as in XYB: luma, red against green, and blue against green.
#define RED_WEIGHT  19661 // 0.3 in Q16
#define BLUE_WEIGHT 6554  // 0.1 in Q16

// Products of two int16s, so compilers see a multiply high and keep the sums in 16-bit lanes
static inline int16_t multiply_high(int16_t value, int16_t weight) { return (int16_t)((value * weight) >> 16); }

// Y less G, products floored as a 16-bit multiply high does
static inline int16_t luma_lift(int16_t red_green, int16_t blue_green) {
	return (int16_t)(multiply_high(red_green, RED_WEIGHT) + multiply_high(blue_green, BLUE_WEIGHT));
}

// R, G and B planes become Y, red against green and blue against green, in place. Fixed point planes keep the
// products' fractions.
static inline void color_forward(int16_t* LFI_RESTRICT ref_r, int16_t* LFI_RESTRICT ref_g, int16_t* LFI_RESTRICT ref_b, size_t count) {
	for (size_t i = 0; i < count; i++) {
		int16_t g = ref_g[i], red_green = (int16_t)(ref_r[i] - g), blue_green = (int16_t)(ref_b[i] - g);
		ref_r[i] = (int16_t)(g + luma_lift(red_green, blue_green));
		ref_g[i] = red_green;
		ref_b[i] = blue_green;
	}
}

// MSVC won't vectorize selects or comparisons, so it gets sign-mask arithmetic and one uint32 store per pixel.
// GCC and clang turn selects into min and max, and vectorize interleaved 16-bit stores best.
#if defined(_MSC_VER) && !defined(__clang__)
static inline int16_t clamp_u8(int16_t value) {
	int16_t positive = (int16_t)(value & ~(value >> 15)), over = (int16_t)(positive - 255);
	return (int16_t)(positive - (over & ~(over >> 15)));
}
static inline int16_t cap_u8(int16_t value, int16_t cap) { // clamped first, so the difference can't overflow
	int16_t clamped = clamp_u8(value), clamped_cap = clamp_u8(cap), over = (int16_t)(clamped - clamped_cap);
	return (int16_t)(clamped_cap + (over & (over >> 15)));
}
static inline void put_rgba(uint8_t* out, int32_t i, int16_t r, int16_t g, int16_t b, int16_t a) {
	((uint32_t*)out)[i] = (uint32_t)(uint16_t)r | (uint32_t)(uint16_t)g << 8 | (uint32_t)(uint16_t)b << 16 | (uint32_t)(uint16_t)a << 24;
}
#else
static inline int16_t clamp_u8(int16_t value) { return (int16_t)(value < 0 ? 0 : value > 255 ? 255 : value); }
static inline int16_t cap_u8(int16_t value, int16_t cap) { return clamp_u8(value < cap ? value : cap); }
static inline void put_rgba(uint8_t* out, int32_t i, int16_t r, int16_t g, int16_t b, int16_t a) {
	((uint16_t*)out)[i*2]     = (uint16_t)(r | g << 8);
	((uint16_t*)out)[i*2 + 1] = (uint16_t)(b | a << 8);
}
#endif

// Fractional bits of fixed point planes
#define FIXED_BITS 4

// Everything fits 16 bits, so the math runs in 16-bit lanes. RGB is held to cap, which premultiplied alpha sets to
// the alpha row and other uses to 255s. Fixed point planes are rounded off after converting.
static inline void color_inverse_row(const int16_t* LFI_RESTRICT y, const int16_t* LFI_RESTRICT red_green, const int16_t* LFI_RESTRICT blue_green, const int16_t* LFI_RESTRICT a, const int16_t* LFI_RESTRICT cap, uint8_t* LFI_RESTRICT out_rgba, int32_t width, bool fixed) {
	int32_t i = 0;
#if LFI_SSE2
	for (__m128i half = _mm_set1_epi16(fixed ? 1 << (FIXED_BITS - 1) : 0), count = _mm_cvtsi32_si128(fixed ? FIXED_BITS : 0),
	     red = _mm_set1_epi16(RED_WEIGHT), blue = _mm_set1_epi16(BLUE_WEIGHT); i + 8 <= width; i += 8) {
		__m128i rg8  = _mm_loadu_si128((const __m128i*)(red_green + i)), bg8 = _mm_loadu_si128((const __m128i*)(blue_green + i));
		__m128i g    = _mm_sub_epi16(_mm_sub_epi16(_mm_loadu_si128((const __m128i*)(y + i)), _mm_mulhi_epi16(rg8, red)), _mm_mulhi_epi16(bg8, blue));
		__m128i r    = _mm_sra_epi16(_mm_add_epi16(_mm_add_epi16(g, rg8), half), count);
		__m128i b    = _mm_sra_epi16(_mm_add_epi16(_mm_add_epi16(g, bg8), half), count);
		g = _mm_sra_epi16(_mm_add_epi16(g, half), count);
		__m128i cap8 = _mm_loadu_si128((const __m128i*)(cap + i));
		__m128i rb   = _mm_packus_epi16(_mm_min_epi16(r, cap8), _mm_min_epi16(b, cap8)); // packus clamps after the min
		__m128i ga   = _mm_packus_epi16(_mm_min_epi16(g, cap8), _mm_loadu_si128((const __m128i*)(a + i)));
		__m128i rg   = _mm_unpacklo_epi8(rb, ga), ba = _mm_unpackhi_epi8(rb, ga);
		_mm_storeu_si128((__m128i*)(out_rgba + i * 4),      _mm_unpacklo_epi16(rg, ba));
		_mm_storeu_si128((__m128i*)(out_rgba + i * 4 + 16), _mm_unpackhi_epi16(rg, ba));
	}
#endif
	// Its own loop with literal constants, which compilers keep in 16-bit lanes
	for (; fixed && i < width; i++) {
		int16_t g = (int16_t)(y[i] - luma_lift(red_green[i], blue_green[i]));
		int16_t r = (int16_t)((int16_t)(g + red_green [i] + 8) >> FIXED_BITS);
		int16_t b = (int16_t)((int16_t)(g + blue_green[i] + 8) >> FIXED_BITS);
		g = (int16_t)((int16_t)(g + 8) >> FIXED_BITS);
		put_rgba(out_rgba, i, cap_u8(r, cap[i]), cap_u8(g, cap[i]), cap_u8(b, cap[i]), clamp_u8(a[i]));
	}
	for (; i < width; i++) {
		int16_t g = (int16_t)(y[i] - luma_lift(red_green[i], blue_green[i]));
		put_rgba(out_rgba, i, cap_u8((int16_t)(g + red_green[i]), cap[i]), cap_u8(g, cap[i]), cap_u8((int16_t)(g + blue_green[i]), cap[i]), clamp_u8(a[i]));
	}
}

// ---- lfi_internal.h

// Internal to the Lift Image encoder and decoder


#include <stdbool.h>
#include <stdlib.h>

#define LFI_VERSION        1
#define TILE_LOG2          9
#define MAX_LEVELS         6
#define MAX_BANDS          (1 + 3 * MAX_LEVELS)
#define SYMBOL_END        0         // ends a band, the rest of which is zero
#define SYMBOL_ZERO_RUN    (15 << 5) // skips ZERO_RUN_LENGTH zeros without a coefficient
#define SYMBOL_NEGATIVE    ENT_TWIN  // other symbols are run class << 5 | sign | magnitude class
#define ZERO_RUN_LENGTH    2048
#define CONTEXT_TILE       0         // per-tile scale symbols, then chroma from luma scales, before the band contexts
#define SCALE_ONE          8         // tile scale symbol for 1x
#define HDR_CODES_PER_STOP 128
#define HDR_CODES          4096
#define CFL_MIN_LEVEL      2         // chroma detail bands from this level up are predicted from luma's
#define CFL_BLOCK          32        // one prediction scale per square block of this many coefficients
#define CFL_MAX_BLOCKS     64        // in a band at CFL_MIN_LEVEL of the largest tile, 1024
#define CFL_RANGE          16        // scales are k / 8 for k from -CFL_RANGE to CFL_RANGE
#define CFL_SYMBOL         16        // scale k is symbol CFL_SYMBOL + CFL_RANGE + k in CONTEXT_TILE

// Tile step multipliers in 1/16, a quarter octave apart
static const uint8_t tile_multipliers[16] = { 4, 5, 6, 7, 8, 10, 11, 13, 16, 19, 23, 27, 32, 38, 45, 54 };

static inline int32_t scale_step(int32_t step, int32_t scale) {
	int32_t scaled = (step * tile_multipliers[scale] + 8) >> 4;
	return scaled < 1 ? 1 : scaled;
}

typedef struct {
	int32_t x, y, w, h;
	int32_t level;       // 0 for LL
	int32_t orientation; // HL, LH, HH
	int32_t slot;        // the band's index, which picks its step and offset
} band_t;

static inline int32_t level_count(int32_t w, int32_t h) {
	int32_t levels = 0;
	while (levels < MAX_LEVELS && w >= 16 && h >= 16) {
		w = (w + 1) >> 1;
		h = (h + 1) >> 1;
		levels++;
	}
	return levels;
}

// Band 0 is LL, then HL, LH, HH for each level from coarsest to finest
static inline band_t band_rect(int32_t w, int32_t h, int32_t levels, int32_t band_index) {
	int32_t widths[MAX_LEVELS + 1] = { w }, heights[MAX_LEVELS + 1] = { h };
	for (int32_t l = 1; l <= levels; l++) {
		widths [l] = (widths [l-1] + 1) >> 1;
		heights[l] = (heights[l-1] + 1) >> 1;
	}
	if (band_index == 0) {
		band_t ll = { 0, 0, widths[levels], heights[levels], 0, 0, 0 };
		return ll;
	}

	int32_t level       = levels - (band_index - 1) / 3;
	int32_t orientation = (band_index - 1) % 3;
	int32_t low_w       = widths [level], region_w = widths [level-1];
	int32_t low_h       = heights[level], region_h = heights[level-1];
	band_t  band        = { low_w, low_h, region_w - low_w, region_h - low_h, level, orientation, band_index }; // HH, below and right of LL
	if (orientation == 0) { band.y = 0; band.h = low_h; }                                                 // HL, right of LL
	if (orientation == 1) { band.x = 0; band.w = low_w; }                                                 // LH, below LL
	return band;
}

// HL bands respond to vertical edges, so their coefficients line up in columns
static inline bool column_scan(band_t band) { return band.level && band.orientation == 0; }

// Whether planes are luma, red against green, blue against green and maybe alpha, rather than gray, normal XY or data
static inline bool is_color(int32_t planes, bool data) { return planes >= 3 && !data; }

// Red against green and blue against green in the color modes. Other planes are coded like luma.
static inline bool is_chroma(bool color, int32_t plane) { return color && (plane == 1 || plane == 2); }

static inline uint32_t band_context(bool color, int32_t plane, int32_t level) {
	uint32_t plane_class = is_chroma(color, plane) ? 1 : plane == 3 ? 2 : 0;
	uint32_t level_group = level == 0 ? 3 : level > 3 ? 2 : (uint32_t)level - 1;
	return 1 + plane_class * 4 + level_group;
}

static inline int32_t cfl_blocks(band_t band) { return ((band.w + CFL_BLOCK - 1) / CFL_BLOCK) * ((band.h + CFL_BLOCK - 1) / CFL_BLOCK); }

// Chroma's prediction from luma's detail value without its reconstruction offset. It wraps in 16 bits, so it
// vectorizes in 16-bit lanes, and the encoder predicts with the same wrapped values.
static inline int16_t cfl_predict(int32_t k, int16_t luma) { return (int16_t)((int16_t)(k * luma + 4) >> 3); }

// LOCO-I's median edge detector
static inline int32_t median_edge(int32_t left, int32_t up, int32_t up_left) {
	int32_t lower = left < up ? left : up, upper = left < up ? up : left;
	return up_left >= upper ? lower : up_left <= lower ? upper : left + up - up_left;
}

static inline int32_t median_edge_at(const int16_t* row, int32_t stride, int32_t x, int32_t y) {
	if (y == 0) return x ? row[x-1] : 0;
	if (x == 0) return row[x - stride];
	return median_edge(row[x-1], row[x - stride], row[x - stride - 1]);
}

// Lifting steps wrap at 16 bits, so loops vectorize to 16-bit lanes and stay exactly invertible
static inline int16_t predict(int16_t a, int16_t b) { return (int16_t)((int16_t)(a + b) >> 1); }
// About (5 (a + b) - (far_a + far_b)) / 16, ordered to stay within 16 bits for fixed point planes
static inline int16_t update(int16_t far_a, int16_t a, int16_t b, int16_t far_b) {
	int16_t inner = (int16_t)(a + b), outer = (int16_t)(far_a + far_b);
	return (int16_t)((int16_t)(inner + (inner >> 2) - (outer >> 2) + 2) >> 2);
}

// High values outside a band repeat its edge ones
static inline int32_t clamp_index(int32_t i, int32_t count) { return i < 0 ? 0 : i >= count ? count - 1 : i; }

// Container, as docs/format.md specifies: a fixed header and sizes, quantizer parameters, tables, segments from MIPn
// down to MIP0, then a CRC and 12 zero bytes.
#define HEADER_FIXED       18
#define SIZE_COUNT         (1 + 2 * (MAX_LEVELS + 1))
#define MAX_SIZE           65535 // width, height and layers
#if HEADER_FIXED + 5 * SIZE_COUNT != LFI_HEADER_MAX
#error LFI_HEADER_MAX should cover the fixed header and the longest sizes
#endif
#define FLAG_LINEAR        1
#define FLAG_CUTOUT        2     // alpha was only 0 and 255, and is snapped back to that
#define FLAG_HDR           4     // planes hold log2 brightness codes, with 0 for zero
#define FLAG_PREMULTIPLIED 8     // RGB is clamped to A after decoding
#define FLAG_CUBEMAP       16    // layers are cube faces +X, -X, +Y, -Y, +Z, -Z, six per cube
#define FLAG_FIXED         32    // lossy planes carry FIXED_BITS fractional bits, rounded off after the wavelet
#define FLAG_DATA          64    // 3 or 4 independent planes, with no color transform
#define MAX_STEP_CODE      0xCF  // keeps scaled steps times offsets within 32 bits

static const uint8_t lfi_signature[8] = { 0x89, 'L', 'F', 'I', 0x0D, 0x0A, 0x1A, 0x0A }; // catches 7-bit, line ending and DOS EOF damage

typedef struct {
	size_t offset, size; // in the file, in bytes
} span_t;

typedef struct {
	int32_t        w, h, layers;
	int32_t        planes;
	int32_t        tile;
	int32_t        levels;  // the same for every tile, so every tile can stop at any mip
	bool           linear, cutout, hdr, premultiplied, cubemap, fixed, data;
	int32_t        log_min; // log2 of HDR code 1, in 1/HDR_CODES_PER_STOP stops
	const uint8_t* steps;   // step codes, set with offsets and log_min once the data holds them
	const int8_t*  offsets;
	span_t         tables;
	span_t         streams[MAX_LEVELS + 1][2]; // per segment
	size_t         file_size;
} header_t;

// R8, RG8 or RGBA8, and R16F or RGBA16F for HDR
static inline int32_t pixel_size(int32_t planes, bool hdr) { return (planes < 3 ? planes : 4) * (hdr ? 2 : 1); }

// CRC-32 as in zlib and PNG, a nibble at a time
static inline uint32_t crc32(const uint8_t* data, size_t size) {
	static const uint32_t nibble[16] = {
		0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
		0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
	};
	uint32_t crc = 0xFFFFFFFF;
	for (size_t i = 0; i < size; i++) {
		crc ^= data[i];
		crc  = nibble[crc & 15] ^ (crc >> 4);
		crc  = nibble[crc & 15] ^ (crc >> 4);
	}
	return ~crc;
}

// An 8-bit float of 4 exponent and 4 mantissa bits, so steps to 31 are exact and larger ones within 3%
static inline int32_t step_value(uint32_t code) { return code >> 4 ? (int32_t)(16 | (code & 15)) << ((code >> 4) - 1) : (int32_t)code; }

static inline int32_t header_step(header_t header, int32_t plane, int32_t slot) { return step_value(header.steps[plane * (1 + 3 * header.levels) + slot]); }

// Detail bands only, since LL bands have none
static inline int32_t header_offset(header_t header, int32_t plane, int32_t slot) { return header.offsets[plane * 3 * header.levels + slot - 1]; }

// Mip k is the image at 1/2^k size, rounded up
static inline int32_t mip_size(int32_t size, int32_t mip) { return (size + (1 << mip) - 1) >> mip; }

// ---- ent_dec.c

// Reads the table header. Streams use ent_decoder_t's bit positions instead.
typedef struct {
	const uint8_t* next;
	uint64_t       bits;
	uint32_t       count;
} bit_reader_t;

// count <= 56
static uint32_t bit_peek(bit_reader_t* ref_reader, uint32_t count) {
	if (ref_reader->count < count) {
		ref_reader->bits  |= read_u64(ref_reader->next) << ref_reader->count;
		ref_reader->next  += (63 - ref_reader->count) >> 3;
		ref_reader->count |= 56;
	}
	return (uint32_t)(ref_reader->bits & ((1ull << count) - 1));
}

static uint32_t bit_read(bit_reader_t* ref_reader, uint32_t count) {
	uint32_t value = bit_peek(ref_reader, count);
	ref_reader->bits  >>= count;
	ref_reader->count  -= count;
	return value;
}

// Where reading has got to: the buffered bits came from the bytes just before next
static const uint8_t* bit_reader_position(bit_reader_t reader) { return reader.next - (reader.count >> 3); }

// Elias gamma: value + 1 as zeros, a one, then as many bits below it. More than 16 zeros give 2^17, which no caller accepts.
static uint32_t read_gamma(bit_reader_t* ref_reader) {
	uint32_t window = bit_peek(ref_reader, 17);
	if (!window) return 1u << 17;
	uint32_t zeros = bit_length(window & (0u - window)) - 1;
	bit_read(ref_reader, zeros + 1);
	return ((1u << zeros) | bit_read(ref_reader, zeros)) - 1;
}

// tables_mem must hold ENT_TABLES_SIZE bytes, and data must be readable 16 bytes past size. extra_bits gives the
// raw bit count after a symbol of a table, which entries fold in so one shift consumes both, or ENT_INVALID.
// Context 0 has table 0 to itself. Returns false for tables that are malformed.
bool ent_open_tables(uint32_t* tables_mem, const uint8_t* data, size_t size, ent_extra_bits_fn extra_bits) {
	bit_reader_t reader = { data, 0, 0 };
	uint32_t     entry_by_symbol_state[2 * ENT_PROB_SCALE]; // before extra bits
	for (uint32_t symbol_state = 1; symbol_state < 2 * ENT_PROB_SCALE; symbol_state++) {
		uint32_t state_bits = ENT_PROB_BITS + 1 - bit_length(symbol_state);
		entry_by_symbol_state[symbol_state] = state_bits | state_bits << 8 | ((symbol_state << state_bits) - ENT_PROB_SCALE) << 16;
	}

	uint8_t* map    = (uint8_t*)(tables_mem + ENT_CONTEXTS * ENT_TABLE_WORDS);
	uint32_t shared = bit_read(&reader, 4) + 1; // tables for the other contexts
	if (shared >= ENT_CONTEXTS) return false;
	map[0] = 0;
	for (uint32_t context = 1; context < ENT_CONTEXTS; context++) {
		map[context] = (uint8_t)(1 + bit_read(&reader, bit_length(shared - 1)));
		if (map[context] > shared) return false;
	}

	for (uint32_t t = 0; t <= shared; t++) {
		uint32_t  used    = read_gamma(&reader);
		uint32_t* table   = tables_mem + t * ENT_TABLE_WORDS;
		uint16_t* symbols = (uint16_t*)(table + ENT_PROB_SCALE);
		uint32_t  next[4][ENT_SYMBOLS]; // symbol state counters per quarter of the states, so four chains run side by side
		uint8_t   extra[ENT_SYMBOLS];
		uint32_t  filled = 0, precision = used ? bit_read(&reader, 2) : 0, implied = used ? read_gamma(&reader) : 0, implied_twin = 0;
		int32_t   listed = -1, implied_symbol = 0;
		// An unused table decodes as symbol 0 without reading, should a corrupt stream select it
		if (!used) memset(table, 0, ENT_TABLE_WORDS * sizeof(uint32_t));
		if (used > ENT_SYMBOLS || (used && implied >= used)) return false;
		// The implied symbol's frequency is what the others leave, so its states are handed out last
		for (uint32_t i = 0; used && i <= used; i++) {
			int32_t  symbol      = implied_symbol;
			uint32_t twin        = implied_twin, listed_freq = ENT_PROB_SCALE - filled;
			if (i < used) {
				listed += (int32_t)read_gamma(&reader) + 1;
				if (listed >= ENT_SYMBOLS) return false;
				twin = !(listed & ENT_TWIN) && bit_read(&reader, 1);
				if (i == implied) { implied_symbol = listed; implied_twin = twin; continue; }
				uint32_t length = bit_read(&reader, 4); // the frequency's, then its top precision bits below the leading one
				if (!length || length > ENT_PROB_BITS) return false;
				uint32_t kept = length - 1 < precision ? length - 1 : precision;
				symbol      = listed;
				listed_freq = (1u << (length - 1)) + (bit_read(&reader, kept) << (length - 1 - kept));
			}
			if (bit_reader_position(reader) > data + size || !listed_freq || filled + listed_freq > ENT_PROB_SCALE || (twin && listed_freq < 2)) return false;
			for (int32_t half = symbol; half <= (twin ? (symbol | ENT_TWIN) : symbol); half += ENT_TWIN) {
				uint32_t freq     = !twin ? listed_freq : half == symbol ? (listed_freq + 1) / 2 : listed_freq / 2;
				uint64_t quarters = 0; // states per quarter, 16 bits each
				extra[half] = (uint8_t)extra_bits(t, (uint32_t)half);
				if (extra[half] == ENT_INVALID) return false;
				for (uint32_t j = 0; j < freq; j++, filled++) {
					uint32_t state = (filled * ENT_SPREAD_STEP) & (ENT_PROB_SCALE - 1);
					symbols[state] = (uint16_t)half;
					quarters += 1ull << (state >> (ENT_PROB_BITS - 2) << 4);
				}
				next[0][half] = freq;
				next[1][half] = next[0][half] + (uint32_t)(quarters       & 0xFFFF);
				next[2][half] = next[1][half] + (uint32_t)(quarters >> 16 & 0xFFFF);
				next[3][half] = next[2][half] + (uint32_t)(quarters >> 32 & 0xFFFF);
			}
		}
		// Over a symbol's states in order, its symbol state counts from freq to 2 freq - 1
		for (uint32_t i = 0; used && i < ENT_PROB_SCALE / 4; i++) {
			for (uint32_t quarter = 0; quarter < 4; quarter++) {
				uint32_t state = quarter * (ENT_PROB_SCALE / 4) + i;
				table[state] = entry_by_symbol_state[next[quarter][symbols[state]]++] + extra[symbols[state]];
			}
		}
	}
	return bit_reader_position(reader) <= data + size;
}

// A segment's two streams, one after the other, which must be readable 16 bytes past their end. Streams too short
// for their initial state start past their end, which decoding reports.
ent_decoder_t ent_open_stream(const uint32_t* tables, const uint8_t* data, size_t size0, size_t size1) {
	const uint8_t* stream1 = data + size0, * end = stream1 + size1;
	return ent_streams(tables, stream1, end, ENT_PROB_BITS - (int64_t)size0 * 8, ENT_PROB_BITS - (int64_t)size1 * 8,
	                   read_u32(data) & (ENT_PROB_SCALE - 1), read_u32(stream1) & (ENT_PROB_SCALE - 1));
}

// ---- lfi_dec.c

// MSVC vectorizes one uint32 store but not two int16 ones, which clang vectorizes only when odd goes first
static inline void put_pair(int16_t* LFI_RESTRICT out, int32_t i, int16_t even, int16_t odd) {
#if defined(_MSC_VER) && !defined(__clang__)
	((uint32_t*)out)[i] = (uint16_t)even | (uint32_t)(uint16_t)odd << 16;
#else
	out[2*i + 1] = odd;
	out[2*i]     = even;
#endif
}

static void update_low(int16_t* LFI_RESTRICT low, const int16_t* LFI_RESTRICT high, int32_t count) {
	for (int32_t i = 0; i < count; i++) low[i] = (int16_t)(low[i] - update(high[i-2], high[i-1], high[i], high[i+1]));
}

static void predict_pairs(int16_t* LFI_RESTRICT out, const int16_t* LFI_RESTRICT even, const int16_t* LFI_RESTRICT high, int32_t count) {
	int32_t i = 0;
#if LFI_SSE2
	for (; i + 8 <= count; i += 8) {
		__m128i low = _mm_loadu_si128((const __m128i*)(even + i));
		__m128i odd = _mm_add_epi16(_mm_loadu_si128((const __m128i*)(high + i)), _mm_srai_epi16(_mm_add_epi16(low, _mm_loadu_si128((const __m128i*)(even + i + 1))), 1));
		_mm_storeu_si128((__m128i*)(out + 2*i),     _mm_unpacklo_epi16(low, odd));
		_mm_storeu_si128((__m128i*)(out + 2*i + 8), _mm_unpackhi_epi16(low, odd));
	}
#endif
	for (; i < count; i++) put_pair(out, i, even[i], (int16_t)(high[i] + predict(even[i], even[i+1])));
}

// A row in inverse_level's buffers holds L, then H at HIGH_AT(width) with two values of room on each side. The
// rows' producers fill that room, as stores just before the vector loads would stall them.
#define HIGH_AT(width) (((width) + 1) / 2 + 2)
static void pad_high(int16_t* high, int32_t count) {
	if (!count) return;
	high[-2] = high[-1] = high[0];
	high[count] = high[count + 1] = high[count - 1];
}

// One row of a level into out. The low half becomes the even samples in place.
static void inverse_row(int16_t* LFI_RESTRICT out, int16_t* LFI_RESTRICT row, int32_t width) {
	int32_t  low_count = (width + 1) >> 1, high_count = width >> 1;
	int16_t* high      = row + HIGH_AT(width);
	if (!high_count) { out[0] = row[0]; return; }
	update_low(row, high, low_count);
	row[low_count] = row[low_count - 1]; // over high's room, no longer needed
	predict_pairs(out, row, high, high_count);
	if (low_count > high_count) out[2*high_count] = row[high_count];
}

static void inverse_even(int16_t* LFI_RESTRICT out, const int16_t* LFI_RESTRICT low, const int16_t* LFI_RESTRICT far_above, const int16_t* LFI_RESTRICT high_above, const int16_t* LFI_RESTRICT high_below, const int16_t* LFI_RESTRICT far_below, int32_t width) {
	for (int32_t x = 0; x < width; x++) out[x] = (int16_t)(low[x] - update(far_above[x], high_above[x], high_below[x], far_below[x]));
}

static void inverse_odd(int16_t* LFI_RESTRICT out, const int16_t* LFI_RESTRICT high, const int16_t* LFI_RESTRICT even_above, const int16_t* LFI_RESTRICT even_below, int32_t width) {
	for (int32_t x = 0; x < width; x++) out[x] = (int16_t)(high[x] + predict(even_above[x], even_below[x]));
}

// A level's detail bands, each contiguous
typedef struct {
	const int16_t* hl;
	const int16_t* lh;
	const int16_t* hh;
} details_t;

// Even row 2 * pair of a vertically lifted level, with ll holding the LL rows low_w apart
static void even_row(int16_t* out, const int16_t* ll, details_t details, int32_t pair, int32_t w, int32_t high_h) {
	int32_t low_w = (w + 1) >> 1, high_w = w >> 1;
	int32_t far_above = clamp_index(pair - 2, high_h), above = clamp_index(pair - 1, high_h), below = clamp_index(pair, high_h), far_below = clamp_index(pair + 1, high_h);
	inverse_even(out, ll + pair * low_w, details.lh + far_above * low_w, details.lh + above * low_w, details.lh + below * low_w, details.lh + far_below * low_w, low_w);
	inverse_even(out + HIGH_AT(w), details.hl + pair * high_w, details.hh + far_above * high_w, details.hh + above * high_w, details.hh + below * high_w, details.hh + far_below * high_w, high_w);
	pad_high(out + HIGH_AT(w), high_w);
}

// The plane's LL region becomes the level, a row pair at a time through rows so the traffic stays in L1.
// Output rows overwrite LL rows before they're read, so those are copied to ll_copy first.
static void inverse_level(int16_t* plane, int32_t stride, int32_t w, int32_t h, details_t details, int16_t* ll_copy, int16_t* rows) {
	int32_t  low_w     = (w + 1) >> 1, high_w = w >> 1, low_h = (h + 1) >> 1, high_h = h >> 1, high_at = HIGH_AT(w);
	int16_t* even      = rows;
	int16_t* next_even = rows + w + 4;
	int16_t* odd       = rows + 2 * (w + 4);
	if (!high_h) {
		memcpy(even,           plane,      (size_t)low_w  * sizeof(int16_t));
		memcpy(even + high_at, details.hl, (size_t)high_w * sizeof(int16_t));
		pad_high(even + high_at, high_w);
		inverse_row(plane, even, w);
		return;
	}

	for (int32_t y = 0; y < low_h; y++) memcpy(ll_copy + y * low_w, plane + y * stride, (size_t)low_w * sizeof(int16_t));
	even_row(even, ll_copy, details, 0, w, high_h);
	for (int32_t pair = 0; pair < high_h; pair++) {
		if (pair + 1 < low_h) even_row(next_even, ll_copy, details, pair + 1, w, high_h);
		const int16_t* even_below = pair + 1 < low_h ? next_even : even;
		inverse_odd(odd,         details.lh + pair * low_w,  even,         even_below,         low_w);
		inverse_odd(odd + high_at, details.hh + pair * high_w, even + high_at, even_below + high_at, high_w);
		pad_high(odd + high_at, high_w);
		inverse_row(plane + (2*pair)     * stride, even, w);
		inverse_row(plane + (2*pair + 1) * stride, odd,  w);
		int16_t* swap = even; even = next_even; next_even = swap;
	}
	if (low_h > high_h) inverse_row(plane + (h - 1) * stride, even, w);
}

// The loops read one past the end and the last pair is redone. Without restrict, clang won't vectorize
// these, as out could overlap the input.
static void interpolate_row(int16_t* LFI_RESTRICT out, const int16_t* LFI_RESTRICT low, int32_t width) {
	int32_t low_count = (width + 1) >> 1, high_count = width >> 1;
	int32_t i = 0;
#if LFI_SSE2
	for (; i + 8 <= high_count; i += 8) {
		__m128i even = _mm_loadu_si128((const __m128i*)(low + i)), odd = _mm_srai_epi16(_mm_add_epi16(even, _mm_loadu_si128((const __m128i*)(low + i + 1))), 1);
		_mm_storeu_si128((__m128i*)(out + 2*i),     _mm_unpacklo_epi16(even, odd));
		_mm_storeu_si128((__m128i*)(out + 2*i + 8), _mm_unpackhi_epi16(even, odd));
	}
#endif
	for (; i < high_count; i++) {
		int16_t even = low[i], next_even = low[i + 1];
		put_pair(out, i, even, predict(even, next_even));
	}
	if (low_count > high_count) out[2*high_count]     = low[high_count];
	else                        out[2*high_count - 1] = predict(low[high_count - 1], low[high_count - 1]);
}

// interpolate_row of the rows between above and below
static void interpolate_between(int16_t* LFI_RESTRICT out, const int16_t* LFI_RESTRICT above, const int16_t* LFI_RESTRICT below, int32_t width) {
	int32_t low_count = (width + 1) >> 1, high_count = width >> 1;
	int32_t i = 0;
#if LFI_SSE2
	for (; i + 8 <= high_count; i += 8) {
		__m128i even      = _mm_srai_epi16(_mm_add_epi16(_mm_loadu_si128((const __m128i*)(above + i)),     _mm_loadu_si128((const __m128i*)(below + i))), 1);
		__m128i next_even = _mm_srai_epi16(_mm_add_epi16(_mm_loadu_si128((const __m128i*)(above + i + 1)), _mm_loadu_si128((const __m128i*)(below + i + 1))), 1);
		__m128i odd       = _mm_srai_epi16(_mm_add_epi16(even, next_even), 1);
		_mm_storeu_si128((__m128i*)(out + 2*i),     _mm_unpacklo_epi16(even, odd));
		_mm_storeu_si128((__m128i*)(out + 2*i + 8), _mm_unpackhi_epi16(even, odd));
	}
#endif
	for (; i < high_count; i++) {
		int16_t even = predict(above[i], below[i]), next_even = predict(above[i + 1], below[i + 1]);
		put_pair(out, i, even, predict(even, next_even));
	}
	int16_t last_even = predict(above[low_count - 1], below[low_count - 1]);
	if (low_count > high_count) out[2*high_count]     = last_even;
	else                        out[2*high_count - 1] = predict(last_even, last_even);
}

// inverse_level for a level whose detail bands are all zero, where lifting reduces to interpolation.
// Bottom up, so each L row is read before its plane row is overwritten. Rows 0 and 1 read themselves.
static void interpolate_level(int16_t* plane, int32_t stride, int32_t w, int32_t h, int16_t* row_copy) {
	int32_t low_w = (w + 1) >> 1, low_h = (h + 1) >> 1;
	for (int32_t y = h - 1; y >= 0; y--) {
		const int16_t* above = plane + (y >> 1) * stride;
		const int16_t* below = (y >> 1) + 1 < low_h ? above + stride : above;
		if (y < 2) {
			memcpy(row_copy, y ? below : above, (size_t)(low_w + 1) * sizeof(int16_t));
			if (y) below = row_copy;
			else   above = row_copy;
		}
		if (y & 1) interpolate_between(plane + y * stride, above, below, w);
		else       interpolate_row    (plane + y * stride, above, w);
	}
}

// Zero run from run class: run_base plus a mantissa of run_mantissa_bits
static const uint8_t  run_mantissa_bits[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 3, 4,  5,  6,  7,   8,   9,   10 };
static const uint16_t run_base         [16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 16, 32, 64, 128, 256, 512, 1024 };

// Raw bits after a symbol: run mantissa, then magnitude mantissa. Tile and prediction scales, which table 0 holds,
// have none. Symbols outside the alphabet are refused when the tables are built, so decoding never meets them.
static uint32_t symbol_extra_bits(uint32_t table, uint32_t symbol) {
	if (table == CONTEXT_TILE) return symbol <= CFL_SYMBOL + 2 * CFL_RANGE ? 0 : ENT_INVALID;
	if (!(symbol & 15))          return symbol == SYMBOL_END || symbol == SYMBOL_ZERO_RUN ? 0 : ENT_INVALID;
	return run_mantissa_bits[symbol >> 5] + (symbol & 15) - 1;
}

#define BAND_SLACK (2 * ZERO_RUN_LENGTH) // int16s a corrupt contiguous band can write past its end before decode_band notices

typedef struct {
	int16_t*        line;   // contiguous: the band's end. Scanned: the current line
	int32_t         pos;    // contiguous: offset from the end, below 0. Scanned: position in the line
	int32_t         inner, outer, length, lines; // scanned: lines of length coefficients, inner apart, outer apart, lines left
	const uint32_t* values; // from band_values
} scan_t;

// Per sign and magnitude class, the value at mantissa 0 and the signed step per mantissa. Unsigned, since
// corrupt steps can overflow, and the values wrap to 16 bits anyway.
static void band_values(int32_t step, int32_t offset, uint32_t* out_values) {
	for (uint32_t sign_class = 0; sign_class < 32; sign_class++) {
		uint32_t magnitude_class = sign_class & 15, negative = sign_class & SYMBOL_NEGATIVE;
		uint32_t base            = magnitude_class ? (1u << (magnitude_class - 1)) * (uint32_t)step + (uint32_t)offset : 0;
		out_values[sign_class * 2]     = negative ? 0u - base           : base;
		out_values[sign_class * 2 + 1] = negative ? 0u - (uint32_t)step : (uint32_t)step;
	}
}

// Moves run coefficients along. A scanned band returns false past its end, and decode_band checks contiguous ones.
static inline bool advance(scan_t* ref_scan, int32_t run, bool contiguous) {
	if (contiguous) { ref_scan->pos += run; return true; }
	for (ref_scan->pos += run; ref_scan->pos >= ref_scan->length; ref_scan->pos -= ref_scan->length) {
		ref_scan->line += ref_scan->outer;
		if (--ref_scan->lines <= 0) return false;
	}
	return true;
}

// Decodes one symbol of a state and places its coefficient. Returns 1 to go on, 0 at the band's end, or -1
// for a scanned band overrun. A contiguous band skips the line wrapping.
static inline int32_t decode_symbol(scan_t* ref_scan, const uint8_t* end, int64_t* ref_pos, uint32_t* ref_state, const uint32_t* table, bool contiguous) {
	uint32_t symbol = ent_table_symbol(table, *ref_state);
	uint32_t extra  = ent_step(ref_state, end, ref_pos, table);
	if (!(symbol & 15)) {
		if (symbol == SYMBOL_END) return 0;
		return advance(ref_scan, ZERO_RUN_LENGTH, contiguous) ? 1 : -1;
	}
	uint32_t run_bits = run_mantissa_bits[symbol >> 5];
	if (!advance(ref_scan, run_base[symbol >> 5] + (int32_t)(extra & ((1u << run_bits) - 1)), contiguous)) return -1;

	const uint32_t* value       = ref_scan->values + (symbol & 31) * 2;
	int16_t         coefficient = (int16_t)(value[0] + (extra >> run_bits) * value[1]);
	if (contiguous) ref_scan->line[ref_scan->pos++] = coefficient;
	else            ref_scan->line[ref_scan->pos++ * ref_scan->inner] = coefficient;
	return 1;
}

// Unrolled over the two states, so neither needs swapping. Returns whether any coefficient was placed, or -1 if corrupt.
// Overruns are checked once per pair: each stream reads once in between, which ENT_PAD covers, and a contiguous
// band can overrun by two runs, which BAND_SLACK covers.
static inline int32_t decode_band(ent_decoder_t* ref_decoder, uint32_t context, scan_t scan, bool contiguous) {
	const int16_t*  start_line = scan.line;
	int32_t         start_pos  = scan.pos;
	const uint32_t* table      = ent_table(ref_decoder, context);
	const uint8_t*  end0       = ref_decoder->end[0],   * end1   = ref_decoder->end[1];
	int64_t         pos0       = ref_decoder->pos[0],     pos1   = ref_decoder->pos[1];
	uint32_t        state0     = ref_decoder->state[0],   state1 = ref_decoder->state[1];
	int32_t         result;
	for (;;) {
		if ((result = decode_symbol(&scan, end0, &pos0, &state0, table, contiguous)) <= 0) { *ref_decoder = ent_streams(ref_decoder->tables, end1, end0, pos1, pos0, state1, state0); break; }
		if ((result = decode_symbol(&scan, end1, &pos1, &state1, table, contiguous)) <= 0) { *ref_decoder = ent_streams(ref_decoder->tables, end0, end1, pos0, pos1, state0, state1); break; }
		if (pos0 > 0 || pos1 > 0 || (contiguous && scan.pos > 0)) return -1;
	}
	if (result < 0 || ref_decoder->pos[0] > 0 || ref_decoder->pos[1] > 0 || (contiguous && scan.pos > 0)) return -1;
	return scan.line != start_line || scan.pos != start_pos;
}

static int32_t decode_scanned   (ent_decoder_t* ref_decoder, uint32_t context, scan_t scan) { return decode_band(ref_decoder, context, scan, false); }
static int32_t decode_contiguous(ent_decoder_t* ref_decoder, uint32_t context, scan_t scan) { return decode_band(ref_decoder, context, scan, true); }

// A chroma band's prediction scale for each block. Returns whether any is nonzero, or -1 if corrupt.
static int32_t cfl_scales(ent_decoder_t* ref_decoder, band_t band, int8_t* out_scales) {
	int32_t any = 0;
	for (int32_t b = 0; b < cfl_blocks(band); b++) {
		uint32_t symbol = ent_decode(ref_decoder, CONTEXT_TILE);
		if (symbol < CFL_SYMBOL) return -1;
		out_scales[b] = (int8_t)((int32_t)symbol - CFL_SYMBOL - CFL_RANGE);
		any |= out_scales[b];
	}
	return ref_decoder->pos[0] > 0 || ref_decoder->pos[1] > 0 ? -1 : any != 0;
}

static void cfl_add_row(int16_t* LFI_RESTRICT out, const int16_t* LFI_RESTRICT luma, int32_t count, int32_t k, int16_t offset) {
	for (int32_t i = 0; i < count; i++) {
		int16_t v = luma[i];
		out[i] = (int16_t)(out[i] + cfl_predict(k, (int16_t)(v > 0 ? v - offset : v < 0 ? v + offset : 0)));
	}
}

// Adds chroma's prediction, from luma's band of the same shape and the offset luma's values were reconstructed with
static void cfl_add(int16_t* out, const int16_t* luma, band_t band, const int8_t* scales, int16_t offset) {
	int32_t across = (band.w + CFL_BLOCK - 1) / CFL_BLOCK;
	for (int32_t y = 0; y < band.h; y++)
		for (int32_t x = 0; x < band.w; x += CFL_BLOCK) {
			int32_t k = scales[y / CFL_BLOCK * across + x / CFL_BLOCK];
			if (k) cfl_add_row(out + y * band.w + x, luma + y * band.w + x, band.w - x < CFL_BLOCK ? band.w - x : CFL_BLOCK, k, offset);
		}
}

// Into a contiguous buffer, zeroed unless the band starts with SYMBOL_END. Returns 0 for a band that is all zero, or -1 if corrupt.
static int32_t decode_detail(ent_decoder_t* ref_decoder, int16_t* out, band_t band, int32_t plane_index, header_t header, int32_t scale) {
	int32_t  step = scale_step(header_step(header, plane_index, band.slot), scale);
	uint32_t values[64];
	band_values(step, (header_offset(header, plane_index, band.slot) * step + 32) >> 6, values);
	uint32_t context = band_context(is_color(header.planes, header.data), plane_index, band.level);
	if (column_scan(band) && band.w && band.h) {
		scan_t columns = { out, 0, band.w, 1, band.h, band.w, values };
		return decode_scanned(ref_decoder, context, columns);
	}
	scan_t run = { out + band.w * band.h, -band.w * band.h, 0, 0, 0, 0, values };
	return decode_contiguous(ref_decoder, context, run);
}

// Residuals from MED prediction, decoded into zeroed contiguous residuals, then into the plane. Returns false if corrupt.
static bool decode_ll(ent_decoder_t* ref_decoder, int16_t* plane, int32_t stride, band_t band, int32_t plane_index, header_t header, int32_t scale, int16_t* residuals) {
	uint32_t values[64];
	band_values(1, 0, values);
	scan_t run = { residuals + band.w * band.h, -band.w * band.h, 0, 0, 0, 0, values };
	if (decode_contiguous(ref_decoder, band_context(is_color(header.planes, header.data), plane_index, 0), run) < 0) return false;
	for (int32_t y = 0; y < band.h; y++) {
		int16_t* row = residuals + y * band.w;
		for (int32_t x = 0; x < band.w; x++) row[x] = (int16_t)(row[x] + median_edge_at(row, band.w, x, y));
	}
	uint32_t step = (uint32_t)scale_step(header_step(header, plane_index, band.slot), scale);
	for (int32_t y = 0; y < band.h; y++)
		for (int32_t x = 0; x < band.w; x++) plane[(size_t)y * stride + x] = (int16_t)((uint32_t)residuals[y * band.w + x] * step);
	return true;
}

static void round_fixed(int16_t* LFI_RESTRICT row, int32_t width) {
	for (int32_t i = 0; i < width; i++) row[i] = (int16_t)((int16_t)(row[i] + (1 << (FIXED_BITS - 1))) >> FIXED_BITS);
}

// width pixels of a plane row, in the file's pixel layout. opaque is a row of 255s.
static void write_row(int16_t* LFI_RESTRICT planes, size_t plane_size, header_t header, const int16_t* LFI_RESTRICT opaque, uint8_t* LFI_RESTRICT out, int32_t width) {
	// The color inverse rounds its own planes, after converting
	for (int32_t p = is_color(header.planes, header.data) ? 3 : 0; header.fixed && p < header.planes; p++) round_fixed(planes + p * plane_size, width);
	int16_t* alpha = planes + 3*plane_size; // only with 4 planes, which cutout implies
	if (header.cutout) for (int32_t i = 0; i < width; i++) alpha[i] = alpha[i] >= 128 ? 255 : 0; // a flag in the loop condition stops MSVC vectorizing
	int32_t i = 0;
	switch (header.planes) {
	case 1:
#if LFI_SSE2
		for (; i + 16 <= width; i += 16) _mm_storeu_si128((__m128i*)(out + i), _mm_packus_epi16(_mm_loadu_si128((const __m128i*)(planes + i)), _mm_loadu_si128((const __m128i*)(planes + i + 8))));
#endif
		for (; i < width; i++) out[i] = (uint8_t)clamp_u8(planes[i]);
		break;
	case 2:
#if LFI_SSE2
		for (; i + 16 <= width; i += 16) {
			__m128i x = _mm_packus_epi16(_mm_loadu_si128((const __m128i*)(planes + i)),              _mm_loadu_si128((const __m128i*)(planes + i + 8)));
			__m128i y = _mm_packus_epi16(_mm_loadu_si128((const __m128i*)(planes + plane_size + i)), _mm_loadu_si128((const __m128i*)(planes + plane_size + i + 8)));
			_mm_storeu_si128((__m128i*)(out + 2*i),      _mm_unpacklo_epi8(x, y));
			_mm_storeu_si128((__m128i*)(out + 2*i + 16), _mm_unpackhi_epi8(x, y));
		}
#endif
		for (; i < width; i++) ((uint16_t*)out)[i] = (uint16_t)(clamp_u8(planes[i]) | clamp_u8(planes[plane_size + i]) << 8);
		break;
	default: {
		const int16_t* a = header.planes == 4 ? alpha : opaque;
		if (!header.data) { color_inverse_row(planes, planes + plane_size, planes + 2*plane_size, a, header.premultiplied ? a : opaque, out, width, header.fixed); break; }
		for (; i < width; i++) put_rgba(out, i, clamp_u8(planes[i]), clamp_u8(planes[plane_size + i]), clamp_u8(planes[2*plane_size + i]), clamp_u8(a[i]));
	} break;
	}
}

// 2^(f/128) for f in 0..127, as the 23 mantissa bits of a float, rounded to nearest. A constant table rather
// than exp2f, so decoding is bit-exact everywhere: libm exp2f is off by an ulp at some of these.
static const uint32_t pow2_mantissa[128] = {
	0x000000, 0x00B1ED, 0x0164D2, 0x0218AF, 0x02CD87, 0x038359, 0x043A29, 0x04F1F6,
	0x05AAC3, 0x066491, 0x071F62, 0x07DB35, 0x08980F, 0x0955EE, 0x0A14D5, 0x0AD4C6,
	0x0B95C2, 0x0C57CA, 0x0D1ADF, 0x0DDF04, 0x0EA43A, 0x0F6A81, 0x1031DC, 0x10FA4D,
	0x11C3D3, 0x128E72, 0x135A2B, 0x1426FF, 0x14F4F0, 0x15C3FF, 0x16942D, 0x17657D,
	0x1837F0, 0x190B88, 0x19E046, 0x1AB62B, 0x1B8D3A, 0x1C6573, 0x1D3EDA, 0x1E196E,
	0x1EF532, 0x1FD228, 0x20B051, 0x218FAF, 0x227043, 0x23520F, 0x243516, 0x251958,
	0x25FED7, 0x26E595, 0x27CD94, 0x28B6D5, 0x29A15B, 0x2A8D26, 0x2B7A3A, 0x2C6897,
	0x2D583F, 0x2E4934, 0x2F3B79, 0x302F0E, 0x3123F6, 0x321A32, 0x3311C4, 0x340AAF,
	0x3504F3, 0x360094, 0x36FD92, 0x37FBF0, 0x38FBAF, 0x39FCD2, 0x3AFF5B, 0x3C034A,
	0x3D08A4, 0x3E0F68, 0x3F179A, 0x40213B, 0x412C4D, 0x4238D2, 0x4346CD, 0x44563F,
	0x45672A, 0x467991, 0x478D75, 0x48A2D8, 0x49B9BE, 0x4AD226, 0x4BEC15, 0x4D078C,
	0x4E248C, 0x4F4319, 0x506334, 0x5184DF, 0x52A81E, 0x53CCF1, 0x54F35B, 0x561B5E,
	0x5744FD, 0x587039, 0x599D16, 0x5ACB94, 0x5BFBB8, 0x5D2D82, 0x5E60F5, 0x5F9613,
	0x60CCDF, 0x62055B, 0x633F89, 0x647B6D, 0x65B907, 0x66F85B, 0x68396A, 0x697C38,
	0x6AC0C7, 0x6C0719, 0x6D4F30, 0x6E9910, 0x6FE4BA, 0x713231, 0x728177, 0x73D290,
	0x75257D, 0x767A41, 0x77D0DF, 0x79295A, 0x7A83B3, 0x7BDFED, 0x7D3E0C, 0x7E9E11,
};

// Float bits of 2^(k/128), flushed to 0 below and clamped to the largest float above the normal range
static uint32_t pow2_bits(int32_t k) {
	int32_t exponent = (k >> 7) + 127;
	return exponent <= 0 ? 0 : exponent >= 255 ? 0x7F7FFFFF : (uint32_t)exponent << 23 | pow2_mantissa[k & 127];
}

// Positive float bits to an unsigned float with a 5 bit exponent biased by 15, which half, R11 and B10 all share.
// Rounds to nearest even, flushes below the range to zero, and clamps above it to the largest finite value.
static uint16_t to_ufloat(uint32_t bits, int32_t mantissa_bits) {
	if (bits >> 31 || !(bits << 1)) return 0;
	int32_t  exponent = (int32_t)(bits >> 23 & 0xFF) - 127 + 15;
	uint32_t mantissa = (bits & 0x7FFFFF) | 0x800000;
	uint32_t largest  = 30u << mantissa_bits | ((1u << mantissa_bits) - 1);
	uint32_t shift    = (uint32_t)(23 - mantissa_bits + (exponent <= 0 ? 1 - exponent : 0));
	if (exponent >= 31) return (uint16_t)largest;
	if (shift > 24)     return 0;
	uint32_t value = mantissa >> shift, remainder = mantissa & ((1u << shift) - 1), half = 1u << (shift - 1);
	value += remainder > half || (remainder == half && (value & 1));
	uint32_t result = exponent > 0 ? ((uint32_t)(exponent - 1) << mantissa_bits) + value : value; // a rounding carry moves into the exponent
	return (uint16_t)(result < largest ? result : largest);
}

// 32 bit entries, since AVX2 gathers no narrower
typedef struct {
	uint32_t ufloat    [HDR_CODES]; // code to half, or to R11 and G11
	uint32_t ufloat_b10[HDR_CODES]; // code to B10
	uint32_t mantissa9 [1281];      // RGB9E5 mantissa for a channel (i - 129)/128 stops above 2^(exponent - 24). 0 below that
} hdr_tables_t;

static void hdr_tables(lfi_format_e format, int32_t log_min, hdr_tables_t* out_tables) {
	for (int32_t code = 0; code < HDR_CODES && format != lfi_format_rgb9e5; code++) {
		uint32_t bits = code ? pow2_bits(code - 1 + log_min) : 0;
		out_tables->ufloat    [code] = to_ufloat(bits, format == lfi_format_native ? 10 : 6);
		out_tables->ufloat_b10[code] = to_ufloat(bits, 5);
	}
	// Rounded half away from zero, from the 24 bit mantissa of 2^(k/128) with k in -128..1151
	out_tables->mantissa9[0] = 0;
	for (int32_t i = 1; i < 1281 && format == lfi_format_rgb9e5; i++) {
		int32_t  k        = i - 129, exponent = k >> 7;
		uint32_t mantissa = ((1u << 23) + pow2_mantissa[k & 127] + (1u << (22 - exponent))) >> (23 - exponent);
		out_tables->mantissa9[i] = mantissa > 511 ? 511 : mantissa;
	}
}

static inline int16_t clamp_code(int16_t code) { return code < 0 ? 0 : code > HDR_CODES - 1 ? HDR_CODES - 1 : code; }

// R, G and B codes of a row, in 16-bit lanes like the LDR path
static void hdr_codes(const int16_t* LFI_RESTRICT planes, size_t plane_size, int32_t channels, int16_t* LFI_RESTRICT out_r, int16_t* LFI_RESTRICT out_g, int16_t* LFI_RESTRICT out_b, int32_t width) {
	if (channels == 1) {
		for (int32_t i = 0; i < width; i++) out_r[i] = clamp_code(planes[i]);
		memset(out_g, 0, (size_t)width * sizeof(int16_t));
		memset(out_b, 0, (size_t)width * sizeof(int16_t));
		return;
	}
	for (int32_t i = 0; i < width; i++) {
		int16_t red_green = planes[plane_size + i], blue_green = planes[2*plane_size + i];
		int16_t g         = (int16_t)(planes[i] - luma_lift(red_green, blue_green));
		out_r[i] = clamp_code((int16_t)(g + red_green));
		out_g[i] = clamp_code(g);
		out_b[i] = clamp_code((int16_t)(g + blue_green));
	}
}

// Shared exponent from the brightest channel, which then gets a 9 bit mantissa in 256..511
// Turns codes in place into mantissa9 indexes, and fills out_exponents. Selects rather than
// branches, so this vectorizes and leaves only the lookups for the packing loop. log_min splits into
// whole stops and a fraction so the math fits 16-bit lanes, where SSE2 has min and max. Stops beyond
// +-64 of the codes' range only push every index to the same end, so they saturate there.
static void rgb9e5_indexes(int16_t* LFI_RESTRICT ref_r, int16_t* LFI_RESTRICT ref_g, int16_t* LFI_RESTRICT ref_b, int32_t log_min, int16_t* LFI_RESTRICT out_exponents, int32_t width) {
	int32_t stops    = log_min >> 7;
	int16_t lowest   = (int16_t)(-stops < -64 ? -64 : -stops > 64 ? 64 : -stops), highest = (int16_t)(31 - stops < -64 ? -64 : 31 - stops > 64 ? 64 : 31 - stops);
	int16_t fraction = (int16_t)(log_min & 127), shift = (int16_t)(stops < -64 ? -64 : stops > 64 ? 64 : stops);
	for (int32_t i = 0; i < width; i++) {
		int16_t r = ref_r[i], g = ref_g[i], b = ref_b[i];
		int16_t brightest = r > g ? r : g;
		brightest = brightest > b ? brightest : b;
		int16_t whole    = (int16_t)(((int16_t)(brightest - 1 + fraction) >> 7) + 16); // exponent less stops
		int16_t exponent = (int16_t)(whole + shift);
		exponent = exponent < 0 ? 0 : exponent > 31 ? 31 : exponent;
		whole    = whole < lowest ? lowest : whole > highest ? highest : whole;
		int16_t base    = (int16_t)((whole - 24) * HDR_CODES_PER_STOP - fraction - 128); // code whose mantissa sits at mantissa9[1]
		int16_t index_r = (int16_t)(r - base), index_g = (int16_t)(g - base), index_b = (int16_t)(b - base);
		ref_r[i]         = (int16_t)(r ? (index_r < 0 ? 0 : index_r > 1280 ? 1280 : index_r) : 0);
		ref_g[i]         = (int16_t)(g ? (index_g < 0 ? 0 : index_g > 1280 ? 1280 : index_g) : 0);
		ref_b[i]         = (int16_t)(b ? (index_b < 0 ? 0 : index_b > 1280 ? 1280 : index_b) : 0);
		out_exponents[i] = (int16_t)(brightest ? exponent : 0);
	}
}

// HDR rows go out as R16F or RGBA16F with alpha 1, or as packed RG11B10F or RGB9E5 with gray in R.
// rows holds four rows of width values.
static void write_row_hdr(const int16_t* LFI_RESTRICT planes, size_t plane_size, header_t header, lfi_format_e format, const hdr_tables_t* LFI_RESTRICT tables, int16_t* LFI_RESTRICT rows, uint8_t* LFI_RESTRICT out, int32_t width) {
	int16_t* r = rows, * g = rows + width, * b = rows + 2 * width;
	hdr_codes(planes, plane_size, header.planes, r, g, b, width);

	uint32_t* pixels = (uint32_t*)out;
	switch (format) {
	case lfi_format_rg11b10f:
		for (int32_t i = 0; i < width; i++) pixels[i] = tables->ufloat[r[i]] | (uint32_t)tables->ufloat[g[i]] << 11 | (uint32_t)tables->ufloat_b10[b[i]] << 22;
		break;
	case lfi_format_rgb9e5: {
		int16_t* exponents = rows + 3 * width;
		rgb9e5_indexes(r, g, b, header.log_min, exponents, width);
		for (int32_t i = 0; i < width; i++) pixels[i] = tables->mantissa9[r[i]] | tables->mantissa9[g[i]] << 9 | tables->mantissa9[b[i]] << 18 | (uint32_t)exponents[i] << 27;
	} break;
	default:
		if (header.planes == 1) {
			uint16_t* half = (uint16_t*)out;
			for (int32_t i = 0; i < width; i++) half[i] = (uint16_t)tables->ufloat[r[i]];
			break;
		}
		for (int32_t i = 0; i < width; i++) {
			pixels[i*2 + 0] = tables->ufloat[r[i]] | tables->ufloat[g[i]] << 16;
			pixels[i*2 + 1] = tables->ufloat[b[i]] | 0x3C00u << 16;
		}
		break;
	}
}

static int32_t read_u16(const uint8_t* at) { return at[0] | at[1] << 8; }

// Validates the header. size may cover only a prefix of the file: what lies past it is located but not read, and
// steps stays NULL until the data holds the quantizer parameters.
static lfi_result_e parse_header(const uint8_t* data, size_t size, header_t* out_header) {
	if (size < HEADER_FIXED) return lfi_error_truncated;
	if (memcmp(data, lfi_signature, sizeof(lfi_signature)) || data[8] != LFI_VERSION) return lfi_error_format;

	uint32_t tile_log2 = data[10], flags = data[11];
	header_t header;
	memset(&header, 0, sizeof(header));
	header.w             = read_u16(data + 12);
	header.h             = read_u16(data + 14);
	header.layers        = read_u16(data + 16);
	header.planes        = data[9];
	header.linear        = (flags & FLAG_LINEAR)        != 0;
	header.cutout        = (flags & FLAG_CUTOUT)        != 0;
	header.hdr           = (flags & FLAG_HDR)           != 0;
	header.premultiplied = (flags & FLAG_PREMULTIPLIED) != 0;
	header.cubemap       = (flags & FLAG_CUBEMAP)       != 0;
	header.fixed         = (flags & FLAG_FIXED)         != 0;
	header.data          = (flags & FLAG_DATA)          != 0;
	if (flags >= FLAG_DATA * 2) return lfi_error_format;
	if (header.w < 1 || header.h < 1 || header.layers < 1 || header.planes < 1 || header.planes > 4 || tile_log2 < 5 || tile_log2 > 10 ||
	    (header.hdr && (header.fixed || (header.planes != 1 && header.planes != 3))) || ((header.cutout || header.premultiplied) && header.planes != 4) ||
	    (header.data && (header.planes < 3 || header.hdr || header.cutout || header.premultiplied)) ||
	    (header.cubemap && (header.layers % 6 || header.w != header.h)))
		return lfi_error_corrupt;
	header.tile   = 1 << tile_log2;
	header.levels = level_count(header.w < header.tile ? header.w : header.tile, header.h < header.tile ? header.h : header.tile);

	// Sizes as little-endian base 128 of at most 32 bits: the tables, then each segment's two streams, coarsest first
	size_t sizes[SIZE_COUNT], at = HEADER_FIXED;
	for (int32_t i = 0; i < 3 + 2 * header.levels; i++) {
		sizes[i] = 0;
		for (int32_t shift = 0;; shift += 7, at++) {
			if (at >= size) return lfi_error_truncated;
			if (shift == 28 && data[at] > 15) return lfi_error_corrupt;
			sizes[i] |= (size_t)(data[at] & 0x7F) << shift;
			if (data[at] < 0x80) break;
		}
		at++;
	}

	size_t params = (header.hdr ? 4 : 0) + (size_t)header.planes * (1 + 6 * header.levels);
	size_t offset = at + params;
	for (int32_t i = 0; i < 3 + 2 * header.levels; i++) {
		span_t span = { offset, sizes[i] };
		if (i) header.streams[header.levels - (i - 1) / 2][(i - 1) % 2] = span;
		else   header.tables = span;
		if (sizes[i] > SIZE_MAX - ENT_PAD - offset) return lfi_error_corrupt;
		offset += sizes[i];
	}
	header.file_size = offset + ENT_PAD;
	if (size >= at + params) {
		header.log_min = header.hdr ? (int32_t)read_u32(data + at) : 0;
		header.steps   = data + at + (header.hdr ? 4 : 0);
		header.offsets = (const int8_t*)(header.steps + header.planes * (1 + 3 * header.levels));
		if (header.log_min < -(1 << 20) || header.log_min > 1 << 20) return lfi_error_corrupt;
	}
	*out_header = header;
	return lfi_ok;
}

static size_t mip_end(header_t header, int32_t mip) { return header.streams[mip][1].offset + header.streams[mip][1].size + ENT_PAD; }

lfi_result_e lfi_info(const uint8_t* data, size_t size, lfi_info_t* out_info) {
	header_t     header;
	lfi_result_e result = parse_header(data, size, &header);
	if (result != lfi_ok) return result;
	lfi_info_t info = { header.w, header.h, header.layers, header.planes, pixel_size(header.planes, header.hdr), header.levels + 1, header.file_size,
	                    header.linear, header.premultiplied, header.hdr, header.cubemap, header.data };
	*out_info = info;
	return lfi_ok;
}

// Checks the file's CRC, for data from untrusted transports. Decoding is memory safe without it.
lfi_result_e lfi_verify(const uint8_t* data, size_t size) {
	header_t     header;
	lfi_result_e result = parse_header(data, size, &header);
	if (result != lfi_ok) return result;
	if (size < header.file_size) return lfi_error_truncated;
	size_t end = header.file_size - ENT_PAD;
	return crc32(data, end) == read_u32(data + end) ? lfi_ok : lfi_error_checksum;
}

// The file prefix that decoding this mip reads, or 0 for an invalid header or mip
size_t lfi_mip_bytes(const uint8_t* data, size_t size, int32_t mip) {
	header_t header;
	if (parse_header(data, size, &header) != lfi_ok || mip < 0 || mip > header.levels) return 0;
	return mip_end(header, mip);
}

size_t lfi_scratch_size(const uint8_t* data, size_t size) {
	header_t header;
	if (parse_header(data, size, &header) != lfi_ok) return 0;
	size_t tile_area = (size_t)header.tile * header.tile;
	return ENT_TABLES_SIZE + (tile_area * (header.planes + 1) + tile_area / 4 + 4 * header.tile + 16 + 2 * BAND_SLACK) * sizeof(int16_t); // inverse_level's rows have room around H
}

// The cache holds, per tile in raster order through each layer, its luma and chroma scales and then each plane's
// LL region at that mip
size_t lfi_cache_size(const uint8_t* data, size_t size, int32_t mip) {
	header_t header;
	if (parse_header(data, size, &header) != lfi_ok || mip < 0 || mip > header.levels) return 0;
	// Tile sizes are separable, so the sum of LL areas is the product of the summed tile widths and heights at this mip
	int32_t full_tile = mip_size(header.tile, mip), partial_w = header.w % header.tile, partial_h = header.h % header.tile;
	size_t  columns   = (size_t)(header.w / header.tile) * full_tile + (partial_w ? mip_size(partial_w, mip) : 0);
	size_t  rows      = (size_t)(header.h / header.tile) * full_tile + (partial_h ? mip_size(partial_h, mip) : 0);
	size_t  tiles     = (size_t)((header.w + header.tile - 1) / header.tile) * ((header.h + header.tile - 1) / header.tile);
	return (2 * tiles + header.planes * columns * rows) * header.layers * sizeof(int16_t);
}

// Progressive decoding without redoing work: continues from the cache an earlier step left at cache_mip,
// or from nothing when opt_cache is NULL, and can leave a cache at mip for the next step. Layers follow one
// another in out_pixels.
lfi_result_e lfi_decode_step(const uint8_t* data, size_t size, const int16_t* opt_cache, int32_t cache_mip, int32_t mip, lfi_format_e format, int16_t* opt_out_cache, uint8_t* opt_out_pixels, void* scratch_mem) {
	header_t     header;
	lfi_result_e result = parse_header(data, size, &header);
	if (result != lfi_ok) return result;
	if (mip < 0 || mip > header.levels || (opt_cache && (cache_mip <= mip || cache_mip > header.levels))) return lfi_error_argument;
	if (size < mip_end(header, mip)) return lfi_error_truncated;
	for (int32_t slot = 0; slot < header.planes * (1 + 3 * header.levels); slot++)
		if (header.steps[slot] - 1u >= MAX_STEP_CODE) return lfi_error_corrupt;

	uint32_t*     entropy_tables = (uint32_t*)scratch_mem;
	ent_decoder_t segments[MAX_LEVELS + 1];
	if (!ent_open_tables(entropy_tables, data + header.tables.offset, header.tables.size, symbol_extra_bits)) return lfi_error_corrupt;
	for (int32_t s = header.levels; s >= mip; s--)
		segments[s] = ent_open_stream(entropy_tables, data + header.streams[s][0].offset, header.streams[s][0].size, header.streams[s][1].size);

	size_t   tile_area   = (size_t)header.tile * header.tile;
	int16_t* planes      = (int16_t*)((uint8_t*)scratch_mem + ENT_TABLES_SIZE);
	int16_t* scratch     = planes + tile_area * header.planes;
	int16_t* bands       = scratch;                     // a level's detail bands, at most 3/4 of a tile
	int16_t* ll_copy     = scratch + tile_area / 4 * 3; // inverse_level's LL copy, at most 1/4
	int16_t* rows        = scratch + tile_area;
	int16_t* opaque      = rows + 3 * header.tile + 16;
	int16_t* luma_bands  = opaque + header.tile + BAND_SLACK; // a quarter tile
	for (int32_t i = 0; i < header.tile; i++) opaque[i] = 255;
	int32_t  out_w       = mip_size(header.w, mip);
	int32_t  pixel_bytes = format == lfi_format_native ? pixel_size(header.planes, header.hdr) : 4;
	size_t   layer_bytes = (size_t)out_w * mip_size(header.h, mip) * pixel_bytes;
	hdr_tables_t hdr_lookup;
	if (header.hdr && opt_out_pixels) hdr_tables(format, header.log_min, &hdr_lookup);
	int32_t  top         = opt_cache ? cache_mip : header.levels; // finest level already reconstructed, or none
	bool     color       = is_color(header.planes, header.data);

	for (int32_t layer = 0; layer < header.layers; layer++)
	for (int32_t tile_y = 0; tile_y < header.h; tile_y += header.tile) {
		for (int32_t tile_x = 0; tile_x < header.w; tile_x += header.tile) {
			int32_t tile_w     = header.w - tile_x < header.tile ? header.w - tile_x : header.tile;
			int32_t tile_h     = header.h - tile_y < header.tile ? header.h - tile_y : header.tile;
			size_t  plane_size = (size_t)tile_w * tile_h;
			band_t  decoded    = band_rect(tile_w, tile_h, mip, 0); // the region this mip decodes into

			ent_decoder_t* coarsest = &segments[header.levels];
			int32_t        luma_scale, chroma_scale;
			if (opt_cache) {
				band_t ll    = band_rect(tile_w, tile_h, cache_mip, 0);
				luma_scale   = *opt_cache++;
				chroma_scale = *opt_cache++;
				for (int32_t p = 0; p < header.planes; p++)
					for (int32_t y = 0; y < ll.h; y++, opt_cache += ll.w)
						memcpy(planes + p * plane_size + (size_t)y * tile_w, opt_cache, (size_t)ll.w * sizeof(int16_t));
				if ((uint32_t)luma_scale >= 16 || (uint32_t)chroma_scale >= 16) return lfi_error_argument;
			} else {
				luma_scale   = (int32_t)ent_decode(coarsest, CONTEXT_TILE);
				chroma_scale = (int32_t)ent_decode(coarsest, CONTEXT_TILE);
				if (coarsest->pos[0] > 0 || coarsest->pos[1] > 0 || luma_scale >= 16 || chroma_scale >= 16) return lfi_error_corrupt;
			}

			// Luma's bands from CFL_MIN_LEVEL up stay for chroma's prediction, each level's after an LL's worth of room
			bool luma_nonzero[MAX_LEVELS + 1][3];
			memset(luma_nonzero, 0, sizeof(luma_nonzero));

			for (int32_t p = 0; p < header.planes; p++) {
				int16_t* plane = planes + p * plane_size;
				int32_t  scale = is_chroma(color, p) ? chroma_scale : luma_scale;
				if (!opt_cache) {
					band_t ll = band_rect(tile_w, tile_h, header.levels, 0);
					memset(bands, 0, (size_t)ll.w * ll.h * sizeof(int16_t));
					if (!decode_ll(coarsest, plane, tile_w, ll, p, header, scale, bands)) return lfi_error_corrupt;
				}
				// Each level's bands decode just before its inverse, so they share one buffer
				// A band starting with SYMBOL_END is all zero, and is only cleared if inverse_level reads it
				for (int32_t level = top; level > mip; level--) {
					ent_decoder_t*  segment   = &segments[level - 1];
					const uint32_t* table     = ent_table(segment, band_context(color, p, level));
					bool            predicted = color && level >= CFL_MIN_LEVEL;
					band_t          low       = band_rect(tile_w, tile_h, level, 0);
					int16_t*        luma      = luma_bands + (size_t)low.w * low.h;
					int16_t*        detail[3];
					size_t          detail_size[3];
					int32_t         nonzero[3];
					for (int32_t o = 0; o < 3; o++) {
						band_t band    = band_rect(tile_w, tile_h, header.levels, 1 + 3 * (header.levels - level) + o);
						detail[o]      = o ? detail[o - 1] + detail_size[o - 1] : predicted && p == 0 ? luma : bands;
						detail_size[o] = (size_t)band.w * band.h;
						int8_t  scales[CFL_MAX_BLOCKS];
						int32_t from_luma = predicted && is_chroma(color, p) ? cfl_scales(segment, band, scales) : 0;
						if (from_luma < 0) return lfi_error_corrupt;
						if (ent_table_symbol(table, segment->state[0]) != SYMBOL_END) memset(detail[o], 0, detail_size[o] * sizeof(int16_t));
						if ((nonzero[o] = decode_detail(segment, detail[o], band, p, header, scale)) < 0) return lfi_error_corrupt;
						if (predicted && p == 0) luma_nonzero[level][o] = nonzero[o] != 0;
						if (!from_luma || !luma_nonzero[level][o]) continue;
						if (!nonzero[o]) memset(detail[o], 0, detail_size[o] * sizeof(int16_t));
						nonzero[o] = 1;
						int32_t luma_step = scale_step(header_step(header, 0, band.slot), luma_scale);
						cfl_add(detail[o], luma + (detail[o] - bands), band, scales, (int16_t)((header_offset(header, 0, band.slot) * luma_step + 32) >> 6));
					}

					band_t region = band_rect(tile_w, tile_h, level - 1, 0);
					if (!nonzero[0] && !nonzero[1] && !nonzero[2]) { interpolate_level(plane, tile_w, region.w, region.h, rows); continue; }
					for (int32_t o = 0; o < 3; o++)
						if (!nonzero[o]) memset(detail[o], 0, detail_size[o] * sizeof(int16_t));
					details_t details = { detail[0], detail[1], detail[2] };
					inverse_level(plane, tile_w, region.w, region.h, details, ll_copy, rows);
				}
			}

			if (opt_out_cache) {
				*opt_out_cache++ = (int16_t)luma_scale;
				*opt_out_cache++ = (int16_t)chroma_scale;
				for (int32_t p = 0; p < header.planes; p++)
					for (int32_t y = 0; y < decoded.h; y++, opt_out_cache += decoded.w)
						memcpy(opt_out_cache, planes + p * plane_size + (size_t)y * tile_w, (size_t)decoded.w * sizeof(int16_t));
			}
			for (int32_t y = 0; opt_out_pixels && y < decoded.h; y++) {
				uint8_t* row_out = opt_out_pixels + layer_bytes * layer + ((size_t)((tile_y >> mip) + y) * out_w + (tile_x >> mip)) * pixel_bytes;
				if (header.hdr) write_row_hdr(planes + (size_t)y * tile_w, plane_size, header, format, &hdr_lookup, scratch, row_out, decoded.w);
				else            write_row    (planes + (size_t)y * tile_w, plane_size, header, opaque, row_out, decoded.w);
			}
		}
	}
	return lfi_ok;
}

#ifdef LFI_ENCODER

// ---- ent_enc.c

#include <stdlib.h>

static void buffer_reserve(buffer_t* ref_buffer, size_t size) {
	if (ref_buffer->size + size <= ref_buffer->capacity) return;
	size_t capacity = ref_buffer->capacity ? ref_buffer->capacity : 256;
	while (capacity < ref_buffer->size + size) capacity *= 2;
	ref_buffer->data     = (uint8_t*)realloc(ref_buffer->data, capacity);
	ref_buffer->capacity = capacity;
}

void buffer_push(buffer_t* ref_buffer, const void* data, size_t size) {
	buffer_reserve(ref_buffer, size);
	memcpy(ref_buffer->data + ref_buffer->size, data, size);
	ref_buffer->size += size;
}

void buffer_u32(buffer_t* ref_buffer, uint32_t value) { buffer_push(ref_buffer, &value, 4); }

// Branchless: stores all 8 bytes, then advances past the whole ones. Takes up to 56 bits.
static void bit_write(bit_writer_t* ref_writer, uint64_t value, uint32_t count) {
	buffer_reserve(&ref_writer->out, 8);
	ref_writer->bits  |= value << ref_writer->count;
	ref_writer->count += count;
	memcpy(ref_writer->out.data + ref_writer->out.size, &ref_writer->bits, 8);
	ref_writer->out.size += ref_writer->count >> 3;
	ref_writer->bits    >>= ref_writer->count & ~7u;
	ref_writer->count    &= 7;
}

// The last write already stored the partial byte
static void bit_flush(bit_writer_t* ref_writer) { ref_writer->out.size += ref_writer->count != 0; }

void ent_grow(ent_encoder_t* ref_encoder) {
	ref_encoder->capacity    = ref_encoder->capacity ? ref_encoder->capacity * 2 : 4096;
	ref_encoder->symbols     = (uint16_t*)realloc(ref_encoder->symbols,     ref_encoder->capacity * sizeof(uint16_t));
	ref_encoder->extra       = (uint32_t*)realloc(ref_encoder->extra,       ref_encoder->capacity * sizeof(uint32_t));
	ref_encoder->extra_count = (uint8_t*) realloc(ref_encoder->extra_count, ref_encoder->capacity);
}

// Twins get at least 2, so each half is nonzero
static void normalize(const uint32_t* histogram, const uint8_t* twin, uint16_t* out_freq) {
	uint64_t total = 0;
	for (int32_t s = 0; s < ENT_SYMBOLS; s++) total += histogram[s];
	memset(out_freq, 0, ENT_SYMBOLS * sizeof(uint16_t));
	if (!total) return;

	int32_t sum = 0, likeliest = 0;
	for (int32_t s = 0; s < ENT_SYMBOLS; s++) {
		if (!histogram[s]) continue;
		uint32_t freq = (uint32_t)(histogram[s] * ENT_PROB_SCALE / total);
		out_freq[s] = (uint16_t)(freq > 1u + twin[s] ? freq : 1u + twin[s]);
		sum += out_freq[s];
		if (out_freq[s] > out_freq[likeliest]) likeliest = s;
	}
	// Rounding error lands on the likeliest symbols, where it costs the fewest bits
	while (sum > (int32_t)ENT_PROB_SCALE) {
		for (int32_t s = 0; s < ENT_SYMBOLS && sum > (int32_t)ENT_PROB_SCALE; s++)
			if (out_freq[s] > 1 + twin[s] && out_freq[s] * 4 >= out_freq[likeliest]) { out_freq[s]--; sum--; }
	}
	out_freq[likeliest] += (uint16_t)(ENT_PROB_SCALE - sum);
}

// Elias gamma: value + 1 as zeros, a one, then as many bits below it
static void write_gamma(bit_writer_t* ref_writer, uint32_t value) {
	uint32_t length = bit_length(value + 1);
	bit_write(ref_writer, (uint64_t)1 << (length - 1), length);
	bit_write(ref_writer, value + 1 - (1u << (length - 1)), length - 1);
}

// Keeps each frequency's top precision bits below its leading one, rounded to nearest, or down where nearest leaves
// the implied symbol too little. That symbol gets the rest. Returns the bits the stored frequencies take.
static uint32_t round_freqs(const uint16_t* listed_freq, const uint8_t* twin, uint32_t precision, int32_t implied, uint16_t* out_freq) {
	for (uint32_t nearest = 1;; nearest = 0) {
		uint32_t sum = 0, bits = 0;
		for (int32_t s = 0; s < ENT_SYMBOLS; s++) {
			out_freq[s] = listed_freq[s];
			if (!listed_freq[s] || s == implied) continue;
			uint32_t dropped = bit_length(listed_freq[s]) - 1 > precision ? bit_length(listed_freq[s]) - 1 - precision : 0;
			out_freq[s] = (uint16_t)((listed_freq[s] + (nearest << dropped >> 1)) >> dropped << dropped);
			sum        += out_freq[s];
			bits       += 4 + (bit_length(out_freq[s]) - 1 < precision ? bit_length(out_freq[s]) - 1 : precision);
		}
		if (sum + 1 + twin[implied] > ENT_PROB_SCALE) continue;
		out_freq[implied] = (uint16_t)(ENT_PROB_SCALE - sum);
		return bits;
	}
}

// log2 of a value of at least 1, in 1/65536, within 0.008 bits: the top bit, then the mantissa fraction x as
// x + 0.3465 x (1 - x). Integer math, so every build makes the same choices.
static int64_t log2_q16(uint64_t value) {
	int32_t top = 63;
	while (!(value >> top)) top--;
	int64_t x = (int64_t)(top > 16 ? value >> (top - 16) : value << (16 - top)) - 65536;
	return ((int64_t)top << 16) + x + (x * (65536 - x) * 22710 >> 32);
}

// Bits to code a histogram with listed frequencies, whose twins split as the decoder splits them, in 1/65536 bits.
// symbol_bits holds each frequency's cost.
static int64_t data_bits(const uint32_t* histogram, const uint8_t* twin, const uint16_t* listed_freq, const int64_t* symbol_bits) {
	int64_t bits = 0;
	for (int32_t s = 0; s < ENT_SYMBOLS; s++) {
		if (!listed_freq[s]) continue;
		bits += histogram[s] * symbol_bits[twin[s] ? (listed_freq[s] + 1) / 2 : listed_freq[s]];
		if (twin[s]) bits += histogram[s | ENT_TWIN] * symbol_bits[listed_freq[s] / 2];
	}
	return bits;
}

static uint32_t gamma_bits(uint32_t value) { return 2 * bit_length(value + 1) - 1; }

typedef struct {
	uint8_t  twin[ENT_SYMBOLS];
	uint16_t freq[ENT_SYMBOLS]; // listed, so a twin's covers both, and the implied symbol's is what the others leave
	uint32_t used, precision, implied_index;
	int32_t  implied;
	int64_t  bits;              // of the table and the symbols it codes, in 1/65536 bits
} table_plan_t;

// How a table lists a histogram, at the precision that codes the table and its symbols in the fewest bits
static void plan_table(const uint32_t* histogram, const int64_t* symbol_bits, table_plan_t* out_plan) {
	uint32_t merged[ENT_SYMBOLS];
	uint16_t listed_freq[ENT_SYMBOLS], rounded[ENT_SYMBOLS];
	memset(out_plan, 0, sizeof(*out_plan));
	memcpy(merged, histogram, sizeof(merged));
	// Twins are listed once, merged, where an even split costs less than listing both
	for (int32_t s = 0; s < ENT_SYMBOLS; s++) {
		if ((s & ENT_TWIN) || !merged[s] || !merged[s | ENT_TWIN]) continue;
		// Merging costs the signs' bits, total less their entropy a log2(total / a) + b log2(total / b)
		uint64_t a = merged[s], b = merged[s | ENT_TWIN], total = a + b;
		if ((int64_t)total * 65536 - (int64_t)a * (log2_q16(total) - log2_q16(a)) - (int64_t)b * (log2_q16(total) - log2_q16(b)) > 16 * 65536) continue;
		out_plan->twin[s]     = 1;
		merged[s]            += merged[s | ENT_TWIN];
		merged[s | ENT_TWIN]  = 0;
	}
	normalize(merged, out_plan->twin, listed_freq);
	uint32_t list_bits = 0;
	for (int32_t s = 0, previous = -1; s < ENT_SYMBOLS; s++) {
		if (!listed_freq[s]) continue;
		if (listed_freq[s] > listed_freq[out_plan->implied]) { out_plan->implied = s; out_plan->implied_index = out_plan->used; }
		list_bits += gamma_bits((uint32_t)(s - previous - 1)) + !(s & ENT_TWIN);
		previous   = s;
		out_plan->used++;
	}
	out_plan->bits = (int64_t)gamma_bits(out_plan->used) << 16;
	if (!out_plan->used) return;
	int64_t best = 0;
	for (uint32_t p = 0; p < 4; p++) {
		int64_t bits = ((int64_t)round_freqs(listed_freq, out_plan->twin, p, out_plan->implied, rounded) << 16) + data_bits(histogram, out_plan->twin, rounded, symbol_bits);
		if (p && bits >= best) continue;
		best                = bits;
		out_plan->precision = p;
		memcpy(out_plan->freq, rounded, sizeof(rounded));
	}
	out_plan->bits += ((int64_t)(2 + gamma_bits(out_plan->implied_index) + list_bits) << 16) + best;
}

// Bits saved by contexts a and b sharing a table, which is planned in the scratch slot after the contexts'
static int64_t merge_gain(uint32_t (*histogram)[ENT_SYMBOLS], const int64_t* symbol_bits, table_plan_t* plans, int32_t a, int32_t b) {
	for (int32_t s = 0; s < ENT_SYMBOLS; s++) histogram[ENT_CONTEXTS][s] = histogram[a][s] + histogram[b][s];
	plan_table(histogram[ENT_CONTEXTS], symbol_bits, &plans[ENT_CONTEXTS]);
	return plans[a].bits + plans[b].bits - plans[ENT_CONTEXTS].bits;
}

// out_tables gets the context map and tables. Context 0 has table 0 to itself. out_streams[2k] and out_streams[2k + 1]
// get segment k's two streams, each starting with its state.
void ent_write(ent_encoder_t* encoders, int32_t count, buffer_t* out_tables, buffer_t* out_streams) {
	uint32_t     (*histogram )[ENT_SYMBOLS]    = (uint32_t (*)[ENT_SYMBOLS])   calloc(ENT_CONTEXTS + 1, sizeof(*histogram));
	uint16_t     (*freq      )[ENT_SYMBOLS]    = (uint16_t (*)[ENT_SYMBOLS])   calloc(ENT_CONTEXTS, sizeof(*freq));
	uint16_t     (*cumulative)[ENT_SYMBOLS]    = (uint16_t (*)[ENT_SYMBOLS])   calloc(ENT_CONTEXTS, sizeof(*cumulative));
	uint16_t     (*encode    )[ENT_PROB_SCALE] = (uint16_t (*)[ENT_PROB_SCALE])calloc(ENT_CONTEXTS, sizeof(*encode)); // state for symbol s at symbol state xs, at cumulative[s] + xs - freq[s]
	table_plan_t*  plans                       = (table_plan_t*)malloc((ENT_CONTEXTS + 1) * sizeof(table_plan_t));
	int64_t        symbol_bits[ENT_PROB_SCALE + 1]; // to code a symbol of each frequency, in 1/65536 bits
	for (uint32_t f = 1; f <= ENT_PROB_SCALE; f++) symbol_bits[f] = ((int64_t)ENT_PROB_BITS << 16) - log2_q16(f);
	for (int32_t k = 0; k < count; k++)
		for (size_t i = 0; i < encoders[k].count; i++) histogram[encoders[k].symbols[i] >> ENT_SYMBOL_BITS][encoders[k].symbols[i] & (ENT_SYMBOLS - 1)]++;

	// Used contexts from 1 up start with a table each, then two tables merge while that saves bits, the most first.
	// A merged table lives on in the lower context.
	int64_t gain[ENT_CONTEXTS][ENT_CONTEXTS];
	uint8_t merged_into[ENT_CONTEXTS], alive[ENT_CONTEXTS];
	int32_t live = 0;
	for (int32_t c = 0; c < ENT_CONTEXTS; c++) {
		plan_table(histogram[c], symbol_bits, &plans[c]);
		merged_into[c] = (uint8_t)c;
		alive[c]       = c && plans[c].used;
		live          += alive[c];
	}
	for (int32_t a = 1; a < ENT_CONTEXTS; a++)
		for (int32_t b = a + 1; b < ENT_CONTEXTS; b++)
			if (alive[a] && alive[b]) gain[a][b] = merge_gain(histogram, symbol_bits, plans, a, b);
	while (live > 1) {
		int32_t a = 0, b = 0;
		for (int32_t x = 1; x < ENT_CONTEXTS; x++)
			for (int32_t y = x + 1; y < ENT_CONTEXTS; y++)
				if (alive[x] && alive[y] && (!a || gain[x][y] > gain[a][b])) { a = x; b = y; }
		// One table fewer can also take a bit off each context's table index
		if (gain[a][b] + ((int64_t)15 * (bit_length((uint32_t)live - 1) - bit_length((uint32_t)live - 2)) << 16) <= 0) break;
		for (int32_t s = 0; s < ENT_SYMBOLS; s++) histogram[a][s] += histogram[b][s];
		plan_table(histogram[a], symbol_bits, &plans[a]);
		alive[b] = 0;
		live--;
		for (int32_t c = 0; c < ENT_CONTEXTS; c++) merged_into[c] = merged_into[c] == b ? (uint8_t)a : merged_into[c];
		for (int32_t x = 1; x < ENT_CONTEXTS; x++)
			if (alive[x] && x != a) gain[x < a ? x : a][x < a ? a : x] = merge_gain(histogram, symbol_bits, plans, x < a ? x : a, x < a ? a : x);
	}
	// Tables in context order, and unused contexts take table 1
	uint8_t  table_of[ENT_CONTEXTS] = {0}, number[ENT_CONTEXTS] = {0};
	uint32_t shared                 = 0;
	alive[1] |= !live;
	for (int32_t c = 1; c < ENT_CONTEXTS; c++) number[c]   = alive[c] ? (uint8_t)++shared : 0;
	for (int32_t c = 1; c < ENT_CONTEXTS; c++) table_of[c] = alive[merged_into[c]] ? number[merged_into[c]] : 1;

	bit_writer_t tables;
	memset(&tables, 0, sizeof(tables));
	bit_write(&tables, shared - 1, 4);
	for (int32_t c = 1; c < ENT_CONTEXTS; c++) bit_write(&tables, table_of[c] - 1u, bit_length(shared - 1));
	for (int32_t c = 0; c < ENT_CONTEXTS; c++) {
		if (c && !alive[c]) continue;
		const table_plan_t* plan        = &plans[c];
		const uint8_t*      twin        = plan->twin;
		const uint16_t*     listed_freq = plan->freq;
		uint32_t            t           = number[c];
		write_gamma(&tables, plan->used);
		if (plan->used) {
			bit_write(&tables, plan->precision, 2);
			write_gamma(&tables, plan->implied_index);
		}
		for (int32_t s = 0, previous = -1; s < ENT_SYMBOLS; s++) {
			if (!listed_freq[s]) continue;
			write_gamma(&tables, (uint32_t)(s - previous - 1));
			if (!(s & ENT_TWIN)) bit_write(&tables, twin[s], 1);
			previous = s;
			if (s == plan->implied) continue;
			uint32_t length = bit_length(listed_freq[s]), kept = length - 1 < plan->precision ? length - 1 : plan->precision;
			bit_write(&tables, length, 4);
			bit_write(&tables, (listed_freq[s] - (1u << (length - 1))) >> (length - 1 - kept), kept);
		}

		// The implied symbol's states come last, once the decoder knows its frequency
		uint16_t symbols[ENT_PROB_SCALE];
		uint32_t next[ENT_SYMBOLS];
		int32_t  sum    = 0;
		uint32_t spread = 0;
		for (int32_t i = 0; plan->used && i <= ENT_SYMBOLS; i++) {
			int32_t listed = i < ENT_SYMBOLS ? i : plan->implied;
			if (!listed_freq[listed] || (i < ENT_SYMBOLS && listed == plan->implied)) continue;
			freq[t][listed] = twin[listed] ? (uint16_t)((listed_freq[listed] + 1) / 2) : listed_freq[listed];
			if (twin[listed]) freq[t][listed | ENT_TWIN] = listed_freq[listed] / 2;
			for (int32_t s = listed; s <= (twin[listed] ? (listed | ENT_TWIN) : listed); s += ENT_TWIN) {
				cumulative[t][s] = (uint16_t)sum;
				sum             += freq[t][s];
				next[s]          = freq[t][s];
				for (uint32_t j = 0; j < freq[t][s]; j++, spread = (spread + ENT_SPREAD_STEP) & (ENT_PROB_SCALE - 1)) symbols[spread] = (uint16_t)s;
			}
		}
		for (uint32_t state = 0; plan->used && state < ENT_PROB_SCALE; state++) {
			uint32_t s = symbols[state];
			encode[t][cumulative[t][s] + next[s]++ - freq[t][s]] = (uint16_t)state;
		}
	}
	bit_flush(&tables);
	*out_tables = tables.out;

	for (int32_t k = 0; k < count; k++) {
		ent_encoder_t* encoder     = &encoders[k];
		uint16_t*      state_bits  = (uint16_t*)malloc((encoder->count + 1) * sizeof(uint16_t));
		uint8_t*       state_count = (uint8_t*) malloc(encoder->count + 1);
		// States run backwards, so the decoder meets each symbol's state bits right after the symbol.
		// Symbol i uses stream i & 1, so the two states swap after every symbol.
		uint32_t state = ENT_PROB_SCALE, other = ENT_PROB_SCALE;
		for (size_t i = encoder->count; i-- > 0;) {
			uint32_t t           = table_of[encoder->symbols[i] >> ENT_SYMBOL_BITS], s = encoder->symbols[i] & (ENT_SYMBOLS - 1);
			uint32_t symbol_freq = freq[t][s];
			// The least shift that brings the state below 2 * symbol_freq
			uint32_t shift       = ENT_PROB_BITS + 1 - bit_length(symbol_freq - 1);
			shift         -= state < symbol_freq << shift;
			state_bits [i] = (uint16_t)(state & ((1u << shift) - 1));
			state_count[i] = (uint8_t)shift;
			uint32_t next  = ENT_PROB_SCALE + encode[t][cumulative[t][s] + (state >> shift) - symbol_freq];
			state = other;
			other = next;
		}
		uint32_t states[2] = { other, state };
		for (int32_t j = 0; j < 2; j++) {
			bit_writer_t writer;
			memset(&writer, 0, sizeof(writer));
			bit_write(&writer, states[j] - ENT_PROB_SCALE, ENT_PROB_BITS);
			for (size_t i = (size_t)j; i < encoder->count; i += 2)
				bit_write(&writer, state_bits[i] | (uint64_t)encoder->extra[i] << state_count[i], state_count[i] + encoder->extra_count[i]);
			bit_flush(&writer);
			out_streams[2 * k + j] = writer.out;
		}
		free(state_bits);
		free(state_count);
	}

	free(histogram);
	free(plans);
	free(freq);
	free(cumulative);
	free(encode);
}

void ent_free(ent_encoder_t* ref_encoder) {
	free(ref_encoder->symbols);
	free(ref_encoder->extra);
	free(ref_encoder->extra_count);
	memset(ref_encoder, 0, sizeof(*ref_encoder));
}

// ---- lfi_enc.c

#include <math.h>

// 1 quantizes for rate, for files about 1.5% smaller. 0 leaves that out, for a smaller and faster encoder.
#ifndef LFI_ENCODER_QUALITY
#define LFI_ENCODER_QUALITY 1
#endif

// The loops skip the mirrored edges, which are done after
static void forward_row(int16_t* LFI_RESTRICT out_low, int16_t* LFI_RESTRICT out_high, const int16_t* LFI_RESTRICT row, int32_t width) {
	int32_t low_count = (width + 1) >> 1, high_count = width >> 1, last = width - 1;
	if (!high_count) { out_low[0] = row[0]; return; }
	for (int32_t i = 0; i < low_count - 1; i++) out_high[i] = (int16_t)(row[2*i + 1] - predict(row[2*i], row[2*i + 2]));
	if (!(width & 1)) out_high[high_count - 1] = (int16_t)(row[last] - predict(row[last - 1], row[last - 1]));
	for (int32_t i = 0; i < low_count; i++) {
		int16_t far_a = out_high[clamp_index(i - 2, high_count)], a = out_high[clamp_index(i - 1, high_count)];
		int16_t b     = out_high[clamp_index(i, high_count)],     far_b = out_high[clamp_index(i + 1, high_count)];
		out_low[i] = (int16_t)(row[2*i] + update(far_a, a, b, far_b));
	}
}

// Region w*h at the plane origin becomes [L H] columns and [L; H] rows
static void forward_level(int16_t* LFI_RESTRICT plane, int32_t stride, int32_t w, int32_t h, int16_t* LFI_RESTRICT scratch) {
	int32_t low_w = (w + 1) >> 1, low_h = (h + 1) >> 1, high_h = h >> 1;
	for (int32_t y = 0; y < h; y++) forward_row(scratch + y*w, scratch + y*w + low_w, plane + y*stride, w);

	if (!high_h) { memcpy(plane, scratch, (size_t)w * sizeof(int16_t)); return; }
	for (int32_t i = 0; i < high_h; i++) {
		const int16_t* even      = scratch + (2*i)     * w;
		const int16_t* odd       = scratch + (2*i + 1) * w;
		const int16_t* next_even = scratch + (2*i + 2 < h ? 2*i + 2 : 2*i) * w;
		int16_t*       high      = plane + (low_h + i) * stride;
		for (int32_t x = 0; x < w; x++) high[x] = (int16_t)(odd[x] - predict(even[x], next_even[x]));
	}
	for (int32_t i = 0; i < low_h; i++) {
		const int16_t* even       = scratch + (2*i) * w;
		const int16_t* far_above  = plane + (low_h + clamp_index(i - 2, high_h)) * stride;
		const int16_t* high_above = plane + (low_h + clamp_index(i - 1, high_h)) * stride;
		const int16_t* high_below = plane + (low_h + clamp_index(i, high_h))     * stride;
		const int16_t* far_below  = plane + (low_h + clamp_index(i + 1, high_h)) * stride;
		int16_t*       low        = plane + i * stride;
		for (int32_t x = 0; x < w; x++) low[x] = (int16_t)(even[x] + update(far_above[x], high_above[x], high_below[x], far_below[x]));
	}
}

// Each nonzero is one symbol: (zero run class << 5 | sign | magnitude class), for runs below ZERO_RUN_LENGTH.
// Extra bits, low to high: run mantissa, magnitude mantissa.
static uint32_t value_symbol(uint32_t run, int32_t value, uint32_t* out_extra, uint32_t* out_extra_count) {
	uint32_t magnitude       = (uint32_t)(value < 0 ? -value : value);
	uint32_t magnitude_class = bit_length(magnitude);
	uint32_t run_class = run, run_bits = 0, run_mantissa = 0;
	if (run >= 8) {
		run_bits     = bit_length(run) - 1;
		run_class    = run_bits + 5;
		run_mantissa = run - (1u << run_bits);
	}
	*out_extra       = run_mantissa | (magnitude - (1u << (magnitude_class - 1))) << run_bits;
	*out_extra_count = run_bits + magnitude_class - 1;
	return run_class << 5 | (value < 0 ? SYMBOL_NEGATIVE : 0) | magnitude_class;
}

// The first nonzero at or after i, or count, skipping zeros four at a time
static inline int32_t next_nonzero(const int16_t* values, int32_t i, int32_t count) {
	while (i + 4 <= count && !read_u64((const uint8_t*)(values + i))) i += 4;
	while (i < count && !values[i]) i++;
	return i;
}

// log2 in 1/256 bits, reading the 8 bits below the top one linearly
static uint32_t log2_fixed(uint32_t value) {
	uint32_t top = bit_length(value) - 1;
	return top << 8 | ((top >= 8 ? value >> (top - 8) : value << (8 - top)) & 255);
}

// Each symbol's cost with its extra bits, in 1/256 bits for every context, from symbol counts. Unseen symbols count as half.
static void symbol_costs(const uint32_t* counts, uint16_t* out_costs) {
	for (int32_t context = 0; context < ENT_CONTEXTS; context++) {
		const uint32_t* count = counts + context * ENT_SYMBOLS;
		uint32_t        total = 1;
		for (int32_t s = 0; s < ENT_SYMBOLS; s++) total += count[s];
		for (uint32_t s = 0; s < ENT_SYMBOLS; s++) {
			uint32_t run_class = s >> 5, magnitude_class = s & 15;
			uint32_t extra     = magnitude_class ? (run_class >= 8 ? run_class - 5 : 0) + magnitude_class - 1 : 0;
			out_costs[context * ENT_SYMBOLS + s] = (uint16_t)(log2_fixed(total) + 256 - log2_fixed(count[s] ? 2 * count[s] : 1) + (extra << 8));
		}
	}
}

// Bits of a value after run zeros, or of the band's end for 0, in 1/256 bits
static int32_t value_cost(const uint16_t* costs, uint32_t run, int32_t value) {
	if (!value) return costs[SYMBOL_END];
	uint32_t extra, extra_count, long_runs = 0;
	for (; run >= ZERO_RUN_LENGTH; run -= ZERO_RUN_LENGTH) long_runs += costs[SYMBOL_ZERO_RUN];
	return (int32_t)(long_runs + costs[value_symbol(run, value, &extra, &extra_count)]);
}

// Weight of a bit against squared error, in 1/64 squared steps
static const int64_t rate_weight = 9;

// Whether lowering a ±1 or a power of two one bin saves more bits, weighted by rate_weight, than the error it adds.
// A ±1 drops out, merging its zero run into the next value's, and a power of two drops a magnitude class.
// Bin b reconstructs at b + offset/64 steps, and 0 at 0.
static bool rate_lowers(const uint16_t* costs, int32_t run, int32_t value, int32_t next_run, int32_t next_value, uint32_t magnitude, int64_t step, int32_t offset) {
	int32_t bin = value < 0 ? -value : value, lower = value < 0 ? value + 1 : value - 1, saved;
	if (bin & (bin - 1)) return false;
	if (lower) saved = value_cost(costs, (uint32_t)run, value) - value_cost(costs, (uint32_t)run, lower);
	else       saved = value_cost(costs, (uint32_t)run, value) + value_cost(costs, (uint32_t)next_run, next_value) - value_cost(costs, (uint32_t)(run + 1 + next_run), next_value);
	// Errors in 1/64 of a coefficient unit
	int64_t target = (int64_t)magnitude * 64;
	int64_t here   = target - (bin * 64 + offset) * step;
	int64_t below  = lower ? target - ((bin - 1) * 64 + offset) * step : target;
	return 4 * (below * below - here * here) < rate_weight * saved * step * step;
}

static void put_value(ent_encoder_t* ref_encoder, uint32_t context, uint32_t run, int32_t value) {
	for (; run >= ZERO_RUN_LENGTH; run -= ZERO_RUN_LENGTH) ent_symbol(ref_encoder, context, SYMBOL_ZERO_RUN, 0, 0);
	uint32_t extra, extra_count, symbol = value_symbol(run, value, &extra, &extra_count);
	ent_symbol(ref_encoder, context, symbol, extra, extra_count);
}

static void put_band(ent_encoder_t* ref_encoder, uint32_t context, const int16_t* quantized, int32_t count) {
	for (int32_t i = 0, run_start = 0; (i = next_nonzero(quantized, i, count)) < count; run_start = ++i) put_value(ref_encoder, context, (uint32_t)(i - run_start), quantized[i]);
	ent_symbol(ref_encoder, context, SYMBOL_END, 0, 0);
}

// Codes a band as put_band does, first lowering values in place where rate_lowers says to. A loop of its own, since
// the decisions would crowd put_band's registers.
static void put_band_for_rate(ent_encoder_t* ref_encoder, uint32_t context, int16_t* ref_quantized, const uint16_t* magnitudes, int32_t count, int64_t step, int32_t offset, const uint16_t* costs) {
	for (int32_t i = next_nonzero(ref_quantized, 0, count), previous = -1, next; i < count; i = next) {
		next = next_nonzero(ref_quantized, i + 1, count);
		int32_t value = ref_quantized[i], run = i - previous - 1;
		if (rate_lowers(costs, run, value, next - i - 1, next < count ? ref_quantized[next] : 0, magnitudes[i], step, offset)) {
			value = ref_quantized[i] = (int16_t)(value < 0 ? value + 1 : value - 1);
			if (!value) continue;
		}
		put_value(ref_encoder, context, (uint32_t)run, value);
		previous = i;
	}
	ent_symbol(ref_encoder, context, SYMBOL_END, 0, 0);
}

// Rounding offset of the quantizer, in steps
static const float deadzone = 0.35f;
// A ±1 with zero scan neighbors is dropped below this many steps, since it costs a symbol for little error
static const float isolated = 1.0f;

// Floor division of magnitude + bias by step. For sums below 2^16, the estimate from a 16-bit reciprocal is exact or one short.
static void quantize(const int16_t* LFI_RESTRICT source, int16_t* LFI_RESTRICT out_quantized, uint16_t* LFI_RESTRICT out_magnitudes, int32_t count, uint32_t bias, uint32_t step) {
	uint32_t reciprocal = 65536 / step;
	for (int32_t i = 0; i < count; i++) {
		uint32_t magnitude = (uint16_t)(source[i] < 0 ? -source[i] : source[i]);
		uint32_t sum       = magnitude + bias;
		uint32_t bin       = sum * reciprocal >> 16;
		bin += sum - bin * step >= step;
		out_magnitudes[i] = (uint16_t)magnitude;
		out_quantized [i] = (int16_t)(source[i] < 0 ? -(int32_t)bin : (int32_t)bin);
	}
}

// How far a row's nonzeros sit into their bins, summed, which fits 32 bits. ref_nonzero counts them.
static int32_t bin_fraction(const int16_t* LFI_RESTRICT quantized, const uint16_t* LFI_RESTRICT magnitudes, int32_t count, int32_t step, int64_t* ref_nonzero) {
	int32_t magnitude_sum = 0, bin_sum = 0, nonzero = 0;
	for (int32_t i = 0; i < count; i++) {
		int32_t value = quantized[i], magnitude = magnitudes[i], bin = value < 0 ? -value : value;
		magnitude_sum += bin ? magnitude : 0;
		bin_sum       += bin;
		nonzero       += bin != 0;
	}
	*ref_nonzero += nonzero;
	return magnitude_sum - bin_sum * step;
}

// Drops each ±1 below limit whose neighbors, quantized[i-1] and quantized[i+1], are zero. No drop changes another's neighbors.
static void drop_isolated(const int16_t* LFI_RESTRICT quantized, const uint16_t* LFI_RESTRICT magnitudes, int16_t* LFI_RESTRICT out_kept, int32_t count, int32_t limit) {
	for (int32_t i = 0; i < count; i++) {
		int32_t value = quantized[i], neighbors = quantized[i-1] | quantized[i+1];
		bool    keep  = (value * value != 1) | (magnitudes[i] >= limit) | (neighbors != 0);
		out_kept[i] = (int16_t)(keep ? value : 0);
	}
}

// scratch holds three times the band size and two more: the quantized values in scan order with a zero on each
// side, their source magnitudes, then the transposed source or the values kept. Detail bands are quantized for rate
// given costs, with offset in 1/64 steps, else by the isolated rule. ref_centroid accumulates as in bin_fraction.
// opt_out_values gets a detail band's coded values without offset, w x h, for chroma's prediction.
static void encode_band(ent_encoder_t* ref_encoder, const int16_t* plane, int32_t stride, band_t band, uint32_t context, int32_t step, const uint16_t* opt_costs, int32_t offset, int16_t* scratch, double* ref_centroid, int16_t* opt_out_values) {
	int32_t        count      = band.w * band.h;
	int16_t*       quantized  = scratch + 1;
	uint16_t*      magnitudes = (uint16_t*)(quantized + count + 1);
	int16_t*       kept       = (int16_t*)(magnitudes + count);
	const int16_t* source     = plane + (size_t)band.y * stride + band.x;
	int32_t        rows       = band.h, length = band.w;
	if (column_scan(band)) {
		// 16 rows at a time, since a whole column at a power of two stride falls in a few cache sets
		for (int32_t top = 0; top < band.h; top += 16) {
			int32_t bottom = band.h - top < 16 ? band.h : top + 16;
			for (int32_t x = 0; x < band.w; x++)
				for (int32_t y = top; y < bottom; y++) kept[x * band.h + y] = source[(size_t)y * stride + x];
		}
		source = kept;
		rows   = band.w;
		length = stride = band.h;
	}
	for (int32_t y = 0; y < rows; y++)
		quantize(source + (size_t)y * stride, quantized + y * length, magnitudes + y * length, length, (uint32_t)(step * deadzone), (uint32_t)step);
	quantized[-1] = quantized[count] = 0;

	int16_t* coded = quantized;
	if (band.level && !opt_costs && step > 1) {
		drop_isolated(quantized, magnitudes, kept, count, (int32_t)(step * isolated));
		coded = kept;
	}
	// MED residuals, in reverse so the predictor reads unmodified neighbors
	for (int32_t y = band.level ? 0 : band.h; y-- > 0;) {
		int16_t* row = quantized + y * band.w;
		for (int32_t x = band.w; x-- > 0;) row[x] = (int16_t)(row[x] - median_edge_at(row, band.w, x, y));
	}
	if (LFI_ENCODER_QUALITY && band.level && opt_costs) put_band_for_rate(ref_encoder, context, coded, magnitudes, count, step, offset, opt_costs + context * ENT_SYMBOLS);
	else                                                put_band(ref_encoder, context, coded, count);
	if (!band.level) return;
	for (int32_t i = 0; opt_out_values && i < count; i++)
		opt_out_values[column_scan(band) ? i % band.h * band.w + i / band.h : i] = (int16_t)(coded[i] * step);
	int64_t fraction = 0, nonzero = 0;
	for (int32_t at = 0; at < count; at += length) fraction += bin_fraction(coded + at, magnitudes + at, length, step, &nonzero);
	ref_centroid[0] += (double)fraction / step;
	ref_centroid[1] += (double)nonzero;
}

// 1D synthesis energy of one coefficient at each level, for MSE-balanced steps
static const float gain_low [MAX_LEVELS + 1] = { 1, 1.5f,     2.75f,    5.375f,   10.6875f, 21.34375f, 42.67188f };
static const float gain_high[MAX_LEVELS + 1] = { 0, 0.70898f, 0.88184f, 1.49561f, 2.85718f, 5.64734f,  11.26117f };
// Error energy of each plane relative to the first. Chroma's follows from the color inverse. Soft alpha gets half the
// luma step since its errors show as shimmering edges, while cutout alpha is snapped back to 0 and 255 and survives
// twice the luma step exactly.
static float plane_gain(bool color, int32_t plane, bool cutout) {
	static const float color_gains[3] = { 1, 0.67f / 3, 0.83f / 3 };
	if (!color) return 1;
	return plane < 3 ? color_gains[plane] : cutout ? 0.25f : 4;
}

static float band_gain(int32_t levels, int32_t slot) {
	if (!slot) return gain_low[levels] * gain_low[levels];
	int32_t level = levels - (slot - 1) / 3;
	return ((slot - 1) % 3 == 2 ? gain_high[level] : gain_low[level]) * gain_high[level];
}

// Step multipliers for levels 1, 2, 3 and coarser. Color luma's shift bits to the middle frequencies, which
// SSIMULACRA2 and Butteraugli weigh more than squared error does.
static const float level_multipliers[3][4] = {
	{ 1,    1,     1,     1      }, // gray, normal and alpha
	{ 1.6f, 0.8f,  0.5f,  0.5f   }, // luma
	{ 1.8f, 1.17f, 0.99f, 0.765f }, // chroma
};

// Each band's step code, nearest its step in ratio
static void step_table(float base, int32_t channels, bool color, int32_t levels, bool cutout, bool fixed, uint8_t* out_codes) {
	float unit = fixed ? (float)(1 << FIXED_BITS) : 1;
	for (int32_t p = 0; p < channels; p++) {
		const float* multipliers = level_multipliers[!color || p == 3 ? 0 : 1 + is_chroma(color, p)];
		for (int32_t slot = 0; slot < 1 + 3 * levels; slot++) {
			int32_t  level = slot ? levels - (slot - 1) / 3 : 4;
			float    step  = base <= 1 ? 1 : unit * multipliers[(level < 4 ? level : 4) - 1] * base / sqrtf(plane_gain(color, p, cutout) * band_gain(levels, slot));
			// Nearer the next code once step^2 passes the product of the two, which doubles hold exactly
			uint32_t code  = 1;
			while (code < MAX_STEP_CODE && (double)step * step > (double)step_value(code) * step_value(code + 1)) code++;
			out_codes[p * MAX_BANDS + slot] = (uint8_t)code;
		}
	}
}

// Either 8 bit or float RGBA pixels
typedef struct {
	const uint8_t* opt_rgba8;
	const float*   opt_rgbaf;
	int32_t        log_min; // HDR code 1, in 1/HDR_CODES_PER_STOP stops
} source_t;

// Mantissas at which log2 reaches each half step of 1/128 stop, ceil(2^23 (2^((k + 1/2) / 128) - 1))
static const uint32_t half_stop_mantissas[128] = {
	0x0058D8, 0x010B41, 0x01BEA2, 0x0272FC, 0x032851, 0x03DEA2, 0x0495F0, 0x054E3D,
	0x06078B, 0x06C1DA, 0x077D2B, 0x083982, 0x08F6DE, 0x09B541, 0x0A74AD, 0x0B3523,
	0x0BF6A5, 0x0CB933, 0x0D7CD0, 0x0E417D, 0x0F073B, 0x0FCE0D, 0x1095F2, 0x115EEE,
	0x122900, 0x12F42C, 0x13C072, 0x148DD4, 0x155C54, 0x162BF2, 0x16FCB1, 0x17CE93,
	0x18A198, 0x1975C2, 0x1A4B14, 0x1B218E, 0x1BF932, 0x1CD201, 0x1DABFE, 0x1E872B,
	0x1F6387, 0x204117, 0x211FDA, 0x21FFD2, 0x22E103, 0x23C36C, 0x24A710, 0x258BF0,
	0x26720E, 0x27596D, 0x28420C, 0x292BF0, 0x2A1718, 0x2B0387, 0x2BF13F, 0x2CE042,
	0x2DD090, 0x2EC22D, 0x2FB51A, 0x30A958, 0x319EE9, 0x3295D0, 0x338E0F, 0x3487A6,
	0x358298, 0x367EE7, 0x377C95, 0x387BA4, 0x397C15, 0x3A7DEA, 0x3B8126, 0x3C85CA,
	0x3D8BD9, 0x3E9354, 0x3F9C3D, 0x40A696, 0x41B261, 0x42BFA1, 0x43CE57, 0x44DE86,
	0x45F02E, 0x470353, 0x4817F7, 0x492E1B, 0x4A45C2, 0x4B5EED, 0x4C79A0, 0x4D95DB,
	0x4EB3A1, 0x4FD2F5, 0x50F3D8, 0x52164D, 0x533A55, 0x545FF3, 0x55872A, 0x56AFFA,
	0x57DA68, 0x590674, 0x5A3421, 0x5B6372, 0x5C9468, 0x5DC706, 0x5EFB4F, 0x603144,
	0x6168E7, 0x62A23C, 0x63DD45, 0x651A03, 0x66587A, 0x6798AB, 0x68DA9A, 0x6A1E48,
	0x6B63B8, 0x6CAAEC, 0x6DF3E7, 0x6F3EAC, 0x708B3C, 0x71D99A, 0x7329CA, 0x747BCC,
	0x75CFA5, 0x772556, 0x787CE1, 0x79D64B, 0x7B3194, 0x7C8EC1, 0x7DEDD2, 0x7F4ECC,
};

// log2 of a positive float in 1/HDR_CODES_PER_STOP stops, rounded to nearest, from its bits rather than libm so
// every build agrees. Values below the normal range count as the smallest normal. A quadratic estimate of the
// fraction, x + 0.3465 x (1 - x), is within one code for every mantissa, and the table settles it exactly.
static int32_t log2_stops(float value) {
	uint32_t bits;
	memcpy(&bits, &value, sizeof(bits));
	if (bits >> 23 == 0) return -126 * HDR_CODES_PER_STOP;
	int64_t  x        = (bits & 0x7FFFFF) >> 7; // the mantissa in 1/65536
	uint32_t mantissa = bits & 0x7FFFFF, fraction = (uint32_t)((x + (x * (65536 - x) * 22710 >> 32)) * HDR_CODES_PER_STOP + 32768) >> 16;
	fraction += fraction < HDR_CODES_PER_STOP && mantissa >= half_stop_mantissas[fraction];
	fraction -= fraction > 0 && mantissa < half_stop_mantissas[fraction - 1];
	return ((int32_t)(bits >> 23) - 127) * HDR_CODES_PER_STOP + (int32_t)fraction;
}

// log2 brightness in 1/HDR_CODES_PER_STOP stops, with 0 for zero and below
static int32_t hdr_code(float value, int32_t log_min) {
	if (!(value > 0)) return 0;
	int32_t code = log2_stops(value < 65504 ? value : 65504) - log_min + 1;
	return code < 1 ? 1 : code > HDR_CODES - 1 ? HDR_CODES - 1 : code;
}

// One int16 plane per coded channel, tile-local
static void load_tile(source_t source, int32_t w, int32_t tile_x, int32_t tile_y, int32_t tile_w, int32_t tile_h, int32_t channels, int16_t* out_planes) {
	size_t plane_size = (size_t)tile_w * tile_h;
	for (int32_t y = 0; y < tile_h; y++) {
		size_t   pixel = (size_t)(tile_y + y) * w + tile_x;
		int16_t* row   = out_planes + (size_t)y * tile_w;
		for (int32_t x = 0; x < tile_w; x++)
			for (int32_t c = 0; c < channels; c++)
				row[c * plane_size + x] = (int16_t)(source.opt_rgbaf ? hdr_code(source.opt_rgbaf[(pixel + x) * 4 + c], source.log_min) : source.opt_rgba8[(pixel + x) * 4 + c]);
	}
}

// Replaces the color planes under fully transparent pixels with a push-pull fill from the visible ones: averages
// pushed down a pyramid, then pulled back up bilinearly. Smooth fills cost few bits, and keep the coarse
// bands that reduced mips decode from free of hidden colors.
static void bleed_fill(int16_t* planes, size_t plane_size, int32_t tile_w, int32_t tile_h) {
	int32_t widths [16] = { tile_w }, heights[16] = { tile_h }, levels = 1;
	size_t  offsets[16] = { 0 }, total = plane_size;
	while (widths[levels - 1] > 1 || heights[levels - 1] > 1) {
		widths [levels] = (widths [levels - 1] + 1) >> 1;
		heights[levels] = (heights[levels - 1] + 1) >> 1;
		offsets[levels] = total;
		total          += (size_t)widths[levels] * heights[levels];
		levels++;
	}
	int64_t* texels = (int64_t*)calloc(total * 4, sizeof(int64_t)); // color plane sums, then weight. Integers, so every build fills alike.

	const int16_t* alpha = planes + 3 * plane_size;
	for (size_t i = 0; i < plane_size; i++) {
		if (!alpha[i]) continue;
		for (int32_t c = 0; c < 3; c++) texels[i*4 + c] = planes[c * plane_size + i];
		texels[i*4 + 3] = 1;
	}
	for (int32_t l = 1; l < levels; l++)
		for (int32_t y = 0; y < heights[l]; y++)
			for (int32_t x = 0; x < widths[l]; x++) {
				int64_t* sum = texels + (offsets[l] + (size_t)y * widths[l] + x) * 4;
				for (int32_t child = 0; child < 4; child++) {
					int32_t child_x = 2*x + (child & 1), child_y = 2*y + (child >> 1);
					if (child_x >= widths[l-1] || child_y >= heights[l-1]) continue;
					const int64_t* finer = texels + (offsets[l-1] + (size_t)child_y * widths[l-1] + child_x) * 4;
					for (int32_t c = 0; c < 4; c++) sum[c] += finer[c];
				}
			}

	// Normalize each level to an average with 8 fractional bits, then fill empty texels from the level above. A
	// texel's center sits a quarter coarse texel from two coarse centers, so its weights are 0, 1/4 or 3/4.
	for (int32_t l = levels - 1; l >= 0; l--)
		for (int32_t y = 0; y < heights[l]; y++)
			for (int32_t x = 0; x < widths[l]; x++) {
				int64_t* texel = texels + (offsets[l] + (size_t)y * widths[l] + x) * 4;
				if (texel[3] > 0) {
					for (int32_t c = 0; c < 3; c++) texel[c] = (texel[c] * 512 + (texel[c] < 0 ? -texel[3] : texel[3])) / (2 * texel[3]);
					texel[3] = 1;
					continue;
				}
				if (l == levels - 1) continue;
				int32_t quarter_x = 2 * x - 1, quarter_y = 2 * y - 1; // the coarse position, in quarter texels
				int32_t x0 = clamp_index(quarter_x >> 2, widths[l+1]), x1 = clamp_index((quarter_x >> 2) + 1, widths[l+1]), weight_x = quarter_x & 3;
				int32_t y0 = clamp_index(quarter_y >> 2, heights[l+1]), y1 = clamp_index((quarter_y >> 2) + 1, heights[l+1]), weight_y = quarter_y & 3;
				const int64_t* coarser = texels + offsets[l+1] * 4;
				for (int32_t c = 0; c < 3; c++) {
					int64_t top    = coarser[((size_t)y0 * widths[l+1] + x0) * 4 + c] * (4 - weight_x) + coarser[((size_t)y0 * widths[l+1] + x1) * 4 + c] * weight_x;
					int64_t bottom = coarser[((size_t)y1 * widths[l+1] + x0) * 4 + c] * (4 - weight_x) + coarser[((size_t)y1 * widths[l+1] + x1) * 4 + c] * weight_x;
					texel[c] = (top * (4 - weight_y) + bottom * weight_y + 8) >> 4;
				}
			}

	for (size_t i = 0; i < plane_size; i++)
		for (int32_t c = 0; !alpha[i] && c < 3; c++) planes[c * plane_size + i] = (int16_t)((texels[i*4 + c] + 128) >> 8);
	free(texels);
}

// Dark regions get finer steps, since their errors become visible once a texture is lit
static int32_t dark_scale(const int16_t* luma, size_t count, float quarters_per_halving) {
	int64_t sum = 0;
	for (size_t i = 0; i < count; i++) sum += luma[i];
	float mean = (float)sum / (float)count / (1 << FIXED_BITS); // lossy color, which this is for, is fixed point
	if (mean >= 64) return SCALE_ONE;
	int32_t halvings = log2_stops(mean > 1 ? mean : 1) - 6 * HDR_CODES_PER_STOP; // log2(mean / 64), in 1/128 halvings
	long    scale    = SCALE_ONE + lroundf(quarters_per_halving * (float)halvings / HDR_CODES_PER_STOP);
	return scale < 0 ? 0 : (int32_t)scale;
}

static const uint8_t zeros[ENT_PAD] = { 0 }; // the file's end padding

// Little-endian base 128
static void push_size(buffer_t* ref_buffer, size_t size) {
	uint8_t bytes[10], count = 0;
	for (; size >= 0x80; size >>= 7) bytes[count++] = (uint8_t)(size | 0x80);
	bytes[count++] = (uint8_t)size;
	buffer_push(ref_buffer, bytes, count);
}

// A row of chroma less its prediction with scale k. Returns the squared error saved, c^2 - (c - p)^2 = p (2c - p).
static int64_t cfl_row(const int16_t* LFI_RESTRICT chroma, const int16_t* LFI_RESTRICT luma, int16_t* LFI_RESTRICT out_residual, int32_t count, int32_t k) {
	int64_t saved = 0;
	for (int32_t i = 0; i < count; i++) {
		int32_t c = chroma[i], p = cfl_predict(k, luma[i]);
		saved          += (int64_t)p * (2 * c - p);
		out_residual[i] = (int16_t)(c - p);
	}
	return saved;
}

// Predicts a chroma band from luma's coded values of the same shape. Each block takes the scale k / 8 with the
// least squared error, kept where it saves more than a squared step, and writes it to the tile context.
// out_residual gets the band less its prediction, w x h.
static void cfl_residual(ent_encoder_t* ref_encoder, const int16_t* plane, int32_t stride, band_t band, const int16_t* luma, int32_t step, int16_t* out_residual) {
	const int16_t* chroma = plane + (size_t)band.y * stride + band.x;
	for (int32_t top = 0; top < band.h; top += CFL_BLOCK)
		for (int32_t left = 0; left < band.w; left += CFL_BLOCK) {
			int32_t bottom = band.h - top < CFL_BLOCK ? band.h : top + CFL_BLOCK, width = band.w - left < CFL_BLOCK ? band.w - left : CFL_BLOCK;
			// Exact in doubles, below 2^40, and kept scalar since compilers won't reorder them, which saves code
			double sum_cross = 0, sum_energy = 0;
			for (int32_t y = top; y < bottom; y++)
				for (int32_t x = left; x < left + width; x++) {
					int32_t l = luma[y * band.w + x];
					sum_cross  += chroma[(size_t)y * stride + x] * l;
					sum_energy += l * l;
				}
			int64_t energy = (int64_t)sum_energy, scaled = 8 * (int64_t)sum_cross;
			int32_t k      = energy ? (int32_t)((scaled + (scaled < 0 ? -energy : energy) / 2) / energy) : 0;
			k = k < -CFL_RANGE ? -CFL_RANGE : k > CFL_RANGE ? CFL_RANGE : k;
			for (;;) {
				int64_t saved = 0;
				for (int32_t y = top; y < bottom; y++)
					saved += cfl_row(chroma + (size_t)y * stride + left, luma + y * band.w + left, out_residual + y * band.w + left, width, k);
				if (!k || saved >= (int64_t)step * step) break;
				k = 0;
			}
			ent_symbol(ref_encoder, CONTEXT_TILE, (uint32_t)(CFL_SYMBOL + CFL_RANGE + k), 0, 0);
		}
}

// A band slot's reconstruction offset in 1/64 steps, from where its nonzero values sit within their bins
static int8_t centroid_offset(const double* centroid) {
	if (!centroid[1]) return 0;
	long offset = lround(centroid[0] / centroid[1] * 64);
	return (int8_t)(offset < -128 ? -128 : offset > 127 ? 127 : offset);
}

// Tiles are coded independently. Each wavelet level of every tile goes to that level's segment,
// so a file prefix decodes to a smaller mip.
static uint8_t* encode(source_t source, int32_t w, int32_t h, const lfi_opt_t* opt, size_t* out_size) {
	int32_t layers = opt->layers > 1 ? opt->layers : 1;
	*out_size = 0;
	if (w < 1 || h < 1 || w > MAX_SIZE || h > MAX_SIZE || layers > MAX_SIZE || opt->channels < 0 || opt->channels > 4 ||
	    (opt->cubemap && (w != h || layers % 6)) || (opt->tile_log2 && (opt->tile_log2 < 5 || opt->tile_log2 > 10)))
		return NULL;

	size_t pixels = (size_t)w * h * layers;
	bool   hdr    = source.opt_rgbaf != NULL;
	bool   alpha  = false;
	bool   binary = true;
	for (size_t i = 0; !hdr && i < pixels; i++) {
		alpha  |= source.opt_rgba8[i*4 + 3] != 255;
		binary &= source.opt_rgba8[i*4 + 3] == 0 || source.opt_rgba8[i*4 + 3] == 255;
	}
	int32_t  tile_log2   = opt->tile_log2 ? opt->tile_log2 : TILE_LOG2;
	int32_t  tile        = 1 << tile_log2;
	int32_t  plane_count = hdr ? (opt->channels == 1 ? 1 : 3) : opt->channels ? opt->channels : alpha ? 4 : 3;
	bool     data        = opt->data && !hdr && plane_count >= 3;
	bool     color       = is_color(plane_count, data);
	bool     cutout      = !data && (opt->alpha == lfi_alpha_cutout || (opt->alpha == lfi_alpha_auto && binary));
	int32_t  levels      = level_count(w < tile ? w : tile, h < tile ? h : tile);
	size_t   tile_area   = (size_t)tile * tile;
	int16_t* planes      = (int16_t*)malloc(tile_area * (plane_count + 1) * sizeof(int16_t));
	int16_t* scratch     = planes + tile_area * plane_count;

	uint8_t step_codes[4 * MAX_BANDS];
	bool    fixed = !hdr && opt->step > 1;
	step_table(opt->step, plane_count, color, levels, cutout, fixed, step_codes);

	// Lossy bands are quantized for rate, from the symbol costs and offsets of the tiles so far. The first tile
	// gets an extra pass by the isolated rule to start them. Lossless codes every value as it is.
	bool          rate_aware = LFI_ENCODER_QUALITY && opt->step > 1, counted = false;
	double        centroid[4 * MAX_BANDS][2] = {{0}};
	double        estimate[4 * MAX_BANDS][2] = {{0}}; // the extra pass's centroids
	uint32_t*     counts = (uint32_t*)calloc(ENT_CONTEXTS * ENT_SYMBOLS, sizeof(uint32_t));
	uint16_t*     costs  = (uint16_t*)malloc(ENT_CONTEXTS * ENT_SYMBOLS * sizeof(uint16_t));
	int16_t*      luma_values = (int16_t*)malloc(tile_area / 4 * sizeof(int16_t)); // luma's coded bands from CFL_MIN_LEVEL up
	int16_t*      residual    = (int16_t*)malloc(tile_area / 16 * sizeof(int16_t)); // a predicted chroma band, at most a sixteenth tile
	ent_encoder_t segments[MAX_LEVELS + 1];
	memset(segments, 0, sizeof(segments));
	for (int32_t layer = 0; layer < layers; layer++)
	for (int32_t tile_y = 0; tile_y < h; tile_y += tile) {
		for (int32_t tile_x = 0; tile_x < w; tile_x += tile) {
			int32_t  tile_w       = w - tile_x < tile ? w - tile_x : tile;
			int32_t  tile_h       = h - tile_y < tile ? h - tile_y : tile;
			size_t   plane_size   = (size_t)tile_w * tile_h;
			source_t layer_source = source;
			if (hdr) layer_source.opt_rgbaf += (size_t)layer * w * h * 4;
			else     layer_source.opt_rgba8 += (size_t)layer * w * h * 4;
			load_tile(layer_source, w, tile_x, tile_y, tile_w, tile_h, plane_count, planes);
			int16_t* alpha_plane = planes + 3 * plane_size;
			for (size_t i = 0; plane_count == 4 && cutout && opt->alpha == lfi_alpha_cutout && i < plane_size; i++) alpha_plane[i] = alpha_plane[i] >= 128 ? 255 : 0;
			for (size_t i = 0; fixed && i < plane_size * plane_count; i++) planes[i] = (int16_t)(planes[i] * (1 << FIXED_BITS));
			if (color) color_forward(planes, planes + plane_size, planes + 2 * plane_size, plane_size);
			if (color && plane_count == 4 && !opt->premultiplied && opt->step > 1) bleed_fill(planes, plane_size, tile_w, tile_h);

			bool    darkened     = color && opt->step > 1 && !hdr;
			int32_t luma_scale   = darkened ? dark_scale(planes, plane_size, opt->dark / 2) : SCALE_ONE;
			int32_t chroma_scale = darkened ? dark_scale(planes, plane_size, opt->dark)     : SCALE_ONE;
			ent_symbol(&segments[levels], CONTEXT_TILE, (uint32_t)luma_scale,   0, 0);
			ent_symbol(&segments[levels], CONTEXT_TILE, (uint32_t)chroma_scale, 0, 0);

			for (int32_t p = 0; p < plane_count; p++)
				for (int32_t level = 0; level < levels; level++) {
					band_t region = band_rect(tile_w, tile_h, level, 0);
					forward_level(planes + p * plane_size, tile_w, region.w, region.h, scratch);
				}

			size_t starts[MAX_LEVELS + 1];
			for (int32_t k = 0; k <= levels; k++) starts[k] = segments[k].count;
			for (int32_t pass = rate_aware && !counted ? 0 : 1; pass < 2; pass++) {
				const uint16_t* pass_costs = rate_aware && pass ? costs : NULL;
				double        (*sums)[2]   = pass ? centroid : estimate;
				for (int32_t p = 0; p < plane_count; p++) {
					int32_t scale   = is_chroma(color, p) ? chroma_scale : luma_scale;
					size_t  luma_at = 0;
					for (int32_t band_index = 0; band_index < 1 + 3 * levels; band_index++) {
						band_t         band      = band_rect(tile_w, tile_h, levels, band_index);
						int32_t        slot      = p * MAX_BANDS + band.slot;
						int32_t        step      = scale_step(step_value(step_codes[slot]), scale);
						ent_encoder_t* segment   = &segments[band.level ? band.level - 1 : levels];
						bool           predicted = color && band.level >= CFL_MIN_LEVEL;
						int16_t*       luma        = luma_values + luma_at;
						const int16_t* band_plane  = planes + p * plane_size;
						int32_t        band_stride = tile_w;
						band_t         band_at     = band;
						luma_at += predicted ? (size_t)band.w * band.h : 0;
						if (predicted && is_chroma(color, p)) {
							cfl_residual(segment, band_plane, band_stride, band, luma, step, residual);
							band_plane  = residual;
							band_stride = band.w;
							band_at.x   = band_at.y = 0;
						}
						encode_band(segment, band_plane, band_stride, band_at, band_context(color, p, band.level), step, pass_costs,
						            centroid_offset(centroid[slot][1] ? centroid[slot] : estimate[slot]), scratch, sums[slot], predicted && p == 0 ? luma : NULL);
					}
				}
				if (!rate_aware) continue;
				for (int32_t k = 0; k <= levels; k++) for (size_t i = starts[k]; i < segments[k].count; i++) counts[segments[k].symbols[i]]++;
				symbol_costs(counts, costs);
				counted = true;
				for (int32_t k = 0; !pass && k <= levels; k++) segments[k].count = starts[k];
			}
		}
	}
	free(planes);
	free(counts);
	free(costs);
	free(luma_values);
	free(residual);

	// Segments coarsest first, so prefixes decode
	buffer_t      tables, streams[2 * (MAX_LEVELS + 1)];
	ent_encoder_t ordered[MAX_LEVELS + 1];
	for (int32_t s = 0; s <= levels; s++) ordered[s] = segments[levels - s];
	ent_write(ordered, levels + 1, &tables, streams);
	for (int32_t s = 0; s <= levels; s++) ent_free(&ordered[s]);

	uint32_t flags = (opt->linear ? FLAG_LINEAR : 0) | (plane_count == 4 && cutout && opt->step > 1 ? FLAG_CUTOUT : 0) |
	                 (hdr ? FLAG_HDR : 0) | (color && plane_count == 4 && opt->premultiplied ? FLAG_PREMULTIPLIED : 0) | (opt->cubemap ? FLAG_CUBEMAP : 0) |
	                 (fixed ? FLAG_FIXED : 0) | (data ? FLAG_DATA : 0);
	uint8_t  fixed_header[HEADER_FIXED - sizeof(lfi_signature)] = { LFI_VERSION, (uint8_t)plane_count, (uint8_t)tile_log2, (uint8_t)flags,
		(uint8_t)w, (uint8_t)(w >> 8), (uint8_t)h, (uint8_t)(h >> 8), (uint8_t)layers, (uint8_t)(layers >> 8) };
	buffer_t out;
	memset(&out, 0, sizeof(out));
	buffer_push(&out, lfi_signature, sizeof(lfi_signature));
	buffer_push(&out, fixed_header,  sizeof(fixed_header));
	push_size(&out, tables.size);
	for (int32_t i = 0; i < 2 * (levels + 1); i++) push_size(&out, streams[i].size);
	if (hdr) buffer_u32(&out, (uint32_t)source.log_min);
	for (int32_t p = 0; p < plane_count; p++) buffer_push(&out, step_codes + p * MAX_BANDS, 1 + 3 * (size_t)levels);
	for (int32_t p = 0; p < plane_count; p++)
		for (int32_t slot = p * MAX_BANDS + 1; slot <= p * MAX_BANDS + 3 * levels; slot++) {
			int8_t offset = centroid_offset(centroid[slot]);
			buffer_push(&out, &offset, 1);
		}
	buffer_push(&out, tables.data, tables.size);
	free(tables.data);
	for (int32_t i = 0; i < 2 * (levels + 1); i++) {
		buffer_push(&out, streams[i].data, streams[i].size);
		free(streams[i].data);
	}
	buffer_u32 (&out, crc32(out.data, out.size));
	buffer_push(&out, zeros, ENT_PAD - 4);

	*out_size = out.size;
	return out.data;
}

uint8_t* lfi_encode(const uint8_t* rgba, int32_t w, int32_t h, const lfi_opt_t* opt, size_t* out_size) {
	source_t source = { rgba, NULL, 0 };
	return encode(source, w, h, opt, out_size);
}

// Codes log2 brightness at 1/128 stop over the 32 stops below the brightest value, so step is in 1/128 stops.
// Channels are 1 or 3.
uint8_t* lfi_encode_hdr(const float* rgba, int32_t w, int32_t h, const lfi_opt_t* opt, size_t* out_size) {
	int32_t channels  = opt->channels == 1 ? 1 : 3;
	float   brightest = 0;
	for (size_t i = 0; i < (size_t)w * h * (opt->layers > 1 ? opt->layers : 1); i++)
		for (int32_t c = 0; c < channels; c++) brightest = rgba[i*4 + c] > brightest ? rgba[i*4 + c] : brightest;
	int32_t  top    = brightest > 0 ? log2_stops(brightest < 65504 ? brightest : 65504) : 0;
	source_t source = { NULL, rgba, top - (HDR_CODES - 2) };
	return encode(source, w, h, opt, out_size);
}

#endif // LFI_ENCODER

#endif // LFI_IMPLEMENTATION

// MIT License
//
// Copyright (c) 2026 Nick Klingensmith
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
