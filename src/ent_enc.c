#include "ent.h"

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
