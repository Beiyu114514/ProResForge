# DNG→ProRes 线路新增 Apple Log 映射 —— 实施方案

状态:待评审(未动代码) · 2026-10-05
前置阅读:`docs/INTERFACES.md`(契约)、`docs/DNGZIP-PROGRES-PLAN.md`(DNG zip 设计)

---

## 1. 背景与目标

DNG 序列 zip(MotionCam)→ ProRes 线路目前输出**线性光**数据(LibRaw `gamm=0,0`),按 bt709 打标,想要 log 母版只能靠用户自己挂 LUT(plan 文档 §"LUT bake 通道")。但线性域上的 3D LUT 网格在暗部精度极差(−6 stops 的内容挤进 1~2 个网格点),不是合格的 log 输出方案。

目标:给该线路新增内置色彩映射档 **Apple Log**——输出与 iPhone 原生 Apple Log ProRes 同构的素材(2020 原色 + Apple Log 曲线 + colr 9-2-9),可直接进 Resolve/FCP 用官方 Apple Log LUT 或 CST 分级。

设计原则:**`dngmap=none`(默认)行为与今天逐字节一致**;所有改动收敛在 zip 分支的 LibRaw 参数、filtergraph 描述串、流打标三处。

## 2. 可行性验证结论(2026-10-05 已完成,证据摘要)

| # | 验证项 | 方法 | 结果 |
|---|--------|------|------|
| 1 | 现有输入是线性光 | WSL host LibRaw 0.21.4,与 app 完全相同参数(`gamm=0,0`)实测 | 正常线性图像(均值 12.5%),✅ |
| 2 | Apple Log 公式公开且逐通道 | Apple 白皮书 2023-09 ↔ colour-science ↔ AMPAS IDT CTL 三方一致 | 分段 log2+抛物线,常数齐全,✅ |
| 3 | 曲线可用 `lutrgb` 精确实现 | FFmpeg 8.1,`format=gbrp16,lutrgb=…` 对参考公式逐点比对 | 误差 ≤1/65535,✅ |
| 4 | 性能无影响 | 4K 422 HQ 12 帧端到端实测 | ~1s(lutrgb 为预计算查表),✅ |
| 5 | 容器可打标 9-2-9 | FFmpeg 6.1 movenc.c 源码 + 8.1 实测 colr 字节 | `nclc 0009 0002 0009`,与 Apple 自家文件一致,✅(需 `movflags=+write_colr`,见 §5.2) |
| 6 | LibRaw 2020 直出 | host 版 `-o 8` + 版本头核对(app 同为 0.21.4) | `output_color=8`=Rec2020 线性可用,✅ |

验证过程中的假象排除:`dcraw_emu` 对 `-g` 第一参数取倒数(`gamm[0]=1/atof(arg)`,见 LibRaw 0.21.4 samples 源码),其 `-g 0 0` 全黑是 `1/0=inf` 所致,**不代表** app 直设 `gamm[0]=0` 的行为。

## 3. 总体设计

### 3.1 链路(applelog 档)

```
Bayer raw
  → LibRaw:WB 三档不变,色彩矩阵一次到位 camera→BT.2020(output_color=8),
    gamm[0]=gamm[1]=0 线性,rgb48le
  → filtergraph(在 format=<yuv> 前插入):
      format=gbrp16 → lutrgb(逐通道 Apple Log OETF) → scale=out_color_matrix=bt2020nc
  → format=<out_fmt>(10/12bit YUV,limited range)
  → [既有 lut3d 通道] → [fps] → prores_ks(.mov)
  → 打标:colorspace=bt2020nc, primaries=bt2020, trc=unspecified
  → mov muxer:movflags=+write_colr → colr 原子 9-2-9
```

不经过 709:2020 直出避免相机域→sRGB 域的既有 gamut squeeze,也不需要 RGB→RGB 矩阵滤镜。全链色彩损失点只有一次 camera→2020 的极饱和色 clamp(与一切 raw→2020 工具一致)和端点 10-bit 量化(与 iPhone 母带同级)。

### 3.2 规格键(契约 §3 新增)

| 键 | 类型 | 取值 | 默认 | 说明 |
|----|------|------|------|------|
| `dngmap` | str | `none` / `applelog` | `none` | 仅 `src=dngzip` 有效,其他输入忽略。`none` 行为与现状完全一致 |

