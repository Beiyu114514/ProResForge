# ProResForge 内部接口契约(唯一权威,所有代码以此为准)

目标:把 MotionCam 拍摄 raw 转出的 Apple ProRes(.mov / .mxf)转码为 H.264 / H.265 / VP9 / ProRes,
可嵌入 LUT(.cube / .3dl)烘焙进画面,支持批量队列、硬件编码优先、SAF 无拷贝 IO。

- 包名 / namespace / applicationId: `com.proresforge.app`
- 工程根目录: `ProResForge/`
- 最低 Android: 24 (API 24); target/compile: 35; Kotlin 2.0+; Compose BOM; AGP 8.7+
- native CMake target 名: `ffmpeg-transcode` → 产物 `libffmpeg-transcode.so`,
  Kotlin 侧 `System.loadLibrary("ffmpeg-transcode")`

## 1. 目录结构(各模块只写自己负责的部分)

```
ProResForge/
  settings.gradle.kts, build.gradle.kts, gradle.properties   [gradle 代理]
  gradle/libs.versions.toml, gradle/wrapper/*                [gradle 代理]
  app/build.gradle.kts, app/proguard-rules.pro               [gradle 代理]
  app/src/main/AndroidManifest.xml, app/src/main/res/**      [gradle 代理]
  app/src/main/cpp/**                                        [native 代理]
  scripts/build-ffmpeg-android.sh                            [native 代理]
  app/src/main/java/com/proresforge/app/**                   [kotlin 代理]
  docs/, sample-luts/, README.md                             [lead]
```

CPP 源文件固定为 4 个 `.cpp` + 4 个 `.h`: `native-lib.cpp`(JNI 入口)、
`transcode.cpp`+`transcode.h`(转码管线)、`preview.cpp`+`preview.h`(抽帧预览)、
`dngzip.cpp`+`dngzip.h`(DNG 序列 zip: ZIP 中央目录解析 + zlib inflate + LibRaw
单帧解码,见 §10)、`dngframeq.h`(§10.7 的并行解码帧队列,header-only,BEGIN 边界助手)。
`strspec.h`(spec k=v 解析,header-only)。
CMakeLists.txt 只允许引用这些文件名(`app/src/main/cpp/CMakeLists.txt:29-34`
正是这 4 个 .cpp + 上述头文件)。

## 2. JNI 契约(签名逐字一致,改一处两边同步)

Kotlin 侧 `com.proresforge.app.engine.FFmpeg`(object,单例):

```kotlin
package com.proresforge.app.engine

object FFmpeg {
    init { System.loadLibrary("ffmpeg-transcode") }

    interface Progress {
        /** pct: 0..100 进度; -1 表示该 stage 无百分比。stage 见 STAGES。 */
        fun onProgress(pct: Int, stage: String, frame: Long, totalFrames: Long)
    }
    /** @return 0 成功; 1 用户取消; 2 输出格式/编码器不支持; 3 输入文件损坏或无法解码; <0 = ffmpeg 错误码的相反数(AVERROR 取正) */
    external fun nativeRun(fd: Int, length: Long, outFd: Int, spec: String, progress: Progress): Int
    external fun nativeCancel(): Int
    /** 返回 "k=v;k=v" 点分串,失败返回空串。key 见 §4 */
    external fun nativeInfo(fd: Int, length: Long): String
    /** 抽 atUs 处一帧(seek 后取最近关键帧之后解码到 atUs),返回 JPEG 字节;失败返回 null。
     *  dngQuality/dngColor/dngMap 是 DNG 序列 zip 的解码档位(与 spec 的 dngq/dngcolor/dngmap
     *  同序),预览必须与转码同参数,否则预览画面和成品不一致;普通容器忽略。 */
    external fun nativePreview(
        fd: Int, length: Long, atUs: Long,
        lutPath: String?, lutIntensity: Double,
        graded: Boolean, maxW: Int,
        dngQuality: Int, dngColor: Int, dngMap: Int
    ): ByteArray?
    /** 把 AppLog 的日志文件绝对路径交给 native:FFmpeg av_log 从此与 Kotlin 共写该文件(O_APPEND)。 */
    external fun initNativeLogging(logFilePath: String)
    /** 只清「待生效」的取消标记。上一个任务在 nativeRun 之前就被取消时,取消位会残留,
     *  不清掉会让下一个任务开局即取消;转码运行中调它无效(那时取消的是当前任务)。 */
    external fun clearPendingCancel()
}
```

C 侧符号(注意 object 的方法仍是实例方法,JNI 第二参数是 jobject):

```
Java_com_proresforge_app_engine_FFmpeg_nativeRun(JNIEnv*, jobject, jint fd, jlong length, jint outFd, jstring spec, jobject progress)
Java_com_proresforge_app_engine_FFmpeg_nativeCancel(JNIEnv*, jobject)
Java_com_proresforge_app_engine_FFmpeg_nativeInfo(JNIEnv*, jobject, jint fd, jlong length)
Java_com_proresforge_app_engine_FFmpeg_nativePreview(JNIEnv*, jobject, jint fd, jlong length, jlong atUs, jstring lutPath, jdouble lutIntensity, jboolean graded, jint maxW, jint dngQuality, jint dngColor, jint dngMap)
Java_com_proresforge_app_engine_FFmpeg_initNativeLogging(JNIEnv*, jobject, jstring logFilePath)
Java_com_proresforge_app_engine_FFmpeg_clearPendingCancel(JNIEnv*, jobject)
```

### fd 语义(重要)
- fd 均为 Kotlin `contentResolver.openFileDescriptor(uri, mode)!!.detachFd()` 得到,native 拥有,
  native 内部用 `pread(fd,buf,n,off)` 读、`pwrite(fd,buf,n,off)` 写,自己维护偏移,用完 close(fd)。
- 输入输出都要自定义 `AVIOContext`(avio_alloc_context),read/seek/write/seek 都要实现;
  seek 必须支持 AVSEEK_SIZE(输入返回 length 参数,输出返回已写字节数)。
- 输入 avformat_open_input 的 url 传空串 `""`,并设 `ctx->pb`;探测容器后再打开。
- LUT 不传 fd:Kotlin 把选中的 LUT 文件暂存到 `cacheDir/luts/<sha256(uri)>.<cube|3dl>` 后传绝对路径,
  由 FFmpeg lut3d filter 的 `file=` 选项读取(依赖 `--enable-protocol=file`)。

## 3. spec 字符串(UTF-8,`\n` 分行,每行 `k=v`,值内不得出现 `\n`)

| key | 类型 | 说明 |
|---|---|---|
| `vcodec` | str | 首选编码器名,见 §3.2 fallback 链 |
| `pixfmt` | str | 输出像素格式:`yuv420p` / `yuv420p10le` / `yuv422p10le` |
| `w`,`h` | int | 输出宽高,0 = 保持源宽高(仍按 rotate/显示矩阵修正方向) |
| `fpsnum`,`fpsden` | int | 0,0 = 源帧率;否则 `fps` filter 固定 |
| `rc` | str | `crf`(质量优先) / `bitrate`(目标码率) / `q`(VP9 CQ) |
| `crf` | int | rc=crf 时使用 |
| `bitrate` | long | rc=bitrate 时(bps) |
| `gop` | int | 关键帧间隔秒,0 = 编码器默认(换算成帧数给编码器) |
| `profile` | str | 可空,如 `high` `main` `main10` |
| `lut` | str | LUT 绝对路径(cacheDir),空 = 不应用 LUT |
| `lutint` | double | 0.0~1.0,<1 时用 split+blend 与原帧混合;仅在 `lut` 行存在时写(无 LUT 时 `lut`/`lutint` 两行都省略,B11) |
| `scale` | str | `lanczos`(默认) / `bicubic` / `fast_bilinear` |
| `rotate` | str | `auto`(按显示矩阵,ProRes 常见) / `0` / `90` / `180` / `270` |
| `cspace`,`cprim`,`ctrc` | str | 可空输出色彩标签,如 `bt709`(记入流 metadata,不转换) |
| `audio` | str | `copy`(容器兼容则直接拷贝,必要时 bsf) / `aac`(重采样 48k stereo) / `drop` |
| `faststart` | int | 0/1,MP4/MOV 结束时二次扫描把 moov 前置 |
| `title` | str | 可空元数据 |
| `src` | str | `dngzip`(默认空=普通容器输入)。MotionCam 的 DNG 序列 zip: 输入是 ZIP,解出每帧 DNG 经 LibRaw 解码为 rgb48le 送 filtergraph,见 §10 |
| `dngq` | int | `src=dngzip` 时有效: LibRaw 画质档 `0`(快速,half_size=1+user_qual=0) / `1`(均衡,user_qual=2 PPG) / `2`(精细,user_qual=3 AHD)。默认 1 |
| `dngcolor` | str | `src=dngzip` 时有效: `camera`(相机/机身 WB,默认) / `auto`(LibRaw 自动 WB) / `d65`(固定日光,user_mul 全 1) |
| `dngmap` | str | `src=dngzip` 时有效: `none`(线性,sRGB 输出,默认) / `applelog`(Rec.2020 线性 + Apple Log OETF) |
| `dngthreads` | int | `src=dngzip` 时有效: 并行解码线程数,`0`(默认)= 按内存与核数自动推算,见 §10.7 |
| `dngdepth` | int | `src=dngzip` 时有效: 预取深度(已解码帧在队列里排队的上限),`0`(默认)= 2(host 复核过「随 `dngthreads` 联动」的提案,结论是不联动,见 §10.7.2) |

### 3.2 编码器 fallback 链(C 端按序尝试 avcodec_find_encoder_by_name,全失败返回 2)
- H.264: `h264_mediacodec` → `libx264`
- H.265: `hevc_mediacodec` → `libx265`
- VP9:  `libvpx-vp9`(无回退,必须编进 FFmpeg)
- ProRes: `prores_ks`(必须编进)

码率控制映射:
- libx264: `preset=medium`, rc=crf → `crf=<n>`; rc=bitrate → `bitrate=<bps>`+`nal-hrd=cbr` 可选;
  容器要求 even 尺寸。
