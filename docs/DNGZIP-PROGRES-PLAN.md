# 方案:DNG 序列 ZIP → Apple ProRes 422 HQ(ProResForge 扩展)

> 目标:让 ProResForge 在 Android 手机上,把 MotionCam 老的「DNG 序列 zip 压缩包」
> (每包约 900 帧 `frame-#####.dng`)直接转成 ProRes 422 HQ 的 `.mov`。
> 本文只定方案与改动点,不含实现代码。所有锚点(文件:行号)按当前仓库状态。

---

## 0. 一页结论(TL;DR)

**链路**:SAF fd(zip) → 自研 ZIP 中心目录解析 + zlib inflate(逐帧, 全程内存, 零拷贝 fd)
→ **LibRaw**(DNG 解码 + 去马赛克 + 相机白平衡 + 色彩矩阵 → 16bit sRGB)
→ AVFrame`rgb48le` → 现有 filtergraph(scale/setsar/format/lut3d/fps)
→ 现有 `prores_ks profile=hq` → `.mov`(faststart 复用)。

**为什么必须引入 LibRaw**:FFmpeg 6.1 的 `tiff.c` 确实认 DNG(DNGVersion / CinemaDNG、
uncompressed strips、lossless-JPEG tiles、LinearRaw、DNG color scaling),但对 photometric CFA
只输出 `AV_PIX_FMT_BAYER_RGGB16/BGGR16/GBRG16/GRBG16` 等 Bayer 格式——
**没有 demosaic、没有白平衡、没有色彩矩阵、不读 opcode 列表**(且 "DNG uncompressed tiled images"
" DNG JPG-compressed tiled non-bayer" 在源码里是 `AVERROR_PATCHWELCOME`)。
用它等于把现场白平衡/色彩矩阵丢掉,输出必然偏色,不能作为交付路径。
FFmpeg 的 `libraw` demuxer 要 **7.1** 才有,违反契约 §6 的 FFmpeg 6.1 pin,一并否决。

**改动面**:1 个新 native 源文件对(`dngzip.h/.cpp`)+ LibRaw 交叉编译 + `transcode.cpp`
新 source 分支 + Kotlin 白名单/参数卡/MediaFacts;FFmpeg 配置一行不动。
**工作量**:约 9.5 人日(M0~M5,见 §8)。

---

## 1. 第 0 步:真实样本摸底(0.5 天,必做,决定后续开关)

在写任何代码前,拿 1 个真实 MotionCam DNG zip 跑下面几项(host 侧):

| # | 问题 | 命令 / 方法 | 影响 |
|---|---|---|---|
| 1 | 是否 >4GiB 或 >65535 条目(ZIP64) | `zipinfo -v file.zip \| head -50`,看 EOCD/ZIP64 段 | ZIP64 解析是**必需**还是兜底 |
| 2 | 条目名模式与数量 | `unzip -l \| head`, `unzip -l \| wc -l` | 帧序号排序规则、nb_frames |
| 3 | 每个 DNG 的压缩方式 | `unzip -v file.zip`(Method 列:Stored / Defl:N) | LibRaw 需不需要 zlib;是否要 libjpeg |
| 4 | DNG 是 CFA 还是 LinearRaw | `exiftool -PhotometricInterpretation -CFAPattern -DNGVersion -Compression -BitsPerSample -Make -Model frame-00001.dng` | CFA → 必须 demosaic(LibRaw);LinearRaw → 直接 3 通道 |
| 5 | 有没有 WB/颜色 tag | `exiftool -AsShotNeutral -ColorMatrix1 -ColorMatrix2 -ForwardMatrix -CalibrationIlluminant ...` | 决定 `use_camera_wb` 能不能默认开 |
| 6 | 帧率从哪来 | 看 zip 内是否有 json/txt metadata、EOCD comment;`exiftool -DateTimeOriginal` 亚秒精度能否算 | 没有 → UI 强制用户选 fps(M4) |

