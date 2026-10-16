<p align="center"><img src="docs/images/lift_header.svg" alt="Lift Image" width="100%"></p>

**Smaller than JPEG, faster to decode than JPEG or PNG, and every file is a mip chain you can stream.** One dependency-free header, in C or C++.

![Parrot crop: source, JPEG q75 and q90, and Lift Image at steps 8, 16 and 32](https://raw.githubusercontent.com/maluoi/LiftImage-lab/main/examples/compare/parrot.png)

*At the size of JPEG q75 (top middle), Lift Image at step 8 (bottom left) scores 82 on SSIMULACRA2 instead of 77. At step 16 (bottom middle) it matches q75's 37 dB PSNR in 0.49 bits per pixel instead of 0.85.*

## Why

- **Fast.** On one thread, Lift decodes 2 to 16× faster than JPEG, WebP, AVIF and JPEG XL at the same perceptual quality, at 300 to 700 megapixels a second on a 2 GHz core. Its files are the size of WebP's and mozjpeg's and smaller than libjpeg-turbo's, while jpegli's are 5% smaller, AVIF's 12% and JPEG XL's 16%.
- **Lossless too.** Lossless files are 18% smaller than PNG, and decode 3.2× faster.
- **HDR.** HDR decodes straight to RGBA16F, RG11B10F or RGB9E5, from files about 10× smaller than Radiance `.hdr`.
- **Streaming.** Every file is a mip chain. Any prefix decodes to a smaller mip, so you can show mip 2 while the rest arrives, then continue without redoing work.
- **One asset for every GPU.** The output is plain pixels, ready to upload or to hand to your runtime BC or ASTC encoder.
- **Texture aware.** It handles gray, normal maps, packed data like occlusion, roughness and metalness, cutout, blended and premultiplied alpha, array layers and cubemaps.
- **Small.** The decoder is about 45 KB of machine code (43 KB from clang, 47 KB from gcc, built for AVX2), and the encoder adds 53 to 71 KB, all from about 1,800 lines of C with no dependencies. For scale, on Fedora's x86-64 builds the WebP decoder library is 297 KB, AVIF's dav1d decoder alone is 1.8 MB, and libjxl is 4.1 MB with its encoder.
- **Safe and exact.** The decoder is integer-only and bit-exact on every compiler and platform, and the encoder writes the same bytes for the same input on any of them. Every entry point validates its input and is fuzzed, so untrusted files are safe to open. It builds warning-free as C11, C++11 and with MSVC, and reads files in place, so you can map them straight from disk.

## How it compares

Quality is judged by [SSIMULACRA2](https://github.com/cloudinary/ssimulacra2), a perceptual metric, with PSNR alongside as the traditional measure. Speeds are at the same SSIMULACRA2, on one thread.

| Lift Image versus | File size at the same SSIMULACRA2 | File size at the same PSNR | Decoding | Encoding |
|---|---|---|---|---|
| stb_image_write and stb_image | 14% smaller | 31% smaller | 2.6× faster | 1.2× slower |
| libjpeg-turbo | 15% smaller | 31% smaller | 2.0× faster | 8.7× slower |
| libjpeg-turbo, progressive | 10% smaller | 28% smaller | 5.9× faster | 1.3× slower |
| mozjpeg | 1% smaller | 23% smaller | 3.6× faster | 4.3× faster |
| mozjpeg, tuned for PSNR | 18% smaller | 3% smaller | 4.0× faster | 6.3× faster |
| jpegli | 5% larger | 16% smaller | 5.0× faster | 1.5× slower |
| WebP | 5% smaller | 1% larger | 8.0× faster | 4.7× faster |
| WebP, method 6 | the same | 3% larger | 7.8× faster | 11× faster |
| AVIF | 14% larger | 41% larger | 9.1× faster | 24× faster |
| JPEG XL | 19% larger | 1% larger | 16× faster | 13× faster |

The test images are 33 lossless images from [LiftImage-lab](https://github.com/maluoi/LiftImage-lab), which holds the test corpus and benchmarks: textures, photos and screenshots. Sizes and speeds are averaged over the quality range of Lift's steps 4 to 32, about SSIMULACRA2 44 to 88, wherever both codecs reach. The progressive, mozjpeg, jpegli and AVIF rows take the better of 4:2:0 and 4:4:4 at each quality, baseline libjpeg-turbo uses 4:2:0, and stb_image_write picks its own. JPEGs decode with libjpeg-turbo, except stb's, which stb_image decodes. Baseline libjpeg-turbo and stb only reach the upper 60% of that range. The newer formats use their libraries' defaults: libwebp 1.6, jpegli from libjxl 0.11.2, libavif 1.3 with libaom 3.13 at speed 6 and dav1d 1.5 on one thread, and libjxl 0.11.2 at effort 7. The lab repository reproduces every row.

| Lift Image versus | File size | Decoding |
|---|---|---|
| PNG, both lossless | 18% smaller | 3.2× faster |
| WebP, both lossless | 10% larger | 3.0× faster |
| JPEG XL at effort 7, both lossless | 19% larger | 39× faster |
| Radiance `.hdr`, Lift at step 16 | 10× smaller | 3.2× faster |

The lossless rows use the same 33 images, and the HDR row its 10 environment maps, decoded by stb_image. More side-by-side crops, including normal maps, packed material data, alpha and HDR, are in [the lab's `examples/compare`](https://github.com/maluoi/LiftImage-lab/tree/main/examples/compare).

## Using it

With CMake, fetch this repository and link the `lift_image` library:

```cmake
include(FetchContent)
FetchContent_Declare(lift_image GIT_REPOSITORY https://github.com/maluoi/LiftImage.git GIT_TAG main GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(lift_image)
target_link_libraries(your_app PRIVATE lift_image)
```

Then `#include "lfi.h"`. The library stays optimized in your Debug builds, since the codec's loops are several times slower unoptimized. Set `LFI_ENCODER` to `OFF` before `FetchContent_MakeAvailable` to leave the encoder out.

Or copy [`dist/lift_image.h`](dist/lift_image.h) into your project. In exactly one C or C++ file, define the implementation:

```c
#define LFI_IMPLEMENTATION // the decoder
#define LFI_ENCODER        // and the encoder, if you need it (link with -lm)
#include "lift_image.h"
```

Everywhere else, include `lift_image.h` plainly for the API.

The encoder quantizes for rate by default, which makes files about 2% smaller. Define `LFI_ENCODER_QUALITY 0` next to `LFI_ENCODER` to leave that out, for about 10 KB less code and lossy encodes about 20% faster. Decoding is the same either way.

### Encode

```c
lfi_opt_t opt = { .step = 16 }; // 0 is lossless, larger steps make smaller files: try 8 to 32
size_t    size;
uint8_t*  file = lfi_encode(rgba, w, h, &opt, &size);
// ...write file to disk, then free(file)
```

`rgba` holds 8-bit RGBA pixels. Options cover the channel count (gray, normal XY, color, color and alpha), `data` for channels that aren't color, such as packed material maps, how to treat alpha, linear or sRGB, array layers and cubemaps. See `lfi_opt_t` in the header.

### Decode

```c
lfi_info_t info;
if (lfi_info(file, size, &info) != lfi_ok) return;
uint8_t* pixels  = malloc((size_t)info.w * info.h * info.layers * info.pixel_size);
void*    scratch = malloc(lfi_scratch_size(file, size));
if (lfi_decode(file, size, 0, lfi_format_native, pixels, scratch) != lfi_ok) return;
// ...upload pixels, or hand them to your BC7 or ASTC encoder
```

Pixels come out as R8 for gray, RG8 for normal maps, and RGBA8 for color. Decoding uses no global state and never allocates, though it needs about 64 KB of stack. Keep one scratch buffer per thread, and decode many files in parallel. The third argument picks a mip. Mip `k` is `(w + 2^k - 1) >> k` wide.

### Stream

Mips are stored coarsest first, so a prefix of the file holds the small mips. `lfi_mip_bytes` says how much of the file a mip needs, and `lfi_decode_step` can leave a cache behind to continue from later:

```c
// The first lfi_mip_bytes(file, size, 2) bytes have arrived
int16_t* cache = malloc(lfi_cache_size(file, received, 2));
lfi_decode_step(file, received, NULL, 0, 2, lfi_format_native, cache, small_pixels, scratch);

// Later, once the whole file is here, pick up from the cache
lfi_decode_step(file, size, cache, 2, 0, lfi_format_native, NULL, pixels, scratch);
```

For a 768x512 photo at step 16, mip 2 needs the first 13 KB of a 23 KB file. The finished image is identical to decoding the whole file at once. To learn a file's size from its start, `lfi_info` needs at most the first `LFI_HEADER_MAX` bytes, 93, and reports `file_size`.

### HDR

```c
lfi_opt_t opt = { .step = 16 }; // in 1/128 stops
uint8_t*  file = lfi_encode_hdr(rgba_float, w, h, &opt, &size);
lfi_decode(file, size, 0, lfi_format_rg11b10f, packed_pixels, scratch); // or lfi_format_rgb9e5, or native RGBA16F
```

## Command line tool

`make` builds `build/lfi`, as does building this repository with CMake:

```
lfi encode [options] <out.lfi> <image>...   several images become array layers, .hdr input becomes HDR
lfi decode [--mip N] <in.lfi> <out.png|out.hdr>
lfi info <in.lfi>
lfi verify <in.lfi>
lfi thumbnail [--size N] <in.lfi> <out.png>
```

`lfi encode --step 16 parrot.lfi parrot.png` makes a file like the one above. Run `lfi` without arguments for every option.

## Plugins

`plugins/` teaches other software the format, so far file managers and image viewers, with thumbnails that decode only the small mip they show:

- **Windows:** a WIC codec, for Explorer thumbnails and previews, Photos and Paint.
- **macOS:** Quick Look thumbnails and previews in Finder.
- **Linux:** a MIME type and thumbnailer for GNOME and most file managers, which `make install` adds, plus plugins for Dolphin and for Qt apps such as Gwenview.

See [plugins/README.md](plugins/README.md).

## How it works

Color goes through a reversible opponent transform with axes like JPEG XL's XYB (luma, red against green, blue against green), while packed data skips color entirely. Then comes a 9/3 integer lifting wavelet in 16-bit lanes over 512x512 tiles, carrying 4 fractional bits when lossy. Chroma detail is predicted from luma's. Coefficients get deadzone quantization with rate-aware decisions and per-band centroid offsets, then tANS entropy coding over two interleaved streams that carry each symbol's raw bits inline. Each wavelet level is stored in its own segment, which is what makes every file prefix a valid smaller mip.

The format is fully specified in [`docs/format.md`](docs/format.md), with test vectors in [`docs/vectors`](docs/vectors).

## Building and testing

```
make -j8                  # build/lfi
make test                 # decodes the test vectors and checks their hashes
make fuzz && build/fuzz_decode -max_total_time=300 build/fuzz_corpus
```

With CMake, `ctest` runs the same vector test. `make` builds for your compiler's default CPU target; add `ARCH=-march=native` for this machine's full instruction set. `src/` is the source of truth, and `dist/lift_image.h` is generated from it by `tools/amalgamate.py`.

Benchmarks, comparisons against other codecs, and the corpus behind every number here live in [LiftImage-lab](https://github.com/maluoi/LiftImage-lab), which clones this repository inside itself.

## License

MIT, see [`LICENSE`](LICENSE). The single header carries the same text, so copying just that file is enough.