- libx265: `crf=<n>` / `bitrate=<bps>`;pixfmt yuv420p10le 时 profile main10。
- libvpx-vp9: rc=q → `cq-level=<n>`, `cpu-used=4`, `row-mt=1`, `deadline=good`;rc=bitrate → `b=<bps>`。
- prores_ks: 用具名 profile —— `hq`(422 HQ,默认)/`standard`(422)/`lt`(422)/`proxy`(422) /
  `4444`(yuv444p10le)。真实枚举 `PROXY=0, LT=1, STANDARD=2, HQ=3, 4444=4`,
  **不要用数字**(早期契约写的 `profile=2=HQ` 与真实枚举错位一位,2 是 standard)。
  码率无意义可忽略。
- h264_mediacodec/hevc_mediacodec: rc=bitrate → `bitrate=<bps>`;rc=crf/q → 按
  `估算bps = crf→画面复杂度`(简单做法: 用源码率×(crf映射系数:crf18=0.35, crf23=0.18, crf28=0.10,
  线性插值) 给 bitrate;`bitrate_mode=vbr`(**下划线**,n6.1 选项名);`gop_size=fps*(gop/fps)`(取整到帧);
  `time_base=1/fps`;`max_b_frames=0`;只支持 8bit 4:2:0(10bit 请求会被编码器协商降级)。
  码率兜底:源码率<=0 时按 `w*h*fps*0.5` 估算,clamp [200kbps,80Mbps](实现取值)。
  不需要设置 profile(由像素格式决定)。

## 4. nativeInfo 返回 key(用于 UI 信息卡与 HDR 提示)
`codec`(如 prores) `codec_tag` `profile` `w` `h` `pixfmt`(如 yuv422p10le) `fpsnum` `fpsden`
`duration_us`(-1 未知) `bitrate` `nb_frames`(-1) `rotation`(0/90/180/270)
`container`(mov/mp4/mkv...) `audio_codec`(空=无) `audio_ch` `audio_rate`
`cspace` `cprim` `ctrc`(smpte2084/bic 空间时 UI 告警建议先加 tonemap LUT)
`frames`(-1 未知;DNG zip = ZIP 内 frame-NNNNN.dng 帧数,容器输入 = nb_frames)

### 4.1 DNG zip(MotionCam)识别
`nativeInfo` 读输入前 4 字节,是 ZIP 魔数 `PK\x03\x04` 时走 zip 分支: 按中央目录数
`frame-NNNNN.dng` 条目,首帧送 LibRaw 取**解包前的原始全尺寸** w/h(`codec=dng`,
`pixfmt=rgb48le`, `container=zip`,
`fpsnum=fpsden=0`, `duration_us=0`, `nb_frames=frames=<帧数>`)。
探测为节省时间仍使用 `dngq=0` 的 `half_size=1` RGB 缓冲,但该半尺寸只属于
探测内部临时像素缓冲,不得写入 `w`/`h`;转码路径的 `w`/`h` 仍取实际 RGB 缓冲尺寸。
不是 DNG zip(无匹配条目/首帧解不开)→ 返回空串,Kotlin 侧沿用"不可解析"终态。

## 5. STAGES(Progress.stage 取值)
`init`(打开输入/构建 filter)→ `decode`(音视频解码与重编码主循环)→ `audio`(音频阶段)
`finalize`(faststart/写尾)→ `done` / `error`。UI 直接展示 stage 文字映射(中文由 Kotlin 负责)。

## 6. FFmpeg 编译要求(native 代理负责,scripts/build-ffmpeg-android.sh)
- FFmpeg 6.1 LTS;**pin 到发布 tag `n6.1.6`**(libavcodec 60.31.102;S-1 整改:
  已按现存产物版本头与上游发布信息核对),不再跟分支 tip;
  x264 `stable`@`b35605ac`(X264_BUILD 165)、x265 `4.2`(X265_BUILD 216)、
  libvpx `v1.17.0`、`LibRaw-cmake`@`eb98e43` 同理
  (commit 以脚本头部的 pin 表为准,clone 后由 `verify_pin()` 复核,不符即终止),
  构建产物与 pin 的对应关系写入 `prebuilt/BUILDINFO`
- abi: `arm64-v8a`(必须)、`armeabi-v7a`(必须),脚本支持 `x86_64` 可选
- configure 必须包含: `--enable-gpl --enable-version3 --enable-libx264 --enable-libx265
  --enable-libvpx --enable-mediacodec --enable-swscale --enable-avfilter --disable-network
  `__`(*n6.1 的 `--enable-network=no` 不生效,configure 只 warn;正确写法是 `--disable-network`)*
- 解码: prores, pcm_s16le, aac, opus, vorbis, flac(尽量)
- 编码: libx264, libx265, libvpx-vp9, prores_ks, aac, mjpeg
- 过滤器: scale, format, setsar, setpts, fps, transpose, lut3d, lut1d, split, blend, buffersink,
  aresample, aformat(全部 default-on 的已含,显式确认)
- 协议: file(LUT 读取)
- ABI 输出到 `app/src/main/cpp/prebuilt/<abi>/{include,lib}`,.so 与 CMake IMPORTED 对应
- **APK 打包**:AGP 的 jniLibs 约定是 `<srcDir>/<abi>/*.so`,不接受 `<abi>/lib/` 嵌套。
  `app/build.gradle.kts` 已注册 `Sync` 任务 `syncFfmpegJniLibs`(展平 `prebuilt/<abi>/lib/*.so`
  → `build/generated/ffmpegJniLibs/<abi>/`)并注册为 `sourceSets.main.jniLibs.srcDir`;
  `cpp/CMakeLists.txt:98-103` 明写「不再做任何 `file(COPY())`」:AGP 只打包链接图内的
   SHARED IMPORTED 目标,兜底复制早已移除,不要再加回来(曾经的重复兜底会双份 .so 入库)。
  源码树默认放仓库外(`~/.prf-ffmpeg-build`,`PRF_BUILD_DIR` 可覆盖);`prebuilt/` 不上 git。
- x264/x265/vpx 用 NDK toolchain 交叉编译;Android API level 24;`-fPIC -O3 -fno-strict-aliasing`
- 脚本要幂等(存在则跳过下载/编译),输出 7 个库:`libavformat/libavcodec/libavfilter/libavutil/libswscale/libswresample/libpostproc`
  (`libavfilter.so` 的 DT_NEEDED 依赖 `libpostproc.so`,必须进链接图/APK,见 CMakeLists.txt:46-52)
- **LibRaw(DNG zip 转码必需,M1 起引入)**: 静态库 `libraw.a` + 头文件 `include/libraw/`,
  由同一脚本构建: `LibRaw/LibRaw.git@0.21.4`(源码)+ `LibRaw/LibRaw-cmake.git`(CMake 脚本,
  用 `-D LIBRAW_PATH=<源码>` 指向前者),NDK android.toolchain.cmake 交叉编译,
  `ANDROID_STL=c++_static`, `BUILD_SHARED_LIBS=OFF`, `ENABLE_EXAMPLES/OPENMP/LCMS/RAWSPEED/JASPER=OFF`;
  zlib 用 NDK sysroot 自带(脚本显式探测 `<sysroot>/usr/lib/<triplet>/libz.{a,so}`),
  libjpeg 默认关(MotionCam DNG 是 Compression=7 无损 JPEG,LibRaw 内置 LJpeg 可解;
   仅 Compression=8 的老基线 JPEG DNG 才需要,可用 `LIBRAW_JPEG=ON` 打开)。
   前提:该开关只有在「自备 libjpeg 能被 find_package 找到」时才可能生效
   (`build-ffmpeg-android.sh:470` 清掉 `CMAKE_DISABLE_FIND_PACKAGE_JPEG`,NDK sysroot
   无 libjpeg);单纯 `ON` 也可能因找不到包而静默不支持,别当成「开了就一定有基线 JPEG 解」。
  产物落在同一 `prebuilt/<abi>/{include,lib}`,CMake 新增静态 IMPORTED target `ffmpeg-raw`,
  `target_link_libraries(ffmpeg-transcode PRIVATE ffmpeg-raw z)`。
  app 不重分发 LibRaw 源码,随包发布的二进制须附版权声明:
  LGPL-2.1(LibRaw/LibRaw-cmake,`COPYING`/`LICENSE.LGPL`)+ dcraw(CDDL-1.0 的双许可,
  `LICENSE.CDDL`);`app/src/main/cpp/prebuilt/README.md` 已记录,新增 ABI 时同步更新。
  **LibRaw provenance 门禁(2026-10-06 整改,A 段)**:只看「`libraw.a` 存在」不再足够。
  脚本为每个 ABI 写 per-ABI manifest(prebuilt/<abi>/libraw-manifest-<abi>,key=value),
  字段含 abi / api_level / ndk_revision / libraw_tag / libraw_cmake_sha /
  raw_r_target=raw_r / libraw_nothreads=absent / raw_r_cmake_flags(不含 LIBRAW_NOTHREADS)/
  threads_lib / script_sha256 / libraw_sha256 / symbols_sha256 / members_sha256 /
  build_time_utc / manual=false;manifest 缺、字段不符、`libraw.a` SHA-256 不符或脚本自身
  sha 变化 → 删除该 ABI 的 `lib/libraw.a` + manifest 并从源码强制重建 `raw_r`。
  CMake configure(`app/src/main/cpp/CMakeLists.txt` L130-262 prf-libraw-gate 段)与
  Gradle release 门禁(`app/build.gradle.kts` L54-357 门禁 + L349-357 挂 release variants)
  都会校验 manifest;`manual=true` → GradleException「BUILDINFO 不再允许手工替换,必须重跑
  scripts/build-ffmpeg-android.sh」。`scripts/verify-prebuilt.sh`(新增)可独立复验。
  **当前副本状态(2026-10-07 更新,B-07)**:两 ABI 均有脚本生成的有效 manifest
  (manual=false,`libraw_sha256` 与实测一致),CMake configure / Gradle release 门禁
  在本副本上**应当通过**;FFmpeg/x264/x265/libvpx 的版本头已与脚本 pin 对齐
  (见 `prebuilt/BUILDINFO` 的 provenance 说明),commit 级归属以下一次完整重建复核。
- 脚本头部注释写清依赖(NDK、yasm/nasm for x264 arm 不需要;autoconf/pkg-config+python)

## 7. native 内部实现要点
- 解码器线程: `avctx->thread_count = 4; thread_type = FF_THREAD_FRAME|FF_THREAD_SLICE`(prores 安全)
- filter graph 自绘: buffer → (autorotate transpose) → scale(w,h,lanczos) → setsar=1 →
  format(pixfmt) → [lut3d (intensity<1 时 split+blend)] → fps(if fixed) → buffersink
- mediacodec 编码前需把 frame `pts` 改成编码器 time_base,并使用 `AV_PIX_FMT_MEDIACODEC` 无关:
  libavcodec 的 mediacodec 封装自动处理(直接 send_frame 即可,不需要 av_hwframe)
- audio copy: `avformat` 直通 packets;容器不兼容时走 aac 路径(aac_adtstoasc 等 bsf 处理)
- faststart: MP4/MOV 在写完后重新打开输出,把 moov box 前移(可用 ffmpeg 的 `avformat` 二次读写)
- 取消: 全局 `std::atomic<bool> g_cancel`,读 packet/写 frame 每个循环检查,取消时正常收尾并
  删除输出文件(C 端 unlink 输出路径需要 Kotlin 传的 outFd 不可 unlink → 由 Kotlin 侧负责删残file)
- 进度: 解码端用 `frame->pts` 换算 `av_rescale_q` 到 AV_TIME_BASE,与 duration 比较;
  音频 stage 单独报

## 8. Kotlin 层契约(kotlin 代理负责)

### 8.1 data/model.kt(单一数据类定义处,全 App 引用)
```kotlin
enum class OutFormat(val labelRes: Int, val ext: String, val mime: String) {
    H264(R.string.fmt_h264, "mp4", "video/mp4"),
    H265(R.string.fmt_h265, "mp4", "video/mp4"),
    VP9 (R.string.fmt_vp9,  "webm","video/webm"),
    PRORES(R.string.fmt_prores,"mov","video/quicktime")
}
data class FormatSpec(
    val format: OutFormat, val vcodec: String, val pixFmt: String,
    val supports10Bit: Boolean, val audioModes: List<AudioMode>,
    val crfHigh: Int, val crfMedium: Int, val crfLow: Int,   // H264/H265 三档 CRF(HIGH 画质最好=数值最小)
    val vp9CqHigh: Int, val vp9CqMedium: Int, val vp9CqLow: Int  // VP9 三档 CQ(B1/B2:不再是 crfQuality/vp9Cq 单字段)
)
enum class AudioMode { COPY, AAC, DROP }
enum class ScaleMode(val labelRes: Int, val shortEdge: Int) { ORIG(R.string.scale_orig,0), P1080(R.string.scale_1080,1080), P720(R.string.scale_720,720), P50(R.string.scale_50,-1), P25(R.string.scale_25,-2) }
enum class FpsMode(val labelRes: Int, val fps: Int) { ORIG(R.string.fps_orig,0), F60(R.string.fps_60,60), F30(R.string.fps_30,30), F24(R.string.fps_24,24), F25(R.string.fps_25,25), F48(R.string.fps_48,48), F50(R.string.fps_50,50) }
enum class QualityTier { HIGH, MEDIUM, LOW }
data class JobSpec(
    val inputUri: Uri, val inputName: String,
    val outputUri: Uri, val outputName: String,      // outputUri 必须已创建好(SAF CreateDocument)后传入
    val format: OutFormat, val tier: QualityTier,
    val scale: ScaleMode, val fps: FpsMode,
    val lutPath: String?, val lutName: String?, val lutIntensity: Float,
    val audio: AudioMode, val tenBit: Boolean, val faststart: Boolean,
    // B3:下面三个字段仅 DNG 序列 zip 消费(普通容器忽略);有默认值
    val dngQuality: DngQuality = DngQuality.BALANCED,
    val dngColor: DngColorMode = DngColorMode.CAMERA,
    val dngMap: DngMap = DngMap.NONE
)
enum class JobState { QUEUED, RUNNING, DONE, FAILED, CANCELLED }
data class JobRecord(
    val id: String, val spec: JobSpec, val state: JobState,
    val pct: Int, val stage: String, val frame: Long, val totalFrames: Long,
    val error: String?, val startedAt: Long, val endedAt: Long,
    val outSize: Long, val inSize: Long, val durationMs: Long
)
// B4:MediaFacts 字段表(nativeInfo 的 "k=v;k=v" 解析结果,contract §4):
//   codec: String            容器内首选视频流编码名(空 = 探测失败)
//   width/height: Int        显示宽高(>0,否则视为探测失败)
//   fpsNum/fpsDen: Int       容器帧率(未知为 0)
//   durationUs: Long         时长微秒(未知 -1)
//   bitrate: Long            码率 bps(未知 0)
//   nbFrames: Long           容器帧数(未知 -1;zip 场景与 frames 同值)
//   rotation: Int            容器/矩阵旋转角 0/90/180/270
//   container: String        容器格式名(如 mov,mp4;zip 为 "zip")
//   pixFmt: String           像素格式名
//   audioCodec: String?      音频编码(null = 无音轨)
//   audioChannels: Int       声道数(无音轨 0)
//   audioRate: Int           音频采样率(无音轨 0)
//   cspace/cprim/ctrc: String? 色彩标签(null = 容器未标)
//   isHdr: Boolean           cspace/cprim/ctrc 任一为 smpte2084/arib-std-b67
//   frames: Long = -1L       DNG zip 总帧数(nativeInfo "frames" 键;普通容器 -1)
//   isDngZip: Boolean(计算属性)codec=="dng" 或 container=="zip"
data class MediaFacts(/* 字段见上表;由 data/MediaProbe.parseInfo 从 nativeInfo 串解析 */)
```

### 8.2 其它 Kotlin 模块与硬性要求
- `engine/FFmpeg.kt`: §2 契约,`nativeRun` 须把 JobSpec 序列化成 spec 字符串(strspec.h 同款 k=v,\n 分隔)。
  bitrate 换算: H.264/H.265 rc=bitrate,bitrate = tier 对应 `Mbps * 1080p 折算 * 像素比 * fps比`;
  公式:`bps = baseMbps * (w*h)/(1920*1080) * (fps/30)`;CRF 档见 FormatSpec。
     ProRes 母版档: w/h 保持,crf 忽略(prores_ks 无crf),profile 按名取 `hq`/`4444`
   (真实枚举 PROXY=0,LT=1,STANDARD=2,HQ=3,4444=4,别用数字),lutint 照常。
- `engine/JobBus.kt`: object,`StateFlow<Map<String, JobRecord>>` +
  `fun enqueue(list: List<JobSpec>): List<String>`(返回生成的任务 id,供启动服务
  失败时回滚)+ `fun removeQueued(ids)`(仅移除仍 QUEUED 的记录,K-04 回滚用)+
  `fun cancel(id)` / 前台通知取消当前任务(广播 → Service → `JobBus.cancel(JobBus.currentId.value)`),
  队列串行执行由 Service 驱动;队列仅存内存(进程死后不恢复)。
  `data/JobStore.kt` 用 `JobStore.get(context)` 取进程内单例(多实例会互相覆盖同一 json)。
- `engine/TranscodeService.kt`: 前台服务,`foregroundServiceType="mediaProcessing"`,
  循环消费 JobBus 队列,每 job 调 `FFmpeg.nativeRun`(fd 用 detachFd);
  进度回调 → 更新 JobBus + 通知(节流 1s / 1%);`nativeCancel()` 由 cancel 触发,
  服务 `onDestroy()` 时也会触发(阻塞 JNI 不会被协程取消中断);
  失败/成功后写 JobStore 历史并删除残file `DocumentFile.fromSingleUri` canDelete。
- `data/SettingsStore.kt`: SharedPreferences 保存最近 format/tier/lut(audio/tenBit/faststart/
  dng 三项同);强度 lutIntensity 由 HomeViewModel 的 setter 即时落盘(B14:
  LutManager 只写 uri+名称,**不**写强度,见下条)。
- `data/LutFile.kt`: 只接受 `.cube/.3dl`(实现;_doc 曾提 .dat,已不作为目标);
  解析头部(TITLE / LUT_3D_SIZE / LUT_1D_SIZE / SIZE,数据行兜底)校验;`suspend fun stage(uri): StagedLut`
  写入 `context.cacheDir/luts/<sha256-of-uri>.<ext>`(tmp 带 nanoTime 后缀,避免并发互踩);
  LUT 上限 16MB;stage 成功时记录最近使用(uri + 名称,**刻意不含强度**,B14:
  强度滑条由 SettingsStore.lutIntensity 单独负责)到 SettingsStore。
- `data/JobStore.kt`: kotlinx.serialization,`filesDir/jobs.json`,最多留 200 条,提供
  `Flow<List<HistoryItem>>`;字段: id/时间戳/输入名/输出名/format/state/err/lutName/intensity/输入输出大小/耗时。
  坏档不清空(S-3):解析失败备份成 jobs.json.corrupt-<时间戳> 并打日志后从空历史开始。
- `data/OutputDir.kt`: SAF `OpenDocumentTree`(初始指向真实存在的 `primary:Movies/ProResForge`,
  若 MediaStore 查不到则交给系统选择器),持久化权限;`suspend fun outputUriFor(name): OutputDoc?`(B8:
  是 suspend;`OutputDoc(uri, name)` 里 name 取 provider 侧实际采用名,个别 provider 会自行去重);
  批量用 `suspend fun outputDocsFor(names): List<OutputDoc?>`(目录枚举一次,失败项为 null,
  调用方回滚用 `suspend fun deleteCreated(docs)`,L-新B);输出命名 `原名`(去掉旧扩展名,
  截到 60 字符)+ format 扩展名,重名在同一次批量入队内也追加 ` (2)`…`(99)`,之后退化为时间戳。
- UI(Compose Material3,默认中文,英文见 `values-en/`): MainActivity → AppNav(底部 3 tab: 转码/任务/历史) →
  HomeScreen(多选文件 + 信息卡 + 格式 + 质量 + 缩放帧率 + LUT(含前后对比缩略图,调用 nativePreview)+ 音频 + 输出目录 + 开始按钮)
  / JobsScreen / HistoryScreen(打开、分享、重试)。`ui/HomeViewModel.kt`: activity scope,
  `StateFlow<HomeUiState>`,文件选择、facts 解析(nativeInfo)、LUT 暂存、开始(预检 → enqueue)。
  入队 guard 用 `AtomicBoolean`(判定+置位原子,防重复入队);入队前有 preflight(输出目录未授权 /
  探测失败的文件直接挡住)。
  权限: POST_NOTIFICATIONS(33+)运行时请求(进程内只问一次)。

### 8.3 Manifest / res 归属(gradle 代理,契约在此固化)
- 权限: `FOREGROUND_SERVICE`, `FOREGROUND_SERVICE_MEDIA_PROCESSING`, `POST_NOTIFICATIONS`, `WAKE_LOCK`
- `<service android:name=".engine.TranscodeService" android:exported="false"
    android:foregroundServiceType="mediaProcessing" />`
- MainActivity `android:exported="true"` + LAUNCHER intent-filter;theme `@style/Theme.ProResForge`
- strings 默认中文(`values/strings.xml`),附 `values-en/strings.xml` 英文
- 主题 `Theme.ProResForge` parent `Theme.Material3.DayNight.NoActionBar`;自适应图标
  `mipmap-anydpi-v26/ic_launcher.xml` + `drawable/ic_launcher_foreground.xml`(矢量胶片图案)

## 9. 交付验收标准(lead 逐项核对)
1. `native-lib.cpp` 中 6 个 JNI 函数名/参数与 §2 一字不差:
   `nativeRun` / `nativeCancel` / `nativeInfo` / `nativePreview` / `initNativeLogging` /
   `clearPendingCancel`(`scripts/check-contracts.sh` 每次自动核对)
2. `app/src/main/java/com/proresforge/app/engine/FFmpeg.kt` 的 external 声明与 §2 一致
3. CMake 引用的源文件集合 == `native-lib.cpp transcode.cpp preview.cpp dngzip.cpp`,链接 prebuilt 同名 abi
4. Manifest service 全限定名 `.engine.TranscodeService` 与 Kotlin 类路径一致
5. app/build.gradle.kts 的 externalNativeBuild 指向 `src/main/cpp/CMakeLists.txt`
6. Kotlin 编译单元不引用不存在的类(所有跨文件引用以 §8 为准)

## 10. DNG 序列 zip(MotionCam)专项契约

输入是 MotionCam 拍完导出的 `.zip`: 里面有 ~900 个 `frame-00001.dng`(零填充 5 位),
单帧 4K/4444XQ 无压缩 DNG ~10MB+ 量级。整条链不落临时文件、不解压到 cacheDir,
全程内存: SAF fd → ZIP 中央目录解析 → zlib inflate 单帧 → LibRaw → rgb48le AVFrame →
既有 filtergraph → prores_ks(.mov)。设计取舍见 `docs/DNGZIP-PROGRES-PLAN.md`。

### 10.1 为什么不用 FFmpeg 自带 demuxer
- FFmpeg 6.1 的 `tiff.c` 对 DNG 只能给 CFA Bayer 帧(`AV_PIX_FMT_BAYER_*16`),没有解马赛克、
  没有相机 WB、没有色彩矩阵、不跑 DNG opcodes;DNG tiled 路径直接 `AVERROR_PATCHWELCOME`。
- FFmpeg 7.1 才有 `libraw` demuxer,但会违反 §6 的 6.1 锁定 → 自己链 LibRaw 0.21.x(§6)。

### 10.2 `dngzip.h` / `dngzip.cpp`(native 侧新增文件)
```cpp
namespace proresforge {
struct DngZipEntry { std::string name; uint16_t method; uint64_t csize, usize, lho; };
class DngZip {
public:
    static bool is_zip(const uint8_t *head, size_t n);   // 头 4 字节 PK\x03\x04
    int  open(int fd, int64_t len, std::string &err);    // 解析中央目录,按帧序号自然排序
    size_t frame_count() const;                          // ^frame[_-]?\d+\.dng$ 或回退的全部 .dng
    const DngZipEntry &entry(size_t i) const;            // 已按自然序排好(不是字典序、不是 lho 序)
    int  read_frame(size_t i, std::vector<uint8_t> &out, std::string &err); // 单帧解压到内存
    bool has_audio() const;                              // basename=audio.wav 时为真
    const DngZipEntry &audio_entry() const;
    int  read_audio(std::vector<uint8_t> &out, std::string &err); // audio.wav 解压到内存
private:
    int m_fd = -1; int64_t m_len = 0;
    std::vector<DngZipEntry> m_frames;
    DngZipEntry m_audio;
    bool m_have_audio = false;
};
}
```
字节级规则: 尾部 64KB 窗口扫 EOCD(`0x06054b50`,总条目@10,CD 偏移@16);
ZIP64 sentinel `0xFFFFFFFF` 时读 ZIP64 EOCD locator(`0x07064b50`,EOCD 前 20 字节)→
ZIP64 EOCD(`0x06064b50`,条目数在 locator+32)。中央目录项 `0x02014b50`: method@10,
csize@20/usize@24(8B ZIP64 走 extra id 0x0001,字段顺序不保证)、name_len@28/extra_len@30/
comment_len@32、lho@42。本地头 `0x04034b50`: name_len@26/extra_len@28,
数据起点 = lho + 30 + name_len(**本地** extra_len)(本地 extra 可能与中央目录不同,必须按本地算)。
inflate 用 `inflateInit2(&strm, -15)` 裸 deflate,按 1MB pread 喂,`inflateReset` 复用同一个 z_stream;
解出长度必须 == usize。**不许** mmap 整个 zip、**不许**写临时文件。

**条目匹配与回退(2026-10-04 真机反馈「导入 zip 后无法解析」后修订)**:
1. 主规则 `^frame[_-]?\d+\.dng$`(大小写不敏感)—— 位数**不写死**。
   历史约定是 `frame-#####.dng`(连字符 5 位),但不同导出工具/版本会给
   `frame_000000.dng`(下划线 6 位)、`frame-1.dng`(1 位)、`raw/frame-00001.dng`(带目录)。
