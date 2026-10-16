// lfi: command line tool for Lift Image files, built on the single-header library
//   lfi encode [options] <out.lfi> <image>...   several images become array layers, .hdr input becomes HDR
//   lfi decode [--mip N] <in.lfi> <out.png|out.hdr>
//   lfi info <in.lfi>
//   lfi verify <in.lfi>
//   lfi hash [--mip N] [--format native|rg11b10f|rgb9e5] <in.lfi>
//   lfi thumbnail [--size N] <in.lfi> <out.png>

#define LFI_IMPLEMENTATION
#define LFI_ENCODER
#include "lift_image.h"
#include "thumbnail.h"

#include "stb_image.h"
#include "stb_image_write.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* usage =
	"usage:\n"
	"  lfi encode [options] <out.lfi> <image>...   several images become array layers, .hdr input becomes HDR\n"
	"    --step N         quantizer step, 0 for lossless, default 16. HDR steps are in 1/128 stops\n"
	"    --channels N     1 gray, 2 normal XY, 3 color, 4 color + alpha, default by alpha\n"
	"    --alpha MODE     auto, blend or cutout\n"
	"    --premultiplied  RGB is premultiplied by alpha\n"
	"    --linear         mark the pixels as linear rather than sRGB\n"
	"    --dark N         finer steps in dark tiles, try 4\n"
	"    --tile N         tile size as a power of two, 5 to 10, default 9\n"
	"    --cubemap        layers are cube faces +X -X +Y -Y +Z -Z\n"
	"    --data           3 or 4 channels of independent data, like packed material maps\n"
	"  lfi decode [--mip N] <in.lfi> <out.png|out.hdr>   layers go to out_0.png, out_1.png, ...\n"
	"  lfi info <in.lfi>\n"
	"  lfi verify <in.lfi>\n"
	"  lfi hash [--mip N] [--format native|rg11b10f|rgb9e5] <in.lfi>   CRC-32 of the decoded pixels\n"
	"  lfi thumbnail [--size N] <in.lfi> <out.png>   fits within N, default 256, reading only the mip it needs\n";

static const char* result_names[] = { "ok", "truncated", "not a Lift Image, or an unknown version or feature", "corrupt", "checksum mismatch", "bad argument" };

static uint8_t* read_file(const char* path, size_t* out_size) {
	FILE* f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	*out_size = (size_t)ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t* data = malloc(*out_size ? *out_size : 1);
	*out_size = fread(data, 1, *out_size, f);
	fclose(f);
	return data;
}

static bool is_hdr(const char* path) {
	const char* ext = strrchr(path, '.');
	return ext && !strcmp(ext, ".hdr");
}

static int32_t cmd_encode(int32_t argc, char** argv) {
	lfi_opt_t opt = { .step = 16 };
	int32_t   a   = 0;
	for (; a < argc && argv[a][0] == '-' && argv[a][1] == '-'; a++) {
		const char* o = argv[a];
		if      (!strcmp(o, "--premultiplied")) opt.premultiplied = true;
		else if (!strcmp(o, "--linear"))        opt.linear        = true;
		else if (!strcmp(o, "--cubemap"))       opt.cubemap       = true;
		else if (!strcmp(o, "--data"))          opt.data          = true;
		else if (a + 1 >= argc)                 { fprintf(stderr, "%s needs a value\n", o); return 1; }
		else if (!strcmp(o, "--step"))          opt.step      = (float)atof(argv[++a]);
		else if (!strcmp(o, "--channels"))      opt.channels  = atoi(argv[++a]);
		else if (!strcmp(o, "--dark"))          opt.dark      = (float)atof(argv[++a]);
		else if (!strcmp(o, "--tile"))          opt.tile_log2 = atoi(argv[++a]);
		else if (!strcmp(o, "--alpha")) {
			const char* m = argv[++a];
			opt.alpha = !strcmp(m, "cutout") ? lfi_alpha_cutout : !strcmp(m, "blend") ? lfi_alpha_blend : lfi_alpha_auto;
		}
		else { fprintf(stderr, "unknown option %s\n", o); return 1; }
	}
	if (argc - a < 2) { fputs(usage, stderr); return 1; }
	const char* out_path = argv[a++];
	int32_t     layers   = argc - a, w = 0, h = 0;
	bool        hdr      = is_hdr(argv[a]);
	opt.layers = layers;

	// Layers follow one another in one buffer, so every input must match the first one's size
	uint8_t* pixels = NULL;
	for (int32_t i = 0; i < layers; i++) {
		int32_t lw, lh, comp;
		void*   img = hdr ? (void*)stbi_loadf(argv[a + i], &lw, &lh, &comp, 4) : (void*)stbi_load(argv[a + i], &lw, &lh, &comp, 4);
		if (!img)                       { fprintf(stderr, "can't load %s: %s\n", argv[a + i], stbi_failure_reason()); return 1; }
		if (i && (lw != w || lh != h)) { fprintf(stderr, "%s is %dx%d, but layers must all be %dx%d\n", argv[a + i], lw, lh, w, h); return 1; }
		w = lw, h = lh;
		size_t bytes = (size_t)w * h * 4 * (hdr ? sizeof(float) : 1);
		pixels = realloc(pixels, bytes * layers);
		memcpy(pixels + bytes * i, img, bytes);
		stbi_image_free(img);
	}

	size_t   size = 0;
	uint8_t* data = hdr ? lfi_encode_hdr((const float*)pixels, w, h, &opt, &size) : lfi_encode(pixels, w, h, &opt, &size);
	if (!data) { fprintf(stderr, "can't encode with these options\n"); return 1; }
	FILE* f = fopen(out_path, "wb");
	if (!f || fwrite(data, 1, size, f) != size) { fprintf(stderr, "can't write %s\n", out_path); return 1; }
	fclose(f);
	printf("%s: %dx%d x%d, %zu bytes, %.3f bpp\n", out_path, w, h, layers, size, size * 8.0 / ((double)w * h * layers));
	free(data);
	free(pixels);
	return 0;
}

