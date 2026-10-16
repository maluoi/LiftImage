# Lift Image format, version 1

Lift Image (`.lfi`) stores 8-bit and HDR images, array textures and cubemaps for real-time engines. A decoder turns a file, or any prefix of it, into plain pixels at a chosen mip level, using integer arithmetic only. This document specifies the bitstream completely, so an independent decoder can produce output identical to the reference one bit for bit.

The pipeline, from file to pixels:

1. **Container.** A header gives the sizes of the entropy tables and of one data segment per mip, which follow the quantizer parameters.
2. **Entropy decoding.** tANS with two interleaved states yields symbols, each followed by raw bits, which become wavelet coefficients.
3. **Dequantization.** Coefficients scale by a per-band step and recenter by a per-band offset.
4. **Inverse wavelet.** A 9/3 integer lifting transform runs per tile, from the coarsest level down to the requested mip.
5. **Color.** A reversible opponent transform turns planes back into RGB, or HDR log codes turn into half floats, RG11B10F or RGB9E5.

Status: draft. Version 1 may still change before it's frozen.

## Conventions

- All multi-byte integers are little-endian. `u8`, `u16`, `u32` are unsigned, `i8`, `i32` are two's complement.
- A `size` is an unsigned integer of 1 to 5 bytes, 7 bits per byte from the least significant, where a byte's top bit is set if another byte follows. It must fit in 32 bits, so a fifth byte is at most 15.
- Bit streams are read least significant bit first: bit `i` of a stream is bit `i % 8` of byte `i / 8`. Reading `n` bits gives a value whose bit `j` is the `j`th bit read.
- A `gamma` in a bit stream is an Elias gamma code: count `z` zero bits up to the first one bit, then read `z` more bits `m`, and the value is `2^z + m − 1`. More than 16 zeros make the file corrupt.
- `>>` on a negative number is an arithmetic shift, rounding toward negative infinity.
- `wrap16(v)` keeps the low 16 bits of `v` as a signed value. Every plane value and every lifting result is `wrap16`ed, so arithmetic is exact on any platform.
- `clamp(v, lo, hi)` limits `v` to `[lo, hi]`. `ceil_half(v)` is `(v + 1) >> 1`. `bitlen(v)` is the number of bits needed for `v`, with `bitlen(0) = 0`.

## Container

A file is, back to back:

1. The fixed header, 18 bytes.
2. Sizes, `3 + 2 × levels` of them, where `levels` is defined under **Tiles and levels**: the bytes of the entropy tables, then for each segment from `MIPlevels` down to `MIP0`, the bytes of its stream 0 and of its stream 1.
3. The quantizer parameters (PARM), whose size follows from the header.
4. The entropy tables (TABL).
5. The segments, `MIPlevels` down to `MIP0`, each its stream 0 then its stream 1.
6. A `u32` CRC of every byte before it, then 12 zero bytes.

### Header

| Offset | Type | Field |
|---|---|---|
| 0 | `u8[8]` | Signature `89 4C 46 49 0D 0A 1A 0A` |
| 8 | `u8` | Version, 1 |
| 9 | `u8` | Channels, 1 to 4 |
| 10 | `u8` | `tile_log2`, 5 to 10 |
| 11 | `u8` | Flags |
| 12 | `u16` | Width, at least 1 |
| 14 | `u16` | Height, at least 1 |
| 16 | `u16` | Layers, at least 1 |

The signature's first byte has its high bit set, and its last four bytes catch line ending conversion and DOS end-of-file damage, as in PNG. The header and sizes take at most 93 bytes, which is all a reader needs to learn the file's size.

Flags:

| Bit | Name | Meaning |
|---|---|---|
| 0 | linear | Pixels are linear rather than sRGB. Informational: decoding is the same |
| 1 | cutout | Alpha snaps to 0 or 255. Requires 4 channels |
| 2 | hdr | Planes hold log2 brightness codes. Requires 1 or 3 channels |
| 3 | premultiplied | RGB is premultiplied by alpha, and decoding keeps RGB ≤ A. Requires 4 channels |
| 4 | cubemap | Layers are cube faces in the order +X, −X, +Y, −Y, +Z, −Z, six per cube. Requires width = height and layers divisible by 6 |
| 5 | fixed | Planes carry 4 fractional bits through dequantization and the wavelet, which are rounded off before output. Not with hdr |
| 6 | data | 3 or 4 channels of independent data, with no color transform. Not with hdr, cutout or premultiplied |

