// libFuzzer harness: a small image and options from the input bytes, encoded and decoded. Lossless must
// round trip exactly, and every encode must decode.
// Build with `make fuzz`, run as build/fuzz_roundtrip -max_total_time=300

#include "lfi.h"

#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
	if (size < 9) return 0;
	int32_t   w      = 1 + data[0] % 80, h = 1 + data[1] % 80, layers = 1 + data[2] % 3;
	bool      hdr    = data[3] & 1, lossless = data[3] & 2;
	lfi_opt_t opt    = {
		.step          = lossless ? 0 : 1 + data[4] % 64,
		.channels      = data[5] % 5,
		.alpha         = lossless ? lfi_alpha_auto : (lfi_alpha_e)(data[6] % 3), // forced cutout snaps alpha even when lossless
		.premultiplied = !lossless && data[6] & 4,
		.dark          = data[6] & 8 ? 4 : 0,
		.tile_log2     = data[7] & 1 ? 5 : 0,
		.data          = data[7] & 2,
		.layers        = layers,
	};
	size_t   n   = (size_t)w * h * layers * 4;
	uint8_t* rgb = malloc(n);
	float*   f   = malloc(n * sizeof(float));
	for (size_t i = 0; i < n; i++) {
		rgb[i] = data[8 + i % (size - 8)];
		f[i]   = rgb[i] * rgb[i] / 64.0f;
	}

	size_t   bytes = 0;
	uint8_t* file  = hdr ? lfi_encode_hdr(f, w, h, &opt, &bytes) : lfi_encode(rgb, w, h, &opt, &bytes);
	lfi_info_t info;
	if (!file || lfi_info(file, bytes, &info) != lfi_ok || lfi_verify(file, bytes) != lfi_ok) abort();
	void*    scratch = malloc(lfi_scratch_size(file, bytes));
	uint8_t* out     = malloc(n * 2);
	for (int32_t mip = 0; mip < info.mips; mip++)
		if (lfi_decode(file, lfi_mip_bytes(file, bytes, mip), mip, lfi_format_native, out, scratch) != lfi_ok) abort();
	if (lfi_decode(file, bytes, 0, lfi_format_native, out, scratch) != lfi_ok) abort();

	// Lossless 8-bit decodes to the source channels, with alpha 255 for 3 channels
	for (size_t i = 0; lossless && !hdr && i < (size_t)w * h * layers; i++)
		for (int32_t c = 0; c < info.pixel_size; c++)
			if (out[i * info.pixel_size + c] != (info.channels == 3 && c == 3 ? 255 : rgb[i * 4 + c])) abort();

	free(scratch);
	free(out);
	free(file);
	free(rgb);
	free(f);
	return 0;
}