**LibRaw 依赖开关决策(由 #3/#4 决定)**:

- `Compression=7`(lossless JPEG, MotionCam 常见)→ LibRaw **自带** LJpeg 解码器,**不需要 libjpeg**。
- `Compression=8/32946`(deflate)→ 需要 **zlib**(NDK 27 sysroot 自带 `libz.a`,不用自己编)。
- `Compression=8` 且是 JPEG(基线/"DNG lossy")→ 才需要交叉编 **libjpeg-turbo**(罕见,默认不编)。

---

## 2. 目标链路与预算

```
SAF 打开 zip fd(现有 Kotlin detachFd 路径,无新 JNI 签名)
   │
   ├─ dngzip.cpp: ZipIndex ── 读 EOCD(0x06054b50,尾部 64KB 滑窗)/ZIP64 locator
   │      (0x07064b50)+ ZIP64 EOCD(0x06064b50)/ 中央目录(0x02014b50)
   │      → 只保留 frame-\d{5}\.dng 且 method ∈ {0 存储, 8 deflate},按文件名自然序
   │
   ├─ 每帧: local header(0x04034b50)→ 跳过 name+extra → pread(1MB 块, 复用现有 FdIoCtx 风格)
   │        → inflateInit2(strm, -15) raw deflate → 单帧 DNG 字节(复用 buffer)
   │
   ├─ LibRaw: open_buffer → unpack → dcraw_process → imgdata.image(ushort (*)[4])
   │        recycle() 每帧复位(不做逐帧 malloc)
   │
   ├─ AVFrame rgb48le(packed 16bit, pts = 帧号) → av_buffersrc_write_frame
   │
   ├─ 现有 filtergraph:scale → setsar=1:1 → format(=编码器像素)→ [lut3d split+blend] → fps
   │
   └─ 现有编码器:prores_ks profile=hq,yuv422p10le → mov,faststart_move_moov 复用
```

**内存预算(arm64, 4K CFA)**:raw 16bit 33MB + LibRaw image 4×16bit 33MB + AVFrame rgb48 24MB
+ 编码队列 ≈ **150MB 峰值**;1080p 约 40MB。6K/8K 要在 UI 上警告(§6.7)。

**速度预算(arm64 顶级 SoC, 单线程 dcraw_process)**:4K AHD ≈ 1~4 s/帧 → 900 帧 15~60 分钟;
1080p ≈ 3~8 帧/s。首版即提供「快速(=half_size/PPG)/ 均衡 / 精细(AHD)」三档(§5)。

---

## 3. 完全复用的部分(不动 = 不出回归)

- `fd_io_*`(transcode.cpp:96-219):自定义 AVIO 是**全 seekable** 的(`fd_io_seek`@96 支持
  `AVSEEK_SIZE/SEEK_SET/CUR/END`),ZIP 里的随机读(local header + 数据)直接用同款 `pread` 即可,
  不需要把 zip 解到临时文件、不需要 image2 demuxer。
- filtergraph 其余段、`prores_ks` 编码与 profile 命名(`hq`,不用数字)、Mov mux + `faststart_move_moov`@365。
- 进度/取消机制(`STAGES init/decode/audio/finalize/done/error` + `on_progress` + `g_cancel`)。
- SAF fd 零拷贝 IO、批量队列(每个 zip = 一个 job,多个 chunk 顺序排队)、OutputDir 命名。
- LUT bake 通道(`lut3d split+blend`)——想要 log/ACES 母版,用户直接挂 LUT,**不需要新代码**。

---

## 4. 改动清单(逐文件,含锚点)

| 文件 | 改动 | 锚点 | 契约(INTERFACES.md)影响 |
|---|---|---|---|
| `scripts/build-ffmpeg-android.sh` | 插入 LibRaw 交叉编译段,装到同一 `prebuilt/<abi>/{include,lib}` | 现有段 1)x264 / 2)x265 / 3)libvpx / 4)FFmpeg | §6 构建产物清单 + 版本 pin + 许可行 |
| `app/src/main/cpp/CMakeLists.txt` | 追加 IMPORTED STATIC `libraw` + include 目录 + `-lz` | `FFMPEG_LIBS`@51 | §1 源码清单(新增 dngzip) |
| `app/src/main/cpp/dngzip.h/.cpp`(**新**) | ZIP 索引(ZIP64)+ 逐帧解压 | — | §1 源码本地实现清单 |
| `app/src/main/cpp/transcode.cpp` | `src=dngzip` 新 source 分支 + LibRaw 帧源 | `run_transcode`@905、`build_chain`@484、`build_descr`@570、`encode_out`@662、`pump_decoder`@746 | §3 spec 新增键 / §4 nativeInfo 新增键 |
| `app/src/main/cpp/preview.cpp` | `nativePreview` 识别 zip magic,`atUs` 语义改为「帧索引」 | — | §4 预览语义补一行 |
| `app/src/main/java/com/proresforge/app/engine/FFmpeg.kt` | `buildSpec`@125 输出 `src/dngq/dngcolor` 键 | @125-290 | §3 |
| `.../ui/HomeViewModel.kt` | `PROBEABLE_EXTENSIONS`@447 加 `zip`;MediaFacts 补 frames | @447、@149 | — |
| `.../ui/HomeScreen.kt` | picker mime@132 加 `application/zip`;zip 参数卡 | @132 | — |
| `docs/INTERFACES.md` | §1 源清单、§3 spec 键、§4 nativeInfo/预览、§6 产物/许可 | — | 本文落地时同步改 |

