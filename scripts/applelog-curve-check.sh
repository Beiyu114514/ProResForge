#!/usr/bin/env bash
set -euo pipefail

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
expr="if(gte(val/65535,0.01),0.08550479*log(val/65535+0.00964052)/0.6931471805599453+0.69336945,47.28711236*pow(val/65535+0.05641088,2))*65535"

ffmpeg -hide_banner -loglevel error -f lavfi \
  -i "nullsrc=s=65536x1,format=gbrp16,geq=r='X':g='X':b='X'" \
  -vf "lutrgb=r='$expr':g='$expr':b='$expr'" -frames:v 1 -f rawvideo "$tmp/mapped.raw"
test -s "$tmp/mapped.raw"

python - "$tmp/mapped.raw" <<'PY'
import math
import struct
import sys

data = open(sys.argv[1], "rb").read()
if len(data) != 65536 * 3 * 2:
    raise SystemExit(f"unexpected output size: {len(data)}")
anchors = [0.00, 0.01, 0.18, 1.00]
beta, gamma, delta, c = 0.00964052, 0.08550479, 0.69336945, 47.28711236
for r in anchors:
    x = round(r * 65535)
    linear = x / 65535
    if linear >= 0.01:
        mapped = gamma * math.log2(linear + beta) + delta
    else:
        mapped = c * (linear + 0.05641088) ** 2
    expected = round(mapped * 65535)
    # gbrp16 planes are G, B, R.
    actual = struct.unpack_from("<H", data, (2 * 65536 + x) * 2)[0]
    if abs(actual - expected) > 1:
        raise SystemExit(f"anchor {r}: got {actual}, expected {expected}; file size={len(data)}")
    print(f"R={r:.2f}: {actual} (expected {expected})")
PY
