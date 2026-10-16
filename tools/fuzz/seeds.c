// Writes small valid files covering each mode, as a starting corpus for fuzz_decode.
// usage: seeds <out_dir>

#include "lfi.h"

#include <stdio.h>
#include <stdlib.h>

static void write_seed(const char* dir, const char* name, const uint8_t* data, size_t size) {
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s.lfi", dir, name);
	FILE* f = fopen(path, "wb");
	fwrite(data, 1, size, f);
	fclose(f);
}

int main(int argc, char** argv) {
	if (argc < 2) return 1;
	enum { w = 72, h = 40, layers = 6 };
	uint8_t* rgba = malloc((size_t)w * h * layers * 4);
	float*   f    = malloc((size_t)w * h * layers * 4 * sizeof(float));
	for (int32_t i = 0; i < w * h * layers; i++) {
		int32_t x = i % w, y = i / w % h;
		uint8_t v[4] = { (uint8_t)(x * 3 + y), (uint8_t)(y * 5), (uint8_t)(x ^ y), (uint8_t)(x > 30 ? 255 : x * 8) };
		for (int32_t c = 0; c < 4; c++) { rgba[i * 4 + c] = v[c]; f[i * 4 + c] = v[c] * v[c] / 32.0f; }
	}
	struct { const char* name; lfi_opt_t opt; bool hdr; } seeds[] = {
		{ .name = "rgb16",     .opt = { .step = 16, .channels = 3 } },
		{ .name = "rgba8",     .opt = { .step = 8 } },
		{ .name = "lossless",  .opt = { .step = 0 } },
		{ .name = "gray32",    .opt = { .step = 32, .channels = 1 } },
		{ .name = "normal8",   .opt = { .step = 8,  .channels = 2 } },
		{ .name = "cutout16",  .opt = { .step = 16, .channels = 4, .alpha = lfi_alpha_cutout } },
		{ .name = "premul16",  .opt = { .step = 16, .channels = 4, .premultiplied = true, .dark = 4 } },
		{ .name = "tiles32",   .opt = { .step = 16, .tile_log2 = 5 } },
		{ .name = "array8",    .opt = { .step = 8,  .layers = 3 } },
		{ .name = "hdr16",     .opt = { .step = 16 }, .hdr = true },
		{ .name = "hdrgray16", .opt = { .step = 16, .channels = 1 }, .hdr = true },
	};
	for (size_t s = 0; s < sizeof(seeds) / sizeof(seeds[0]); s++) {
		size_t   size;
		uint8_t* data = seeds[s].hdr ? lfi_encode_hdr(f, w, h, &seeds[s].opt, &size) : lfi_encode(rgba, w, h, &seeds[s].opt, &size);
		write_seed(argv[1], seeds[s].name, data, size);
		free(data);
	}
	size_t   size;
	uint8_t* cube = lfi_encode(rgba, h, h, &(lfi_opt_t){ .step = 16, .layers = 6, .cubemap = true }, &size);
	write_seed(argv[1], "cube16", cube, size);
	free(cube);
	return 0;
}