### 4.1 `scripts/build-ffmpeg-android.sh` — LibRaw 段

放进 FFmpeg 之后(位置任意,FFmpeg 不依赖它),与 x264 同款 `clone_once` + NDK cmake toolchain,
装到**同一 prefix**:

```bash
LIBRAW_REPO="${LIBRAW_REPO:-https://github.com/LibRaw/LibRaw-cmake.git}"  # 0.21+/0.22 的 CMake 构建仓
# 关键开关(据 vcpkg portfile 证实的选项名):
cmake -S "${SRC}/libraw" -B "${SRC}/libraw/build/${abi}" -G "Unix Makefiles" \
  -DCMAKE_TOOLCHAIN_FILE="${NDK}/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI="${abi}" -DANDROID_PLATFORM="android-${API_LEVEL}" \
  -DANDROID_STL=c++_static \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_SHARED_LIBS=OFF \
  -DENABLE_EXAMPLES=OFF \
  -DCMAKE_REQUIRE_FIND_PACKAGE_ZLIB=1 \
  -DCMAKE_REQUIRE_FIND_PACKAGE_JPEG=0 \        # 仅 DNG lossy 才需要,默认关
  -DCMAKE_REQUIRE_FIND_PACKAGE_Jasper=0 \
  -DCMAKE_REQUIRE_FIND_PACKAGE_OpenMP=0 \      # 关 OpenMP:首版单线程,行为可预期
  -DENABLE_LCMS=OFF -DENABLE_RAWSPEED=OFF \
  -DCMAKE_INSTALL_PREFIX="${prefix}"
```

要点:
- `find_package(ZLIB)` 会命中 NDK sysroot 的 `libz.a`(已实测 27.0.12077973 两个 ABI 都有),
  **不需要**自己编 zlib。
- 关 OpenMP / lcms / Jasper / RawSpeed / 示例;产物只有 `libraw.a` + `libraw/*.h`。
- 脚本头注释的耗时提示、产物校验段(§5)把 `raw` 加进库存在性检查。
- 版本 pin:`LIBRAW_TAG=0.21.4`(或摸底后升 0.22.x,记进 INTERFACES §6)。

### 4.2 `app/src/main/cpp/CMakeLists.txt`

