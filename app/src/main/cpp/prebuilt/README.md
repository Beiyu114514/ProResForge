# prebuilt/ —— FFmpeg 交叉编译产物 + LibRaw provenance manifest(架构/验收必读)

> 注:本目录当前**已随仓库存量拷入**(SDK Manager 分发体),见仓库根 `.gitignore`
> 里的 `app/src/main/cpp/prebuilt/` 条目——变更后新增的产物不会入库,请在干净机器上
> 用下面的脚本重新生成。若你拿到的副本缺该目录,构建会停在 CMake 的 FATAL_ERROR。

本目录由脚本 **`scripts/build-ffmpeg-android.sh`** 自动生成,内容是按 ABI 分目录的
FFmpeg 6.1 及其依赖(x264 / x265 / libvpx)与 **LibRaw 0.21.4**(DNG 序列 zip 解码,
见 `docs/INTERFACES.md` §10)的 Android 交叉编译结果:

```
prebuilt/
├── arm64-v8a/
│   ├── include/   # libavformat/libavcodec/... 的头文件 + libraw/*.h
│   ├── lib/       # libavformat.so libavcodec.so libavfilter.so libavutil.so
│   │              # libswscale.so libswresample.so libpostproc.so(这 7 个进 APK,
│   │              # postproc 是 libavfilter 的 DT_NEEDED,不链它设备上 dlopen 即失败)
│   │              # + libavdevice.so(ffmpeg 默认编出,本项目不用、不打包)
│   │              # libx264.a libx265.a libvpx.a + libraw.a + *.pc
│   ├── libraw-manifest-arm64-v8a      # ← 段A:LibRaw provenance manifest(per-ABI)
│   └── BUILDINFO                     # (脚本统一写在 prebuilt/BUILDINFO)
├── armeabi-v7a/
│   ├── include/
│   ├── lib/
│   └── libraw-manifest-armeabi-v7a
└── ...
```

> ⚠️ **目录里最重要的一句话:`lib/libraw.a` 存在 ≠ 线程安全。**
> LibRaw 0.21.4 在单线程(`LIBRAW_NOTHREADS`)构建下产出的是**同名文件、同批符号**,
> 目录存在性检查(老逻辑)发现不了任何异常;但它把 dcraw 的 `getbithuff` 位缓冲、
> `ph1_bithuff` / `cielab`(cbrt、xyz_cam)/ `pana_data` / `sony_decrypt` 编成
> **函数内 static(进程级共享)**,两个 worker 同时解同一帧会互踩缓存,输出帧每次
> 都不一样(host 侧 2/4 线程探针实测 sha 每 run 必变)。这正是 P0 级"存在性 skip"
> 漏洞:只有 provenance(构建来源)能区分 `raw_r`(线程安全)与 `raw`(单线程)两种产物。
> 因此从 2026-10-06 起,LibRaw 的准入判据从"文件在不在"升级为
> **per-ABI manifest 门禁**(段A),Release 构建必须过 manifest,不接受"看起来对"的 .a。

## 生成方式

```bash
# 全量(arm64 + armeabi-v7a)或单 ABI
NDK=/path/to/android-ndk ./scripts/build-ffmpeg-android.sh [arm64|arm|all]
```

* 需要在联网、装有 `git make yasm perl python3 cmake pkg-config` 的 Linux/macOS 上进行;
* 8 核机器全量(arm64 + armeabi-v7a)约 20~40 分钟;
* 脚本幂等:源码目录已存在则跳过 clone;**但 2026-10-06 起 LibRaw 不再"产物在就跳过"**:
  安装前先跑 provenance 校验(`libraw_provenance_ok()`),manifest 缺失/字段不符/
  `libraw.a` 实际 SHA-256 与 manifest 不一致/脚本自身 SHA 变化 → **删掉该 ABI 的
  raw 构建产物与该 manifest,强制从源码重建 `raw_r` target**,重建成功后再重新生成
  manifest。FFmpeg/x264/x265/vpx 的克隆幂等行为不变。

## LibRaw provenance manifest(段A 门禁的核心文件)

