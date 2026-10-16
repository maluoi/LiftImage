// libFuzzer harness: any bytes through every decoder entry point, which must stay in bounds and terminate.
// Build with `make fuzz`, run as build/fuzz_decode -max_total_time=300 tools/fuzz/seeds

#include "lfi.h"

#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
	uint8_t* copy = malloc(size ? size : 1);
	memcpy(copy, data, size);

	lfi_info_t info;
	if (lfi_info(copy, size, &info) != lfi_ok || (uint64_t)info.w * info.h * info.layers > (1 << 22)) { free(copy); return 0; }
	lfi_verify(copy, size);

	void*    scratch = malloc(lfi_scratch_size(copy, size));
	uint8_t* pixels  = malloc((size_t)info.w * info.h * info.layers * 8);
	int16_t* cache   = malloc(lfi_cache_size(copy, size, 1) + 1);
	for (int32_t format = 0; format < 3; format++) lfi_decode(copy, size, 0, (lfi_format_e)format, pixels, scratch);
	size_t prefix = lfi_mip_bytes(copy, size, info.mips - 1);
	lfi_decode(copy, prefix < size ? prefix : size, info.mips - 1, lfi_format_native, pixels, scratch);
	if (info.mips > 1 && lfi_decode_step(copy, size, NULL, 0, 1, lfi_format_native, cache, pixels, scratch) == lfi_ok)
		lfi_decode_step(copy, size, cache, 1, 0, lfi_format_native, NULL, pixels, scratch);
	free(scratch);
	free(pixels);
	free(cache);
	free(copy);
	return 0;
}
