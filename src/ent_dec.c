#include "ent.h"

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
