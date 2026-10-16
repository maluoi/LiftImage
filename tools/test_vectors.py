#!/usr/bin/env python3
# Decodes every test vector and checks its hashes against the table in docs/format.md
# usage: tools/test_vectors.py <lfi command line tool>
import os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LFI  = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "lfi")

files, failures = 0, 0
for line in open(os.path.join(ROOT, "docs", "format.md")):
	row = re.match(r"\| `(\w+\.lfi)` \|[^|]*\| `(\w+)` \| `(\w+)` \|(.*)\|$", line)
	if not row: continue
	path   = os.path.join(ROOT, "docs", "vectors", row.group(1))
	checks = [([], row.group(2)), (["--mip", "1"], row.group(3))]
	checks += [(["--format", name.lower()], expected) for name, expected in re.findall(r"(\w+) `(\w+)`", row.group(4))]
	for args, expected in checks:
		result = subprocess.run([LFI, "hash", *args, path], capture_output=True, text=True).stdout.strip()
		if result != expected:
			failures += 1
			print(f"FAIL {row.group(1)} {' '.join(args)}: {result or 'no output'}, expected {expected}")
	files += 1

print(f"{files} vectors, {failures} failures" if files else "no vectors found in docs/format.md")
sys.exit(1 if failures or not files else 0)
