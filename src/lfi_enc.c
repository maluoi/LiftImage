#include "lfi_internal.h"

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
