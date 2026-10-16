#pragma once

// Previews for OS integration: the first layer of a Lift Image as straight RGBA8, decoded from only the file prefix
// its smallest covering mip needs. Include lift_image.h first. Compiles as C11 and C++11.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static inline float half_to_float(uint16_t h) {
	int32_t e = h >> 10 & 31, m = h & 1023;
	float   v = e ? ldexpf(1 + m / 1024.0f, e - 15) : ldexpf(m / 1024.0f, -14);
	return h >> 15 ? -v : v;
}

// HDR shows as it would at 0 EV, clipped
static inline uint8_t linear_to_srgb8(float v) {
	v = v < 0 ? 0 : v > 1 ? 1 : v;
	v = v <= 0.0031308f ? 12.92f * v : 1.055f * powf(v, 1 / 2.4f) - 0.055f;
	return (uint8_t)(v * 255 + 0.5f);
}

// The smallest mip whose longer side is still at least size, or mip 0 for images smaller than that
static inline int32_t thumbnail_mip(const lfi_info_t* info, int32_t size) {
	int32_t longer = info->w > info->h ? info->w : info->h, mip = 0;
	while (mip + 1 < info->mips && (longer + (2 << mip) - 1) >> (mip + 1) >= size) mip++;
	return mip;
}

static inline void thumbnail_pixel(const uint8_t* pixel, const lfi_info_t* info, uint8_t* out_rgba) {
	if (info->hdr) {
		const uint16_t* half = (const uint16_t*)pixel;
		for (int32_t c = 0; c < 3; c++) out_rgba[c] = linear_to_srgb8(half_to_float(half[info->channels == 1 ? 0 : c]));
		out_rgba[3] = 255;
	} else if (info->channels == 1) {
		out_rgba[0] = out_rgba[1] = out_rgba[2] = pixel[0];
		out_rgba[3] = 255;
	} else if (info->channels == 2) {
		// Normal XY, with Z rebuilt so it looks like the normal map it is
		float x = pixel[0] / 127.5f - 1, y = pixel[1] / 127.5f - 1, zz = 1 - x * x - y * y;
		out_rgba[0] = pixel[0];
		out_rgba[1] = pixel[1];
		out_rgba[2] = (uint8_t)(sqrtf(zz > 0 ? zz : 0) * 127.5f + 128);
		out_rgba[3] = 255;
	} else {
		// Data mode's fourth channel is data rather than coverage
		uint8_t alpha = info->channels == 4 && !info->data ? pixel[3] : 255;
		for (int32_t c = 0; c < 3; c++) {
			int32_t v = info->premultiplied ? (alpha ? (pixel[c] * 255 + alpha / 2) / alpha : 0) : pixel[c];
			out_rgba[c] = (uint8_t)(v > 255 ? 255 : v);
		}
		out_rgba[3] = alpha;
	}
}

// Decodes the first layer of a mip to RGBA8 pixels to free, or NULL. data needs only lfi_mip_bytes of the file
static inline uint8_t* thumbnail_decode(const uint8_t* data, size_t data_size, int32_t mip, int32_t* out_w, int32_t* out_h) {
	lfi_info_t info;
	if (lfi_info(data, data_size, &info) != lfi_ok) return NULL;
	int32_t  w       = (info.w + (1 << mip) - 1) >> mip, h = (info.h + (1 << mip) - 1) >> mip;
	size_t   count   = (size_t)w * h;
	uint8_t* decoded = (uint8_t*)malloc(count * info.pixel_size * info.layers);
	void*    scratch = malloc(lfi_scratch_size(data, data_size));
	bool     ok      = decoded && scratch && lfi_decode(data, data_size, mip, lfi_format_native, decoded, scratch) == lfi_ok;
	uint8_t* rgba    = ok ? (uint8_t*)malloc(count * 4) : NULL;
	for (size_t i = 0; rgba && i < count; i++) thumbnail_pixel(decoded + i * info.pixel_size, &info, rgba + i * 4);
	free(scratch);
	free(decoded);
	*out_w = w;
	*out_h = h;
	return rgba;
}

// Box filters RGBA8 down to fit within size, keeping the aspect, for hosts that don't scale thumbnails themselves.
// Returns new pixels to free
static inline uint8_t* thumbnail_fit(const uint8_t* rgba, int32_t w, int32_t h, int32_t size, int32_t* out_w, int32_t* out_h) {
	int32_t longer = w > h ? w : h;
	int32_t fit_w  = longer <= size ? w : (int32_t)((int64_t)w * size / longer), fit_h = longer <= size ? h : (int32_t)((int64_t)h * size / longer);
	fit_w = fit_w < 1 ? 1 : fit_w;
	fit_h = fit_h < 1 ? 1 : fit_h;
	uint8_t* out = (uint8_t*)malloc((size_t)fit_w * fit_h * 4);
	for (int32_t y = 0; out && y < fit_h; y++) {
		int32_t y0 = (int32_t)((int64_t)y * h / fit_h), y1 = (int32_t)((int64_t)(y + 1) * h / fit_h);
		for (int32_t x = 0; x < fit_w; x++) {
			int32_t x0 = (int32_t)((int64_t)x * w / fit_w), x1 = (int32_t)((int64_t)(x + 1) * w / fit_w), count = (x1 - x0) * (y1 - y0);
			for (int32_t c = 0; c < 4; c++) {
				int32_t sum = count / 2;
				for (int32_t sy = y0; sy < y1; sy++)
					for (int32_t sx = x0; sx < x1; sx++) sum += rgba[((size_t)sy * w + sx) * 4 + c];
				out[((size_t)y * fit_w + x) * 4 + c] = (uint8_t)(sum / count);
			}
		}
	}
	*out_w = fit_w;
	*out_h = fit_h;
	return out;
}

// A thumbnail of a file on disk, reading only the prefix its mip needs
static inline uint8_t* thumbnail_load(const char* path, int32_t size, int32_t* out_w, int32_t* out_h) {
	FILE* file = fopen(path, "rb");
	if (!file) return NULL;
	uint8_t    header[LFI_HEADER_MAX];
	size_t     header_size = fread(header, 1, sizeof(header), file);
	lfi_info_t info;
	int32_t    mip    = lfi_info(header, header_size, &info) == lfi_ok ? thumbnail_mip(&info, size) : -1;
	size_t     prefix = mip >= 0 ? lfi_mip_bytes(header, header_size, mip) : 0;
	uint8_t*   data   = prefix ? (uint8_t*)malloc(prefix) : NULL;
	bool       loaded = data && fseek(file, 0, SEEK_SET) == 0 && fread(data, 1, prefix, file) == prefix;
	fclose(file);
	uint8_t*   rgba   = loaded ? thumbnail_decode(data, prefix, mip, out_w, out_h) : NULL;
	free(data);
	return rgba;
}