`dngcolor`(WB)与 `dngmap` 正交,组合自由。

### 3.3 Apple Log 编码函数(白皮书常数,不可改动)

```
P = γ·log2(R+β)+δ    若 R ≥ Rt
P = c·(R−R₀)²        若 R₀ ≤ R < Rt
P = 0                若 R < R₀

R₀=−0.05641088  Rt=0.01  c=47.28711236  β=0.00964052  γ=0.08550479  δ=0.69336945
```

- 输入 R 为场景线性反射率(0.18 = 18% 灰卡),与 LibRaw 线性输出的白点归一(clip white = 1.0)直接衔接,不需要曝光偏置。
- 反函数(验证/回归用):`R = 2^((P−δ)/γ) − β`(P ≥ Pt);`R = sqrt(P/c) + R₀`(0 ≤ P < Pt);`Pt = c·(Rt−R₀)² ≈ 0.2085`。
- 锚点(数值回归的期望值,16-bit 码):

| R(线性入) | P(Apple Log 出) | 16-bit 码 | 备注 |
|------------|------------------|-----------|------|
| 0.0 | 0.150479 | ≈9859 | 抛物线上缘(黑位抬起,符合 Apple 设计) |
| 0.01(Rt) | 0.2085 | ≈13664 | 两段在此连续 |
| 0.18 | 0.48827 | ≈32001 | 18% 灰锚点(白皮书/colour-science 一致) |
| 1.0 | 0.69455 | ≈45518 | clip white;曲线全程 ∈[0,1] 不削顶 |

## 4. 改动清单

### 4.1 native(`app/src/main/cpp/`)

**`strspec.h`**
- 新增字符串键 `SpecKeys::kDngMap = "dngmap"`,并加入 §308 行处的键集合(随现有 `kDngColor` 模式)。

**`dngzip.cpp`**
- `apply_libraw_params`(613 行)增加映射参数:
  - `applelog`:`output_color = 8`(Rec2020 线性直出);其余参数(`gamm=0,0`、`output_bps=16`、`no_auto_bright=1`、WB 三档)**一律不动**。
  - `none`:现状 `output_color = 1`。
- `dng_decode_to_rgb48` 及调用链相应加一个参数(从 spec 读出后传入)。

**`transcode.cpp`**
- `build_descr`(675 行):当 `src=dngzip` 且 `dngmap=applelog`,在 `setsar=1,` 之后、`format=<pfname>,` 之前插入:

  ```
  format=gbrp16,lutrgb=r='<EXPR>':g='<EXPR>':b='<EXPR>',scale=out_color_matrix=bt2020nc,
  ```

  `<EXPR>`(含单引号原样写入 graph 描述串):

  ```
  if(gte(val/65535,0.01),0.08550479*log(val/65535+0.00964052)/0.6931471805599453+0.69336945,47.28711236*pow(val/65535+0.05641088,2))*65535
  ```

  说明:`log` 是自然对数,除以 ln2=0.6931471805599453 即 log2;`scale` 同尺寸只做 RGB→YUV 的 2020ncl 矩阵变换(压掉 swscale 默认 601 矩阵的既有瑕疵);输出 range 默认 tv,与 Apple 文件一致。
- 打标(1397 行 zip 分支):
  - `dngmap=applelog` 时:显式置 `color_space=AVCOL_SPC_BT2020_NCL`、`color_primaries=AVCOL_PRI_BT2020`,**跳过** trc 的 bt709 兜底(保持 `AVCOL_TRC_UNSPECIFIED`);
  - `dngmap=none`:现兜底逻辑原样保留。
- muxer 选项(1664 行 `avformat_write_header(oc, nullptr)`):applelog 时传 dict `{"movflags", "+write_colr"}`(仅此档;`none` 不传,不改变既有输出)。残留 dict 按惯例检查后释放。

### 4.2 Kotlin(`app/src/main/java/com/proresforge/app/`)

