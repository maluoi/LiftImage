#pragma once

// Internal to the Lift Image encoder and decoder

#include "lfi.h"
#include "ent.h"
#include "color.h"

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