```cmake
# 与 FFMPEG_LIBS 并列,新增静态导入;注意:IMPORTED STATIC 不能直接用
# set_target_properties(IMPORTED_LOCATION) 的形式放进 FFMPEG_LIBS 循环(那是给 .so 的),
# 单独写:
add_library(ffmpeg-raw STATIC IMPORTED)
set_target_properties(ffmpeg-raw PROPERTIES
    IMPORTED_LOCATION "${FFMPEG_PREBUILT}/lib/libraw.a")
target_include_directories(ffmpeg-transcode PRIVATE "${FFMPEG_PREBUILT}/include")
target_link_libraries(ffmpeg-transcode PRIVATE ffmpeg-raw z)
```

- LibRaw 是 C++:最终 `.so` 必须**静态**带 `libc++_static.a`(CMakeLists 已 `project(... C CXX)`,
  AGP 默认 `ANDROID_STL=c++_static`;x265 那段已有同样先例)。**绝不允许**引入
  `libc++_shared.so` 运行时依赖,否则 APK 打包图缺库 → 整条 native dlopen 失败(CMakeLists:44-49
  的注释就是上一次踩的坑)。
- 若 NDK 的 `find_library` 找不到 `z`,fallback `target_link_libraries(... z)` 已经写在上面。

### 4.3 新文件 `app/src/main/cpp/dngzip.h/.cpp`(方案级接口)

```c
struct DngZipEntry {
    std::string name;      // "frame-00001.dng"
    uint16_t    method;    // 0 = stored, 8 = deflate(其余 → 明确报错)
    uint64_t    csize, usize, lho;
};

class DngZip {
public:
    static bool is_zip(const uint8_t *head, size_t n);          // "PK\x03\x04"
    int  open(int fd, int64_t len, std::string &err);           // 解析 EOCD/ZIP64/CD
    size_t frame_count() const;                                 // ^frame[_-]?\d+\.dng$ 或回退的全部 .dng
    const DngZipEntry &entry(size_t i) const;
    // 解压第 i 帧到 out(buf 复用,自动 resize);失败返回 AVERROR_INVALIDDATA + err 文案
    int  read_frame(size_t i, std::vector<uint8_t> &out, std::string &err);
private:
    int m_fd; std::vector<DngZipEntry> m_frames; int64_t m_len;
};
```

字节级规则(实现时照此写,别再翻文档):

- **EOCD** sig `0x06054b50`,从 `max(0, len-65536)` 起滑窗找;total entries@10(2B,LE),
  CD offset@16(4B)。
- **ZIP64**:EOCD 里上述任一字段为 `0xFFFFFFFF` / CD offset == `0xFFFFFFFF` → 找
  ZIP64 EOCD locator(sig `0x07064b50`,`0xFFFFFFFF` 前的 20 字节)→ 取其 ZIP64 EOCD 偏移
  (sig `0x06064b50`)→ CD 项数的真实字段在 locator+32(8B)。
- **中央目录项** sig `0x02014b50`:method@10(2B)、csize@20(8B for zip64 / 4B)、usize@24、
  name_len@28(2B)、extra_len@30(2B)、comment_len@32(2B)、lho@42(4B / zip64 时由 extra 的
  `0x0001` 字段给 8B)。**ZIP64 的 8B 值在 extra field 里**,不能假设它们同序。
- **local header** sig `0x04034b50`:name_len@26、extra_len@28,数据起点 =
  `lho + 30 + name_len + extra_len`(extra 可能比 CD 里长,必须以 local 的为准)。
- deflate:`inflateInit2(&strm, -15)`(raw,无 zlib/gzip 头);输入用 1MB 块 `pread` 喂;
  输出按 `usize` 预分配并校验实际解压长度。
- 条目过滤:名字匹配 `^frame-\d{5}\.dng$`(大小写不敏感)、`usize>0`;按文件名**自然序**排序
  (`frame-00002.dng` 必须排在 `frame-00010.dng` 前),不能用整体字典序、更不能按 lho 排
  (MotionCam 追加写时 lho 恰好递增,但别的打包器不一定,排序以名为准)。
- 不落临时文件、不 mmap 整个 zip;内存里只有「当前帧 DNG 字节」。

### 4.4 `transcode.cpp` — `src=dngzip` source 分支

分支点:`run_transcode`@905 在 `avformat_open_input` 之前读 spec 的 `src` 键:

- `src=dngzip`:跳过 avformat,走 `dngzip.open(fd, length)` → LibRaw 打开**第一帧**拿
  `idata.colors/bitdepth/sizes.width/height` → 组装 filtergraph;
  filtergraph 的前半段从「源流参数」换成固定
  `buffer=video_size=WxH:pix_fmt=rgb48le:time_base=1/fps:sar=1`(复用 `build_chain`@484 的
  字符串组装函数,只是 source 段不同,`autorotate` 不进图——DNG 无旋转语义)。
- 帧源替换 `pump_decoder`@746:每帧
  `LibRaw::open_buffer(ptr,size) → unpack() → dcraw_process` →
  把 `imgdata.image[i][0..2]`(每像素 4 个 ushort 交错,**只取前 3 通道**)按行写进
  `av_frame_get_buffer` 出来的 `AV_PIX_FMT_RGB48LE` 帧,`pts = i`(time_base `1/fps`),
  `av_buffersrc_write_frame`,然后走**现有**的 `drain_graph`@693 / `encode_out`@662。
- 每帧结束 `recycle()`(LibRaw 语义:`recycle()` 释放本轮资源,下一轮 `open_buffer` 前调用;
  致命错误后已自动 recycle,重复调用无害)。
- `fpsnum/fpsden` 用 spec 里的值(zip 无容器帧率,Kotlin 侧 §5 强制用户选,缺省 25)。
- 音频:zip 没有音轨 → **不建 audio stream**(`build_descr`@570 的 audio 分支只在存在 stream 时
  触发;验收用例 §7 覆盖 `audio=copy/aac` 都不炸)。
- 颜色标记:LibRaw `output_color=1` 出来是 sRGB(Rec709 primaries + sRGB TRC),
  spec 里给 `cspace/cprim/ctrc`(bt709 / bt709 / sRGB-TRC,视 `build_descr` 的映射表支持的字符串
  填;不支持时至少 prim=bt709,不能空着让消费者猜)。
- `faststart_move_moov`@365、`STAGES`、`on_progress`(进度分母 = `frame_count()`)、
  `g_cancel` 帧边界检查:全部原样复用。
- **并行解码(2026-10-05,§10.7)**:上面的「每帧 read_frame + dng_decode_to_rgb48」
  不再在主线程里做,而是交给 `DngFrameQueue`(`app/src/main/cpp/dngframeq.h`,新增)的 N 条
  工作线程;主线程只按帧号顺序 `next()` 取帧 → 滤镜图 → 编码 → 写盘。输出顺序/进度/
  取消/失败语义与串行版一致。线程数/深度由 `dngthreads`/`dngdepth` 决定(默认按内存预算
  自动)。实测解码占整链 86%(0.351s / 0.407s),2~4 线程 1.7~3× 收尾。

**LibRaw 参数表(直接对应 UI 的画质/色彩档)**:

| spec 键 | `imgdata.params` | 说明 |
|---|---|---|
| `dngq=0`(快速) | `half_size=1; user_qual=0` | 半分辨率 demosaic,4K ≈ 2~4× 速度 |
| `dngq=1`(均衡) | `user_qual=2`(PPG) | 默认 |
| `dngq=2`(精细) | `user_qual=3`(AHD) | 默认档的最细选择,4K 约秒级/帧 |
| `dngcolor=camera` | `use_camera_wb=1` | 用 DNG 的 AsShotNeutral,**默认** |
| `dngcolor=auto` | `use_auto_wb=1` | 灰世界近似,用于缺 WB tag 的文件 |
| `dngcolor=d65` | `use_camera_wb=1` + `user_mul[0..3]=1,1,1,1` 派生固定 | 固定日光白平衡 |
| 公共 | `output_bps=16; output_color=1; no_auto_bright=1; gamm[0]=gamm[1]=0` | 16bit 输出、sRGB gamma、不自动拉亮度(否则高光语义被改) |