文件名:**`prebuilt/<abi>/libraw-manifest-<abi>`**(key=value 纯文本,# 开头为注释),
由 `write_libraw_manifest()` 在 **raw_r 真重建成功后**写入。三处独立消费它:
`scripts/build-ffmpeg-android.sh`(重建判定)、`scripts/verify-prebuilt.sh`(宿主机验收)、
`app/src/main/cpp/CMakeLists.txt`(configure 时门禁)、`app/build.gradle.kts`
Release 门禁(Kotlin 复读,Windows 上不依赖 bash)。

| 字段 | 含义 | 生成方式 |
|---|---|---|
| `abi` | 该 manifest 归属的 ABI,必须与 `ANDROID_ABI`/`abiFilters` 一致 | 脚本参数回填 |
| `api_level` | Android NDK platform(本项目固定 24) | `API_LEVEL` |
| `ndk_revision` | NDK `Pkg.Revision`(如 `27.0.12077973`) | 读 NDK `source.properties` |
| `libraw_tag` | LibRaw 源码 pin(固定 `0.21.4`) | `LIBRAW_TAG` |
| `libraw_cmake_sha` | LibRaw-cmake 脚本 pin(`eb98e43…`) | `LIBRAW_CMAKE_SHA` |
| `raw_r_target` | 必须为 `raw_r`(线程安全 target);单线程 target `raw` 一律拒绝 | 字面量 |
| `libraw_nothreads` | 必须为 `absent`(编译 flags 里不得出现 `LIBRAW_NOTHREADS`) | 从实际 cmake flags 判定 |
| `raw_r_cmake_flags` | 本次 raw_r 实际使用的 cmake flags(门禁复检不含 `LIBRAW_NOTHREADS`) | 脚本 flags 列表 |
| `threads_lib` | 线程库(固定 `pthread`,NDK 侧) | 字面量 |
| `script_sha256` | 生成该 manifest 的构建脚本自身 SHA-256(脚本变→强制重建) | `sha256sum "$0"` |
| `libraw_sha256` | `lib/libraw.a` 的实际 SHA-256(核心:文件被换/改,门禁必失败) | `sha256sum libraw.a` |
| `symbols_sha256` | 存档内**全量 defined symbol**(探测 + 规范化排序)后的 SHA-256 | `llvm-nm --defined-only` |
| `members_sha256` | 存档成员列表排序后的 SHA-256 | `llvm-ar t` |
| `tls_probe` | `LibRaw_TLS` 符号存在性(`present`/`absent`,信息性 warn,不单独 FAIL) | `llvm-nm` grep |
| `build_time_utc` | 生成时刻(UTC,ISO-8601) | `date -u` |
| `manual` | 必须为 `false`;手工替换产物一律写不下来 | 字面量 `false` |

> `manual=true` 永远只能来自**手动**把 `.a`/`.so` 拷进 `prebuilt/`(或手写 manifest)。
> 脚本只在真重建成功后才写 `manual=false` 的 manifest;一旦有人手工替换过 lib,
> 发布门禁直接拒绝并提示:**"BUILDINFO 不再允许手工替换,必须重跑
> scripts/build-ffmpeg-android.sh"**。`manual` 值若非 `true`/`false` 也判 FATAL
> (无法判定,宁可不放行)。

### 当前这份 prebuilt 的状态(2026-10-07 更新)

两份 per-ABI `libraw-manifest-<abi>` 由脚本生成(manual=false,`libraw_sha256` 与
磁盘上的 `lib/libraw.a` 实测一致),`BUILDINFO` 于 2026-10-07 按「产物在场」重建:

* CMake configure / Gradle Release 门禁在本副本上**应当通过**;若被拒绝,
  说明 lib 或 manifest 被改动过,按门禁输出重跑脚本即可;
* 重新生成产物只有一条正道:在联网 Linux/macOS 上重跑
  `scripts/build-ffmpeg-android.sh`(勿手工拷库进去;手工拷贝会一直被拦)。
  全量约 20~40 分钟/ABI;
* FFmpeg/x264/x265/libvpx 四个二进制的版本头已与脚本 pin 逐项对齐(见下表与
  `BUILDINFO` 的 provenance 说明);commit 级归属待下次完整重建复核。

### 验收脚本(宿主机,不依赖 NDK 全量构建)

```bash
# 1) 校验某个 ABI 的 prebuilt 目录(默认所有期望值从构建脚本 pin + 脚本自身 sha 取)
./scripts/verify-prebuilt.sh app/src/main/cpp/prebuilt/arm64-v8a
#   可用 --abi/--ndk-bin/--no-symbols/--print-manifest 调整;退出码 0=PASS,1=FAIL
./scripts/verify-prebuilt.sh app/src/main/cpp/prebuilt/arm64-v8a --ndk-bin "$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"

# 2) 段D:核对编译命令真的用了 -O3 -DNDEBUG(Release 优化级别硬化)
./scripts/check-native-flags.sh app/build/.cxx/Release/*/compile_commands.json --expect-release
```

`verify-prebuilt.sh` 的校验项:manifest 存在且可解析 → required 字段非空 →
`manual=false` → `libraw_nothreads=absent` → `raw_r_target=raw_r` →
`libraw_sha256` 与磁盘上 `lib/libraw.a` 实测 SHA-256 一致 → `abi`/`api_level`/
`ndk_revision`/`libraw_tag`/`libraw_cmake_sha`/`script_sha256` 与期望一致 →
`symbols_sha256`/`members_sha256` 用 NDK `llvm-nm`/`llvm-ar` 复算一致
(缺工具时除非 `--no-symbols`,否则报错退出——静默跳过等于没校验)。
`tls_probe` 只出告警。命中任一项即 FAIL,并打印该重跑哪条命令。

> 行为验证(2T/4T 多轮回合对比 sha 必须逐 run 不同 → 恒定)属**真机/host 探针**,
> 目前只能由 HOST_TEST/F 段覆盖,`prebuilt/` 里的 manifest 只是 provenance 凭证,
> **不代表已做过行为验证**。

## CMake 如何消费

`app/src/main/cpp/CMakeLists.txt` 按 `${ANDROID_ABI}` 取
`prebuilt/<abi>/include` 作为头文件搜索路径,并以 `add_library(... SHARED IMPORTED)`
+ `set_target_properties(... IMPORTED_LOCATION ...)` 的方式链接
`prebuilt/<abi>/lib/lib{avformat,avcodec,avformat,avutil,swscale,swresample,postproc}.so`
(共 7 个;postproc 必须留在链接图里,原因见 CMakeLists.txt:43-51),再静态链接
`lib/lib{raw,x264,x265,vpx}.a`(LibRaw/x264/x265/vpx)。

configure 阶段新增 **`prf-libraw-gate` 段**:除了老的存在性检查(`RAW_A`/`RAW_H`),
还要求同目录的 `libraw-manifest-<abi>` 存在,且 `abi == ${ANDROID_ABI}`、
`file(SHA256 ${RAW_A}) == libraw_sha256`、`manual` 不为 `true`、
`raw_r_target == raw_r`、`libraw_nothreads == absent`、flags 不含 `LIBRAW_NOTHREADS`。
任一不满足 → `FATAL_ERROR`,提示重跑 `scripts/build-ffmpeg-android.sh`。
同一文件的 `prf-release-flags` 段保证 Release 只可能是 `-O3 -DNDEBUG`
(段D:剥掉任何外部传入的 `-O0/-O1/-O2/-Os/-Oz/-Ofast`,含 `-ffast-math` 直接 FATAL)。

## 版本 pin(脚本固定,可复现)

`scripts/build-ffmpeg-android.sh` 头部把每个源码都 pin 到发布 tag / 提交 SHA(修订前
FFmpeg 跟 `n6.1` 分支 tip、其余跟默认分支,换机 clone 即可能产出不同版本 —— 2026-10-06
审计正是据此引入 pin + verify_pin),构建前由 `verify_pin()` 复核,不符即终止:

| 库 | pin | 许可证 | 说明 |
|---|---|---|---|
| FFmpeg | `n6.1.6`(libavcodec 60.31.102) | LGPL-2.1 / GPL-3.0(启用 libx264/x265/vpx 时整体按 GPLv3) | `--enable-gpl --enable-version3`;S-1:2026-10-07 按产物版本头从 n6.1.1 重 pin |
| x264 | `stable` @ `b35605ace3ddf7c1a5d67a2eb553f034aef41d55`(2025-06-08;上游无 tag) | GPL-2.0 | X264_BUILD 165,与现存产物一致 |
| x265 | `4.2` @ `e444744c03978c1fb4e037168967020cf2648427` | GPL-2.0(+patent grant) | X265_BUILD 216 与现存产物一致(4.1 是 215,不符) |
| libvpx | `v1.17.0` @ `6df3ec34557879fff673706f4a1d9fbd0f3a6f0e` | BSD-3-Clause | 与现存产物 vpx.pc 的 1.17.0 一致 |
| **LibRaw** | **`0.21.4`** | **LGPL-2.1 与 CDDL-1.0(双许可,取其一)** | LibRaw-cmake 脚本 pin `eb98e43`(上游无 tag) |

LibRaw 用 NDK sysroot 的 zlib 支持 deflate 压缩的 DNG(Compression=8/32946);
MotionCam 最常见的 lossless-JPEG(Compression=7)走 LibRaw 内置 LJpeg,无需外部
libjpeg。脚本 `LIBRAW_JPEG=ON` 才另编 libjpeg-turbo(默认 OFF)。

**`lib/libraw.a` 必须是线程安全版**(2026-10-06 起):脚本构建的是 LibRaw-cmake 的
`raw_r` target(不加 `LIBRAW_NOTHREADS`),产物装成 `libraw.a`(文件名是
`app/src/main/cpp/CMakeLists.txt` 的链接契约,不能改)。源码与编译参数和单线程
target `raw` 完全一致,唯一差别就是这个 define——所以**只能靠 manifest 的
`raw_r_target`/`libraw_nothreads` 字段证明,不能靠文件名或符号名证明**。
装成 `libraw.a` 之前脚本会校验它与 `raw_r` 产物 sha256 一致,防止装错。

为什么必须线程安全版:`app/src/main/cpp/dngzip.cpp` 的 `dng_decode_to_rgb48` 由多
worker 并发调用(`transcode.cpp:130 dng_parallel_params()` 默认
`nthreads = min(4, cores-1)`),而 LibRaw 0.21.4 在带 `LIBRAW_NOTHREADS` 时把上述
static 缓存布到进程级(见上文)。不带该 define 时这些状态走每实例 `LibRaw_TLS`
(`libraw.h:407 tls`,`libraw_internal.h:29`),线程安全。

## 如何核对"这份产物到底是什么版本"

脚本跑完会在 `prebuilt/BUILDINFO` 写一份自动生成记录(勿手改):

* 每个源码 pin 的 **实际 HEAD commit**(clone 后已与 pin 逐一校验);
* NDK 路径与 `Pkg.Revision`、API level、**脚本自身 sha256**;
* 从 `include/` 头文件里 grep 出来的**实际版本宏**(libavcodec x.y.z、X264_BUILD、
  X265_BUILD)——这是"证据",与 pin 期望不一致时说明源码树被动过或上游改了版本号。

**存量副本提示(2026-10-07 更新)**:本副本的 FFmpeg/x264/x265/libvpx 二进制由
更早的脚本版本构建,版本头已与上表 pin 逐项核对一致;commit 级归属以下一次完整
重建为准(自查:`grep FFMPEG_VERSION include/libavutil/ffversion.h`、`X264_BUILD`、
`X265_BUILD`、`lib/pkgconfig/vpx.pc` 的 `Version:`)。若发现版本头与上表不符,
说明 lib 被换过——先删掉对应 ABI 目录后用脚本重跑(约 20~40 分钟/ABI),再以新
`BUILDINFO` + `libraw-manifest-<abi>` 为准。若 `lib/` 里缺某个 `.so`/`.a`,解决
方式同样是重跑脚本——脚本会在缺静态库时直接 die,不再留"一半的产物"。

## 为什么不要提交

* 体积大(每个 ABI 约 30~60 MB),且二进制不可 diff;
* 与 NDK / FFmpeg 版本强绑定,应随脚本重新生成;
* 请在 `.gitignore` 中加入 `app/src/main/cpp/prebuilt/`(脚本与 CMake 都不依赖 git 历史)。
  仓库根 `.gitignore` 已按此配置。
