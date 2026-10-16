#pragma once

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