2. 一个 `frame-*` 都没命中时,**回退成 zip 内所有 `.dng` 条目**(usize>0 或 csize>0
   或 method==8),仍按自然序。别的 `.dng` 不被误掺进来:只要命中过 `frame-*`,就只用它们。
3. 非 `.dng` 的条目(readme/元数据)一律忽略; basename 为 `audio.wav` 的成员单独保留为音频输入。
4. 全链路诊断走 `av_log`(native-lib 的 `pf_av_log` 落 logcat + app.log,用户点「日志」可见):
   `open()` 失败、0 帧(附前 8 个条目名)、命中帧数、回退帧数都会打印。
   **禁止**再出现「探测失败却一条日志都没有」的状态。
5. 单帧解压失败仍是 `AVERROR_INVALIDDATA` + 明确帧号(见 §10.3),不静默跳帧。

### 10.3 转码管线(`transcode.cpp`)
- `run_transcode` 读 spec `src=dngzip`(在读 `src` 之前就要判断),是 zip 就跳过 avformat_open_input:
  `DngZip::open(fd,length)` → 解出首帧 `LibRaw::open_buffer` → `unpack()` + `dcraw_process()`,
  用 `imgdata.sizes.width/height`、`imgdata.colors`、`imgdata.params` 建 filtergraph。
