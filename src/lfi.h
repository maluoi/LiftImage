#pragma once

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