Other flag bits must be zero. A decoder that meets an unknown flag must refuse the file rather than guess.

The CRC is CRC-32 as used by zlib and PNG: polynomial `0xEDB88320` reflected, initial value `0xFFFFFFFF`, final complement. The CRC of the ASCII string `123456789` is `0xCBF43926`.

The CRC at the end lets decoders read up to 16 bytes past any stream without bounds checks.

### Segments

`MIPlevels` holds the tile scales and LL bands, and `MIPk` for `k` below `levels` holds the detail bands of wavelet level `k + 1`.

### Prefixes

Decoding mip `m` needs only the bytes up to the end of `MIPm`, plus the 16 bytes after it. So a file prefix of `end(MIPm) + 16` bytes decodes mip `m`. A streamed file can show a small mip before the rest arrives.

### Validation

A decoder must reject, rather than read out of bounds or loop forever on, any file that breaks a rule in this document. That includes a header field out of range, a size over 32 bits, an invalid quantizer step, an invalid entropy table, a stream that runs past its bytes, and a band that overruns. The CRC protects against transport damage, and checking it is optional, since a validating decoder is memory safe without it.

## Tiles and levels

The image is cut into square tiles of `tile = 2^tile_log2` pixels, in raster order from the top left. The last column and row of tiles may be narrower. Every layer is tiled the same way, and layers follow one another.

All tiles share one level count, so any tile can stop at any mip:

```
levels(w, h):  w = min(width, tile), h = min(height, tile), n = 0
               while n < 6 and w >= 16 and h >= 16: w = ceil_half(w), h = ceil_half(h), n += 1
               return n
```

A tile of `tw × th` pixels has level sizes `w_0 = tw`, `w_l = ceil_half(w_(l−1))`, and likewise `h_l`. Mip `m` of the image is `ceil(width / 2^m)` by `ceil(height / 2^m)`, and a tile at `(tx, ty)` covers `w_m × h_m` pixels at `(tx >> m, ty >> m)` within it.

Each tile holds one plane per channel, laid out in the Mallat arrangement. After `levels` levels the LL band is the top left `w_levels × h_levels`. Level `l`, for `l` from 1 to `levels`, adds three bands within the `w_(l−1) × h_(l−1)` region:

| Band | Orientation `o` | Position | Size |
|---|---|---|---|
| HL | 0 | `(w_l, 0)` | `(w_(l−1) − w_l) × h_l` |
| LH | 1 | `(0, h_l)` | `w_l × (h_(l−1) − h_l)` |
| HH | 2 | `(w_l, h_l)` | `(w_(l−1) − w_l) × (h_(l−1) − h_l)` |

Bands may be empty at coarse levels of small edge tiles.

### Planes

| Channels | Planes |
|---|---|
| 1 | Gray, from R |
| 2 | Normal X and Y, from R and G |
| 3 | Y, RG, BG |
| 4 | Y, RG, BG, A |

With the data flag, 3 or 4 channels are planes of R, G, B and A as they are. With the HDR flag, 1 channel is a gray code plane and 3 are Y, RG, BG of log codes. The **color modes** are 3 or 4 channels without the data flag, and their planes 1 and 2 are **chroma**. Everything else is coded like luma.

## Quantizer parameters (PARM)

With the HDR flag set, first `i32 log_min`, the HDR range, within ±2^20. Then `u8 codes[channels][1 + 3 × levels]`, then `i8 offsets[channels][3 × levels]`, so PARM takes `channels × (1 + 6 × levels)` bytes, plus 4 with HDR.