`no_auto_bright=1` 必须显式给:LibRaw 默认会做半尺寸亮度归一化,会让「曝光」与 DNG 原始语义
不一致(首版要 WYSIWYG,不要惊喜)。

### 4.5 `preview.cpp` — 预览不新增 JNI 签名

`nativePreview(fd, length, atUs, ...)` 里先读 4 字节:`is_zip()` 为真时按 zip 路径解
`atUs` 当**帧索引**解一帧 + 现有 swscale 缩到 `maxW` → JPEG。
理由:§1 的 4 个 JNI 签名是契约,能不扩就不扩;`atUs` 对 zip 没有时间基语义,复用为帧号
并在 INTERFACES §4 写明。

---

## 5. Kotlin / UI 改动

1. **Picker / 白名单**:`HomeScreen.kt:132` 的 mime 数组加
   `"application/zip"`, `"application/x-zip-compressed"`;`HomeViewModel.kt:447`
   `PROBEABLE_EXTENSIONS += "zip"`(理由:zip 也进 native 探测,由 native 自己判是不是 DNG 序列,
   探测失败照原逻辑落终态)。
2. **探测**:`MediaProbe` → `FFmpeg.nativeInfo(fd, length)`(签名不变)。native 侧
   `nativeInfo` 同样认 zip magic:成功时返回
   `codec=dng,w=…,h=…,fpsnum=0,fpsden=0,nb_frames=…,container=zip,duration_us=0,…`;
   MediaFacts 新增 `frames`(用于 UI 显示帧数与预计耗时);zip 里没有 DNG 帧 → 返回空串,
   UI 走现有「不可解析」文案。
3. **buildSpec**(`FFmpeg.kt:125`):当 `JobSpec` 选定 DNG-zip 源时追加
   `src=dngzip`、`dngq=<档>`、`dngcolor=<档>`;fps 的 `FpsMode.ORIG` 对 zip **不可用**
   (facts.fps=0)→ UI 强制选具体 fps;ProRes 的 `rc=bitrate` 与面积比例公式(§3.2)**不变**。
4. **HomeScreen 参数卡**(仅 zip 显示):
   - 帧率:24/25/30/48/50/60(默认 25,给 MotionCam 常用档;用户可改)。
   - 画质档:快速 / 均衡 / 精细(§4.4 表)。
   - 色彩档:相机 WB(默认)/ 自动 WB / 固定日光。
   - 输出尺寸:ORIG / P1080 / P720 / P50 / P25(复用现有 ScaleMode)。
   - 提示文案:「本包 N 帧 ≈ X 分 Y 秒」「预计耗时约 Z 分钟(经验值,真机为准)」
     「建议一次转一个 zip(chunk);多个 chunk 请排队,一次一个」。
5. **JobStore**:zip job 落盘时带上 frames + fps(断点重试与列表展示要用)。

---

## 6. 关键坑与对策(实现前先读)

1. **ZIP64 必需**:900 帧 4K DNG deflate 后普遍 >4GiB,32 位 EOCD 直接溢出。ZIP64 locator
   可能出现在尾 64KB 之外吗?——按规范 locator 紧跟 EOCD,所以 64KB 滑窗一定够;但 ZIP64 EOCD
   本身由 locator 的偏移定位,不在滑窗内,要单独 pread。
2. **自然序排序**:`frame-00002.dng` < `frame-00010.dng`;零填充位数不一致的包
   (混着 `frame-1.dng`)按「数字部分」解析,解析不出来退回字符串序并在 UI 警告。
3. **raw deflate**:`inflateInit2(-15)`;用 `inflateReset` 复用 `z_stream`,不要每帧 Init/End。
4. **LibRaw 的 JPEG**:Compression=7(LJpeg)内置解码,**不编 libjpeg**;只有 baseline JPEG
   ("DNG lossy")才需要;若 M0 发现样本是它,再往 build 脚本插 libjpeg-turbo 段。
5. **image 通道布局**:`imgdata.image` 每像素 4 个 ushort,`colors` 是 3 或 4;拷到
   `rgb48le` 只取前 3 通道;`av_frame_get_buffer` 已处理对齐,按行 memcpy 即可,
   不要用 `imgdata.image` 的裸指针当帧数据(生命周期与 stride 都对不上)。