- source buffer 参数改为 `buffer=video_size=WxH:pix_fmt=rgb48le:time_base=1/fps:sar=1`
  (DNG 无旋转,不做 autorotate);`fpsnum/fpsden` 必须由 spec 给(zip 没有容器帧率,Kotlin 强制用户选,默认 25)。
- 每帧: `open_buffer` → `unpack` → `dcraw_process` → 把 `imgdata.image[i][0..2]`
  (每像素 4 个 ushort 交错,取前 3 通道)逐行拷进 `AV_PIX_FMT_RGB48LE` 帧(`av_frame_get_buffer`),
  `pts=i`(time_base 1/fps)→ `av_buffersrc_write_frame` → 既有 `drain_graph`/`encode_out`;
  每帧结束 `recycle()`(不 recycle 会按帧累积内存)。
- **并行解码**(2026-10-05,见 §10.7):解码占整条链 ~86% 耗时,所以每帧的
  `read_frame + dng_decode_to_rgb48` 搬到 `DngFrameQueue` 的 N 条工作线程里做,
  主线程只按帧号顺序 `next()` 取帧 → 滤镜图 → 编码 → 写盘。帧号严格有序交付,
  失败/取消语义与串行版完全一致(不静默跳帧、错误带帧号)。
- 音频:ZIP 内嵌 `audio.wav` 会先解压到受限内存缓冲,再通过 seekable memory AVIO 交给 FFmpeg WAV demuxer;不写临时文件。
- 色彩标签: 输出 `cprim=bt709` + `ctrc=bt709`(sRGB 的 transfer 名字就是 `bt709`)、
  `cspace=bt709`,与 Kotlin 侧普通路径一致。
- `dngmap=applelog`:LibRaw 使用 `output_color=8` 生成 Rec.2020 线性 RGB,filtergraph 应用 Apple Log OETF 后以
  `bt2020nc` 矩阵转 YUV;流标记为 `primaries=bt2020`、`colorspace=bt2020nc`、`transfer=unknown`。
  MOV/MP4 muxer 使用 `+write_colr` 写 `nclc 9-2-9`。
- 取消沿用 `g_cancel`(每帧边界检查);进度 `total_frames = DngZip::frame_count()`;
  faststart/STAGES/on_progress 全部复用,不新建第二套。
- 单帧解压失败: 返回 `AVERROR_INVALIDDATA`,错误消息明确 `zip 第 N 帧(frame-NNNNN.dng)解压失败`,
  **不许**静默跳帧。