| 文件 | 改动 |
|------|------|
| `data/model.kt` | 新增 `enum class DngMap(val labelRes: Int) { NONE, APPLE_LOG }`,`specValue()` 按 §3.2 返回 `"none"`/`"applelog"`(仿 `DngColorMode`,97 行);`TransformSpec` 加 `val dngMap: DngMap = DngMap.NONE`(126-128 行附近) |
| `engine/FFmpeg.kt` | zip 分支(219-220 行)追加 `sb.append("dngmap=").append(spec.dngMap.specValue()).append('\n')` |
| `ui/HomeViewModel.kt` | 仿 `setDngColor`(285 行)加 `setDngMap`;state 初始与 snapshot(67/107/394/426 行)各加一处 |
| `data/SettingsStore.kt` | 仿 `dngColor`(66-67 行)加 `dngMap` 持久化(`enumOr` + 新 KEY) |
| `ui/HomeScreen.kt` | zip 分支 dngcolor 行(401-403 行)后加一个 `SegmentedRow`:选项 `DngMap.values()` 顺序映射,标题「色彩映射」 |
| `res/values/strings.xml`、`res/values-en/strings.xml` | 新增 `dngmap_none`(线性/Linear)、`dngmap_applelog`(Apple Log)、行标题 `dngmap_title`(色彩映射/Color Mapping) |

注意:`TransformSpec` 是序列化的(JobStore/历史记录),新字段必须带默认值,老 JSON 反序列化才不破;`HistoryItemSerializationTest` 应保持绿。

### 4.3 测试

**`app/src/test/.../DngSpecMappingTest.kt`**(契约回归,仿现有两个用例):
- `dngMapSpecValuesAreContractStrings`:`NONE→"none"`、`APPLE_LOG→"applelog"`;
- 锁定 `DngMap.values()` 顺序(`SegmentedRow` 按下标取值)且 labelRes 互异。

**native 侧无单测框架**,曲线正确性由 §6.1 的脚本回归覆盖(可放 `scripts/applelog-curve-check.sh`,实施时顺手落盘)。

### 4.4 文档(`docs/INTERFACES.md`)

- §3 键表:加 `dngmap` 行;
- §7 filtergraph:补映射段与 `scale=out_color_matrix=bt2020nc` 说明;
- §10.3/§10.4:LibRaw `output_color=8` 说明、打标变化(unspec trc + write_colr → colr 9-2-9)、锚点表引用本方案;
- README 功能列表加一句。

## 5. 关键实现细节与陷阱(均已验证)

### 5.1 lut 表达式

- lut 滤镜表达式里**没有 `max` 变量**(FFmpeg 8.1 实测报 "Undefined constant"),必须用字面量 `65535`;
- 表达式含逗号,在 graph 描述串里必须整段套单引号 `r='…'`(graph parser 的引号规则),不要用 `\,` 转义(实测会连反斜杠一起进表达式求值失败);
- `lutrgb` 在 16 位输入上预计算 3×65536 查表(约 384KB),运行时开销可忽略;
- 本档输出域 [0,1] 内无削顶,黑位 0.1505、白 0.6945,10-bit limited 后中灰 ≈ 码值 123,分布与 iPhone 素材一致。

### 5.2 打标(colr 9-2-9)

- FFmpeg 6.1 movenc:`has_color_info` 要求 **primaries、trc、colorspace 三者全部非 UNSPECIFIED** 才自动写 colr;`movflags=+write_colr` 可强制写,未设字段填 `2`(unknown)。
- Apple Log 无 H.273 传输枚举(6.1 枚举止步 HLG=18),Apple 自家文件即 transfer=unknown。故**忠实做法 = trc 留空 + write_colr**,产物 colr 为 `nclc 0009 0002 0009`,与 iPhone 原生文件逐字节同构。不要用任何"近似"trc 值(如 bt709)凑 has_color_info——那会让下游误判曲线。
- `write_colr` 在 FFmpeg 文档标注 experimental,但 6.1 引入后行为稳定(8.1 实测一致),且 app 锁定自带 6.1(契约 §6),无漂移风险。

### 5.3 三个必坑

1. **trc 兜底**:`transcode.cpp:1401` 会在 trc 为 UNSPECIFIED 时强填 bt709——applelog 分支必须绕开整个兜底块,否则 colr 变 9-**1**-9,曲线被标成 709 gamma。建议契约测试顺带锁死"applelog ⇒ trc 必须为空"。
2. **swscale 默认 601 矩阵**:RGB→YUV 不显式指定矩阵时走 BT.601。applelog 段必须用 `scale=out_color_matrix=bt2020nc`。注意这条瑕疵在现有 `none` 档与 lut3d 通道同样存在(v1 不动,见 §8)。
3. **矩阵方向**(如未来有人改走"LibRaw sRGB 输出 + 矩阵滤镜"路线):709→2020 是全正矩阵 `[0.6274 0.3293 0.0433 / 0.0690 0.9295 0.0015 / 0.0164 0.0921 0.8915]`,凸组合故零裁剪;`[1.6605, −0.5876, …]` 是反方向(2020→709),用反会引入本不存在的饱和色裁剪。方案 A(2020 直出)根本不需要这个滤镜。

