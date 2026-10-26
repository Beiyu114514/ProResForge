#!/usr/bin/env bash
set -euo pipefail

# Apple Log 曲线核对:验证 filtergraph(lutrgb 表达式)的实际输出与
# docs/INTERFACES.md §10.4 的数值锚点表一致(期望值独立记录在契约里,
# 本脚本不重复实现公式 —— 防止「表达式和期望值抄自同一处」的回归盲区)。

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
expr="if(gte(val/65535,0.01),0.08550479*log(val/65535+0.00964052)/0.6931471805599453+0.69336945,47.28711236*pow(val/65535+0.05641088,2))*65535"

ffmpeg -hide_banner -loglevel error -f lavfi \
  -i "nullsrc=s=65536x1,format=gbrp16,geq=r='X':g='X':b='X'" \
  -vf "lutrgb=r='$expr':g='$expr':b='$expr'" -frames:v 1 -f rawvideo "$tmp/mapped.raw"
test -s "$tmp/mapped.raw"

python - "$tmp/mapped.raw" <<'PY'
import struct
import sys

# 契约锚点(docs/INTERFACES.md §10.4):输入码 x → 期望 16-bit 输出码,容差 ±1 LSB。
# 口径:输入按 round(R*65535) 量化、以 x/65535 进曲线,输出 round(P*65535) 取码。
ANCHORS = [
    (0,     9861,  "0.0  黑位抛物线上缘"),
    (655,  13665,  "0.01 两段连续点"),
    (11796, 31999, "0.18 灰卡锚点"),
    (65535, 45518, "1.0  clip white"),
]

data = open(sys.argv[1], "rb").read()
if len(data) != 65536 * 3 * 2:
    raise SystemExit(f"unexpected output size: {len(data)}")
for x, expected, label in ANCHORS:
    # gbrp16 planes are G, B, R.
    actual = struct.unpack_from("<H", data, (2 * 65536 + x) * 2)[0]
    if abs(actual - expected) > 1:
        raise SystemExit(f"anchor {label}: got {actual}, expected {expected}; file size={len(data)}")
    print(f"x={x:5d} ({label}): {actual} (expected {expected})")
PY