6. **16bit → 10bit**:ProRes 要 `yuv422p10le`,由现有 `scale`/`format` 段 + swscale 完成,
   **不要**自己量化;避免二次精度损失。
7. **内存**:4K 峰值 ~150MB(§2);6K/8K 直接超,UI 要有上限判断(宽度或高度 >4096 时提示
   「内存风险,建议降到 P50」)。`recycle()` 必须调,否则逐帧累积。
8. **速度**:首版单线程;UI 给预估。后续优化(不阻塞):LibRaw 自带 `half_mt_*` 多线程渲染示例
   可参考,把 demosaic 放 worker 与 zip inflate / 编码并行(注意 LibRaw 默认非线程安全,
   `build_thread_safe` 若关则每线程一个 RawProcessor)。
9. **取消语义**:帧边界检查 `g_cancel`,立刻返回 `1`(取消);半成品 mov 保留——与现有
   `.mov` 路径行为一致,不搞特殊清理。
10. **损坏条目**:某帧 inflate/解 DNG 失败 → 报 `AVERROR_INVALIDDATA` + 明确文案
    (`zip 第 123 帧(frame-00123.dng)解压失败`),不要静默跳帧(跳帧会让帧率/时长错位)。
11. **白平衡语义**:默认 `use_camera_wb=1` 保留现场 WB;`use_auto_wb` 是灰世界近似,
    色温可能差 500K,只作兜底选项。
12. **颜色标记**:写 Rec709 primaries + sRGB TRC(用 `build_descr`@570 现有映射表里有的
    字符串);ProRes 的 profile 仍承载 422 语义,两个键不冲突。
13. **无音频**:`build_descr`/`encode_out` 在「有 video 无 audio」下必须干净退出
    (验收用例覆盖 `audio=copy` 与 `audio=aac` 两种 spec)。

---

## 7. 验收标准(可测)

| 项 | 标准 |
|---|---|
| 格式 | 输出 `.mov`,`prores_ks profile=hq`(名字而非数字),`yuv422p10le` |
| 完整性 | 帧序列连续无丢帧(逐帧 md5 计数 = zip 帧数);时长 = `nb_frames / fps` |
| 颜色 | 白卡/白墙肉眼中性;与桌面 `LibRaw + ffmpeg` 同参输出的 PSNR ≥ 40dB |
| 探测 | zip → MediaFacts 非空,w/h/nb_frames 正确;非 DNG zip → 「不可解析」终态 |
| 预览 | `nativePreview` 对 zip 取帧号可用,JPEG 与转码首帧一致 |
| 性能(初版基线,记入 README) | arm64 1080p60,900 帧 < 20 min;4K,900 帧 < 90 min(P50 档) |
| 稳定性 | 中途取消不损坏后续任务;内存分档 1080p ≤400MB / 4K ≤500MB(4096×3072 四线程实测 VmHWM 496MB);900 帧连续跑无 OOM |
| 回归 | 现有 `.mov/.mxf` → 各格式用例全部通过(不改 FFmpeg 配置即应天然通过) |

---

## 8. 分期与工作量

