#pragma once

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