A plane's bands are numbered as its segments hold them: band 0 is LL, and band `b = 1 + 3 × (levels − l) + o` is the detail band of level `l` and orientation `o`. Each band has a step code, and each detail band an offset, `offsets[plane][b − 1]`.

A step code is an 8-bit float with 4 bits of exponent and 4 of mantissa, so steps up to 31 are exact. Codes must be from 1 to 207, which keeps every product below within 32 bits:

```
step_value(c) = c < 16 ? c : (16 + (c & 15)) << ((c >> 4) − 1)
```

Each tile scales its steps by two scale symbols, a luma scale for non-chroma planes and a chroma scale for chroma planes:

```
tile_mults = { 4, 5, 6, 7, 8, 10, 11, 13, 16, 19, 23, 27, 32, 38, 45, 54 }
step(plane, b, scale) = max(1, (step_value(codes[plane][b]) × tile_mults[scale] + 8) >> 4)
```

## Entropy coding

### Contexts

Symbols are 9 bits, and each comes from one of 16 contexts, which select its table:

- Context 0 holds tile scales, symbols 0 to 15, and chroma prediction scales, symbols 16 to 48, with no raw bits.
- Context `1 + 4 × class + group` holds band coefficients. `class` is 1 for chroma, 2 for plane 3, which is alpha or a fourth data channel, and 0 otherwise. `group` is 0, 1 or 2 for detail levels 1, 2 and 3, then 2 for any coarser level, and 3 for LL.

### Tables (TABL)

TABL is one bit stream. All tables use `L = 2048` states. Context 0 has table 0 to itself, and contexts 1 to 15 share tables 1 to `shared`:

1. 4 bits, `shared − 1`. `shared` must be at most 15.
2. For each context from 1 to 15, `bitlen(shared − 1)` bits, its table less 1, which must be below `shared`.
3. For each table from 0 to `shared`:
   1. gamma `used`, the number of listed symbols, at most 512. A table with `used = 0` is empty, and nothing more of it follows.
   2. 2 bits `precision`, then gamma `implied`, which must be below `used`. The listed symbol at that index, counting from 0, takes whatever frequency the others leave.
   3. For each listed symbol, with `s` starting at −1:
      - `s += gamma + 1`. Symbols must stay below 512 and so strictly increase.
      - If bit 4 of `s` is clear, 1 bit `twin`. Otherwise `twin` is 0.
      - Unless this is the implied symbol, its frequency `F`: 4 bits `n`, from 1 to 11, then `k = min(n − 1, precision)` bits `m`, and `F = 2^(n−1) + m × 2^(n−1−k)`.
   4. The implied symbol's `F` is 2048 less the sum of the others', and must be at least 1.
   5. A twin lists two symbols, `s` with frequency `ceil(F / 2)` and `s | 16` with `floor(F / 2)`, and needs `F ≥ 2`. Bit 4 of a coefficient symbol is its sign, so twins share one frequency between the signs.

Table 0's symbols must be valid in context 0, and the other tables' symbols must be valid coefficient symbols, as described under **Coefficient symbols**. The table bits must lie within TABL.

### Building a decoding table

Visit the table's symbols in listed order, except that the implied symbol comes last, with each twin's `s | 16` straight after its `s`. Count every state handed out with `k`, from 0. Each symbol with frequency `f` takes the next `f` values of `k`, and puts its symbol at state `(k × 1283) mod 2048`.

Then visit states `x` from 0 to 2047 in increasing order. Give each its symbol's next `xs`, counting from `f` up to `2f − 1`. From `xs`:

```
bits[x] = 12 − bitlen(xs)
base[x] = (xs << bits[x]) − 2048
```

An empty table decodes every state as symbol 0 with no bits, so a decoder never reads past its tables.

### Streams

Each of a segment's two streams starts with an 11-bit initial state.

The segment's symbols alternate between the streams, starting with stream 0: the `i`th symbol, counting from 0 across everything in the segment, belongs to stream `i % 2`. That stream's state `x` gives:

```
symbol = sym[x]
x      = base[x] + read(bits[x])   on that stream
extra  = read(raw bit count of the symbol)   on the same stream
```