| 里程碑 | 内容 | 交付 / 验收 | 人日 |
|---|---|---|---|
| M0 | 样本摸底(§1 六问),写 `docs/DNGZIP-SAMPLE-NOTES.md` 记结论 | zlib/libjpeg 开关决策、帧率来源结论 | 0.5 |
| M1 | build 脚本 LibRaw 段 + CMakeLists 链接 + INTERFACES §6/许可更新 | `libraw.a` 两个 ABI 均装进 prebuilt,链接后 `nm` 有 `libraw_init` | 1.5 |
| M2 | `dngzip.h/.cpp`:ZIP 索引(ZIP64)+ 逐帧解压;LibRaw 单帧解码→`rgb48le` AVFrame;debug JNI 导出第 N 帧为 PPM | 真机解出 3 帧 PPM,与 host 侧 LibRaw 输出 diff < 1 LSB | 2 |
| M3 | `transcode.cpp` 接入 filtergraph/prores_ks/mov/进度/取消;`preview.cpp` zip 分支 | 真机转 3 帧 / 30 帧 / 整包 900 帧成功 | 1.5 |
| M4 | Kotlin:mime/白名单/nativeInfo zip/MediaFacts/参数卡/错误文案/JobStore | UI 端到端跑通;错误路径有中文文案 | 1.5 |
| M5 | 验收(§7)+ 性能数据写 README + 回归 | 1080p60 < 20min,4K < 90min;原用例全绿 | 1 |
| M6(后续,不阻塞) | demosaic 多线程流水;MCRAW 源(mirsadm/motioncam-decoder,Apache-2.0) | 4K 提速 2~4× | 另计 |

**合计约 8.5~9.5 人日。**

后续架构伏笔:把 `dngzip` 抽象成内部 `FrameSource` 接口(open / next_frame / close),
M6 的 `.mcraw` 源(同样来自 MotionCam,含音频与 ns 时间戳)就是第二个实现,不必改
`transcode.cpp` 的主流程。

---

## 9. 风险与备选(含已否决的技术路线)

| ID | 风险 / 备选 | 结论 |
|---|---|---|
| R1 | LibRaw 在 NDK 上编不过(CMake 版本/工具链) | 中。备选:用 autotools(`./configure --host=aarch64-linux-android`)而非 CMake;再退:从 vcpkg/conan 的 Android triplet 反向抄 configure 参数 |
| R2 | 速度不可用(4K 30fps 每秒要 30 帧) | 高。首版就上 half_size 档 + UI 预估;M6 做多线程。这是「后台慢慢转」的产品,不是实时 |
| R3 | 大内存(6K/8K) | 中。UI 上限判断 + 强制 P50 选项 |
| R4 | **直接用 FFmpeg 6.1 `tiff.c` 解 DNG** | **否决**:CFA 只出 Bayer、无 demosaic/WB/色彩矩阵/opcode;`img2.c` 虽有 `dng`→TIFF 映射,同样绕不过去 |
| R5 | **升 FFmpeg 7.1 用 `libraw` demuxer** | **否决**:破坏契约 §6 的 6.1 pin(`--enable-mediacodec`/jni 等配套全得重验),收益只是少写 ~600 行 glue;且 zip 容器仍然要自己解 |
| R6 | 解 zip 到 cacheDir 再交给 FFmpeg image2 | 否决:多占一倍的磁盘(4K chunk 可达 10GB)、多一次全量 IO,违背本仓「SAF 零拷贝 fd」的立意 |
| R7 | MotionCam 新格式 `.mcraw` 成为主要输入 | 已预留:`FrameSource` 接口 + M6(mirsadm/motioncam-decoder,Apache-2.0,解出 DNG 帧 + WAV) |

---

## 10. 新增依赖与许可

| 组件 | 版本 | 许可 | 入库位置 | 备注 |
|---|---|---|---|---|
| LibRaw | pin `0.21.4`(M0 后可升 `0.22.x`) | LGPL-2.1-only **或** CDDL-1.0(双选一) | `prebuilt/<abi>/lib/libraw.a` + `include/libraw` | 与现有 GPL-3.0 FFmpeg 共存无冲突;README §5 许可段补一行 + INTERFACES §6 记版本 |
| zlib | NDK 27 sysroot 自带 | zlib | 不落地 | deflate DNG / zip 解压都必须 |
| libjpeg-turbo(可选) | 3.0.x | BSD-3-Clause 风格 | 同 prefix | **仅当** M0 发现 DNG 是 baseline JPEG(lossy)才加 |

许可落地动作(随 M1 一起做):README §5 增加 LibRaw 条目(注明「按 LGPL-2.1 使用」与源码获取
途径),`docs/INTERFACES.md` §6 的预编译产物清单增加 `libraw.a` 与版本 pin。