// Opens and checks a file, reporting why it can't be used
static uint8_t* open_lfi(const char* path, size_t* out_size, lfi_info_t* out_info) {
	uint8_t* data = read_file(path, out_size);
	if (!data) { fprintf(stderr, "can't read %s\n", path); return NULL; }
	lfi_result_e r = lfi_info(data, *out_size, out_info);
	if (r != lfi_ok) { fprintf(stderr, "%s: %s\n", path, result_names[r]); free(data); return NULL; }
	return data;
}

static int32_t parse_mip(int32_t* ref_argc, char*** ref_argv, int32_t* out_mip, lfi_format_e* out_format) {
	for (; *ref_argc > 1 && (*ref_argv)[0][0] == '-'; *ref_argc -= 2, *ref_argv += 2) {
		const char* o = (*ref_argv)[0], * v = (*ref_argv)[1];
		if      (!strcmp(o, "--mip"))                out_mip[0] = atoi(v);
		else if (!strcmp(o, "--format") && out_format) *out_format = !strcmp(v, "rg11b10f") ? lfi_format_rg11b10f : !strcmp(v, "rgb9e5") ? lfi_format_rgb9e5 : lfi_format_native;
		else { fprintf(stderr, "unknown option %s\n", o); return 1; }
	}
	return 0;
}

// Decodes to newly allocated pixels, with each layer's bytes in out_layer
static uint8_t* decode(const uint8_t* data, size_t size, lfi_info_t info, int32_t mip, lfi_format_e format, size_t* out_layer) {
	int32_t mw = (info.w + (1 << mip) - 1) >> mip, mh = (info.h + (1 << mip) - 1) >> mip;
	*out_layer = (size_t)mw * mh * (format == lfi_format_native ? info.pixel_size : 4);
	uint8_t* pixels  = malloc(*out_layer * info.layers);
	void*    scratch = malloc(lfi_scratch_size(data, size));
	lfi_result_e r   = lfi_decode(data, size, mip, format, pixels, scratch);
	free(scratch);
	if (r != lfi_ok) { fprintf(stderr, "decode failed: %s\n", result_names[r]); free(pixels); return NULL; }
	return pixels;
}

static int32_t cmd_decode(int32_t argc, char** argv) {
	int32_t mip = 0;
	if (parse_mip(&argc, &argv, &mip, NULL) || argc != 2) { fputs(usage, stderr); return 1; }
	size_t     size, layer;
	lfi_info_t info;
	uint8_t*   data = open_lfi(argv[0], &size, &info);
	uint8_t*   px   = data ? decode(data, size, info, mip, lfi_format_native, &layer) : NULL;
	if (!px) return 1;

	int32_t mw = (info.w + (1 << mip) - 1) >> mip, mh = (info.h + (1 << mip) - 1) >> mip;
	size_t  n  = (size_t)mw * mh;
	for (int32_t l = 0; l < info.layers; l++) {
		char path[4096];
		const char* ext = strrchr(argv[1], '.');
		if (info.layers == 1) snprintf(path, sizeof(path), "%s", argv[1]);
		else                  snprintf(path, sizeof(path), "%.*s_%d%s", (int)(ext ? ext - argv[1] : (long)strlen(argv[1])), argv[1], l, ext ? ext : "");
		const uint8_t* src = px + layer * l;
		int32_t ok;
		if (info.hdr) {
			// Half floats to the float RGB or gray that .hdr files hold
			int32_t comps = info.channels == 1 ? 1 : 3;
			float*  f     = malloc(n * comps * sizeof(float));
			for (size_t i = 0; i < n; i++)
				for (int32_t c = 0; c < comps; c++) f[i * comps + c] = half_to_float(((const uint16_t*)src)[i * (comps == 1 ? 1 : 4) + c]);
			ok = stbi_write_hdr(path, mw, mh, comps, f);
			free(f);
		} else if (info.channels == 2) {
			// Normal XY, written as RGB with blue 0
			uint8_t* rgb = calloc(n, 3);
			for (size_t i = 0; i < n; i++) { rgb[i * 3] = src[i * 2]; rgb[i * 3 + 1] = src[i * 2 + 1]; }
			ok = stbi_write_png(path, mw, mh, 3, rgb, mw * 3);
			free(rgb);
		} else {
			ok = stbi_write_png(path, mw, mh, info.pixel_size, src, mw * info.pixel_size);
		}
		if (!ok) { fprintf(stderr, "can't write %s\n", path); return 1; }
		printf("%s: %dx%d\n", path, mw, mh);
	}
	free(px);
	free(data);
	return 0;
}