## 6. 验证方案

### 6.1 曲线数值回归(WSL,桌面 ffmpeg 即可)

灰阶 ramp(`geq=r='X*64':g='X*64':b='X*64'`,gbrp16)→ 走 §4.1 的映射段 → 输出 16-bit RGB → 与 §3.3 锚点表逐点比对,判据:**|偏差| ≤ 2 码(16-bit)**(实测 ≤1)。已验证过的等价命令:

```bash
ffmpeg -f lavfi -i "nullsrc=s=1024x8,format=gbrp16,geq=r='X*64':g='X*64':b='X*64'" \
  -frames:v 1 -pix_fmt rgb48be ramp.png
ffmpeg -i ramp.png -vf "format=gbrp16,lutrgb=r='<EXPR>':g='<EXPR>':b='<EXPR>',format=rgb48be" \
  -frames:v 1 mapped.png   # 读像素对比锚点
```

### 6.2 容器标签

产物 .mov:`ffprobe` 应显示 `color_space=bt2020nc, color_primaries=bt2020, color_transfer=unknown, color_range=tv`;`colr` 原子字节应为 `63 6f 6c 72 6e 63 6c 63 00 09 00 02 00 09`。

### 6.3 真机端到端

1. WSL 构建 APK(现有流程),模拟器/真机导入真实 MotionCam zip,选 Apple Log 档转码;
2. 拉回产物跑 §6.1/§6.2;
3. Resolve 中将片段输入色彩空间设为 **Apple Log / Rec.2020**(或挂 Apple 官方 Apple Log→Rec709 LUT),图像应正常灰平衡、无死黑/灰雾;与 `none` 档 + 后期线性→log 的参考流程交叉比对;
4. `dngmap=none` 档产物与当前版本产物做二进制/逐帧对比,确认零回归。

## 7. 不变式与回归保证

- `dngmap=none`(默认、未传、旧 spec 无此键)时:LibRaw 参数、graph 描述串、打标、muxer 选项全部与现状一致——旧任务重跑应产出相同内容。
- `dngcolor`(WB)三档在两种映射下语义不变。
- 不新增第三方依赖;FFmpeg 仍为锁定 6.1,LibRaw 仍为 0.21.4。

## 8. 限制与已知问题(不阻塞,记录在案)

- 线性 16-bit 中间的深影分辨率约 12 stops(= Apple Log 有效域下限);高光 >1.0 削顶(与现状一致);R<0 噪声保真段因无符号输入不可达。
- transfer=unknown ⇒ Resolve/FCP 需手动选 Apple Log 输入色彩空间;与真实 iPhone 文件行为完全一致,非兼容性缺陷。
- 挂内置 LUT(lut3d 通道)时,其 YUV↔RGB 往返沿用 swscale 默认矩阵(601),对 2020 素材有轻微偏色——现状固有瑕疵,建议后续单独任务修(对 none 档同样受益)。
- Apple Log 与 Apple Log 2(2025-09,iPhone 17 Pro)传输曲线相同、色域不同;本方案实现经典 Apple Log(互操作目标:官方 LUT/Resolve/FCP 支持面最广)。

## 9. 工作量

| 部分 | 估计 |
|------|------|
| native(strspec + dngzip + transcode) | ~60-80 行 |
| Kotlin(model/FFmpeg/VM/UI/Settings/strings) | ~40 行 |
| 测试 + 文档 | ~40 行 |
| WSL 构建 + 真机验证(§6.3) | 半天 |

合计 **0.5~1 个工作日**。建议单独一个 commit/PR,标题建议 `feat(dngzip): Apple Log output mapping for DNG zip line`。

## 10. 未来扩展(不在本次范围)

- `dngmap` 键名刻意中性:后续可加 `rec709`(显示域直出)、Apple Log 2(同曲线换色域)等档位;
- 修 swscale 默认 601 矩阵瑕疵(none 档 + lut3d 通道);
- 中灰锚点微调选项(曝光归一化),当前依赖"clip white = 1.0"的自然归一。