- 16bit → 10bit 交给既有 scale/format 链,native 不做手工量化。

### 10.4 LibRaw 参数(按 spec `dngq` / `dngcolor` / `dngmap`)
| spec | LibRaw 参数 |
|---|---|
| `dngq=0` 快速 | `half_size=1`, `user_qual=0` |
| `dngq=1` 均衡(默认) | `user_qual=2`(PPG) |
| `dngq=2` 精细 | `user_qual=3`(AHD) |
| `dngcolor=camera`(默认) | `use_camera_wb=1` |
| `dngcolor=auto` | `use_auto_wb=1` |
| `dngcolor=d65` | `user_mul[0..3]=1,1,1,1`(不动相机 WB) |
| `dngmap=none`(默认) | `output_color=1`(sRGB,保持现有输出) |
| `dngmap=applelog` | `output_color=8`(Rec.2020 线性);曲线在 filtergraph 中编码 |
| 三者通用 | `output_bps=16`; `no_auto_bright=1`(**必须显式**,
  LibRaw 默认会给半尺寸解算做提亮,改变曝光语义); `gamm[0]=gamm[1]=0` |

`applelog` 档在 `setsar=1` 后、`format=<out_fmt>` 前插入 `format=gbrp16,lutrgb` 逐通道 Apple Log OETF,
再用 `scale=out_color_matrix=bt2020nc` 做 RGB→YUV 的 Rec.2020 非恒定亮度矩阵转换。
Apple Log 曲线常数和数值锚点见 `docs/APPLELOG-MAPPING-PLAN.md` §3.3。

### 10.5 预览(`preview.cpp`)
`nativePreview` 读前 4 字节,`is_zip` 为真走 zip 分支:`atUs` 语义变成**帧下标**
(0 基),解出该帧 → 既有权制/缩放/LUT → mjpeg JPEG。首帧失败时按 §10.3 的空串失败态返回
(UI 显示无预览),**不新增 JNI 签名**(§2 是契约)。zip 分支的失败同样要 `av_log`。

### 10.6 Kotlin / UI
- 文件选择器 mime 增补 `application/zip`、`application/x-zip-compressed`;
  `PROBEABLE_EXTENSIONS` 增补 `zip`;ZIP 带 jpg 内嵌预览图也走"不可解析"终态(只认 DNG 序列)。
- `MediaFacts` 增 `frames`(zip 帧数);zip 的 `fpsnum/fpsden` = 0 → `FpsMode.ORIG` 不可用,
  强制用户选 24/25/30/48/50/60(默认 25);zip 参数卡给 dngq 三档(快速/均衡/精细)、
  dngcolor 三档(相机WB/自动WB/固定日光),缩放仍走 `ScaleMode`。
- `FFmpeg.buildSpec` 对 zip 源追加 `src=dngzip`、`dngq=<0|1|2>`、`dngcolor=<camera|auto|d65>`、
  `dngmap=<none|applelog>`(默认 `none`);
  `FpsMode` 需新增 25/48/50 三档(默认 25)。`dngthreads`/`dngdepth` 不写(默认自动),Kotlin 不暴露旋钮。
- 宽或高 > 4096 时 UI 明确告警(R3: 6K/8K 内存风险)。
- 自然排序遇到零填充位数不一的混杂命名时退回字典序(以播出的帧顺序为准)并在 UI 提示。

### 10.7 并行解码(`dngframeq.h`)
**动机**:2026-10-05 真机反馈「dng 序列转码吃不满线程,arm 设备只能看到单核在跑」。
实测(4096x3072 Apple Log DNG,BALANCED):读帧 0.056s + 解码 0.351s = **0.407s/帧**,
解码占 86%;串行时主线程被 decode 独占,只剩滤镜/编码那 14% 能用第二个核。

**结构**:`DngFrameQueue`(仅头文件,app/src/main/cpp/dngframeq.h)。
- 生产者是 N 条工作线程,派号 `m_assigned++`,解完按 `seq % depth` 挂进环形槽位并
  `notify` 消费者;消费者(主线程)`next()` 只认队首 `m_consumed` —— **帧号严格有序**,
  等价于串行版的输出(progress/时间戳/写盘顺序都不变)。
- 槽位占用判定用「未完成帧数 `assigned - consumed < depth`」而不是 ready 数:解码中的帧也占槽。
- 失败:任一帧解不开 → `m_failed` + 错误信息,`next()` 立刻返回 -1(不死等、不跳帧),
  主线程按 §10.3 老规矩 `AVERROR_INVALIDDATA` + 帧号失败。
- 取消:`cancel()` 置标志并唤醒两侧 cv,`next()` 返回 `kNextCancelled`;`stop()` 先 cancel 再 join,
  所以**不存在调用方忘了 cancel 就 join 卡死**的路径。析构函数也调 stop(双保险)。
- **返回值契约(2026-10-06,L-5/L-8)**:`next()` 返回 `int` —— `0`(kNextOk)成功取到帧;
  `1`(kNextCancelled)已取消(err="已取消");`2`(kNextDrained)队列已取空
  (err="队列已取空(正常收尾)");`<0` 失败(err 有原因)。区分 cancelled 与 drained 是必须的:
  调用方(transcode.cpp:1781-1800)按码分流,drained 只应出现在固定 `i < total_frames` 循环之外
  (属派帧侧 bug,直接报 AVERROR_INVALIDDATA),正常收尾**不会**被误显成「已取消」。
  `start()` 返回 `bool`:false = 没起来(原因走 `last_error()`),调用方置 result=2 后结束;
  `next()` 开头有 `m_started` 守卫:`stop()` 之后/`start()` 之前调用返回 -1(否则 `m_slots`
  已 clear,按下标访问空 vector = UB)。
- 线程栈:pthread 显式创建 + **8MB 栈**。不能用 `std::thread`(拿不到栈大小)——
  Android bionic 给新建原生线程的默认栈只有 **1MB**,而 `dng_decode_to_rgb48()` 里的
  `LibRaw lr;` 实测 `sizeof` = 767,320 字节,同一帧解码实测 **768KB 栈就 SIGSEGV、850KB 才跑通**
  → 1MB 默认只剩 ~15% 余量,NDK Release 的 arm64/armv7 编法稍一变就可能翻过去。
  `pthread_attr_init/setstacksize/setdetachstate` 任一失败都 **fail-fast**(2026-10-06 L-6):
  `start()` 记 `m_err` 并返回 false,调用方 result=2 结束 —— 拒绝「静默退回默认 1MB 栈跑
  LibRaw」,那正好落在实测会 SIGSEGV 的区间里。

**定参(dngthreads/dngdepth,默认 0 = 自动)**:
- 内存优先:LibRaw 解一帧的 `VmHWM` ≈ rgb48le 字节数 × 2.95 + 16MB
  (4096x3072 → 72MB rgb → 213MB 峰值)。N 路并行要 N 份,所以线程数由 **1GB 预算** 反推,
  封顶 4,再封顶 `cores-1`(给主线程的滤镜+编码+写盘留一个核)。实测 4096x3072 开 4 线程
  `VmHWM` ~500MB(bionic/scudo 的 malloc 复用率比 glibc 还高),预算里已留一倍余量给
  编码器/IO buffer;`dngthreads>0` 可强制指定(调参/绕过低内存机型)。
- depth 固定 2:depth=1 会退化成串行(帧 i+1 必须等帧 i 被取走才开始解,实测 433ms→424ms
  没有重叠);depth=3/4 相对 2 几乎没有收益,却每帧多占 72MB。2026-10-06 P1-1 用 60 帧矩阵
  复核「默认 depth 是否随线程数联动(clamp(nthreads,2,cap))」,**结论:不联动**,默认仍 2
  —— 实测定量见 §10.7.2。
- 单核设备降级成 1 线程(仍正确,只是没并行);`dngdepth<2` 被抬到 2。

**实测(主机 x86_64,12 核,WSL 9p 读盘会低估并行收益;下表数字 2026-10-05 修掉死锁后
用仓库现版本头文件重测,与修前一致)**:

| 配置 | 每帧耗时 | 内存 VmHWM |
|---|---|---|
| 1 线程(串行基线) | 419 ms | 213 MB |
| 2 线程 / depth 2 | 235 ms(1.78×) | 421 MB |
| 3 线程 / depth 2 | 247 ms | 460 MB |
| 4 线程 / depth 2 | 239 ms | 496 MB |
| 2 线程 / depth 1 | 424 ms(**没有重叠,别用**) | 253 MB |

纯 LibRaw 解码(同一份内存帧,排除 IO)扩展性更好:368ms → 212ms(2T) → 124ms(4T) → 117ms(8T),
也就是说**上限不在 LibRaw 而在主机 9p 读盘**;真机是本地 fd 直读,应按更接近纯解码的曲线走。

**buffer 复用(重要,别改回 move)**:槽位里的 `raw`(15.7MB)与 `rgb`(72MB)capacity 跨轮复用,
`next()` 用 **swap** 而不是 move 把 buffer 还给槽位 —— 335 帧少 670 次大块 mmap/munmap,
峰值内存也更稳。

**验证**:单测 `~/dzwork/harness/src/test_frameq.cpp`(host,不依赖 FFmpeg/LibRaw):
顺序交付 300 帧无错位 / 3 线程 >1.6×加速 / 第 7 帧失败后消费端停在 7 且错误信息原样带回 /
失败态再次 next 不死等 / cancel 立即生效 / total=3 depth=16 边界 / 不调 stop 直接析构不卡死 /
**§6 慢解码(150ms)× 2/3/4/8 线程 40 帧全交付、看门狗未触发**(§10.7.1 死锁的回归)/
**§7 `thread_count()` 在 start 后为真实线程数、stop 后为 0**。
**TSan 全绿**(`setarch -R` 绕开 WSL 的 TSan 映射问题)。
停滞诊断(`dngframeq: 等第 N 帧已 Xs 无进展...`)由 `test_stalllog.cpp` 单独验证会打。