static int32_t cmd_info(int32_t argc, char** argv) {
	if (argc != 1) { fputs(usage, stderr); return 1; }
	size_t     size;
	lfi_info_t info;
	uint8_t*   data = open_lfi(argv[0], &size, &info);
	if (!data) return 1;
	header_t header;
	parse_header(data, size, &header);
	printf("%s: Lift Image v%u, %zu bytes\n", argv[0], data[8], size);
	printf("  %dx%d, %d layer%s%s, %d channel%s%s%s%s%s\n", info.w, info.h, info.layers, info.layers > 1 ? "s" : "", info.cubemap ? " (cubemap)" : "",
		info.channels, info.channels > 1 ? "s" : "", info.data ? " of data" : "", info.hdr ? ", HDR" : "", info.linear ? ", linear" : "", info.premultiplied ? ", premultiplied" : "");
	printf("  %d mips, tile %d, %.3f bpp\n", info.mips, header.tile, size * 8.0 / ((double)info.w * info.h * info.layers));
	printf("  header and parameters %6zu bytes\n", header.tables.offset);
	printf("  tables                %6zu bytes\n", header.tables.size);
	for (int32_t m = info.mips - 1; m >= 0; m--)
		printf("  mip %d %6zu bytes, decodes from the first %zu\n", m, header.streams[m][0].size + header.streams[m][1].size, lfi_mip_bytes(data, size, m));
	lfi_result_e r = lfi_verify(data, size);
	printf("  checksum %s\n", result_names[r]);
	free(data);
	return r != lfi_ok;
}

static int32_t cmd_verify(int32_t argc, char** argv) {
	if (argc != 1) { fputs(usage, stderr); return 1; }
	size_t   size;
	uint8_t* data = read_file(argv[0], &size);
	if (!data) { fprintf(stderr, "can't read %s\n", argv[0]); return 1; }
	lfi_result_e r = lfi_verify(data, size);
	printf("%s: %s\n", argv[0], result_names[r]);
	free(data);
	return r != lfi_ok;
}

static int32_t cmd_hash(int32_t argc, char** argv) {
	int32_t      mip    = 0;
	lfi_format_e format = lfi_format_native;
	if (parse_mip(&argc, &argv, &mip, &format) || argc != 1) { fputs(usage, stderr); return 1; }
	size_t     size, layer;
	lfi_info_t info;
	uint8_t*   data = open_lfi(argv[0], &size, &info);
	uint8_t*   px   = data ? decode(data, size, info, mip, format, &layer) : NULL;
	if (!px) return 1;
	printf("%08x\n", crc32(px, layer * info.layers));
	free(px);
	free(data);
	return 0;
}

// Exec=lfi thumbnail --size %s %i %o in a freedesktop .thumbnailer
static int32_t cmd_thumbnail(int32_t argc, char** argv) {
	int32_t size = 256;
	if (argc == 4 && !strcmp(argv[0], "--size")) { size = atoi(argv[1]); argc -= 2; argv += 2; }
	if (argc != 2 || size < 1) { fputs(usage, stderr); return 1; }
	int32_t  w, h, fit_w, fit_h;
	uint8_t* rgba = thumbnail_load(argv[0], size, &w, &h);
	if (!rgba) { fprintf(stderr, "can't make a thumbnail of %s\n", argv[0]); return 1; }
	uint8_t* fitted = thumbnail_fit(rgba, w, h, size, &fit_w, &fit_h);
	int32_t  ok     = stbi_write_png(argv[1], fit_w, fit_h, 4, fitted, fit_w * 4);
	free(fitted);
	free(rgba);
	if (!ok) { fprintf(stderr, "can't write %s\n", argv[1]); return 1; }
	return 0;
}

int main(int argc, char** argv) {
	if (argc < 2) { fputs(usage, stderr); return 1; }
	const char* cmd = argv[1];
	if (!strcmp(cmd, "encode"))    return cmd_encode   (argc - 2, argv + 2);
	if (!strcmp(cmd, "decode"))    return cmd_decode   (argc - 2, argv + 2);
	if (!strcmp(cmd, "info"))      return cmd_info     (argc - 2, argv + 2);
	if (!strcmp(cmd, "verify"))    return cmd_verify   (argc - 2, argv + 2);
	if (!strcmp(cmd, "hash"))      return cmd_hash     (argc - 2, argv + 2);
	if (!strcmp(cmd, "thumbnail")) return cmd_thumbnail(argc - 2, argv + 2);
	fputs(usage, stderr);
	return 1;
}
