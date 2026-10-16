CC     ?= cc
ARCH   ?=
CFLAGS ?= -std=c11 -O3 -g $(ARCH) -Wall -Wextra -Wno-unused-function
CODEC   = src/ent_enc.c src/ent_dec.c src/lfi_enc.c src/lfi_dec.c

all: build/lfi

# The single-header library, and the command line tool built on it
dist/lift_image.h: src/*.h $(CODEC) tools/amalgamate.py LICENSE
	python3 tools/amalgamate.py

build/lfi: tools/lfi.c dist/lift_image.h plugins/thumbnail.h build/stb_impl.o | build
	$(CC) -Idist -Iplugins -Ithird_party $(CFLAGS) -o $@ tools/lfi.c build/stb_impl.o -lm

build/stb_impl.o: tools/stb_impl.c | build
	$(CC) -Ithird_party $(CFLAGS) -w -c -o $@ $<

# Decodes docs/vectors and checks every hash against docs/format.md
test: build/lfi
	python3 tools/test_vectors.py build/lfi

# The command line tool, and on Linux the MIME type and a thumbnailer that runs it. Dolphin needs plugins/linux/kde
PREFIX ?= /usr/local
install: build/lfi
	install -Dm755 build/lfi $(DESTDIR)$(PREFIX)/bin/lfi
	install -Dm644 plugins/linux/lift-image.xml $(DESTDIR)$(PREFIX)/share/mime/packages/lift-image.xml
	install -Dm644 plugins/linux/lift-image.thumbnailer $(DESTDIR)$(PREFIX)/share/thumbnailers/lift-image.thumbnailer
	[ -n "$(DESTDIR)" ] || update-mime-database $(PREFIX)/share/mime

# libFuzzer harnesses, with a seed corpus of small valid files
FUZZ = clang -std=c11 -O1 -g $(ARCH) -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined -Isrc

fuzz: build/fuzz_decode build/fuzz_roundtrip build/fuzz_seeds
	mkdir -p build/fuzz_corpus && build/fuzz_seeds build/fuzz_corpus

build/fuzz_%: tools/fuzz/fuzz_%.c $(CODEC) src/*.h | build
	$(FUZZ) -o $@ $< $(CODEC) -lm

build/fuzz_seeds: tools/fuzz/seeds.c $(CODEC) src/*.h | build
	$(CC) -Isrc $(CFLAGS) -o $@ $< $(CODEC) -lm

build:
	mkdir -p build

clean:
	rm -rf build

.PHONY: all clean fuzz test install