#### 10.7.1 真机「卡在转码中(初始化)」死锁 —— 根因与教训(2026-10-05)
**症状**:交付并行化 APK 后真机转码**零进度**停在 `stage_init`(`transcode.cpp:1086 vp.rep(0,"init",0)`
之后,唯一的 `"decode"` 进度要等 `push_decoded` 推进,所以 init = **一帧都没进滤镜图**)。
主机的单测/bench 全绿,真机必现 —— 典型的 interleaving 依赖 bug。

**根因(一个 `size()` 数错了容器)**:
- `dngframeq.h:118-124 int thread_count() const { return (int)(m_workers.size() + m_threads.size()); }`
   —— 修复前只写 `return (int)m_workers.size();`,POSIX 路径下线程全 push 进 `m_threads`,
   `m_workers`(std::thread 退路)永远空,所以那时 `thread_count()` **恒为 0**。
- `start():190 if (thread_count() == 0) m_no_workers = true;` 于是**恒真**,
  `next()` 开头每次都先在自己(消费)线程上跑 `produce_one()`:
  环满(`assigned - consumed == depth`)时它阻塞在 `m_cv_prod` 等空位,
  而 ready 的队首帧应该由它去 `m_cv_cons` 取;worker 也都在等空位
  → **全部线程睡在 `cv_prod`,`cv_cons` 无人问津 = 死锁**。
- 为什么 bench/单测没抓到:单测 decode 只要几 ms(消费线程几乎总能立刻拿到空位),
  真机一帧 350ms,`start()` 返回后 worker 先把两个槽占满,消费线程第一次 `next()`
  就进 `produce_one()` 等空位 → 立刻死锁。**bench 用的还是老版本头文件**
  (WSL 侧 `/mnt/c/.../dzwork` 的副本没跟着仓库更新),所以连《实测》表都是旧代码的数据。

**修法(3 处,`app/src/main/cpp/dngframeq.h`)**:
1. `thread_count()` 改成 `m_workers.size() + m_threads.size()`(POSIX) —— 真实线程数,
   `m_no_workers` 只在**真的**一条线程都没起来时才置位。
2. depth 钳制补回 `if (m_total && m_depth > m_total) m_depth = m_total;`
   (原来那行 `m_depth = m_total ? m_depth : 1;` 是 no-op,等于没钳)。
3. `next()` 的等待由 `wait` 改 `wait_for(15s)` + 谓词循环:语义不变,但万一再出
   「没人唤醒」的回归,每 15s 打一条带现场数据的诊断
   (`dngframeq: 等第 N 帧已 Xs 无进展(线程/depth/已派/已取/失败)`),不会静默挂死。

**复现与验证**(host,`~/dzwork/harness/src/test_deadlock.cpp` / `test_deadlock2.cpp`):
- 修前:nt ∈ {2,3,4,8} × 2 次 = **8/8 全挂**,`consumed=0`、两条 ready 帧无人取、
  看门狗 3s 打印 `thread_count()=0`(明知 4 条 pthread 活着)。
- 修后:nt ∈ {1,2,3,4,8} 全部 `DONE consumed=300`;12 次重复 nt=2/depth=2 零挂;
  depth=8 也正常;单测 + TSan 全绿;真实 5.22GiB zip bench 与修前一致
  (1T 418ms / 2T 235ms / 4T 239ms,VmHWM 213/421/496MB)。
- **教训**:① 头文件里「数线程」这种小函数必须同时覆盖 pthread 与 std::thread 两条路径,
  否则降级分支会被永久打开;② host 测试通过 ≠ 真机通过,慢 IO/慢解码会暴露只在
  特定 interleaving 下出现的死锁;③ 测试用的头文件要和仓库保持同步(本次差点用旧副本下结论)。

#### 10.7.2 P1-1/P1-3:depth 不联动 + AVFrame 跨帧池化(2026-10-06,方案 P1-1 / P1-3)

**P1-1 取证矩阵(host x86_64,AMD Ryzen 5 7500F 6C/12T,WSL 12 vCPU,24GB RAM;输入 60 帧
4096×3072 zip,每帧 DNG 15.7MB;`/usr/bin/time -v` 报 Elapsed 与 MaxRSS;负载 2.4–3.1,
单次跑有噪声,定论以「受控 round-robin 重复」为准)**:

| dngthreads | depth | Elapsed(s) | user(s) | CPU% | MaxRSS |
|---|---|---|---|---|---|
| 2 | 2 | 137.08 | 318.52 | 238% | 935,552 KB |
| 2 | 3 | 103.49 | 275.68 | 274% | 1,064,956 KB |
| 2 | 4 | 118.92 | 306.16 | 264% | 1,207,480 KB |
| 2 | 6 | 131.98 | 355.35 | 275% | 1,379,640 KB |
| 4 | 2 | 48.14 | 141.90 | 309% | 969,428 KB |
| 4 | 3 | 39.18 | 140.01 | 375% | 1,047,016 KB |
| 4 | 4 | 107.75 | 250.84 | 240% | 1,335,332 KB |
| 4 | 6 | 146.00 | 372.39 | 260% | 1,515,160 KB |

12 帧受控 round-robin 4 轮(同一二进制,交替跑,噪声影响互相抵消):T=2 下 depth=2 为
18.5/16.3/23.5/23.7s,而 depth=3 为 40.7/39.1/37.2/37.1s(**d3 慢 ~2×,user CPU 同步涨**);
T=4 下 depth=2 23.9/15.7/25.1/23.1s vs depth=4 30.0/33.5/30.0/33.5s。
**结论:d 越大一致地更慢,瓶颈不在解码 worker(线程级采样:多出的 CPU 100% 在 `prores_ks`
帧线程编码内部,主线程 + 4 worker 各自只 ~4.5s),且每个深度槽位 +130~160MB RSS。**
表中 T=2/T=4 的 d3「更快」是单次负载噪声,与受控重复方向相反,**不满足 ≥10% 门槛 →
`dng_parallel_params` 不改,默认 depth 仍 2**;`dngdepth>0` 强制优先、`<2` 抬到 2、`>8` 钳到 8
的语义不变。host 全管线下 2+ 解码 worker 反而比 1 worker 慢 ~3×(12 帧 sweep:T=1 7.5s →
T=2 17.6~24.3s),user CPU 同步涨 2.3×,判定为宿主 SMT 拥挤 + 9p 读盘,不是 decode bug
(decode-only 微基准 1T 419ms → 2T 235ms 健康)。

**P1-3 第一步(AVFrame 跨帧小池)**,编译宏 `PRF_AVFRAME_POOL`
(**当前代码默认 0,2026-10-06 整改;此处原文「默认 1」为整改前历史口径**,`#else` 保留原逐帧
`av_frame_get_buffer` 路径,便于 A/B 与回退):
> **当前默认值口径(2026-10-06 整改)**:发布默认已由 1 改为 **0**
> (`app/src/main/cpp/transcode.cpp:216-218`)。依据即下方 host A/B 负收益证据原文
> (墙钟慢 1.4~1.7×、RSS +220MB)与 arm64 真机证据缺失;开启(=1)准入条件见本节末尾真机协议。
> 宿主(A/B)证据一律按「历史/宿主证据」保留,不外推真机。
- *所有权结论*(`libavfilter/buffersrc.h` 对 `AV_BUFFERSRC_FLAG_KEEP_REF` 的文档 + 反推):
  flags=0 时 `av_buffersrc_add_frame_flags` 走 `av_frame_move_ref`,**buffer 所有权整体交给
  滤镜图**,我们的壳被 `get_frame_defaults` 重置(老路径里随后的 `av_frame_unref` 是 no-op);
  flags=`AV_BUFFERSRC_FLAG_KEEP_REF`(8) 时走 `av_frame_ref` 建一份新引用(计数 +1),
  **壳继续持有 buffer**,池化才成立。
- *安全复用判据*:`av_buffer_get_ref_count(f->buf[0]) == 1`(只有池持有)才原地覆盖写;
  推帧后计数 2,`drain_graph` 消费完回到 1。计数仍 >1 时该壳退化成
  `av_frame_unref` + `av_frame_get_buffer` 重新分配(正确回退,不写滤镜图还在引用的 buffer)。
  `av_frame_get_buffer` 不检查壳上已有 buffer,所以判据必须自己先做。
- *池大小*:N = depth+2(4096×3072 ≈ 72MB/槽,按 1GB 预算的 1/4 封顶 → N=3;1080p N=4;
  8K N=1),帧数比 N 少时按帧数收。N 不是正确性条件(引用计数兜底),只是余量。
- *host A/B 实测*(60 帧,interleaved 2 轮):nopool 74.01s/183.29user/975,980KB 与
  38.93s/130.47user/963,792KB;pool 126.33s/337.97user/1,207,160KB 与
  132.99s/349.90user/1,202,952KB;池计数「原地复用 57 / 重新分配 3」。minor page faults
  -22%(572k vs 737k),但墙钟慢 1.6~1.7×、RSS +220MB —— **总 user CPU 与 nopool 持平甚至更少,
  墙钟却更长**,线程级采样显示多出的 CPU 全在编码线程内部。
  「KEEP_REF clone vs move」对照实验(xmove:池壳轮转但 push 前 `av_frame_unref` 让 buffersrc
  走 move)与 pool 同档且都差于 nopool,说明负收益来自镜像 A/B 的拓扑(编码线程主导),
  不是 memcpy 侧;内存微基准:每帧 72MB alloc+首触边际仅 ~39.6ms(60 帧 ≈ 2.4s 理论收益),
  撑不起观察到的差(默认值判定依据见下一条)。
- *默认值 = 1 的判定依据*(host 证据全为负,仍保留默认 1 的理由):① host 5 个独立口径
  (60 帧 2 轮 interleaved、12 帧 3 轮 A/B/C、12 帧受控 depth round-robin 8 组)一致显示
  池化墙钟慢 1.4~1.7×、RSS +220MB,而收益(省 72MB alloc)理论上限只有 2.4s/60 帧;
  ② 线程级归因表明多出的 CPU 在 `prores_ks` 编码线程内,与 memcpy/acquire 无关;
  ③ 但 host 拓扑(12 vCPU WSL + 9p 读盘 + SMT 拥挤)与真机 arm64 差异大,且
  `#if/#else` 两路径输出帧数一致(59 packets)、只有性能差异;
  ④ 任务契约要求默认 1 便于真机 A/B 与快速回退,**默认保持 1**。
  真机证据(见下方协议)若确认负收益,把宏改成 0 即回到原行为,两侧代码都在。