Reading past the end of a stream, including in the initial state, makes the file corrupt.

### What each segment holds

For each layer, then each tile in raster order:

- **`MIP` + `levels`:** the luma scale symbol and the chroma scale symbol, both in context 0 and each below 16. Then, for each plane in order, its LL band.
- **`MIPk` for `k < levels`:** for each plane in order, the HL, LH and HH bands of level `k + 1`. In the color modes, each chroma band of level 2 and up comes after its prediction scales, as described under **Chroma from luma**.

## Coefficient symbols

A band is a sequence of positions in scan order:

- **LL, LH and HH** are scanned row by row.
- **HL** is scanned column by column, each column top to bottom, since its detail lines up vertically.

Each band starts at position 0, with every coefficient zero.

| Symbol | Meaning |
|---|---|
| 0 (EOB) | The band ends. The rest stays zero |
| 480 (ZRL) | Skip 2048 positions. The new position must still be inside the band |
| `run_class << 5 \| sign << 4 \| mag_class`, with `mag_class` from 1 to 15 | Skip `run` positions, then place a coefficient and step past it |

Other symbols with `mag_class = 0` are invalid. The raw bits that follow a coefficient symbol are first `run_bits[run_class]` bits of run mantissa, then `mag_class − 1` bits of magnitude mantissa:

```
run_bits = { 0, 0, 0, 0, 0, 0, 0, 0, 3, 4,  5,  6,  7,   8,   9,   10 }
run_base = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 16, 32, 64, 128, 256, 512, 1024 }
run      = run_base[run_class] + run mantissa
q        = 2^(mag_class − 1) + magnitude mantissa
```

Every placed coefficient must lie inside the band.

### Detail bands

With `s = step(plane, b, scale)` for the band's number `b`:

```
offset = (offsets[plane][b − 1] × s + 32) >> 6
value  = wrap16(±(q × s + offset))
```

The sign is negative when bit 4 of the symbol is set.

### Chroma from luma

In the color modes, including HDR color, the chroma detail bands of level 2 and up are predicted from luma's band of the same level and orientation. Each such band is split into blocks of 32 × 32 coefficients, row by row over the band's rows and columns whatever its scan order, with the last blocks in each direction cut short at the band's edge. Before the band, its segment holds one symbol per block in context 0: symbol `32 + k` gives the block's scale `k`, from −16 to 16. A symbol below 16 makes the file corrupt.

Once the band is reconstructed, each of its coefficients adds a prediction from luma's reconstructed value `v` at the same position. With luma's offset `o` for the same band number and luma's tile scale, all in signed 16-bit arithmetic:

```
base  = wrap16(v > 0 ? v − o : v < 0 ? v + o : 0)
p     = wrap16(k × base + 4) >> 3          (arithmetic shift)
value = wrap16(value + p)
```

A block with `k = 0` adds nothing.

### LL bands

LL values are prediction residuals `±q`, without step or offset. Visit the band row by row and add the median edge detector prediction from the values already reconstructed:

```
first row:     predict = x > 0 ? left : 0
first column:  predict = up
elsewhere:     predict = med(left, up, up_left)

med(a, b, c) = c >= max(a, b) ? min(a, b) : c <= min(a, b) ? max(a, b) : a + b − c
```

Each reconstructed value is `wrap16(residual + predict)`. Once the whole band is reconstructed, each value becomes `wrap16(value × step(plane, 0, scale))`.

## Inverse wavelet

For each plane of each tile, run levels from `levels` down to `m + 1` to reach mip `m`. Level `l` turns the `w_(l−1) × h_(l−1)` region back into image samples. First run the 1D inverse down every column, with the top `h_l` rows as low and the rest as high. Then run it along every row, with the left `w_l` columns as low and the rest as high.

The 1D inverse of `n` samples, from `nl = ceil_half(n)` low values `lo` and `nh = n − nl` high values `hi`:

```
if nh == 0: out[0] = lo[0], done

update(c, a, b, d) = wrap16(near + (near >> 2) − (far >> 2) + 2) >> 2
                     with near = wrap16(a + b), far = wrap16(c + d)
predict(a, b)      = wrap16(a + b) >> 1
hi[−2] = hi[−1] = hi[0], hi[nh] = hi[nh + 1] = hi[nh − 1]   (edges repeat)

even[i] = wrap16(lo[i] − update(hi[i − 2], hi[i − 1], hi[i], hi[i + 1]))   for i in 0 … nl − 1
even[nl] = even[nl − 1]                               (edge repeats)
odd[i]  = wrap16(hi[i] + predict(even[i], even[i + 1])) for i in 0 … nh − 1

out[2i] = even[i], out[2i + 1] = odd[i]
```

When the detail bands of a level are all zero, this reduces to linear interpolation. Decoders may take that shortcut as long as the output stays identical.

## Pixels

Decoded pixels are the tile regions at mip `m`, placed in the image of each layer, with layers following one another.

### 8-bit

| Channels | Pixel | Bytes |
|---|---|---|
| 1 | `R = clamp(gray, 0, 255)` | 1 |
| 2 | `R, G` clamped | 2 |
| 3 | RGB from the color inverse, or from its planes with the data flag, `A = 255` | 4 |
| 4 | RGB from the color inverse, or from its planes with the data flag, A from its plane | 4 |

With the fixed flag, gray, normal, data and alpha values first become `wrap16(v + 8) >> 4`, and the color inverse rounds its own results, with `s = 4` and `h = 8`. Without it, `s = h = 0`.

The color inverse undoes a lifting step. RG is red less green, BG is blue less green, and Y is green plus 0.3 RG and 0.1 BG, so about 0.3 R + 0.6 G + 0.1 B. Each product is computed in 32 bits and shifted arithmetically, then everything is in 16-bit arithmetic:

```
g = wrap16(Y − ((RG × 19661) >> 16) − ((BG × 6554) >> 16))
R = wrap16(g + RG + h) >> s, G = wrap16(g + h) >> s, B = wrap16(g + BG + h) >> s
```

With 4 channels, the cutout flag first replaces A with `A >= 128 ? 255 : 0`. With the premultiplied flag, each of R, G and B becomes `min(c, A)` before clamping. Every channel is then clamped to 0 to 255.

### HDR

Each code becomes `clamp(code, 0, 4095)`, after the color inverse for 3 planes, with `s = h = 0`. Code 0 is zero. Code `c ≥ 1` is the value `2^((c − 1 + log_min) / 128)`, defined exactly by these float32 bits:

```
k = c − 1 + log_min, e = (k >> 7) + 127
bits = e <= 0 ? 0 : e >= 255 ? 0x7F7FFFFF : e << 23 | pow2_mantissa[k & 127]
```

`pow2_mantissa[f]` is `round((2^(f/128) − 1) × 2^23)`, rounded to nearest. The 128 values are listed in the reference decoder, and every entry is at least 10^−4 from a rounding tie, so any careful computation reproduces them. Avoid deriving them from `exp2f`, which is off by an ulp for some `f` in common libraries.

The decoder chooses an output format:

| Format | Pixel |
|---|---|
| Native, 1 plane | R16F |
| Native, 3 planes | RGBA16F with A = 1.0 |
| RG11B10F | R and G as 11-bit floats, B as a 10-bit float, packed `R \| G << 11 \| B << 22`. Gray goes in R |
| RGB9E5 | Shared exponent, packed `R \| G << 9 \| B << 18 \| E << 27`. Gray goes in R |

Half, 11-bit and 10-bit floats are unsigned, with a 5-bit exponent biased by 15 and 10, 6 or 5 mantissa bits. Converting a float32 value rounds its mantissa to nearest even, gives denormals below the normal range, flushes to zero below the smallest denormal, and clamps to the largest finite value above the range. A rounding carry moves into the exponent.

RGB9E5 takes its exponent from the largest of the pixel's three codes, `cmax`:

```
E    = cmax == 0 ? 0 : clamp(((cmax − 1 + log_min) >> 7) + 16, 0, 31)
base = (E − 24) × 128 − log_min − 128
m9[i] = min(511, round_half_up(2^((i − 129) / 128))) for i in 1 … 1280, with m9[0] = 0
channel mantissa = code == 0 ? 0 : m9[clamp(code − base, 0, 1280)]
```

`m9` is computed exactly from `pow2_mantissa`: with `k = i − 129` and `e = k >> 7`, `m9[i] = (2^23 + pow2_mantissa[k & 127] + 2^(22 − e)) >> (23 − e)`.

## Test vectors

`docs/vectors/` holds a small file for each mode. Each hash below is the CRC-32 of all decoded pixels, every layer in order, in the native format unless a format is named. A conforming decoder reproduces every one. `lfi hash [--mip N] [--format F] <file>` computes them with the reference tool.

| File | Contents | mip 0 | mip 1 | Other formats at mip 0 |
|---|---|---|---|---|
| `rgb16.lfi` | 72x40 color, step 16 | `e27784f9` | `b6d60746` |  |
| `rgba8.lfi` | Color and alpha, step 8 | `0699fabf` | `726c99a4` |  |
| `lossless.lfi` | Color and alpha, lossless | `8eae19fd` | `00bf64ff` |  |
| `gray32.lfi` | Gray, step 32 | `530064c0` | `42163031` |  |
| `normal8.lfi` | Normal XY, step 8 | `23f66992` | `564ecf46` |  |
| `cutout16.lfi` | Cutout alpha | `9384f76c` | `8f7e8769` |  |
| `premul16.lfi` | Premultiplied alpha, dark tile scales | `8a7531a2` | `7e3a8aec` |  |
| `tiles32.lfi` | 32x32 tiles, step 16 | `164f4505` | `6aafd070` |  |
| `array8.lfi` | 3 array layers | `8cc87adc` | `f4e4e939` |  |
| `cube16.lfi` | Cubemap, 40x40 faces | `19c69709` | `2dd25bc3` |  |
| `data8.lfi` | 4 channels of data, step 8 | `80237955` | `095c7cb6` |  |
| `hdr16.lfi` | HDR color | `d12f8a2f` | `48a80ce2` | RG11B10F `7ad0a866`, RGB9E5 `4199265e` |
| `hdrgray16.lfi` | HDR gray | `33f938a6` | `9a4cf82c` | RG11B10F `f9420f9e`, RGB9E5 `018ff32d` |

## Informative notes

These describe the reference implementation, not requirements.

- **Encoder.** Quantization uses a deadzone of 0.35 steps. By default it then lowers ±1 and power of two values by one bin where the bits saved, priced from the symbols of the tiles so far, outweigh the added error; built for speed, it instead drops isolated ±1 coefficients below 1.0 step. It sets each band's offset to the average position of its values within their bins. Each chroma prediction scale is the least squares fit to luma's coded values, kept where it saves more squared error than one squared step. Steps come from one base step, balanced across bands by their synthesis gain, then scaled by level for color, tuned for SSIMULACRA2 and Butteraugli rather than squared error: luma at 1.6×, 0.8×, 0.5× and 0.5× for levels 1, 2, 3 and coarser, and chroma at 1.8×, 1.17×, 0.99× and 0.765×. Lossy 8-bit files set the fixed flag, with steps in sixteenths. Straight alpha gets a smooth fill under fully transparent pixels, so reduced mips don't pick up hidden colors. Contexts start with a table each, and the two tables whose sharing saves the most bits merge, until no merge saves any. Its decisions use integer math or exactly rounded float operations, so it writes identical files on any compiler and CPU with IEEE floats evaluated at their own precision. Each table takes the frequency precision that codes it and its symbols in the fewest bits.
- **Progressive decoding.** The reference API can keep each plane's LL region at a mip, plus the tile scales, and continue to a finer mip from there. The result is identical to a direct decode.
- **Speed.** The reference decoder reads streams with one 8-byte load per symbol at a bit position. It checks overruns once per pair of symbols, relying on the 16 trailing bytes and scratch slack. It decodes each level's bands just before that level's inverse, so they share one buffer.
