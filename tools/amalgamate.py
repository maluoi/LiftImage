#!/usr/bin/env python3
# Builds the single-header library dist/lift_image.h from src/, which stays the source of truth.
# usage: tools/amalgamate.py

import os, re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEC  = ["ent.h", "color.h", "lfi_internal.h", "ent_dec.c", "lfi_dec.c"]
ENC  = ["ent_enc.c", "lfi_enc.c"]

def body(name):
	text = open(os.path.join(ROOT, "src", name)).read()
	text = re.sub(r'^#pragma once\n', '', text, flags=re.M)
	text = re.sub(r'^#include "[^"]+"\n', '', text, flags=re.M)
	return f"// ---- {name}\n\n" + text.strip() + "\n\n"

def main():
	api     = open(os.path.join(ROOT, "src", "lfi.h")).read().replace("#pragma once\n", "").strip()
	license = open(os.path.join(ROOT, "LICENSE")).read().strip().splitlines()
	out = f"""// lift_image.h: Lift Image, a baked image format for real-time engines. Specified in docs/format.md.
// Generated from src/ by tools/amalgamate.py, so edit those instead.
// MIT License, {license[2]}. The full text is at the end of this file.
//
// In exactly one C or C++ file (C99 or C++11 and later), ideally on its own, since the implementation defines internal names:
//   #define LFI_IMPLEMENTATION    // the decoder
//   #define LFI_ENCODER           // and the encoder, which also needs -lm
//   #define LFI_ENCODER_QUALITY 0 // optional: about 4 KB less encoder and 25% faster lossy encodes, for files about 1.5% larger
//   #include "lift_image.h"
// Everywhere else, include it plainly for the API.

#ifndef LIFT_IMAGE_H
#define LIFT_IMAGE_H

{api}

#endif // LIFT_IMAGE_H

#if defined(LFI_IMPLEMENTATION) && !defined(LIFT_IMAGE_IMPLEMENTED)
#define LIFT_IMAGE_IMPLEMENTED

"""
	out += "".join(body(n) for n in DEC)
	out += "#ifdef LFI_ENCODER\n\n" + "".join(body(n) for n in ENC) + "#endif // LFI_ENCODER\n\n"
	out += "#endif // LFI_IMPLEMENTATION\n\n"
	out += "".join(f"// {line}".rstrip() + "\n" for line in license)
	os.makedirs(os.path.join(ROOT, "dist"), exist_ok=True)
	open(os.path.join(ROOT, "dist", "lift_image.h"), "w").write(out)

if __name__ == "__main__": main()