> **2026-10-06 整改口径(醒目)**:已按上述 host 负收益把**发布默认改为 0**
> (`app/src/main/cpp/transcode.cpp:216-218`)——宿主证据不足以支撑默认 1;
> 重新开启(=1)见下方真机协议(准入:同设备同负载 ≥10% 吞吐提升且 `VmHWM` 不增、
> interleaved 4 轮方向一致),默认 1 的两侧代码都保留,回退只改一个宏。
> 上一条 bullet 的「默认保持 1」是整改前的历史决定,原文保留作历史证据。
- **验证(全部 host)**:
  - 功能:4 帧 zip(`src=dngzip faststart=0 dngthreads=2 dngdepth=2`)rc=0,`duration=0.120s`,
    prores/yuv422p10le,4096×3072,Elapsed 11.66s / MaxRSS 927,156KB,池 N=3。
  - Soak:60 帧 zip,pool/nopool 各 2 轮 rc=0、输出帧数一致(见上表)。
  - ASan:`-fsanitize=address -g -O1`,detect_leaks=0,pool/nopool 各跑 60 帧 ——
    **零报告**(log_path 与 inline 双查),帧数一致 → 无 use-after-free / heap-overflow,
    「refcount==1 才覆盖写」判据成立。
  - 泄漏口径降级说明(如实):本进程混合静态 libav,**LeakSanitizer 不工作** —— 注入
    「静态不可达 malloc(8KB)」探针(只在 WSL 副本,未动仓库)rc=0 且无任何
    LeakSanitizer 段,`__lsan_do_recoverable_leak_check()` 返回 0,LD_PRELOAD libasan 同样
    静默。因此「ASan 验证」只覆盖内存错误类,**不等于零泄漏证据**;`AvFramePool` 的释放靠
    RAII 析构(取消/fail 路径是 `goto` 跳出作用域,析构兜底 `av_frame_free`)。
  - 既有问题(只记录,不属于本段):容器 `counted_video_packets` 比输入帧数少 1
    (4→3、12→11、60→59,harness 进度 4/4 vs 容器 3);同二进制同输入每轮输出字节不同
    (prores 量化/线程排布非确定性)。
  - 偶发 I/O:受控 depth 第 4 轮出现 rc=183
    `DNG: LibRaw unpack 失败: Input/output error`(宿主负载 2.5+);截断帧专用输入
    (bad5,pool/nopool 同错同码)证明错误在 LibRaw 读盘层,非池化引入。
- **第二步(worker 直写目标 buffer 的零拷贝)延后**:方案自称生命周期风险最高(worker 线程
  的生命周期跨帧,与滤镜图/Sink 的所有权交织),无真机 soak 前不落地。
- **真机复测协议**(池化默认值与第二步能否解除延后的判定口径):arm64 **Release** 构建,
  60 帧真机 DNG zip 子集(帧数 ≥ 4×N),负载 <1.0,**interleaved 4 轮**交替跑
  `PRF_AVFRAME_POOL=1/0`,记 `VmHWM`(procstat)与墙钟。判定:池化墙钟优于 nopool ≥10%
  且 `VmHWM` 不高于 nopool 才保留默认 1;否则把宏改成 0(即原行为,回退只改一行)
  ——**「把宏改成 0」这一步已于 2026-10-06 整改执行**(当前发布默认即 0);该协议现在用于
  判定是否值得把默认改回 1。真机 A/B 结果:**未验证/待真机**。

#### 10.7.3 P2:faststart 0/1 实测取证 + 首帧 seed(2026-10-06,方案 P2 段)

> 本节三部分:A = `faststart` 默认值取证(不改默认);B = `DngFrameQueue::seed()` 首帧去重解码
> (已实现,含实测);C = Kotlin `HomeViewModel` 默认值评估(只评估,不改值)。

**A. `faststart` 0/1 实测(host x86_64,WSL,本地 NVMe `/dev/sdd`,页缓存热)**

代码锚点:`faststart_move_moov()` 在 `transcode.cpp:616-693`(proresforge 匿名 namespace,
扫顶层 box → 读 moov(>256MB 放弃)→ 把 `[ftyp 尾, moov 前)` 区域 A 按
`kChunk=4MB, chunk_sz=min(kChunk, moov.size)` 从尾部向前 `pread`+`pwrite` 右移,moov 写到
`a_begin`);调用点 `transcode.cpp:2361`(正常收尾,`// best effort`)与 `transcode.cpp:2425`
(`loop_error` 补救);默认 `faststart=1` 来自 `spec.get_i(SpecKeys::kFastStart, 1LL)`
(`transcode.cpp:1322`,`strspec.h:290`);Kotlin 侧 `HomeUiState.faststart` 默认 true
(`HomeViewModel.kt:62`,从 SettingsStore 恢复 `:108`;VP9 恒写 0,`FFmpeg.kt:228`)。

> **行锚点口径更新(2026-10-06 整改)**:C 段改动后函数体在
> `transcode.cpp:621-754`(新增搬移后顶层 box 自检与具名失败点),调用点为
> 正常收尾 `transcode.cpp:2449-2475`、`loop_error` 收尾 `transcode.cpp:2542-2561`;
> 上文的 `:616-693`/`:2361`/`:2425` 是整改前锚点,原文保留作历史证据;
> `largesize` 支持由 `:632-636` 随函数体顺移(见下方失败处理口径)。

- **4 帧真实转码**(`/tmp/prf_p1_4f.zip`,4096×3072,输出 17.6MB,instrumented interleaved 3 轮):
  `faststart` 阶段 **0.041 / 0.041 / 0.042s**;墙钟 A/B(同一二进制,interleaved 4 轮)
  fs=0 `13.43/11.56/11.64/11.60` vs fs=1 `11.74/13.26/11.62/11.91`(中位 11.62 vs 11.83,
  **噪声内,无显著差**);MaxRSS 两侧一致(~927MB);输出大小区间重叠
  (fs=0 17,592,479–17,596,287;fs=1 17,594,241–17,597,647,符合 prores 非确定 ±0.5% 容差,
  字节重排不改大小)。
- **box 结构**(自写 probe dump):fs=0 = `[ftyp(20)][wide(8)][mdat(17,591,785)][moov(734)]`;
  fs=1 = `[ftyp(20)][moov(734)][wide(8)][mdat(17,587,205)]` → **moov 确实被前置且结构完整**。
- **合成大文件**(probe `gen/move` 模式,支持 `size=1 + 64 位 largesize`;真实 FFmpeg muxer
  写 >4GB mdat 也走 largesize,`faststart_move_moov :632-636` 已支持;口径 = 读+写合计):

  | 成品大小 | moov 大小 | 实测耗时(2 轮) | 折算读+写吞吐 |
  |---|---|---|---|
  | 512 MB | 512 KB | 0.098 / 0.101 s | ~5.1–5.2 GB/s |
  | 1 GB | 512 KB | 0.175 / 0.180 s | ~5.7–5.8 GB/s |
  | 2 GB | 512 KB | 0.378 / 0.397 s | — |
  | 4 GB | 512 KB | 1.083 / 1.164 s | ~3.5–3.8 GB/s |
  | 5 GB | 512 KB | 1.201 / 1.216 s | — |
  | 8 GB | 512 KB | 2.065 / 2.069 s | — |
  | 8 GB | 2 MB | 2.206 / 2.240 s | — |

  **moov 大小几乎不影响耗时**(chunk 已是 512KB,系统调用数不是瓶颈);
  成本 ≈ **2×成品字节 / 盘速**(有效吞吐 ~2–2.9 GB/s)。**5GB ≈ 1.2s、8GB ≈ 2.1s**。
- 既有实现细节(只记录,非 bug):moov 只有 734B 时 `chunk_sz=min(4MB, 734)`,
  17.6MB 被切成 ~24k 个 734B 小 IO(预读失效),实测仍只 0.041s;真实 moov 通常 ≫ 4MB,
  chunk=4MB 正常。
- **结论与产品建议(不改代码默认)**:`faststart` 默认 1 **保留**。理由:① Web 播放 /
  DASH / HLS 兼容性(moov 前置 = 边下边播)是确定的收益;② 实测成本极低(5GB 1.2s、
  8GB 2.1s,页缓存热口径;Android 闪存冷启动会更贵,但同为 1~2s 量级),相对 4K 900 帧
  < 90min 的任务可忽略;③ 关掉只省 1~2s,却让所有云端/Web 消费场景失去流式播放。
  **不建议按成品大小阈值自动关**:数据里没有「>X GB 就显著」的拐点(8GB 也仅 2.1s)。
  若将来真机证据显示某档位成本占比过高,阈值口径应按「成品预估字节 ×2 / 目标盘速
  < 任务总时长 2%」判定,**不要用固定 GB 数**(盘速与任务时长差两个数量级)。

**A′. 当前实现口径(2026-10-06 整改:C 段 faststart 失败可观测)**

> 本节为 faststart 失败处理的**当前有效契约**(对照上文 A 的取证与默认值结论;默认
> `faststart=1` 不变,变的是失败路径的可观测性与任务结果)。

- **成功路径失败 = 任务显式失败**(不再 best-effort 静默):`faststart_move_moov()` 失败时
  写 `AV_LOG_ERROR`,带 `stage=moov-move` / `point`(具名失败点)/ `errno` / `out_len` /
  `out_ctx.size`,并把 `result` 置非 0(`AVERROR(EIO)`)→ 任务显式失败,由既有 done
  错误清理 `vp.rep(-1, "error")` 上报,Kotlin 侧 `deleteOutputDocument` 删除输出文件
  —— 不留下可被误认为完整成品的半搬移文件,也不补写 trailer(`trailer_written` 守卫,
  已写过且仅此一次)。锚点:`transcode.cpp:2449-2475`。
