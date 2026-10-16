#include "lfi_internal.h"

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