- **错误收尾路径(`loop_error`)只告警、不覆盖原始错误**:仅写 `AV_LOG_WARNING`
  (同样带 `stage=moov-move` / point / errno / out_len / out_ctx.size),**不改 result**、
  不重复 `av_write_trailer`。锚点:`transcode.cpp:2542-2561`。
- **搬移后有顶层 box 自检**:`faststart_move_moov()` 在搬移完成后重新扫描顶层 box,
  校验 `[ftyp][moov]`、moov 偏移 = 搬移起点、尺寸不变、box 数/总长度;失败点具名为
  `FaststartFail.point` / `err`。锚点:`transcode.cpp:621-754`。
- **第二步(staging/临时 fd 上 faststart 后验证再提交)未实施**:它需要输出层契约变更
  (Kotlin `createFile`/`detachFd` 流程提供可回滚的临时输出),**列为后续项**。因此当前
  第一步语义是:原地搬移若中途失败,文件可能停在部分搬移状态,但任务会显式失败并删除输出。
- **未验证/待真机**:真机 faststart 失败注入测试未做(建议注入 `ENOSPC`/`EBADF` 或破坏
  staging 副本 moov),运行时 log 与 `result` 联动未在设备验证;host 侧仅完成 NDK Release
  arm64-v8a 编译验证(编译通过不等于功能验证)。

**B. 首帧 seed:`DngFrameQueue::seed()`(已实现,仅 `src=dngzip` 路径生效)**

动机:zip init 段(`transcode.cpp:1369-1377`)已经 `read_frame(0)+dng_decode_to_rgb48` 解过
seq=0 拿 `zip_w/zip_h`(~0.4s/帧 @4096×3072 BALANCED),worker 随后又会从 seq=0 重解同一帧。

- **接口**:`bool seed(DngFrame &&f)`(`dngframeq.h:113-129` 声明、`:208-214` 实现)。
  仅 `start()` 前调用一次;已 seed 过或 start 后调用返回 false(调用方 bug,忽略)。
  `start()` 内 `m_slots.resize(m_depth)` 之后落地(`:235-247`):帧 move 进 slot0,
  `seq=0`、`ready=true`,`m_assigned` 置 1 → worker 只会碰到 seq≥1。
  `stop()` 复位 `m_seed_frame`/`m_seeded`(`:445-453`),同一对象可再 start。
- **不变式(逐条对照过)**:帧号严格有序(`next()` 谓词 `s.seq==m_consumed`);
  `total_frames==1` 时 worker 见 `m_assigned(1)>=m_total(1)`,`produce_one()` 立即返回
  false 退出,`next()` 第一轮即消费 seed 帧(**不卡**);`depth==1` 时空位判定
  `(assigned-consumed)<1` 在首帧被取走后才放行 seq=1,同样正确;
  取消(`kNextCancelled`)/失败(`m_failed` 扩散,`next()` 返回 -1)/`kNextDrained` 语义不变;
  `thread_count()` 未动(仍覆盖 `m_workers`+`m_threads`,见 §10.7.1);**无新增栈大户**
  (seed 只 move 两个 vector,8MB 工作线程栈不受影响);`~DngFrameQueue()` 仍 `stop()` 兜底。
- **调用方**(`transcode.cpp`):`:1291-1294` 声明 `DngFrame seed_frame; bool have_seed=false;`
  (非 zip 路径恒 false → 零影响);`:1378-1386` init 段首帧解码成功后
  `seed_frame.rgb.swap(dng_rgb); seed_frame.raw.swap(dng_buf);` + 记 w/h
  (swap 转移所有权,避免 72MB 拷贝;`dng_buf`/`dng_rgb` 之后不再使用);
  `:1975-1985` 在 `pool.start()` 前 `pool.seed(std::move(seed_frame))`,被拒只告警不失败。
- **pts 语义**:`DngFrame` 没有 pts 字段,pts 由消费端按循环 `i` 给
  (`AvFramePool::acquire(..., (int64_t)i, ...)`,`transcode.cpp:2054`),
  **seed 不改时间戳**。容器层实证:stts run-length 展开 pts 序列 `[0, 512, 1024]`、
  每帧 dur=512 tick、首 chunk offset `36 == 36`,seed 前后**完全一致**。
- **功能/边界实测(host,4 帧 `/tmp/prf_p1_4f.zip`,帧数/duration 与 seed 前一致)**:
  `dngthreads=1 dngdepth=1` 配置下输出与 base **逐字节相同**(md5 `e6d99d883ceeb75c760aa3f3c4e02524`,
  该参数 prores_ks 确定);单帧 `/tmp/prf_p1_1f.zip`(`total_frames==1`)rc=0、0.43–0.53s、
  输出 185B(`[ftyp][moov(149)][wide][mdat(8)]`),**不卡**。
- **取消路径**(自建探针:run_transcode 跑在 std::thread,main 到点 `request_cancel()`):
  4 帧 delay `0/50/200/500ms` → rc=1 且分别 0.03/0.89/0.82/2.42s 内退出;
  delay `1500/3000ms` → rc=0(4 帧在 ~1.5s 内已完成,取消来得晚,与 base 同行为);
  单帧 delay `0/50/200ms` → rc=1,均秒退不卡。
- **A/B 性能(interleaved,同一对二进制 `tcharness_fbase`/`tcharness_fseed`)**:

  | 配置 | base 墙钟 | seed 墙钟 | 差异 | MaxRSS base→seed |
  |---|---|---|---|---|
  | 默认(自动 4 线程 / depth 2) | 11.74 / 11.81 s | 11.53 / 11.17 s | ~2–5%(噪声内) | 927,068/927,132 → 843,156/837,368 KB |
  | `dngthreads=1 dngdepth=1` | 3.61 / 3.65 / 3.57 s | 3.12 / 3.18 / 3.14 s | **-0.46s,12.5%** | 890,384–890,756 → 801,444–801,572 KB |

  默认参数下差异落在 host SMT 噪声内,`dngthreads=1` 下显著;**MaxRSS 两侧一致降 ~87MB**
  (= 一份 72MB rgb + 一份 15.7MB raw:未 seed 时 init 解码帧与 worker 首帧两份同时驻留)。
- **为何默认参数下收益不明显(如实,含对后续优化的指向)**:4 帧端到端阶段时间线
  init `0.000` → decode 100% `1.372` →(视频编码器 flush ~10.1s)→ audio 100% `11.515`
  → finalize/done `11.559`。**编码阶段占 ~86%,解码只占 ~12%**;seed 省掉的那次 seq=0 解码
  在 4 线程并行余量里被吸收。host 上 `dngthreads=1` 反而整体快 3× 是 §10.7.2 记录的
  宿主 SMT 拥挤效应(解码 worker 抢 prores_ks 帧线程),不是可移植结论。
  → **后续性能工作应优先看编码侧(`PRF_PRORES_THREADS`/prores_ks 参数),解码侧已接近下限。**

**C. `HomeViewModel` 默认值评估(只评估,未改任何 Kotlin 文件)**

- `faststart` 默认 true:**保留**。依据见 A(小文件无感,5GB 才 1.2s)+ 流式播放收益确定;
  VP9 已由 `FFmpeg.kt:228` 强制 0,路径自洽。**建议不加「按大小自动关」**,避免 UI 文案
  与实测(成本与任务时长强相关、与文件大小弱相关)脱节。
- `dngthreads` / `dngdepth` **不建议暴露到 UI**:① Kotlin spec 构造(`FFmpeg.kt:234-241`)
  根本不发这两个键,C 侧默认 `0=自动`(`dng_parallel_params` 按 1GB 内存预算反推、封顶 4 且
  `≤cores-1`,depth 固定 2),自动策略已被 §10.7.2 host 矩阵证实不劣于手工拍值;
  ② 暴露需要新增 SettingsStore 键 + HomeScreen 控件 + JobSpec 字段,而 §3 的 27 键契约
  由 `scripts/check-contracts.sh` 校验(键变量 ≡ `kAll[]` ≡ 注释「共 N 个」,每键登记 §3),
  **不能新增键**(只能复用现有键),把一个「0=自动 / 1–8」的专家旋钮交给普通用户,风险大于收益;
  ③ 若真机证据表明自动值不合适,该改的是 `dng_parallel_params` 的定参,而不是让用户兜底。
- 未发现 `HomeViewModel.kt` 的 bug:`faststart` 从 SettingsStore 恢复(`:108`/`:489`),
  data class 默认值 true(`:62`)与 C 侧默认 1 一致;VP9 的 0 覆盖在 spec 构造处,不在 UI 层。

**本节验证命令(WSL Ubuntu-24.04,host FFmpeg `/home/ubuntu/ffx86/inst`,LibRaw
`/home/ubuntu/dzwork/libraw_host`;host 无 ffprobe,容器结构用自写 python/probe box 解析)**:
4 帧 / 单帧功能 + 取消矩阵 + A/B 全部 rc=0/秒退,未见卡死;`probe_faststart stats|dump`
解 moov→trak→mdia→minf→stbl 的 stts/stsz/stco 做帧数与 pts 取证。

### 10.8 验收补充(§9 之外)
- md5 数输出帧数 == zip 帧数;`duration == nb_frames/fps`;容器 `.mov`,
  `prores_ks` profile=`hq`,`pixfmt=yuv422p10le`(按名取,不按数字)。
- 灰卡/白卡中性片对比桌面 LibRaw+ffmpeg 同参数: PSNR ≥ 40dB。
- `nativePreview` 帧下标与首帧转码输出 JPEG 一致;非 DNG zip → 空串 → "不可解析"。
- arm64 1080p60 900 帧 < 20min;4K 900 帧 < 90min(P50 档);900 帧不 OOM(内存分档:1080p ≤400MB / 4K ≤500MB,4096×3072 四线程实测 VmHWM 496MB);取消可用。
- 已有 `.mov`/`.mxf` 回归用例全部保持原结果不变(新增分支不许影响老路径)。
- **条目名兼容(2026-10-04 反馈)**: `frame-#####.dng` / `frame_######.dng` /
  `IMG_####.dng`(大小写不敏感、可带目录前缀)都要能识别;识别不到时 app.log 里要能查到
  zip 的实际条目名前 8 个 + 匹配/回退计数(便于远程定位新导出格式)。
